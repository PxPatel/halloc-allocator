/*
 * bench.c - deterministic benchmark: halloc/hfree vs. glibc malloc/free
 *
 * Every scenario is seeded, so both allocators see the exact same sequence
 * of requests. Each scenario runs one warmup pass, then REPS timed passes,
 * and we report the median time.
 *
 * Build:  gcc -O2 -Wall -Wextra -o bench bench.c allocator.c
 * Run:    ./bench              (all scenarios)
 *         ./bench churn        (only scenarios whose name contains "churn")
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "allocator.h"

#define REPS 7

/* ------------------------------------------------------------------ */
/* Allocator interface: lets every scenario run through the same code  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    void *(*alloc)(size_t);
    void (*release)(void *);
    void *(*resize)(void *, size_t);
} Allocator;

static const Allocator ALLOCATORS[2] = {
    {"halloc", halloc, hfree, hrealloc},
    {"malloc", malloc, free, realloc},
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Written to on every block so the compiler can't optimize allocations away. */
static volatile unsigned long sink;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* xorshift64: tiny, fast, deterministic PRNG */
static unsigned long long rng_state;

static void rng_seed(unsigned long long seed) { rng_state = seed ? seed : 1; }

static unsigned long long rng_next(void) {
    unsigned long long x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void touch(void *p, size_t n) {
    unsigned char *b = p;
    b[0] = (unsigned char)n;
    b[n - 1] = (unsigned char)(n >> 8);
}

static void *xalloc(const Allocator *a, size_t n) {
    void *p = a->alloc(n);
    if (!p) {
        fprintf(stderr, "%s: allocation of %zu bytes failed\n", a->name, n);
        exit(1);
    }
    touch(p, n);
    return p;
}

static void *xresize(const Allocator *a, void *p, size_t n) {
    void *q = a->resize(p, n);
    if (!q) {
        fprintf(stderr, "%s: resize to %zu bytes failed\n", a->name, n);
        exit(1);
    }
    touch(q, n);
    return q;
}

static void xfree(const Allocator *a, void *p) {
    sink += *(unsigned char *)p; /* read the block before releasing it */
    a->release(p);
}

/*
 * Bookkeeping lives in static arrays (not the heap), so we never mix our own
 * bookkeeping into the heap being measured.
 */
#define MAX_PTRS 100000
static void *ptrs[MAX_PTRS];
static size_t order[MAX_PTRS];

static void shuffle(size_t *arr, size_t n) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rng_next() % (i + 1);
        size_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

/* Allocate many small blocks, free them in the order they were allocated. */
static void sc_free_fifo(const Allocator *a) {
    for (size_t i = 0; i < MAX_PTRS; i++) ptrs[i] = xalloc(a, 32);
    for (size_t i = 0; i < MAX_PTRS; i++) xfree(a, ptrs[i]);
}

/* Same, but free in reverse order (most recent first). */
static void sc_free_lifo(const Allocator *a) {
    for (size_t i = 0; i < MAX_PTRS; i++) ptrs[i] = xalloc(a, 32);
    for (size_t i = MAX_PTRS; i-- > 0;) xfree(a, ptrs[i]);
}

/* Tight alloc/free loop: how fast can freed memory be reused? */
static void sc_churn(const Allocator *a) {
    for (size_t i = 0; i < 1000000; i++) {
        void *p = xalloc(a, 16 + (i % 8) * 16);
        xfree(a, p);
    }
}

/* Random sizes, random free order: the closest to a real program. */
#define POOL 4096
static void *pool[POOL];

static void sc_random_mixed(const Allocator *a) {
    memset(pool, 0, sizeof pool);
    for (size_t i = 0; i < 500000; i++) {
        size_t idx = rng_next() % POOL;
        if (pool[idx]) {
            xfree(a, pool[idx]);
            pool[idx] = NULL;
        } else {
            pool[idx] = xalloc(a, 1 + rng_next() % 512);
        }
    }
    for (size_t i = 0; i < POOL; i++)
        if (pool[i]) xfree(a, pool[i]);
}

/*
 * Fragmentation: fill the heap, free every other block, then ask for more.
 * "reuse" requests fit in the holes; "nofit" requests are too big for any
 * single hole, so the allocator must find or create space elsewhere.
 */
static void frag_run(const Allocator *a, size_t second_size) {
    for (size_t i = 0; i < MAX_PTRS; i++) ptrs[i] = xalloc(a, 64);
    for (size_t i = 1; i < MAX_PTRS; i += 2) {
        xfree(a, ptrs[i]);
        ptrs[i] = NULL;
    }
    for (size_t i = 1; i < MAX_PTRS; i += 2) ptrs[i] = xalloc(a, second_size);
    for (size_t i = 0; i < MAX_PTRS; i++) xfree(a, ptrs[i]);
}

static void sc_frag_reuse(const Allocator *a) { frag_run(a, 48); }
static void sc_frag_nofit(const Allocator *a) { frag_run(a, 96); }

/*
 * Coalescing: free neighbors in shuffled order, then request one big block
 * that only fits if the freed blocks merged back together.
 */
static void sc_coalesce(const Allocator *a) {
    enum { COUNT = 5000, ROUNDS = 20, BIG = 100 * 1024 };
    for (int r = 0; r < ROUNDS; r++) {
        for (size_t i = 0; i < COUNT; i++) {
            ptrs[i] = xalloc(a, 64);
            order[i] = i;
        }
        shuffle(order, COUNT);
        for (size_t i = 0; i < COUNT; i++) xfree(a, ptrs[order[i]]);
        void *big = xalloc(a, BIG);
        xfree(a, big);
    }
}

/*
 * Realloc growth: grow one buffer by 1.5x per step while small "blocker"
 * allocations sit right after it, which prevents in-place growth.
 */
static void sc_realloc_growth(const Allocator *a) {
    enum { ROUNDS = 200, MAX_BLOCKERS = 64 };
    void *blockers[MAX_BLOCKERS];
    for (int r = 0; r < ROUNDS; r++) {
        size_t size = 16;
        size_t nb = 0;
        void *p = xalloc(a, size);
        while (size < 65536 && nb < MAX_BLOCKERS) {
            size += size / 2;
            p = xresize(a, p, size);
            blockers[nb++] = xalloc(a, 32);
        }
        xfree(a, p);
        for (size_t i = 0; i < nb; i++) xfree(a, blockers[i]);
    }
}

/*
 * Mixed sizes: mostly small, some medium, a few large, with a sliding window
 * of live blocks (oldest freed first).
 */
static void sc_mixed_sizes(const Allocator *a) {
    enum { WINDOW = 1000, ITERS = 200000 };
    static void *ring[WINDOW];
    memset(ring, 0, sizeof ring);
    for (size_t i = 0; i < ITERS; i++) {
        size_t slot = i % WINDOW;
        if (ring[slot]) xfree(a, ring[slot]);
        unsigned long long r = rng_next() % 100;
        unsigned long long r2 = rng_next();
        size_t size;
        if (r < 70)
            size = 16 + r2 % 113;        /* 16 - 128 bytes     */
        else if (r < 95)
            size = 129 + r2 % 3968;      /* 129 - 4096 bytes   */
        else
            size = 4097 + r2 % 61440;    /* 4 KB - 64 KB       */
        ring[slot] = xalloc(a, size);
    }
    for (size_t i = 0; i < WINDOW; i++)
        if (ring[i]) xfree(a, ring[i]);
}

typedef struct {
    const char *name;
    void (*fn)(const Allocator *);
} Scenario;

static const Scenario SCENARIOS[] = {
    {"free_fifo", sc_free_fifo},
    {"free_lifo", sc_free_lifo},
    {"churn", sc_churn},
    {"random_mixed", sc_random_mixed},
    {"frag_reuse", sc_frag_reuse},
    {"frag_nofit", sc_frag_nofit},
    {"coalesce", sc_coalesce},
    {"realloc_growth", sc_realloc_growth},
    {"mixed_sizes", sc_mixed_sizes},
};

#define NUM_SCENARIOS (sizeof SCENARIOS / sizeof SCENARIOS[0])

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

static double run_median_ms(const Scenario *sc, const Allocator *a) {
    double times[REPS];

    rng_seed(12345); /* warmup pass */
    sc->fn(a);

    for (int i = 0; i < REPS; i++) {
        rng_seed(12345); /* identical request sequence every time */
        double start = now_ms();
        sc->fn(a);
        times[i] = now_ms() - start;
    }
    qsort(times, REPS, sizeof times[0], cmp_double);
    return times[REPS / 2];
}

/*
 * halloc and glibc malloc both grow the heap with sbrk, so they can't share
 * a process: if glibc moves the program break, halloc's next extension is no
 * longer contiguous with its heap. Each (scenario, allocator) pair therefore
 * runs in its own forked process with a fresh heap, and sends back its
 * median time through a pipe. Returns -1.0 if the child crashed.
 */
static double run_isolated(const Scenario *sc, const Allocator *a) {
    int fd[2];
    if (pipe(fd) != 0) {
        perror("pipe");
        exit(1);
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        close(fd[0]);
        double ms = run_median_ms(sc, a);
        ssize_t w = write(fd[1], &ms, sizeof ms);
        _exit(w == (ssize_t)sizeof ms ? 0 : 1);
    }
    close(fd[1]);
    double ms = -1.0;
    ssize_t got = read(fd[0], &ms, sizeof ms);
    close(fd[0]);
    int status;
    waitpid(pid, &status, 0);
    return got == (ssize_t)sizeof ms ? ms : -1.0;
}

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;

    /* If your allocator needs an init call, add it here. */

    printf("Median of %d runs, in milliseconds (lower is better)\n\n", REPS);
    printf("%-16s %12s %12s %14s\n", "scenario", "halloc", "malloc", "halloc/malloc");
    printf("%-16s %12s %12s %14s\n", "--------", "------", "------", "-------------");

    for (size_t s = 0; s < NUM_SCENARIOS; s++) {
        const Scenario *sc = &SCENARIOS[s];
        if (filter && !strstr(sc->name, filter)) continue;

        double h = run_isolated(sc, &ALLOCATORS[0]);
        double m = run_isolated(sc, &ALLOCATORS[1]);
        if (h < 0 || m < 0) {
            printf("%-16s %12s %12s\n", sc->name,
                   h < 0 ? "CRASHED" : "ok", m < 0 ? "CRASHED" : "ok");
        } else {
            printf("%-16s %12.2f %12.2f %13.2fx\n", sc->name, h, m, m > 0 ? h / m : 0.0);
        }
        fflush(stdout);
    }

    printf("\nRatio above 1.00x means halloc is slower than malloc.\n");
    return (int)(sink & 0); /* use sink so it can't be optimized away */
}