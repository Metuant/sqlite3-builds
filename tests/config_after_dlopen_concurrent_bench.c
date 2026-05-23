#include "sqlite3.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
#define BENCH_THREADS 4
#define OPENS_PER_THREAD 128
#define SAMPLE_COUNT (BENCH_THREADS * OPENS_PER_THREAD)

typedef struct {
    const char *mode;
    int thread_index;
    uint64_t *samples;
    int failures;
} bench_thread;

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return 1;
    if (errno == EEXIST) return 1;

    fprintf(stderr, "mkdir(%s) failed: %s\n", path, strerror(errno));
    return 0;
}

static uint64_t elapsed_ns(const struct timespec *start, const struct timespec *end) {
    uint64_t start_ns = (uint64_t)start->tv_sec * 1000000000ULL + (uint64_t)start->tv_nsec;
    uint64_t end_ns = (uint64_t)end->tv_sec * 1000000000ULL + (uint64_t)end->tv_nsec;
    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

static void *bench_worker(void *arg) {
    bench_thread *bt = (bench_thread*)arg;
    char dir[256];
    char path[320];
    int i;

    if (snprintf(dir, sizeof(dir),
                 "/tmp/config_after_dlopen_bench_%s_%ld_%d",
                 bt->mode, (long)getpid(), bt->thread_index) >= (int)sizeof(dir)) {
        fprintf(stderr, "bench dir path too long for %s thread %d\n",
                bt->mode, bt->thread_index);
        bt->failures++;
        return NULL;
    }
    if (!ensure_dir(dir)) {
        bt->failures++;
        return NULL;
    }
    if (snprintf(path, sizeof(path), "%s/library.db", dir) >= (int)sizeof(path)) {
        fprintf(stderr, "bench db path too long for %s thread %d\n",
                bt->mode, bt->thread_index);
        bt->failures++;
        return NULL;
    }

    for (i = 0; i < OPENS_PER_THREAD; i++) {
        sqlite3 *db = NULL;
        struct timespec start;
        struct timespec end;
        int rc;

        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            fprintf(stderr, "clock_gettime(start) failed: %s\n", strerror(errno));
            bt->failures++;
            return NULL;
        }
        rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
        if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
            fprintf(stderr, "clock_gettime(end) failed: %s\n", strerror(errno));
            bt->failures++;
            if (db) sqlite3_close(db);
            return NULL;
        }

        bt->samples[i] = elapsed_ns(&start, &end);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "sqlite3_open_v2(%s) failed in %s thread %d: rc=%d",
                    path, bt->mode, bt->thread_index, rc);
            if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
            fprintf(stderr, "\n");
            bt->failures++;
        }
        if (db) {
            int close_rc = sqlite3_close(db);
            if (close_rc != SQLITE_OK) {
                fprintf(stderr, "sqlite3_close(%s) failed in %s thread %d: rc=%d\n",
                        path, bt->mode, bt->thread_index, close_rc);
                bt->failures++;
            }
        }
    }
    return NULL;
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t av = *(const uint64_t*)a;
    const uint64_t bv = *(const uint64_t*)b;
    return (av > bv) - (av < bv);
}

static uint64_t percentile(const uint64_t *samples, size_t count, unsigned pct) {
    size_t idx = ((count * pct) + 99) / 100;
    if (idx == 0) idx = 1;
    if (idx > count) idx = count;
    return samples[idx - 1];
}

static void classify_latency(const char *mode, const char *metric, uint64_t value) {
    if (value >= 1000000ULL) {
        printf("[bench][%s] FAIL %s=%llu ns reached ms band\n",
               mode, metric, (unsigned long long)value);
    } else if (value >= 1000ULL) {
        printf("[bench][%s] WARN %s=%llu ns reached us band\n",
               mode, metric, (unsigned long long)value);
    }
}

static int run_mode(const char *mode, int disabled) {
    pthread_t threads[BENCH_THREADS];
    bench_thread cases[BENCH_THREADS];
    uint64_t samples[SAMPLE_COUNT];
    int created[BENCH_THREADS];
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    int failures = 0;
    int i;

    if (disabled) {
        if (setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1", 1) != 0) {
            fprintf(stderr, "setenv(SQLITE3_DISABLE_AUTOPRAGMA) failed: %s\n",
                    strerror(errno));
            return 1;
        }
    } else if (unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) {
        fprintf(stderr, "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA) failed: %s\n",
                strerror(errno));
        return 1;
    }

    for (i = 0; i < BENCH_THREADS; i++) {
        cases[i].mode = mode;
        cases[i].thread_index = i;
        cases[i].samples = samples + (i * OPENS_PER_THREAD);
        cases[i].failures = 0;
        created[i] = 0;
        int err = pthread_create(&threads[i], NULL, bench_worker, &cases[i]);
        if (err != 0) {
            fprintf(stderr, "pthread_create(%s thread %d) failed: %s\n",
                    mode, i, strerror(err));
            failures++;
        } else {
            created[i] = 1;
        }
    }

    for (i = 0; i < BENCH_THREADS; i++) {
        if (created[i] && pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "pthread_join(%s thread %d) failed\n", mode, i);
            failures++;
        }
        failures += cases[i].failures;
    }

    if (failures) return 1;

    qsort(samples, SAMPLE_COUNT, sizeof(samples[0]), cmp_u64);
    p50 = percentile(samples, SAMPLE_COUNT, 50);
    p95 = percentile(samples, SAMPLE_COUNT, 95);
    p99 = percentile(samples, SAMPLE_COUNT, 99);

    printf("[bench][%s] p50=%llu ns / p95=%llu ns / p99=%llu ns\n",
           mode,
           (unsigned long long)p50,
           (unsigned long long)p95,
           (unsigned long long)p99);
    classify_latency(mode, "p50", p50);
    classify_latency(mode, "p95", p95);
    classify_latency(mode, "p99", p99);

    return failures ? 1 : 0;
}

int main(void) {
    const char *run_bench = getenv("RUN_BENCH");
    int failures = 0;

    if (!run_bench || strcmp(run_bench, "1") != 0) {
        printf("SKIP bench\n");
        return 0;
    }

    failures += run_mode("positive", 0);
    sqlite3_shutdown();
    failures += run_mode("disabled", 1);
    return failures == 0 ? 0 : 1;
}
