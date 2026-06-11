/*
 * membench — purpose-built tiered-memory access-pattern benchmark.
 *
 * `single` mode: drives ONE pure access pattern over one persistent region for a
 * fixed wall-time, reporting throughput. The unit `scripts/characterize.sh`
 * wraps in `perf stat` to verify each pattern's PMU signature (LLC-miss rate,
 * AOL = A1/A3).
 *
 * `corun` mode: runs chase AND scatter concurrently, one thread each, each doing a
 * FIXED amount of work (-N chase passes, -M scatter passes; 1 pass = one full region
 * traversal = nlines accesses) and reporting its own completion time. Each region
 * is placed independently (-A chase node, -B scatter node), so one binary covers every
 * placement: both fast, both slow, one-each, or unbound (-A -1 -B -1) letting
 * MEMTIS migrate. Metric = per-job completion time + makespan.
 *
 * Calibration: sweep -N/-M/-D in the both-fast run (-A 0 -B 0) until chase and
 * scatter finish in ~equal time and scatter has more llc misses than chase; lock those
 * three, then run the cross-tier placements. Putting chase in the slow tier blows
 * up makespan (latency-bound, no MLP to hide it); putting throttled scatter there
 * barely moves it. That gap is the misplacement cost stock MEMTIS pays (it ranks
 * scatter hotter by miss count) and AOL-weighted hotness avoids.
 */

#define _GNU_SOURCE

#include "membench.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* lines processed per chunk before re-checking the stop flag / work target
 * (~1 MiB). Small enough a worker reacts within a fraction of a ms. */
#define CHUNK_LINES (1u << 14)

typedef size_t (*chunk_fn)(const region_t *, cursor_t *, size_t);

struct worker {
    pthread_t       th;
    int             core;
    int             node;    /* region's NUMA node (for reporting; <0 unbound) */
    const char     *role;    /* "chase" / "scatter" (corun output) */
    const region_t *region;
    chunk_fn        fn;
    volatile int   *stop;
    pthread_barrier_t *start;
    uint64_t        delay;   /* scatter think-time, passed to the cursor */
    uint64_t        target;  /* stop after this many lines; 0 = run until stop flag */
    uint64_t        lines;   /* out: lines this worker processed */
    double          secs;    /* out: self-timed elapsed (barrier release -> done) */
    uint64_t        sink;    /* out: accumulator, kept live so loads aren't DCE'd */
};

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int pin_to_core(int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    cursor_t c;

    if (w->core >= 0 && pin_to_core(w->core) != 0)
        fprintf(stderr, "warn: could not pin worker to core %d: %s\n",
                w->core, strerror(errno));

    cursor_init(&c, w->region);
    c.delay = w->delay;        /* scatter reads this; chase_chunk ignores it */
    pthread_barrier_wait(w->start);

    double t0 = now_sec();
    if (w->target) {           /* fixed-work (corun): self-terminate at target */
        while (w->lines < w->target && !*w->stop)
            w->lines += w->fn(w->region, &c, CHUNK_LINES);
    } else {                   /* timed (single): run until main flags stop */
        while (!*w->stop)
            w->lines += w->fn(w->region, &c, CHUNK_LINES);
    }
    w->secs = now_sec() - t0;

    w->sink = c.acc;
    return NULL;
}

static chunk_fn pattern_fn(const char *name)
{
    if (!strcmp(name, "chase")) return chase_chunk;
    if (!strcmp(name, "scatter"))   return scatter_chunk;
    /* No `seq`: a prefetch control MEMTIS already filters (near-zero LLC
     * misses) is nothing to rank against chase/scatter. */
    return NULL;
}

/* alloc + prepare a region for one pattern on one node. scatter needs a power-of-two
 * line count (the index multiply is only a bijection mod 2^n); chase needs its
 * ring built. Returns 0 on success, -1 on error (region freed on error). */
static int setup_region(region_t *r, chunk_fn fn, size_t region_mb,
                        int node, uint64_t seed)
{
    size_t bytes = region_mb * (1ULL << 20);
    if (region_alloc(r, bytes, node) != 0)
        return -1;
    if (fn == scatter_chunk &&
        (r->nlines == 0 || (r->nlines & (r->nlines - 1)))) {
        fprintf(stderr, "scatter requires a power-of-two line count; -L %zu MB gives "
                        "%zu lines. Use a power-of-two region size.\n",
                region_mb, r->nlines);
        region_free(r);
        return -1;
    }
    if (fn == chase_chunk)
        chase_build(r, seed);
    return 0;
}

/* spawn n workers, release them together, collect results. Termination:
 *   secs > 0  -> timed: sleep `secs`, then flag stop (single mode).
 *   secs <= 0 -> fixed-work: each worker self-terminates at its `target` (corun).
 * Each worker self-times into w->secs. Returns 0, or -1 on spawn failure. */
static int run_workers(struct worker *ws, int n, double secs)
{
    volatile int stop = 0;
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, n + 1);

    for (int i = 0; i < n; i++) {
        ws[i].stop = &stop;
        ws[i].start = &start;
        ws[i].lines = 0;
        ws[i].sink = 0;
        ws[i].secs = 0;
        if (pthread_create(&ws[i].th, NULL, worker_main, &ws[i]) != 0) {
            fprintf(stderr, "pthread_create worker %d failed\n", i);
            return -1;
        }
    }

    pthread_barrier_wait(&start);     /* release workers together */
    if (secs > 0) {                   /* timed: deadline, then flag stop */
        struct timespec req = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
        nanosleep(&req, NULL);
        stop = 1;
    }                                 /* else fixed-work: targets self-terminate */

    for (int i = 0; i < n; i++)
        pthread_join(ws[i].th, NULL);
    pthread_barrier_destroy(&start);
    return 0;
}

/* one machine-readable corun line per region/role; sec = that job's own time */
static void emit_corun(const struct worker *w, size_t region_mb)
{
    double accesses = (double)w->lines;
    printf("membench,mode=corun,role=%s,region_mb=%zu,node=%d,"
           "sec=%.3f,accesses=%.0f,maccess_per_s=%.2f,gib_per_s=%.3f,delay=%llu\n",
           w->role, region_mb, w->node, w->secs, accesses,
           accesses / w->secs / 1e6,
           accesses * CACHELINE / w->secs / (double)(1ULL << 30),
           (unsigned long long)w->delay);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [opts]\n"
        "  -m <single|corun>   mode                      (default single)\n"
        "  -L <MB>             region size (each region) (default 2048)\n"
        "  -c <core0>          first core to pin to      (default 0)\n"
        "  -S <seed>           chase ring RNG seed       (default 1)\n"
        "  -D <n>              scatter think-time: busy-pause iters/chunk (default 0)\n"
        "\n"
        " single mode (timed):\n"
        "  -p <chase|scatter>  access pattern            (default chase)\n"
        "  -s <sec>            run duration              (default 10)\n"
        "  -t <n>              worker threads            (default 1)\n"
        "  -X <node>           NUMA node, <0 = unbound   (default -1)\n"
        "\n"
        " corun mode (fixed work; chase on core0, scatter on core0+1):\n"
        "  -N <passes>         chase work, fractional ok (1 pass = full traversal) (default 20)\n"
        "  -M <passes>         scatter work, fractional ok (1 pass = full traversal) (default 20)\n"
        "  -A <node>           chase region node, <0 unbound (default -1)\n"
        "  -B <node>           scatter region node, <0 unbound (default -1)\n"
        "  -O <chase|scatter>  which region to alloc/first-touch first (default chase)\n"
        "  (both unbound = let MEMTIS place/migrate; -A 0 -B 2 = one per tier)\n"
        "  tune -N/-M/-D in the both-fast run (-A 0 -B 0) until secs match, then\n"
        "  lock them and run the cross-tier placements.\n"
        "\n"
        "Emits machine-readable line(s) on stdout:\n"
        "  single: membench,mode=single,pattern=<p>,region_mb=<L>,threads=<t>,"
        "sec=<s>,accesses=<n>,maccess_per_s=<r>,gib_per_s=<bw>,delay=<D>\n"
        "  corun : membench,mode=corun,role=<chase|scatter>,region_mb=<L>,node=<N>,"
        "sec=<s>,accesses=<n>,maccess_per_s=<r>,gib_per_s=<bw>,delay=<D>  (x2)\n"
        "          + membench,mode=corun,summary,makespan_sec=<m>,sum_sec=<t>\n",
        p);
}

static int run_single(const char *pattern, size_t region_mb, double secs,
                      int threads, int core0, int node, uint64_t seed,
                      uint64_t delay)
{
    chunk_fn fn = pattern_fn(pattern);
    if (!fn) { fprintf(stderr, "unknown pattern: %s\n", pattern); return 2; }
    if (threads < 1) threads = 1;

    region_t region;
    if (setup_region(&region, fn, region_mb, node, seed) != 0)
        return 1;

    fprintf(stderr, "membench single: pattern=%s region=%zuMB threads=%d "
                    "node=%d dur=%.1fs nlines=%zu\n",
            pattern, region_mb, threads, node, secs, region.nlines);

    uint64_t eff_delay = (fn == scatter_chunk) ? delay : 0;   /* chase ignores -D */
    struct worker *ws = calloc(threads, sizeof(*ws));
    for (int i = 0; i < threads; i++) {
        ws[i].core = core0 + i;
        ws[i].node = node;
        ws[i].region = &region;
        ws[i].fn = fn;
        ws[i].delay = eff_delay;
    }

    if (run_workers(ws, threads, secs) != 0) {
        free(ws); region_free(&region); return 1;
    }

    uint64_t total_lines = 0, sink = 0;
    double elapsed = 0;
    for (int i = 0; i < threads; i++) {
        total_lines += ws[i].lines;
        sink ^= ws[i].sink;
        if (ws[i].secs > elapsed) elapsed = ws[i].secs;   /* slowest worker */
    }

    double accesses = (double)total_lines;
    printf("membench,mode=single,pattern=%s,region_mb=%zu,threads=%d,"
           "sec=%.3f,accesses=%.0f,maccess_per_s=%.2f,gib_per_s=%.3f,delay=%llu\n",
           pattern, region_mb, threads, elapsed, accesses,
           accesses / elapsed / 1e6,
           accesses * CACHELINE / elapsed / (double)(1ULL << 30),
           (unsigned long long)eff_delay);
    fprintf(stderr, "  sink=%llu (ignore)\n", (unsigned long long)sink);

    free(ws);
    region_free(&region);
    return 0;
}

static int run_corun(size_t region_mb, int core0, int chase_node, int scatter_node,
                     uint64_t seed, uint64_t delay, double chase_passes,
                     double scatter_passes, int scatter_first)
{
    region_t chreg, screg;

    /* Allocation order = first-touch order, which decides who grabs the fast
     * tier when DRAM is capped (pages go DRAM-first until the cap, then spill).
     * `scatter_first` lets the late-touched region start in the slow tier. */
    if (scatter_first) {
        if (setup_region(&screg, scatter_chunk, region_mb, scatter_node, seed) != 0)
            return 1;
        if (setup_region(&chreg, chase_chunk, region_mb, chase_node, seed) != 0) {
            region_free(&screg);
            return 1;
        }
    } else {
        if (setup_region(&chreg, chase_chunk, region_mb, chase_node, seed) != 0)
            return 1;
        if (setup_region(&screg, scatter_chunk, region_mb, scatter_node, seed) != 0) {
            region_free(&chreg);
            return 1;
        }
    }

    fprintf(stderr, "membench corun: region=%zuMB  alloc=%s-first  "
                    "chase(node=%d core=%d passes=%g)  "
                    "scatter(node=%d core=%d passes=%g D=%llu)\n",
            region_mb, scatter_first ? "scatter" : "chase",
            chase_node, core0, chase_passes,
            scatter_node, core0 + 1, scatter_passes,
            (unsigned long long)delay);

    struct worker ws[2];
    memset(ws, 0, sizeof(ws));
    ws[0].core = core0;     ws[0].node = chase_node; ws[0].role = "chase";
    ws[0].region = &chreg;  ws[0].fn = chase_chunk;       ws[0].delay = 0;
    ws[0].target = (uint64_t)(chase_passes * chreg.nlines + 0.5);
    ws[1].core = core0 + 1; ws[1].node = scatter_node;   ws[1].role = "scatter";
    ws[1].region = &screg; ws[1].fn = scatter_chunk;  ws[1].delay = delay;
    ws[1].target = (uint64_t)(scatter_passes * screg.nlines + 0.5);

    /* target==0 would drop the worker into the timed loop, which in corun has no
     * deadline -> infinite spin. Reject before that can happen. */
    if (ws[0].target == 0 || ws[1].target == 0) {
        fprintf(stderr, "corun: passes too small — target rounds to 0 lines "
                        "(chase=%llu scatter=%llu). Raise -N/-M.\n",
                (unsigned long long)ws[0].target,
                (unsigned long long)ws[1].target);
        region_free(&chreg); region_free(&screg); return 1;
    }

    if (run_workers(ws, 2, 0) != 0) {     /* secs=0 -> fixed-work termination */
        region_free(&chreg); region_free(&screg); return 1;
    }

    emit_corun(&ws[0], region_mb);
    emit_corun(&ws[1], region_mb);
    double makespan = ws[0].secs > ws[1].secs ? ws[0].secs : ws[1].secs;
    printf("membench,mode=corun,summary,makespan_sec=%.3f,sum_sec=%.3f\n",
           makespan, ws[0].secs + ws[1].secs);
    fprintf(stderr, "  sink=%llu (ignore)\n",
            (unsigned long long)(ws[0].sink ^ ws[1].sink));

    region_free(&chreg);
    region_free(&screg);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = "single";
    const char *pattern = "chase";
    size_t region_mb = 2048;
    double secs = 10.0;
    int threads = 1, core0 = 0, node = -1;
    int chase_node = -1, scatter_node = -1;
    uint64_t seed = 1, delay = 0;
    double chase_passes = 20, scatter_passes = 20;
    int scatter_first = 0;
    int opt;

    while ((opt = getopt(argc, argv, "m:p:L:s:t:c:X:S:D:A:B:N:M:O:h")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'p': pattern = optarg; break;
        case 'L': region_mb = strtoull(optarg, NULL, 10); break;
        case 's': secs = strtod(optarg, NULL); break;
        case 't': threads = atoi(optarg); break;
        case 'c': core0 = atoi(optarg); break;
        case 'X': node = atoi(optarg); break;
        case 'S': seed = strtoull(optarg, NULL, 10); break;
        case 'D': delay = strtoull(optarg, NULL, 10); break;
        case 'A': chase_node = atoi(optarg); break;
        case 'B': scatter_node = atoi(optarg); break;
        case 'N': chase_passes = strtod(optarg, NULL); break;
        case 'M': scatter_passes = strtod(optarg, NULL); break;
        case 'O':
            if (!strcmp(optarg, "scatter")) scatter_first = 1;
            else if (!strcmp(optarg, "chase")) scatter_first = 0;
            else { fprintf(stderr, "-O wants chase|scatter, got %s\n", optarg);
                   return 2; }
            break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    if (!strcmp(mode, "single"))
        return run_single(pattern, region_mb, secs, threads, core0, node,
                          seed, delay);
    if (!strcmp(mode, "corun"))
        return run_corun(region_mb, core0, chase_node, scatter_node, seed, delay,
                         chase_passes, scatter_passes, scatter_first);

    fprintf(stderr, "unknown mode: %s (want single|corun)\n", mode);
    return 2;
}
