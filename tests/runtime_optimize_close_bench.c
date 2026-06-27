/*
 * Advisory microbench for runtime optimize inline hook hot exits.
 *
 * Why: target connections with runtime optimize enabled must keep close-adjacent
 * sqlite3_step(SQLITE_DONE), sqlite3_reset(SQLITE_OK), and
 * sqlite3_finalize(SQLITE_OK) exits near their disabled-mode cost even with many
 * idle cached statements. The binary prints p50/p95/p99/max and advisory gate
 * verdicts, but always exits 0 so latency drift is reported without failing CI.
 */
#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

enum { HOOK_ITERATIONS = 50000 };
enum { FINALIZE_ITERATIONS = 20000 };
enum { WARMUP_ITERATIONS = 1000 };
enum { IDLE_PEER_COUNT = 128 };

typedef struct {
    long long p50;
    long long p95;
    long long p99;
    long long max;
} bench_stats;

static int now_ns(long long *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    *out = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

static int cmp_ll(const void *a, const void *b) {
    long long av = *(const long long *)a;
    long long bv = *(const long long *)b;
    return (av > bv) - (av < bv);
}

static bench_stats summarize(long long *samples, int n) {
    bench_stats s;
    qsort(samples, (size_t)n, sizeof(samples[0]), cmp_ll);
    s.p50 = samples[n / 2];
    s.p95 = samples[(n * 95) / 100];
    s.p99 = samples[(n * 99) / 100];
    s.max = samples[n - 1];
    return s;
}

static void print_stats(const char *name, const char *mode, bench_stats s) {
    printf("runtime_optimize_close_bench [%s/%s]: p50_ns=%lld p95_ns=%lld p99_ns=%lld max_ns=%lld\n",
           name, mode, s.p50, s.p95, s.p99, s.max);
}

static int safe_setenv(const char *name, const char *value) {
    errno = 0;
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "setenv(%s=%s) failed: %s\n", name, value, strerror(errno));
        return 0;
    }
    return 1;
}

static int safe_unsetenv(const char *name) {
    errno = 0;
    if (unsetenv(name) != 0) {
        fprintf(stderr, "unsetenv(%s) failed: %s\n", name, strerror(errno));
        return 0;
    }
    return 1;
}

static int reset_runtime_env(void) {
    int ok = 1;
    if (!safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS")) ok = 0;
    return ok;
}

static int enable_runtime_optimize(void) {
    return safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "0");
}

static int disable_runtime_optimize(void) {
    return safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1");
}

static int set_inline_mode(int enabled) {
    if (!reset_runtime_env()) return 0;
    if (enabled) return enable_runtime_optimize();
    return disable_runtime_optimize();
}

static int remove_if_exists(const char *path) {
    if (unlink(path) == 0) return 1;
    if (errno == ENOENT) return 1;
    fprintf(stderr, "unlink(%s) failed: %s\n", path, strerror(errno));
    return 0;
}

static int clean_db(const char *path) {
    char sidecar[512];

    if (!remove_if_exists(path)) return 0;
    if (snprintf(sidecar, sizeof(sidecar), "%s-wal", path) >= (int)sizeof(sidecar)) return 0;
    if (!remove_if_exists(sidecar)) return 0;
    if (snprintf(sidecar, sizeof(sidecar), "%s-shm", path) >= (int)sizeof(sidecar)) return 0;
    return remove_if_exists(sidecar);
}

static int exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_exec(%s) failed: rc=%d: %s\n",
                label, rc, err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return 0;
    }
    sqlite3_free(err);
    return 1;
}

static int open_db(const char *path, sqlite3 **db, const char *label) {
    int rc = sqlite3_open_v2(path, db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2(%s) failed in %s: rc=%d",
                path, label, rc);
        if (*db) fprintf(stderr, ": %s", sqlite3_errmsg(*db));
        fprintf(stderr, "\n");
        if (*db) sqlite3_close(*db);
        *db = NULL;
        return 0;
    }
    return 1;
}

static int seed_rows(sqlite3 *db, const char *label, int rows) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int i;

    if (!exec_sql(db, label,
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE runtime_optimize_data("
            "id INTEGER PRIMARY KEY, bucket INTEGER NOT NULL, name TEXT NOT NULL);"
            "CREATE INDEX runtime_optimize_idx "
            "ON runtime_optimize_data(bucket, name);"
            "BEGIN;")) {
        return 0;
    }
    rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO runtime_optimize_data(bucket, name) VALUES(?1, ?2);",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare seed rows failed in %s: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    for (i = 0; i < rows; i++) {
        char name[64];
        snprintf(name, sizeof(name), "name-%05d", i);
        sqlite3_bind_int(stmt, 1, i % 97);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "seed step failed in %s at row %d: rc=%d: %s\n",
                    label, i, rc, sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize seed rows failed in %s: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    if (!exec_sql(db, label, "COMMIT;")) return 0;
    return exec_sql(db, label,
        "SELECT count(*) FROM runtime_optimize_data "
        "WHERE bucket=42 AND name>='name-00042';");
}

static int create_seeded_db_without_runtime_optimize(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!clean_db(path)) ok = 0;
    if (ok && !open_db(path, &db, label)) ok = 0;
    if (ok && !seed_rows(db, label, 3000)) ok = 0;
    if (db && sqlite3_close(db) != SQLITE_OK) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int step_to_done(sqlite3 *db, sqlite3_stmt *stmt, const char *label) {
    int rc;
    do {
        rc = sqlite3_step(stmt);
    } while (rc == SQLITE_ROW);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite3_step(%s) expected SQLITE_DONE, rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int trigger_full_success(const char *path) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, &db, "hot-target-full")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
            -1,
            &stmt,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare hot target full failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !step_to_done(db, stmt, "hot-target-full")) ok = 0;
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) ok = 0;
    if (db && sqlite3_close(db) != SQLITE_OK) ok = 0;
    return ok;
}

static int prepare_idle_peers(sqlite3 *db, sqlite3_stmt **peers, int n, const char *label) {
    int i;

    for (i = 0; i < n; i++) {
        int rc = sqlite3_prepare_v2(db, "SELECT ?1;", -1, &peers[i], NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare idle peer %s[%d] failed: rc=%d: %s\n",
                    label, i, rc, sqlite3_errmsg(db));
            return 0;
        }
    }
    return 1;
}

static int finalize_idle_peers(sqlite3 *db, sqlite3_stmt **peers, int n, const char *label) {
    int i;
    int ok = 1;

    for (i = 0; i < n; i++) {
        if (peers[i]) {
            int rc = sqlite3_finalize(peers[i]);
            peers[i] = NULL;
            if (rc != SQLITE_OK) {
                fprintf(stderr, "finalize idle peer %s[%d] failed: rc=%d: %s\n",
                        label, i, rc, sqlite3_errmsg(db));
                ok = 0;
            }
        }
    }
    return ok;
}

static int prepare_hot_target(const char *path) {
    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "hot-target-seed")) return 0;
    return trigger_full_success(path);
}

static int time_step_done_once(sqlite3 *db, sqlite3_stmt *stmt, long long *elapsed) {
    long long start;
    long long end;
    int rc;

    rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_reset(step-done setup) rc=%d: %s\n", rc, sqlite3_errmsg(db));
        return 0;
    }
    if (!now_ns(&start)) return 0;
    rc = sqlite3_step(stmt);
    if (!now_ns(&end)) return 0;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite3_step(step-done) rc=%d expected SQLITE_DONE: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    *elapsed = end - start;
    return 1;
}

static int measure_step_done_case(
    const char *path,
    int inline_enabled,
    long long *samples,
    bench_stats *stats
) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_stmt *idle_peers[IDLE_PEER_COUNT];
    int rc;
    int i;
    int ok = 1;

    memset(idle_peers, 0, sizeof(idle_peers));
    if (!set_inline_mode(inline_enabled)) return 0;
    if (!open_db(path, &db, inline_enabled ? "step-enabled" : "step-disabled")) return 0;
    if (!prepare_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "step-done")) ok = 0;
    rc = sqlite3_prepare_v2(db, "SELECT 1 WHERE 0;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare step-done failed: rc=%d: %s\n", rc, sqlite3_errmsg(db));
        finalize_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "step-done");
        sqlite3_close(db);
        return 0;
    }
    for (i = 0; ok && i < WARMUP_ITERATIONS; i++) {
        long long ignored;
        if (!time_step_done_once(db, stmt, &ignored)) ok = 0;
    }
    for (i = 0; ok && i < HOOK_ITERATIONS; i++) {
        if (!time_step_done_once(db, stmt, &samples[i])) ok = 0;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) ok = 0;
    if (!finalize_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "step-done")) ok = 0;
    if (sqlite3_close(db) != SQLITE_OK) ok = 0;
    if (!ok) return 0;
    *stats = summarize(samples, HOOK_ITERATIONS);
    print_stats("step-done", inline_enabled ? "enabled" : "disabled", *stats);
    return 1;
}

static int time_reset_once(sqlite3 *db, sqlite3_stmt *stmt, long long *elapsed) {
    long long start;
    long long end;
    int rc;

    rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) return 0;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "sqlite3_step(reset setup) rc=%d expected SQLITE_ROW: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    if (!now_ns(&start)) return 0;
    rc = sqlite3_reset(stmt);
    if (!now_ns(&end)) return 0;
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_reset(reset timed) rc=%d: %s\n", rc, sqlite3_errmsg(db));
        return 0;
    }
    *elapsed = end - start;
    return 1;
}

static int measure_reset_case(
    const char *path,
    int inline_enabled,
    long long *samples,
    bench_stats *stats
) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_stmt *idle_peers[IDLE_PEER_COUNT];
    int rc;
    int i;
    int ok = 1;

    memset(idle_peers, 0, sizeof(idle_peers));
    if (!set_inline_mode(inline_enabled)) return 0;
    if (!open_db(path, &db, inline_enabled ? "reset-enabled" : "reset-disabled")) return 0;
    if (!prepare_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "reset")) ok = 0;
    rc = sqlite3_prepare_v2(db, "SELECT 1;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare reset failed: rc=%d: %s\n", rc, sqlite3_errmsg(db));
        finalize_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "reset");
        sqlite3_close(db);
        return 0;
    }
    for (i = 0; ok && i < WARMUP_ITERATIONS; i++) {
        long long ignored;
        if (!time_reset_once(db, stmt, &ignored)) ok = 0;
    }
    for (i = 0; ok && i < HOOK_ITERATIONS; i++) {
        if (!time_reset_once(db, stmt, &samples[i])) ok = 0;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) ok = 0;
    if (!finalize_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "reset")) ok = 0;
    if (sqlite3_close(db) != SQLITE_OK) ok = 0;
    if (!ok) return 0;
    *stats = summarize(samples, HOOK_ITERATIONS);
    print_stats("reset", inline_enabled ? "enabled" : "disabled", *stats);
    return 1;
}

static int time_finalize_once(sqlite3 *db, long long *elapsed) {
    sqlite3_stmt *stmt = NULL;
    long long start;
    long long end;
    int rc = sqlite3_prepare_v2(db, "SELECT 1;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare finalize failed: rc=%d: %s\n", rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "sqlite3_step(finalize setup) rc=%d expected SQLITE_ROW: %s\n",
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    if (!now_ns(&start)) return 0;
    rc = sqlite3_finalize(stmt);
    if (!now_ns(&end)) return 0;
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_finalize(timed) rc=%d: %s\n", rc, sqlite3_errmsg(db));
        return 0;
    }
    *elapsed = end - start;
    return 1;
}

static int measure_finalize_case(
    const char *path,
    int inline_enabled,
    long long *samples,
    bench_stats *stats
) {
    sqlite3 *db = NULL;
    sqlite3_stmt *idle_peers[IDLE_PEER_COUNT];
    int i;
    int ok = 1;

    memset(idle_peers, 0, sizeof(idle_peers));
    if (!set_inline_mode(inline_enabled)) return 0;
    if (!open_db(path, &db, inline_enabled ? "finalize-enabled" : "finalize-disabled")) return 0;
    if (!prepare_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "finalize")) ok = 0;
    for (i = 0; ok && i < WARMUP_ITERATIONS; i++) {
        long long ignored;
        if (!time_finalize_once(db, &ignored)) ok = 0;
    }
    for (i = 0; ok && i < FINALIZE_ITERATIONS; i++) {
        if (!time_finalize_once(db, &samples[i])) ok = 0;
    }
    if (!finalize_idle_peers(db, idle_peers, IDLE_PEER_COUNT, "finalize")) ok = 0;
    if (sqlite3_close(db) != SQLITE_OK) ok = 0;
    if (!ok) return 0;
    *stats = summarize(samples, FINALIZE_ITERATIONS);
    print_stats("finalize", inline_enabled ? "enabled" : "disabled", *stats);
    return 1;
}

static long long positive_delta(long long enabled, long long disabled) {
    return enabled > disabled ? enabled - disabled : 0;
}

static long long max_ll(long long a, long long b) {
    return a > b ? a : b;
}

static int gate_hook_delta(const char *name, bench_stats enabled, bench_stats disabled) {
    long long p99_limit = max_ll(100, disabled.p99 / 100);
    long long p99_delta = positive_delta(enabled.p99, disabled.p99);
    long long max_delta = positive_delta(enabled.max, disabled.max);
    if (p99_delta <= p99_limit && max_delta < 1000000LL) {
        printf("PASS gate [%s]: p99_delta_ns=%lld limit=%lld max_delta_ns=%lld\n",
               name, p99_delta, p99_limit, max_delta);
        return 1;
    }
    fprintf(stderr,
            "FAIL gate [%s]: p99_delta_ns=%lld limit=%lld max_delta_ns=%lld limit<1000000\n",
            name, p99_delta, p99_limit, max_delta);
    return 0;
}

int main(void) {
    const char *path = "/tmp/runtime-optimize-close-bench-target-library.db";
    long long *samples = NULL;
    bench_stats step_enabled;
    bench_stats step_disabled;
    bench_stats reset_enabled;
    bench_stats reset_disabled;
    bench_stats finalize_enabled;
    bench_stats finalize_disabled;
    int failures = 0;

    if (sqlite3_initialize() != SQLITE_OK) {
        fprintf(stderr, "ADVISORY FAIL runtime_optimize_close_bench: sqlite3_initialize failed\n");
        return 0;
    }

    samples = malloc(sizeof(long long) * HOOK_ITERATIONS);
    if (!samples) {
        fprintf(stderr, "ADVISORY FAIL runtime_optimize_close_bench: sample allocation failed\n");
        sqlite3_shutdown();
        return 0;
    }

    if (!prepare_hot_target(path)) failures++;

    if (!measure_step_done_case(path, 0, samples, &step_disabled)) failures++;
    if (!measure_step_done_case(path, 1, samples, &step_enabled)) failures++;
    if (!measure_reset_case(path, 0, samples, &reset_disabled)) failures++;
    if (!measure_reset_case(path, 1, samples, &reset_enabled)) failures++;
    if (!measure_finalize_case(path, 0, samples, &finalize_disabled)) failures++;
    if (!measure_finalize_case(path, 1, samples, &finalize_enabled)) failures++;

    if (failures == 0) {
        failures += !gate_hook_delta("step-done", step_enabled, step_disabled);
        failures += !gate_hook_delta("reset", reset_enabled, reset_disabled);
        failures += !gate_hook_delta("finalize", finalize_enabled, finalize_disabled);
    }

    free(samples);
    reset_runtime_env();
    sqlite3_shutdown();
    if (failures == 0) {
        printf("ADVISORY PASS runtime_optimize_close_bench: hot-path gates within limits\n");
    } else {
        printf("ADVISORY FAIL runtime_optimize_close_bench: failures=%d\n", failures);
    }
    return 0;
}
