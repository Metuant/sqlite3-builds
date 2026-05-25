#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ITERATIONS = 20000 };
enum { WARMUP = 1000 };

static int now_ns(long long *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    *out = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 0;
}

static int cmp_ll(const void *a, const void *b) {
    long long av = *(const long long *)a;
    long long bv = *(const long long *)b;
    return (av > bv) - (av < bv);
}

static int measure_clock_overhead(long long *samples) {
    int i;

    for (i = 0; i < ITERATIONS; i++) {
        long long start;
        long long end;

        if (now_ns(&start) != 0) {
            fprintf(stderr, "clock_gettime(start) failed: %s\n", strerror(errno));
            return 1;
        }
        if (now_ns(&end) != 0) {
            fprintf(stderr, "clock_gettime(end) failed: %s\n", strerror(errno));
            return 1;
        }
        samples[i] = end - start;
    }

    qsort(samples, ITERATIONS, sizeof(samples[0]), cmp_ll);
    printf("alloc_latency_bench: clock_overhead_ns=%lld\n",
           samples[ITERATIONS / 2]);
    return 0;
}

static int run_case(const char *name, int size, int touch, long long *samples) {
    int i;

    for (i = 0; i < WARMUP; i++) {
        void *p = sqlite3_malloc(size);
        if (!p) {
            fprintf(stderr, "FAIL [%s]: sqlite3_malloc(%d) returned NULL at warmup %d\n",
                    name, size, i);
            return 1;
        }
        if (touch) memset(p, 0xa5, (size_t)size);
        sqlite3_free(p);
    }

    for (i = 0; i < ITERATIONS; i++) {
        long long start;
        long long end;
        void *p;

        if (now_ns(&start) != 0) {
            fprintf(stderr, "clock_gettime(start) failed: %s\n", strerror(errno));
            return 1;
        }
        p = sqlite3_malloc(size);
        if (!p) {
            fprintf(stderr, "FAIL [%s]: sqlite3_malloc(%d) returned NULL at iteration %d\n",
                    name, size, i);
            return 1;
        }
        if (touch) memset(p, 0xa5, (size_t)size);
        sqlite3_free(p);
        if (now_ns(&end) != 0) {
            fprintf(stderr, "clock_gettime(end) failed: %s\n", strerror(errno));
            return 1;
        }
        samples[i] = end - start;
    }

    qsort(samples, ITERATIONS, sizeof(samples[0]), cmp_ll);
    printf("alloc_latency_bench [%s]: size=%d case=%s median_ns=%lld p95_ns=%lld\n",
           name,
           size,
           touch ? "alloc_touch_free" : "alloc_free",
           samples[ITERATIONS / 2],
           samples[(ITERATIONS * 95) / 100]);
    return 0;
}

int main(void) {
    const char *enabled = getenv("RUN_BENCH");
    long long *samples;
    int failures = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        printf("SKIP alloc_latency_bench: set RUN_BENCH=1 to run\n");
        return 0;
    }

    if (sqlite3_initialize() != SQLITE_OK) {
        fprintf(stderr, "FAIL: sqlite3_initialize failed\n");
        return 1;
    }

    samples = sqlite3_malloc64((sqlite3_uint64)sizeof(long long) * ITERATIONS);
    if (!samples) {
        fprintf(stderr, "FAIL: sample allocation failed\n");
        sqlite3_shutdown();
        return 1;
    }

    failures += measure_clock_overhead(samples);
    failures += run_case("small", 64, 0, samples);
    failures += run_case("small", 64, 1, samples);
    failures += run_case("medium", 4096, 0, samples);
    failures += run_case("medium", 4096, 1, samples);
    failures += run_case("large", 65536, 0, samples);
    failures += run_case("large", 65536, 1, samples);

    sqlite3_free(samples);
    sqlite3_shutdown();
    return failures == 0 ? 0 : 1;
}
