/*
 * slbench — standalone split-lock cost benchmark (no dependencies beyond libc).
 *
 * Measures the cost of a LOCK CMPXCHG on an 8-byte operand that straddles a
 * 64-byte cache-line boundary ("split lock"), against an aligned LOCK CMPXCHG
 * and a plain read-modify-write as baselines. Three modes:
 *
 *   1. isolated  — one op at a time, serialized with rdtscp+lfence, a 200 µs
 *                  quiet gap between samples; reports min/p50/p99. This is the
 *                  per-op hardware latency without queuing effects.
 *   2. saturated — tight loop for 3 s; reports ops/s. Back-to-back bus locks
 *                  queue behind each other's system-wide quiesce, so this is
 *                  the worst case, not the typical per-op cost.
 *   3. duty      — one split op followed by k aligned ops per iteration
 *                  (k = 10..10000); the marginal split cost is the iteration
 *                  time minus the k-aligned-only baseline. Models workloads
 *                  where split locks are interleaved with real work.
 *
 * The buffer is page-aligned and the split offset is 60, so the operand
 * crosses a cache line but never a page (a page-split takes a different,
 * even slower path and is out of scope).
 *
 * Usage: slbench [cpu-to-pin]   (default cpu 4)
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <x86intrin.h>

static double cycles_per_ns;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline uint64_t tsc(void)
{
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

static void calibrate(void)
{
    uint64_t c0 = tsc(), n0 = now_ns();
    uint64_t target = n0 + 200000000ull;
    while (now_ns() < target)
        _mm_pause();
    uint64_t c1 = tsc(), n1 = now_ns();
    cycles_per_ns = (double)(c1 - c0) / (double)(n1 - n0);
}

typedef void (*op_fn)(volatile void *);

static void op_cas(volatile void *p)
{
    __sync_val_compare_and_swap((volatile long long *)p, 0LL, 1LL);
}

static void op_plain(volatile void *p)
{
    *(volatile long long *)p = *(volatile long long *)p + 1;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void isolated(const char *label, volatile void *p, op_fn op)
{
    enum { MAX = 2000 };
    static uint64_t s[MAX];
    int n = 0;
    uint64_t deadline = now_ns() + 5000000000ull; /* cap at 5 s for slow tiers */
    while (n < MAX && now_ns() < deadline) {
        uint64_t gap = now_ns() + 200000; /* 200 µs quiet gap */
        while (now_ns() < gap)
            _mm_pause();
        uint64_t t0 = tsc();
        op(p);
        uint64_t t1 = tsc();
        s[n++] = t1 - t0;
    }
    qsort(s, n, sizeof(uint64_t), cmp_u64);
    printf("isolated  %-22s samples=%4d  min=%11.0f ns  p50=%11.0f ns  p99=%11.0f ns\n",
           label, n,
           s[0] / cycles_per_ns,
           s[n / 2] / cycles_per_ns,
           s[(int)(n * 0.99)] / cycles_per_ns);
}

static void saturated(const char *label, volatile void *p, op_fn op)
{
    uint64_t t0 = now_ns(), deadline = t0 + 3000000000ull;
    long ops = 0;
    while (now_ns() < deadline) {
        for (int i = 0; i < 16; i++)
            op(p);
        ops += 16;
    }
    double el = (now_ns() - t0) / 1e9;
    printf("saturated %-22s ops=%10ld  rate=%12.0f ops/s  avg=%11.0f ns/op\n",
           label, ops, ops / el, el * 1e9 / ops);
}

static void duty(volatile void *split, volatile void *aligned, int k)
{
    uint64_t t0, deadline;
    long iters;

    t0 = now_ns();
    deadline = t0 + 1500000000ull;
    iters = 0;
    while (now_ns() < deadline) {
        for (int i = 0; i < k; i++)
            op_cas(aligned);
        iters++;
    }
    double base = (double)(now_ns() - t0) / iters;

    t0 = now_ns();
    deadline = t0 + 2000000000ull;
    iters = 0;
    while (now_ns() < deadline) {
        op_cas(split);
        for (int i = 0; i < k; i++)
            op_cas(aligned);
        iters++;
    }
    double with = (double)(now_ns() - t0) / iters;

    printf("duty      1 split : %-6d aligned   base=%9.0f ns/iter  with=%12.0f ns/iter  marginal split cost=%11.0f ns\n",
           k, base, with, with - base);
}

int main(int argc, char **argv)
{
    int cpu = argc > 1 ? atoi(argv[1]) : 4;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        perror("sched_setaffinity");

    calibrate();
    printf("pinned to cpu %d, tsc %.2f GHz\n", cpu, cycles_per_ns);

    char *buf;
    if (posix_memalign((void **)&buf, 4096, 4096))
        return 1;
    volatile void *aligned = buf;
    volatile void *split = buf + 60;

    isolated("plain rmw (aligned)", aligned, op_plain);
    isolated("lock cas (aligned)", aligned, op_cas);
    isolated("lock cas (split)", split, op_cas);

    saturated("lock cas (aligned)", aligned, op_cas);
    saturated("lock cas (split)", split, op_cas);

    duty(split, aligned, 10);
    duty(split, aligned, 100);
    duty(split, aligned, 1000);
    duty(split, aligned, 10000);

    return 0;
}