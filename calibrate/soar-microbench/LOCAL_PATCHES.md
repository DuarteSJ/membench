# Local patches to SoarAlto microbench

Upstream: https://github.com/MoatLab/SoarAlto/tree/main/src/microbenchmark

## `-S <N>` flag — configurable seq:pc multiplier

**Why.** Paper §2 hardcodes `op_iter * 26` read loops for the seq thread
so it issues ~26B loads vs ~4B for the pc thread, balancing thread
runtimes on the paper's Optane hardware. The ratio is HW-dependent (a
faster slow tier needs fewer seq iterations to match pc runtime). Made
it a CLI param and changed the default to 46 (it's what works for our machine).

**Files touched.**
- `src/utils.h`  -> added `int seq_mult` to `header_t`
- `src/utils.c`  -> set default `seq_mult = 26`, added `-S` to getopt
- `src/main.c`   -> replaced hardcoded `26` in `bandwidth()` with
                  `header->seq_mult`; dropped the `/* TODO */` comment

**Tuning on machine.** Measure `-R 0.0` and `-R 1.0` runtimes at default
`-S 26`, then set `S = 26 * (t_pchase / t_stream)`.

## `-N <node>` flag. Per-buffer NUMA placement

**Why.** Upstream allocates both buffers via plain `malloc`
(`init_buf_reg_alloc`), so per-buffer NUMA control was impossible.
Required for replicating Figure 1c of the SOAR/ALTO paper, where each
buffer (pchase = "cold-but-latency-bound", seq = "hot-but-bw-bound")
must live on a chosen tier independently.

**Files touched.**
- `src/utils.h`  -> added `int buf_b_numa_node` to `header_t`
- `src/utils.c`  -> default both nodes to `-1` (unbound), added `-N` to getopt
- `src/main.c`   -> each thread picks alloc path by node value:
                  `node < 0` -> `init_buf_reg_alloc` (libc malloc, no mbind);
                  `node >= 0` -> `init_buf` (`numa_alloc_onnode`).
                  Free path mirrors (`aligned_free` vs `numa_free`).

**Flags.**
- `-r <node>` (existing, but previously unused) -> pchase buffer A NUMA node
- `-N <node>` (new)      -> seq    buffer B NUMA node

**Defaults: unbound.** Omitting both flags makes the bench issue no
`mbind`; the kernel allocates wherever capacity is available and
MEMTIS (or whatever other system, if enabled) is free to migrate
pages between tiers. Required for MEMTIS-managed runs since any explicit
`mbind` would pin pages and block migration.

Pass `-r <node>`/`-N <node>` for explicit static placement (Fig 1c
reference bars):

```
./bench -R 0.5              # both unbound -> kernel chooses
./bench -R 0.5 -r 0 -N 0    # All-on-DRAM
./bench -R 0.5 -r 2 -N 0    # Hot-on-DRAM   (pc on Optane, seq on DRAM)
./bench -R 0.5 -r 0 -N 2    # Cold-on-DRAM  (pc on DRAM, seq on Optane)
./bench -R 0.5 -r 2 -N 2    # All-on-Optane (lower bound)
./bench -R 0.5 -r 0 -N -1   # mixed: pc pinned to DRAM, seq unbound
```
