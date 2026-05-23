#include "sqlite3.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THREADS 4
#define STEPS_PER_THREAD 250000
#define RUNS 7

struct bench_arg {
    int id;
    long long elapsed_ns;
};

static long long mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void fatal_sql(sqlite3 *db, const char *label, int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "FATAL: %s rc=%d err=%s\n", label, rc, sqlite3_errmsg(db));
        exit(1);
    }
}

static void setup_db(sqlite3 *db) {
    fatal_sql(db, "setup", sqlite3_exec(db,
        "CREATE TABLE metadata_items("
        "id INTEGER PRIMARY KEY, library_section_id INTEGER, title_sort TEXT, updated_at INTEGER);"
        "CREATE INDEX metadata_items_section_title ON metadata_items(library_section_id, title_sort);"
        "CREATE INDEX metadata_items_updated ON metadata_items(updated_at);"
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<1000)"
        "INSERT INTO metadata_items(id, library_section_id, title_sort, updated_at) "
        "SELECT x, x%8, printf('title-%06d', x), 1700000000+x FROM c;",
        NULL, NULL, NULL));
}

static void *worker(void *argp) {
    struct bench_arg *arg = (struct bench_arg *)argp;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmts[4] = { NULL, NULL, NULL, NULL };
    int i;
    int open_rc = sqlite3_open(":memory:", &db);
    long long start;

    fatal_sql(db, "open", open_rc);
    setup_db(db);
    fatal_sql(db, "prepare1", sqlite3_prepare_v2(db,
        "SELECT title_sort FROM metadata_items WHERE library_section_id=?1 ORDER BY title_sort LIMIT 20",
        -1, &stmts[0], NULL));
    fatal_sql(db, "prepare2", sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM metadata_items WHERE updated_at>?1",
        -1, &stmts[1], NULL));
    fatal_sql(db, "prepare3", sqlite3_prepare_v2(db,
        "SELECT id FROM metadata_items WHERE title_sort>=?1 ORDER BY title_sort LIMIT 1",
        -1, &stmts[2], NULL));
    fatal_sql(db, "prepare4", sqlite3_prepare_v2(db,
        "SELECT library_section_id, MAX(updated_at) FROM metadata_items GROUP BY library_section_id",
        -1, &stmts[3], NULL));

    start = mono_ns();
    for (i = 0; i < STEPS_PER_THREAD; i++) {
        sqlite3_stmt *stmt = stmts[i % 4];
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, (arg->id + i) % 8);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
        }
    }
    arg->elapsed_ns = mono_ns() - start;
    for (i = 0; i < 4; i++) sqlite3_finalize(stmts[i]);
    sqlite3_close(db);
    return NULL;
}

int main(void) {
    pthread_t threads[THREADS];
    struct bench_arg args[THREADS];
    int i;
    int run;
    long long runs[RUNS];

    if (!getenv("RUN_BENCH") || strcmp(getenv("RUN_BENCH"), "1")) {
        printf("SKIP bench\n");
        return 0;
    }
    for (run = 0; run < RUNS; run++) {
        long long max_elapsed = 0;
        for (i = 0; i < THREADS; i++) {
            args[i].id = i;
            args[i].elapsed_ns = 0;
            if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
                fprintf(stderr, "FATAL: pthread_create failed for N>=4 metadata bench\n");
                return 1;
            }
        }
        for (i = 0; i < THREADS; i++) {
            pthread_join(threads[i], NULL);
            if (args[i].elapsed_ns > max_elapsed) max_elapsed = args[i].elapsed_ns;
        }
        runs[run] = max_elapsed;
    }
    for (i = 1; i < RUNS; i++) {
        int j = i;
        long long v = runs[i];
        while (j > 0 && runs[j - 1] > v) {
            runs[j] = runs[j - 1];
            j--;
        }
        runs[j] = v;
    }
    printf("slow_query_metadata_bench threads=%d steps_per_thread=%d runs=%d median_elapsed_ns=%lld\n",
           THREADS, STEPS_PER_THREAD, RUNS, runs[RUNS / 2]);
    return 0;
}
