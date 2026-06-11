# membench

Access-pattern benchmark for evaluating tiered-memory hotness
tracking, specifically, whether **AOL-weighted hotness** (access-latency
weighting from the SOAR/ALTO PMU model) places pages better than stock MEMTIS,
which ranks hotness purely by LLC-miss sample count.

## The core idea

MEMTIS hotness = count of `*_LLC_LOAD_MISS` PEBS samples per page. Two pages
with the **same miss count** can have very different value-of-promotion:

| pattern  | LLC misses | MLP   | per-access cost   | MEMTIS sees | should promote? |
|----------|-----------|-------|-------------------|-------------|-----------------|
| `chase`  | high      | ~1    | full latency      | **hot**     | **yes**         |
| `scatter`| high      | high  | hidden by overlap | **hot**     | no (cheap)      |

Per equal access count `chase` and `scatter` **tie** on miss count, so stock MEMTIS
cannot rank one over the other. The only signal that separates them is AOL
(`A1/A3` ≈ exposed latency per request). Demonstrating that gap is the whole
point.

## Access patterns (`src/workloads.c`)

- **chase** - dependent pointer chase over a single-cycle random ring.
  Each load's address comes from the previous load -> MLP~=1, latency exposed.
- **scatter** - scattered but **data-independent** reads (odd-multiplier / Fibonacci
  hash of a counter, not of loaded data). Prefetcher can't follow it (-> misses)
  but the OOO core overlaps many misses (-> high MLP, latency hidden). The index
  is `(counter * MULT) & (nlines-1)`; that masked multiply is a full bijection
  (every line once, scattered) **only when `nlines` is a power of two**, so the
  region size must be a power of two — `-L` non-pow2 is a hard error, no fallback.
  `-D <n>` adds **think-time** (busy-`pause` between chunks): throttles scatter's
  access cadence so it stops saturating the slow tier's bandwidth, turning it
  into a **high-miss but low-value** region — exactly the page stock MEMTIS
  over-promotes and AOL down-weights. scatter-only; `chase` ignores `-D`.

(No `seq` control: a prefetch-friendly linear read MEMTIS already filters to
near-zero misses is nothing to rank against chase/scatter.)

## Test tiers (fast -> slow)

1. **Characterize** - `scripts/characterize.sh`
   runs chase and scatter under `perf stat` and prints AOL / MLP / LLC-miss-rate /
   throughput. Proves the workloads have the intended PMU signatures **before**
   touching the kernel. Look for chase AOL >> scatter AOL — that gap is the lever.

   It also prints **`k = scatter/chase`** throughput: the corun starting ratio.
   corun regions are equal size (`-L`), so one pass = `nlines` accesses for both;
   equal fast-tier time then needs scatter to run `k x` chase's passes, i.e. start
   the sweep at `-M ≈ k*-N` and fine-tune. `k` depends on region size and machine
   — recalibrate on the target box.

   ```sh
   make -C src
   sudo scripts/characterize.sh           # DUR=5 REGION_MB=2048 THREADS=1 to taste
   ```

   To fit the kernel's AOL parameters (`a`, `b`) on this box, see
   [`calibrate/`](calibrate/README.md) — drives the **SOAR/ALTO microbench**
   (pointer-chase + sequential, two points) on both NUMA tiers and prints the
   scaled kernel `#define`s.

2. **Weight swing (current AOL kernel)** - confirm the kernel's global
   `aol_weight` actually rises in a chase-only window and falls in an scatter-only
   window (watch the `htmm_aol:` printk). *Planned harness.*

3. **Placement / perf (current AOL kernel)** - the co-run that shows AOL beating
   stock on wall time: chase and scatter contend for fast-tier capacity, and the
   makespan exposes which placement policy chose right. Uses `corun` mode below.

## `single` mode (implemented)

Drives one pure pattern for a fixed wall-time; emits a machine-readable line.
Used as the body of tier 1.

`-L` (region MB) must be a power of two for `scatter` (bijection requirement above).

`-D <n>` is scatter-only think-time: `n` busy-`_mm_pause` iterations per ~1 MiB
chunk, throttling cadence *without descheduling*. Sweep it (watch `gib_per_s`)
to drop scatter's bandwidth below the slow-tier ceiling while keeping its LLC-miss
count above `chase`. `n` is a spin count, not a time unit. Needs per machine
re-sweep (PAUSE latency varies). `chase` ignores it.

```sh
./src/membench -p chase   -L 2048 -s 10 -t 1 -c 0            # unbound region
./src/membench -p scatter -L 2048 -s 10 -t 4 -c 0 -X 0       # 4 threads, pinned node 0 (needs NUMA=1)
./src/membench -p scatter -L 2048 -s 10 -t 1 -c 0 -D 2000    # throttled scatter (think-time)
```

## `corun` mode (implemented)

Runs chase **and** scatter concurrently (chase on `core0`, scatter on `core0+1`),
each doing a **fixed amount of work** — `-N` chase passes, `-M` scatter passes,
where 1 pass = one full region traversal (`nlines` accesses). Each reports its own
completion time; a `summary` line carries makespan and sum. This is tier 3: the
metric is **per-job completion time**, not throughput.

Regions are placed independently — `-A` chase node, `-B` scatter node (`<0` =
unbound) — so one binary covers every placement:

| flags          | placement                         |
|----------------|-----------------------------------|
| `-A 0 -B 0`    | both fast (baseline / calibration)|
| `-A 0 -B 2`    | chase fast, scatter slow (**AOL pick**)   |
| `-A 2 -B 0`    | scatter fast, chase slow (**MEMTIS pick**)|
| `-A 2 -B 2`    | both slow                         |
| `-A -1 -B -1`  | unbound — let MEMTIS place/migrate|

**Calibration:** in the both-fast run (`-A 0 -B 0`), sweep `-N`/`-M`/`-D` until the
two `sec=` match (start at `-M ≈ k*-N` from characterize) **and** scatter still has
more LLC misses than chase. Lock those three, then run the cross-tier placements
with them fixed. Putting chase in the slow tier blows up makespan (latency-bound,
no MLP to hide it); putting throttled scatter there barely moves it — that gap is
the misplacement cost stock MEMTIS pays.

For the unbound (MEMTIS-managed) run, the k-calibrated `-N`/`-M` matter: they keep
both jobs spanning ~the same duration, so the faster one doesn't finish early and
let the kernel remigrate the survivor before the contested window is observed.

```sh
# 1. calibrate (both fast): tune -N/-M/-D until the two sec= match
sudo ./src/membench -m corun -L 2048 -A 0 -B 0 -N 5 -M 50 -D 8000 -c 0
# 2. MEMTIS pick (scatter fast, chase slow) — same N/M/D
sudo ./src/membench -m corun -L 2048 -A 2 -B 0 -N 5 -M 50 -D 8000 -c 0
# 3. AOL pick (chase fast, scatter slow)
sudo ./src/membench -m corun -L 2048 -A 0 -B 2 -N 5 -M 50 -D 8000 -c 0
```

Compare `makespan_sec` across runs: MEMTIS's pick should be the worst.
