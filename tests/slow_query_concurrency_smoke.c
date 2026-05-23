#include "sqlite3.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int slow_query_test_record_sql(const char *sql, sqlite3_int64 elapsed_ns);
uint32_t slow_query_test_entries_used(void);
uint32_t slow_query_test_max_entries(void);
void slow_query_test_disable_atexit_dump(void);

int obs_is_disabled(void) {
    const char *v = getenv("SQLITE3_DISABLE_OBSERVABILITY");
    return v && strcmp(v, "1") == 0;
}

int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    (void)trace;
    (void)ctx;
    (void)p;
    (void)x;
    return 0;
}

void obs_logf(const char *fn, const char *fmt, ...) {
    va_list ap;
    if (obs_is_disabled()) return;
    fprintf(stderr, "[sqlite3-builds-obs] test 0 0 %s", fn);
    if (fmt && fmt[0]) {
        fputc(' ', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

static void *worker(void *arg) {
    intptr_t tid = (intptr_t)arg;
    int i;
    char sql[96];
    for (i = 0; i < 700; i++) {
        snprintf(sql, sizeof(sql), "SELECT concurrency_%ld_%d", (long)tid, i);
        slow_query_test_record_sql(sql, 1000);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[4];
    intptr_t i;
    uint32_t entries;

    for (i = 0; i < 4; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "FATAL: pthread_create failed for thread %ld\n", (long)i);
            return 1;
        }
    }
    for (i = 0; i < 4; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "FATAL: pthread_join failed for thread %ld\n", (long)i);
            return 1;
        }
    }
    entries = slow_query_test_entries_used();
    if (entries != slow_query_test_max_entries()) {
        fprintf(stderr, "FATAL: entries_used=%" PRIu32 " want=%" PRIu32 "\n",
                entries, slow_query_test_max_entries());
        return 1;
    }
    slow_query_test_disable_atexit_dump();
    printf("slow query concurrency smoke passed entries_used=%" PRIu32 "\n", entries);
    return 0;
}
