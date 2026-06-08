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
| `mlp`    | high      | high  | hidden by overlap | **hot**     | no (cheap)      |

Per equal access count `chase` and `mlp` **tie** on miss count, so stock MEMTIS
cannot rank one over the other. The only signal that separates them is AOL
(`A1/A3` ≈ exposed latency per request). Demonstrating that gap is the whole
point.

## Access patterns (`src/workloads.c`)

- **chase** - dependent pointer chase over a single-cycle random ring.
  Each load's address comes from the previous load -> MLP~=1, latency exposed.
- **mlp** - scattered but **data-independent** reads (odd-multiplier / Fibonacci
  hash of a counter, not of loaded data). Prefetcher can't follow it (-> misses)
  but the OOO core overlaps many misses (-> high MLP, latency hidden). The index
  is `(counter * MULT) & (nlines-1)`; that masked multiply is a full bijection
  (every line once, scattered) **only when `nlines` is a power of two**, so the
  region size must be a power of two — `-L` non-pow2 is a hard error, no fallback.

(No `seq` control: a prefetch-friendly linear read MEMTIS already filters to
near-zero misses is nothing to rank against chase/mlp.)

## Test tiers (fast -> slow)

1. **Characterize** - `scripts/characterize.sh`
   runs chase and mlp under `perf stat` and prints AOL / MLP / LLC-miss-rate /
   throughput. Proves the workloads have the intended PMU signatures **before**
   touching the kernel. Look for chase AOL >> mlp AOL — that gap is the lever.

   It also prints **`k = mlp/chase`** accesses-per-second: the equal-wall-time
   **calibration multiplier**. Size the co-run regions so mlp does `k x` chase's
   accesses and both finish together when fully DRAM-resident; then forcing
   either region to slow tier isolates placement quality (chase suffers far
   more — latency-bound, no overlap to hide Optane latency). `k` depends on
   region size and machine — recalibrate on the target box at the co-run size.

   ```sh
   make -C src
   sudo scripts/characterize.sh           # DUR=5 REGION_MB=2048 THREADS=1 to taste
   ```

   To fit the kernel's AOL parameters (`a`, `b`) on this box, see
   [`calibrate/`](calibrate/README.md) — drives the **SOAR/ALTO microbench**
   (pointer-chase + sequential, two points) on both NUMA tiers and prints the
   scaled kernel `#define`s.

2. **Weight swing (current AOL kernel)** - confirm the kernel's global
   `aol_weight` actually rises in a chase-only window and falls in an mlp-only
   window (watch the `htmm_aol:` printk). *Planned harness.*

3. **Placement / perf (current AOL kernel, phased)** - the managed run that
   shows AOL beating stock on wall time. Needs `phase` mode below.

## `single` mode (implemented)

Drives one pure pattern for a fixed wall-time; emits a machine-readable line.
Used as the body of tier 1.

`-L` (region MB) must be a power of two for `mlp` (bijection requirement above).

```sh
./src/membench -p chase -L 2048 -s 10 -t 1 -c 0      # unbound region
./src/membench -p mlp   -L 2048 -s 10 -t 4 -c 0 -X 0 # 4 threads, pinned node 0 (needs NUMA=1)
```
