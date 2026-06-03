/*
 * membench — purpose-built tiered-memory access-pattern benchmark.
 *
 * Increment 1 (this file): `single` mode. Drives ONE pure access pattern over
 * one persistent region for a fixed wall-time, reporting throughput. This is
 * the unit that `scripts/characterize.sh` wraps in `perf stat` to verify each
 * pattern's PMU signature (LLC-miss rate, AOL = A1/A3, MLP) on ANY kernel,
 * with no reboot — the fast feedback tier.
 *
 * Increment 2 (planned, see README): `phase` mode. Two persistent regions,
 * worker pool alternates chase / stream_mlp on a timer so the current AOL
 * kernel's single global weight swings between phases, letting AOL-weighted
 * hotness diverge from stock MEMTIS without a per-sample kernel change.
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

/* lines processed per chunk before re-checking the stop flag (~1 MiB). Small
 * enough that a worker reacts to a deadline within a fraction of a ms. */
#define CHUNK_LINES (1u << 14)

typedef size_t (*chunk_fn)(const region_t *, cursor_t *, size_t);

struct worker {
    pthread_t       th;
    int             core;
    const region_t *region;
    chunk_fn        fn;
    volatile int   *stop;
    pthread_barrier_t *start;
    uint64_t        lines;   /* out: lines this worker processed */
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
    pthread_barrier_wait(w->start);

    while (!*w->stop)
        w->lines += w->fn(w->region, &c, CHUNK_LINES);

    w->sink = c.acc;
    return NULL;
}

static chunk_fn pattern_fn(const char *name)
{
    if (!strcmp(name, "chase")) return chase_chunk;
    if (!strcmp(name, "mlp"))   return stream_mlp_chunk;
    /* No `seq`: a prefetch control MEMTIS already filters (near-zero LLC
     * misses) is nothing to rank against chase/mlp. */
    return NULL;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [opts]\n"
        "  -p <chase|mlp>      access pattern            (default chase)\n"
        "  -L <MB>             region size               (default 2048)\n"
        "  -s <sec>            run duration              (default 10)\n"
        "  -t <n>              worker threads            (default 1)\n"
        "  -c <core0>          first core to pin to      (default 0)\n"
        "  -X <node>           NUMA node, <0 = unbound   (default -1)\n"
        "  -S <seed>           chase ring RNG seed       (default 1)\n"
        "\n"
        "Emits one machine-readable line on stdout:\n"
        "  membench,mode=single,pattern=<p>,region_mb=<L>,threads=<t>,"
        "sec=<elapsed>,accesses=<n>,maccess_per_s=<r>,gib_per_s=<bw>\n",
        p);
}

int main(int argc, char **argv)
{
    const char *pattern = "chase";
    size_t region_mb = 2048;
    double secs = 10.0;
    int threads = 1, core0 = 0, node = -1;
    uint64_t seed = 1;
    int opt;

    while ((opt = getopt(argc, argv, "p:L:s:t:c:X:S:h")) != -1) {
        switch (opt) {
        case 'p': pattern = optarg; break;
        case 'L': region_mb = strtoull(optarg, NULL, 10); break;
        case 's': secs = strtod(optarg, NULL); break;
        case 't': threads = atoi(optarg); break;
        case 'c': core0 = atoi(optarg); break;
        case 'X': node = atoi(optarg); break;
        case 'S': seed = strtoull(optarg, NULL, 10); break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    chunk_fn fn = pattern_fn(pattern);
    if (!fn) { fprintf(stderr, "unknown pattern: %s\n", pattern); return 2; }
    if (threads < 1) threads = 1;

    region_t region;
    size_t bytes = region_mb * (1ULL << 20);
    if (region_alloc(&region, bytes, node) != 0)
        return 1;
    if (fn == stream_mlp_chunk &&
        (region.nlines == 0 || (region.nlines & (region.nlines - 1)))) {
        fprintf(stderr, "mlp requires a power-of-two line count; -L %zu MB gives "
                        "%zu lines. Use a power-of-two region size.\n",
                region_mb, region.nlines);
        region_free(&region);
        return 2;
    }
    if (fn == chase_chunk)
        chase_build(&region, seed);

    fprintf(stderr, "membench single: pattern=%s region=%zuMB threads=%d "
                    "node=%d dur=%.1fs nlines=%zu\n",
            pattern, region_mb, threads, node, secs, region.nlines);

    volatile int stop = 0;
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, threads + 1);

    struct worker *ws = calloc(threads, sizeof(*ws));
    for (int i = 0; i < threads; i++) {
        ws[i].core = core0 + i;
        ws[i].region = &region;
        ws[i].fn = fn;
        ws[i].stop = &stop;
        ws[i].start = &start;
        if (pthread_create(&ws[i].th, NULL, worker_main, &ws[i]) != 0) {
            fprintf(stderr, "pthread_create worker %d failed\n", i);
            return 1;
        }
    }

    pthread_barrier_wait(&start);     /* release workers together */
    double t0 = now_sec();
    /* coarse sleep then flag stop; workers exit within one chunk */
    struct timespec req = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
    nanosleep(&req, NULL);
    stop = 1;

    uint64_t total_lines = 0, sink = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(ws[i].th, NULL);
        total_lines += ws[i].lines;
        sink ^= ws[i].sink;
    }
    double elapsed = now_sec() - t0;

    double accesses = (double)total_lines;
    double maccess_per_s = accesses / elapsed / 1e6;
    double gib_per_s = accesses * CACHELINE / elapsed / (double)(1ULL << 30);

    printf("membench,mode=single,pattern=%s,region_mb=%zu,threads=%d,"
           "sec=%.3f,accesses=%.0f,maccess_per_s=%.2f,gib_per_s=%.3f\n",
           pattern, region_mb, threads, elapsed, accesses,
           maccess_per_s, gib_per_s);
    fprintf(stderr, "  sink=%llu (ignore)\n", (unsigned long long)sink);

    free(ws);
    pthread_barrier_destroy(&start);
    region_free(&region);
    return 0;
}
