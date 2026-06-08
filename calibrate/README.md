# calibrate

The AOL-weighted hotness kernel weights each PEBS sample by

```
weight = 1 + P * K          K = 1 / (a + b/AOL)
AOL    = A1 / A3            (exposed memory latency per demand read)
P      = s_LLC / c          (fraction of cycles stalled on L3 misses)
```

`a` and `b` are **hardware-dependent**. `calibrate.sh` measures them on the
target box and prints the scaled values, ready to write to the live sysfs knobs
`/sys/kernel/mm/htmm/htmm_aol_param_{a,b}`.

It uses the "same" benchmark that the **SOAR/ALTO** paper calibrated against
(random pointer-chase, intensive sequential read), so results are
comparable to the paper.

The locally-patched copy of the bench is under [`soar-microbench/`](soar-microbench/).
> [!NOTE]
> (Check [`LOCAL_PATCHES.md`](soar-microbench/LOCAL_PATCHES.md) to see the changes)

## Prereqs

```sh
make -C soar-microbench/src     # (needs libnuma)
```

`bench` flags: `-R 0.0 -A <MB>` = pure pointer-chase, `-R 1.0 -B <MB>` = pure
sequential, `-i <n>` = iterations.

## Method

**Two points**, both at buffer 2048 MB: pchase (`-R 0.0`, high AOL) and stream
(`-R 1.0`, low AOL). Per point:

1. Pilot on the **fast** tier with `-i 1`, time it; pick `-i ≈ DUR/t1` so the
   timed run takes ~`DUR`.
2. Fast tier under `perf` -> `A1, A3, s_LLC, c, t_fast`.
3. Slow tier (no perf) → `t_slow`.
4. `AOL=A1/A3`, `P=s_LLC/c`, `S=t_slow/t_fast − 1`, `K=S/P`.

Then solve `1/K = a + b·(1/AOL)` for `a, b`.

**Why only two.** From our reading of the paper, this is what the authors did.
Two workloads (pchase, stream), two points. Also, it's the easier path.
SoarAlto `-R` gives a clean single-workload PMU signature only at `0.0` (pure
pchase) and `1.0` (pure stream); `0 < R < 1` runs *both* threads at once ->
blended counters, not a usable third point. Two unknowns, two points -> the line
is exact (zero residual).

## Usage

```sh
sudo ./calibrate.sh                              # needs sudo because of perf

FAST=0 SLOW=2 DUR=15 BUF=2048 BENCH=/path/to/bench sudo -E ./calibrate.sh
```

`FAST`/`SLOW` are NUMA node IDs (`numactl -H`; slow = the PMEM/CXL/Optane tier).
CSV -> `results/<host>-calibrate.csv`; the solved `a, b` (scaled) + the exact
sysfs write commands print to stderr.

## Applying the result

The params are live sysfs knobs. Write the scaled values printed by the script:

```sh
echo <a*1024> | sudo tee /sys/kernel/mm/htmm/htmm_aol_param_a
echo <b*1024> | sudo tee /sys/kernel/mm/htmm/htmm_aol_param_b
```

`AOL_SCALE = 1024` is the fixed-point scale.
