#ifndef MEMBENCH_H
#define MEMBENCH_H

#include <stddef.h>
#include <stdint.h>

#define CACHELINE 64

/*
 * A region is one contiguous, page-aligned buffer plus the metadata needed to
 * drive an access pattern over it. Regions persist for the whole run so the
 * kernel's tiering logic (in our case, MEMTIS) can accumulate per-page hotness
 * and migrate them.
 *
 *   numa_node < 0  -> unbound: plain anonymous pages, first-touched. The kernel
 *                     places them and MEMTIS is free to migrate between tiers.
 *                     REQUIRED for managed runs (any mbind pins pages).
 *   numa_node >= 0 -> pinned to that node (reference / static-placement bars).
 */
typedef struct {
    void  *buf;        /* CACHELINE-aligned base */
    size_t size;       /* bytes */
    int    numa_node;  /* <0 unbound, >=0 pinned */
    size_t nlines;     /* size / CACHELINE */
} region_t;

/* One dependent-chase node, padded to a full cache line. */
typedef struct chase_node {
    struct chase_node *next;
    uint64_t pad[CACHELINE / sizeof(uint64_t) - 1];
} chase_node_t;

/*
 * Iterator state so a pattern can be driven in bounded chunks: the worker loop
 * re-checks its stop flag / work target between chunks (a full pass over a
 * multi-GB region is far too coarse a granularity for that).
 */
typedef struct {
    const chase_node_t *cur;  /* chase position */
    size_t   line;            /* stream line counter */
    uint64_t acc;             /* sink: defeats dead-code elimination */
    uint64_t delay;           /* scatter think-time: busy-pause iters per chunk (0=off) */
} cursor_t;

/* ---- access patterns ---------------------------------------------------- */

/*
 * chase: dependent pointer chase. Each load's address comes from the previous
 * load -> MLP ~= 1, full memory latency exposed on every miss.
 * PMU signature: high LLC-miss rate, HIGH AOL (A1/A3), low MLP. Latency-bound.
 */
void chase_build(region_t *r, uint64_t seed);
size_t chase_chunk(const region_t *r, cursor_t *c, size_t lines);

/*
 * scatter: scattered, prefetcher-defeating, but data-INDEPENDENT reads
 * (Fibonacci-hash index computed from a counter, not from loaded data). The
 * OOO core keeps many misses outstanding -> high MLP, latency hidden.
 * PMU signature: high LLC-miss rate (like chase), LOW AOL, high MLP. BW-bound.
 * Requires nlines to be a power of two (odd multiplier mod 2^n is a bijection).
 * Caller must validate this; the chunk loop assumes it.
 */
size_t scatter_chunk(const region_t *r, cursor_t *c, size_t lines);

/* ---- region lifecycle --------------------------------------------------- */

int  region_alloc(region_t *r, size_t bytes, int numa_node);
void region_free(region_t *r);
void cursor_init(cursor_t *c, const region_t *r);

#endif /* MEMBENCH_H */
