#include "sqlite3.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

int slow_query_test_record_sql(const char *sql, sqlite3_int64 elapsed_ns);
uint32_t slow_query_test_entries_used(void);
uint32_t slow_query_test_max_entries(void);
void slow_query_test_disable_atexit_dump(void);

enum { SLOW_QUERY_CONCURRENCY_THREADS = 4 };
#define SLOW_QUERY_CONCURRENCY_MARGIN 1u

struct worker_args {
    uint32_t tid;
    uint32_t queries;
};

int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    (void)trace;
    (void)ctx;
    (void)p;
    (void)x;
    return 0;
}

static void *worker(void *arg) {
    const struct worker_args *args = (const struct worker_args *)arg;
    uint32_t i;
    char sql[96];
    for (i = 0; i < args->queries; i++) {
        snprintf(sql, sizeof(sql), "SELECT concurrency_%" PRIu32 "_%" PRIu32, args->tid, i);
        slow_query_test_record_sql(sql, 1000);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[SLOW_QUERY_CONCURRENCY_THREADS];
    struct worker_args args[SLOW_QUERY_CONCURRENCY_THREADS];
    uint32_t i;
    uint32_t entries;
    uint32_t max_entries = slow_query_test_max_entries();
    uint32_t queries_per_thread =
        (max_entries / (uint32_t)SLOW_QUERY_CONCURRENCY_THREADS) +
        SLOW_QUERY_CONCURRENCY_MARGIN;

    for (i = 0; i < (uint32_t)SLOW_QUERY_CONCURRENCY_THREADS; i++) {
        args[i].tid = i;
        args[i].queries = queries_per_thread;
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "FATAL: pthread_create failed for thread %" PRIu32 "\n", i);
            return 1;
        }
    }
    for (i = 0; i < (uint32_t)SLOW_QUERY_CONCURRENCY_THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "FATAL: pthread_join failed for thread %" PRIu32 "\n", i);
            return 1;
        }
    }
    entries = slow_query_test_entries_used();
    if (entries != max_entries) {
        fprintf(stderr, "FATAL: entries_used=%" PRIu32 " want=%" PRIu32 "\n",
                entries, max_entries);
        return 1;
    }
    slow_query_test_disable_atexit_dump();
    printf("slow query concurrency smoke passed entries_used=%" PRIu32 "\n", entries);
    return 0;
}
