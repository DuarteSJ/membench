/*
 * Access-pattern primitives with deliberately distinct PMU signatures.
 * See membench.h for the contract of each pattern. Both are driven in bounded
 * chunks via a cursor so the worker loop re-checks its stop flag / work target
 * often (a full pass over a multi-GB region is far too coarse for that).
 */

#define _GNU_SOURCE

#include "membench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <immintrin.h>

#include <numa.h>

/* deterministic, full-width PRNG (xorshift64*) so chase rings are reproducible
 * across runs given a seed. rand() tops out at RAND_MAX and can't index a
 * multi-million-node region. */
static uint64_t xs_state;
static inline uint64_t xs_next(void)
{
    uint64_t x = xs_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    xs_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
/* uniform in [0, bound) */
static inline uint64_t xs_below(uint64_t bound)
{
    return xs_next() % bound;
}

/* ---- region lifecycle --------------------------------------------------- */

int region_alloc(region_t *r, size_t bytes, int numa_node)
{
    void *p = NULL;
    long pagesz = sysconf(_SC_PAGESIZE);

    memset(r, 0, sizeof(*r));
    if (bytes < CACHELINE)
        return -1;

    if (numa_node < 0) {
        /* unbound: anonymous pages the kernel can place and migrate */
        if (posix_memalign(&p, CACHELINE, bytes) != 0) {
            fprintf(stderr, "region_alloc: posix_memalign(%zu) failed\n", bytes);
            return -1;
        }
    } else {
        p = numa_alloc_onnode(bytes, numa_node);
        if (!p) {
            fprintf(stderr, "region_alloc: numa_alloc_onnode(node %d) failed\n",
                    numa_node);
            return -1;
        }
    }

    /* first-touch every page so the mapping is fully populated before timing */
    for (size_t off = 0; off < bytes; off += (size_t)pagesz)
        ((volatile char *)p)[off] = 0;

    r->buf = p;
    r->size = bytes;
    r->numa_node = numa_node;
    r->nlines = bytes / CACHELINE;
    return 0;
}

void region_free(region_t *r)
{
    if (!r->buf)
        return;
    if (r->numa_node >= 0)
        numa_free(r->buf, r->size);   /* pinned: from numa_alloc_onnode */
    else
        free(r->buf);                 /* unbound: from posix_memalign */
    memset(r, 0, sizeof(*r));
}

void cursor_init(cursor_t *c, const region_t *r)
{
    c->cur = (const chase_node_t *)r->buf;
    c->line = 0;
    c->acc = 0;
    c->delay = 0;   /* caller sets the scatter think-time after init */
}

/* ---- chase: dependent, low-MLP, latency-bound --------------------------- */

void chase_build(region_t *r, uint64_t seed)
{
    size_t n = r->size / sizeof(chase_node_t);
    chase_node_t *node = r->buf;
    size_t *idx;
    size_t i;

    assert(n > 1);
    xs_state = seed ? seed : 0x123456789ULL;

    idx = malloc(n * sizeof(size_t));
    if (!idx) {
        fprintf(stderr, "chase_build: OOM building %zu-node permutation\n", n);
        exit(1);
    }
    for (i = 0; i < n; i++)
        idx[i] = i;

    /* Sattolo's algorithm: produces a single-cycle permutation, so linking
     * along it yields one Hamiltonian ring that visits every node per pass. */
    for (i = n - 1; i > 0; i--) {
        size_t j = xs_below(i);          /* strictly < i -> single cycle */
        size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (i = 0; i < n; i++)
        node[idx[i]].next = &node[idx[(i + 1) % n]];

    free(idx);
}

size_t chase_chunk(const region_t *r, cursor_t *c, size_t lines)
{
    const chase_node_t *p = c->cur;
    uint64_t acc = c->acc;
    size_t i;

    (void)r;
    for (i = 0; i < lines; i++) {
        acc += p->pad[0];      /* touch the line; dependency is via p = p->next */
        p = p->next;
    }
    c->cur = p;
    c->acc = acc;
    return lines;
}

/* ---- scatter: independent, high-MLP, BW-bound --------------------------- */

size_t scatter_chunk(const region_t *r, cursor_t *c, size_t lines)
{
    uint64_t *base = (uint64_t *)r->buf;
    size_t nlines = r->nlines;
    size_t mask = nlines - 1;
    size_t elems_per_line = CACHELINE / sizeof(uint64_t);  /* 8 */
    uint64_t acc = 0;
    size_t done;

    /* power-of-two line count lets the wrap be a cheap mask instead of a per-
     * access modulo. A division in the hot loop would add latency that masks
     * the memory signal. Validated in main(). */
    assert(nlines && (nlines & mask) == 0);

    for (done = 0; done < lines; done++) {
        /* Sequential, data-independent reads (index from a counter), one u64
         * per line -> high MLP, latency hidden, BW-bound. The high miss rate is
         * just region >> cache (compulsory misses), so no need to scramble the
         * order to defeat the prefetcher. */
        size_t line = c->line & mask;
        acc += base[line * elems_per_line];
        c->line++;
    }
    c->acc += acc;

    /* think-time: throttle the access cadence WITHOUT descheduling (sleep would
     * free the core and cut traffic coarsely). Busy _mm_pause keeps the thread
     * on its core, just slows scatter so it stops saturating tier bandwidth */
    for (uint64_t d = 0; d < c->delay; d++)
        _mm_pause();

    return done;
}
