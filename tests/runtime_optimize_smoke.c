/*
 * Runtime optimize smoke coverage for the patched shared-library hooks.
 *
 * Why: target safe-idle hooks must preserve the caller-visible SQLite contract
 * while changing only owned stats and optimize-log side effects. Passing
 * requires each case to observe the expected STAT1/STAT4 tier, skip/log and
 * kill-switch outcome, busy sqlite3_close and sqlite3_close_v2 zombie semantics,
 * shutdown/reinit behavior, and preserved return/error/progress-handler state;
 * any case failure makes the process exit nonzero.
 */
#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
#define SEEDED_BUSY_TIMEOUT_MS 777
#define RUNTIME_OPTIMIZE_FULL_DEADLINE_NS 15000000000LL
#define INLINE_FULL_LATENCY_ROWS 50000
#define ENABLED_TRACKING_INLINE_ITERATIONS 1000
#define RUNTIME_OPTIMIZE_CLIENTDATA_KEY "sqlite3-builds-runtime-optimize"

typedef enum {
    STATS_EXPECT_NONE = 0,
    STATS_EXPECT_ANALYZED = 1,
    /* WHY: The LIMITED tier runs PRAGMA optimize with SQLite's bounded
     * temporary analysis limit (mask bit 0x10), and a bounded ANALYZE
     * populates sqlite_stat1 but never sqlite_stat4. Full-shape stats come
     * only from the unbounded FULL tier. */
    STATS_EXPECT_STAT1_ONLY = 2
} stats_expectation;

typedef struct {
    const char *name;
    const char *value;
} child_env;

typedef struct {
    const char *label;
    int calls;
    int arg_matches;
    int last_prior_call_count;
} busy_handler_probe;

static void *g_busy_handler_expected_arg;
static int g_busy_handler_unexpected_arg_count;

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
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE_FULL")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_DEADLINE_MS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_DEADLINE_MS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_STMT_TRACE")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_SLOW_QUERY")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL")) ok = 0;
    if (!safe_unsetenv("SQLITE3_SLOW_QUERY_THRESHOLD_MS")) ok = 0;
    if (!safe_unsetenv("SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS")) ok = 0;
    return ok;
}

static int enable_runtime_optimize(void) {
    return safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "0");
}

static int disable_runtime_optimize(void) {
    return safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1");
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
    if (snprintf(sidecar, sizeof(sidecar), "%s-wal", path) >= (int)sizeof(sidecar)) {
        fprintf(stderr, "sidecar path too long for %s-wal\n", path);
        return 0;
    }
    if (!remove_if_exists(sidecar)) return 0;
    if (snprintf(sidecar, sizeof(sidecar), "%s-shm", path) >= (int)sizeof(sidecar)) {
        fprintf(stderr, "sidecar path too long for %s-shm\n", path);
        return 0;
    }
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

static int close_db(sqlite3 *db, const char *label) {
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_close(%s) failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
    }
    return rc == SQLITE_OK;
}

static int open_db(const char *path, int flags, sqlite3 **db, const char *label) {
    int rc = sqlite3_open_v2(path, db, flags, NULL);
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

static int seed_work(sqlite3 *db, const char *label, int rows) {
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
        fprintf(stderr, "sqlite3_prepare_v2(seed insert %s) failed: rc=%d: %s\n",
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
            fprintf(stderr, "sqlite3_step(seed insert %s) failed at row %d: rc=%d: %s\n",
                    label, i, rc, sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_finalize(seed insert %s) failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }

    if (!exec_sql(db, label, "COMMIT;")) return 0;
    return exec_sql(db, label,
        "SELECT count(*) FROM runtime_optimize_data "
        "WHERE bucket=42 AND name>='name-00042';");
}

static int create_seeded_db_without_runtime_optimize_rows(
    const char *path,
    const char *label,
    int rows
) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA")) ok = 0;
    if (!clean_db(path)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !seed_work(db, label, rows)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int create_seeded_db_without_runtime_optimize(const char *path, const char *label) {
    return create_seeded_db_without_runtime_optimize_rows(path, label, 3000);
}

static int table_exists(sqlite3 *db, const char *table, int *exists) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    *exists = 0;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name=?1;",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare table lookup for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step table lookup for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    *exists = sqlite3_column_int(stmt, 0) > 0;
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize table lookup for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int stat_table_count(sqlite3 *db, const char *table, sqlite3_int64 *count) {
    sqlite3_stmt *stmt = NULL;
    char sql[256];
    int rc;
    int exists = 0;

    *count = 0;
    if (!table_exists(db, table, &exists)) return 0;
    if (!exists) return 1;

    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s;", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare stat count for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step stat count for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    *count = sqlite3_column_int64(stmt, 0);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize stat count for %s failed: rc=%d: %s\n",
                table, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static const char *stats_expectation_name(stats_expectation expect) {
    if (expect == STATS_EXPECT_NONE) return "none";
    if (expect == STATS_EXPECT_ANALYZED) return "analyzed";
    if (expect == STATS_EXPECT_STAT1_ONLY) return "stat1-only";
    return "unknown";
}

static int assert_stats_db(sqlite3 *db, const char *label, stats_expectation expect) {
    sqlite3_int64 stat1 = 0;
    sqlite3_int64 stat4 = 0;
    int pass = 0;

    if (!stat_table_count(db, "sqlite_stat1", &stat1)) return 0;
    if (!stat_table_count(db, "sqlite_stat4", &stat4)) return 0;

    if (expect == STATS_EXPECT_NONE) pass = stat1 == 0 && stat4 == 0;
    else if (expect == STATS_EXPECT_ANALYZED) pass = stat1 > 0 && stat4 > 0;
    else if (expect == STATS_EXPECT_STAT1_ONLY) pass = stat1 > 0 && stat4 == 0;

    if (pass) {
        printf("PASS [%s]: sqlite_stat1=%lld sqlite_stat4=%lld\n",
               label, (long long)stat1, (long long)stat4);
        return 1;
    }

    fprintf(stderr,
            "FAIL [%s]: expected stats shape %s, observed sqlite_stat1=%lld sqlite_stat4=%lld\n",
            label, stats_expectation_name(expect), (long long)stat1, (long long)stat4);
    return 0;
}

static int assert_stats_db_with_runtime_disabled(
    sqlite3 *db,
    const char *label,
    stats_expectation expect
) {
    int ok = 1;

    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !assert_stats_db(db, label, expect)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    return ok;
}

static int inspect_stats(const char *path, const char *label, stats_expectation expect) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!open_db(path, SQLITE_OPEN_READONLY, &db, label)) ok = 0;
    if (ok && !assert_stats_db(db, label, expect)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int clear_stat_tables(sqlite3 *db, const char *label) {
    int exists = 0;

    if (!table_exists(db, "sqlite_stat1", &exists)) return 0;
    if (exists && !exec_sql(db, label, "DELETE FROM sqlite_stat1;")) return 0;
    if (!table_exists(db, "sqlite_stat4", &exists)) return 0;
    if (exists && !exec_sql(db, label, "DELETE FROM sqlite_stat4;")) return 0;
    return 1;
}

static int clear_stats_with_runtime_disabled(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !clear_stat_tables(db, label)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int read_runtime_index_stat(sqlite3 *db, const char *label, char *out, size_t out_n) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    const unsigned char *stat;

    if (out_n == 0) return 0;
    out[0] = 0;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT stat FROM sqlite_stat1 WHERE idx='runtime_optimize_idx';",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare runtime index stat %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step runtime index stat %s expected SQLITE_ROW, rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    stat = sqlite3_column_text(stmt, 0);
    snprintf(out, out_n, "%s", stat ? (const char*)stat : "");
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize runtime index stat %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int read_runtime_index_fanout(sqlite3 *db, const char *label, long long *fanout) {
    char stat[256];
    long long rows = 0;
    long long parsed_fanout = 0;

    if (!read_runtime_index_stat(db, label, stat, sizeof(stat))) return 0;
    if (sscanf(stat, "%lld %lld", &rows, &parsed_fanout) != 2) {
        fprintf(stderr, "FAIL [%s]: could not parse sqlite_stat1 stat `%s`\n", label, stat);
        return 0;
    }
    (void)rows;
    *fanout = parsed_fanout;
    return 1;
}

static int force_bad_stat1_with_runtime_disabled(const char *path, const char *label) {
    sqlite3 *db = NULL;
    long long fanout = 0;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !exec_sql(db, label,
            "ANALYZE main;"
            "UPDATE sqlite_stat1 SET stat='3000 1025 1' "
            "WHERE idx='runtime_optimize_idx';")) ok = 0;
    if (ok && !read_runtime_index_fanout(db, label, &fanout)) ok = 0;
    if (ok && fanout != 1025) {
        fprintf(stderr, "FAIL [%s]: seeded fanout=%lld expected 1025\n",
                label, fanout);
        ok = 0;
    }
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int assert_runtime_index_fanout_repaired(const char *path, const char *label) {
    sqlite3 *db = NULL;
    long long fanout = 0;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!open_db(path, SQLITE_OPEN_READONLY, &db, label)) ok = 0;
    if (ok && !read_runtime_index_fanout(db, label, &fanout)) ok = 0;
    if (ok && (fanout <= 0 || fanout > 100)) {
        fprintf(stderr,
                "FAIL [%s]: repaired fanout=%lld expected accurate range 1..100\n",
                label, fanout);
        ok = 0;
    } else if (ok) {
        printf("PASS [%s]: repaired fanout=%lld\n", label, fanout);
    }
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int read_analysis_limit(sqlite3 *db, int *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA main.analysis_limit;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare analysis_limit read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step analysis_limit read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    *out = sqlite3_column_int(stmt, 0);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize analysis_limit read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int set_analysis_limit(sqlite3 *db, int value) {
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA main.analysis_limit=%d;", value);
    return exec_sql(db, "set-analysis-limit", sql);
}

static int monotonic_now_ns(sqlite3_int64 *out, const char *label) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime(%s) failed: %s\n", label, strerror(errno));
        return 0;
    }
    *out = (sqlite3_int64)ts.tv_sec * 1000000000LL + (sqlite3_int64)ts.tv_nsec;
    return 1;
}

static int assert_change_counters(
    sqlite3 *db,
    const char *label,
    int expected_changes,
    sqlite3_int64 expected_total_changes
) {
    int changes = sqlite3_changes(db);
    sqlite3_int64 total_changes = sqlite3_total_changes64(db);

    if (changes == expected_changes && total_changes == expected_total_changes) {
        printf("PASS [%s]: sqlite3_changes=%d sqlite3_total_changes64=%lld\n",
               label, changes, (long long)total_changes);
        return 1;
    }
    fprintf(stderr,
            "FAIL [%s]: sqlite3_changes=%d expected %d; "
            "sqlite3_total_changes64=%lld expected %lld\n",
            label,
            changes,
            expected_changes,
            (long long)total_changes,
            (long long)expected_total_changes);
    return 0;
}

static int assert_runtime_optimize_seeded(sqlite3 *db, const char *label) {
    void *state = sqlite3_get_clientdata(db, RUNTIME_OPTIMIZE_CLIENTDATA_KEY);

    if (state) {
        printf("PASS [%s]: runtime optimize clientdata seeded\n", label);
        return 1;
    }
    fprintf(stderr, "FAIL [%s]: expected runtime optimize clientdata seeded at open\n",
            label);
    return 0;
}

static int read_busy_timeout(sqlite3 *db, int *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA busy_timeout;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare busy_timeout read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step busy_timeout read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    *out = sqlite3_column_int(stmt, 0);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize busy_timeout read failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int set_busy_timeout(sqlite3 *db, const char *label, int value) {
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA busy_timeout=%d;", value);
    return exec_sql(db, label, sql);
}

static int assert_busy_timeout(sqlite3 *db, const char *label, int expected) {
    int observed = -1;

    if (!read_busy_timeout(db, &observed)) return 0;
    if (observed == expected) {
        printf("PASS [%s]: busy_timeout=%d\n", label, observed);
        return 1;
    }
    fprintf(stderr, "FAIL [%s]: busy_timeout=%d expected %d\n",
            label, observed, expected);
    return 0;
}

static int preserved_busy_handler_cb(void *arg, int prior_call_count) {
    busy_handler_probe *probe;

    if (arg != g_busy_handler_expected_arg) {
        g_busy_handler_unexpected_arg_count++;
        return 0;
    }
    probe = (busy_handler_probe*)arg;
    probe->calls++;
    probe->arg_matches++;
    probe->last_prior_call_count = prior_call_count;
    return 0;
}

static int install_busy_handler_probe(
    sqlite3 *db,
    const char *label,
    busy_handler_probe *probe
) {
    int rc;

    memset(probe, 0, sizeof(*probe));
    probe->label = label;
    g_busy_handler_expected_arg = probe;
    g_busy_handler_unexpected_arg_count = 0;
    rc = sqlite3_busy_handler(db, preserved_busy_handler_cb, probe);
    if (rc == SQLITE_OK) return 1;
    fprintf(stderr, "sqlite3_busy_handler(%s) failed: rc=%d: %s\n",
            label, rc, sqlite3_errmsg(db));
    return 0;
}

static int clear_busy_handler_probe(sqlite3 *db, const char *label) {
    int rc = sqlite3_busy_handler(db, NULL, NULL);
    g_busy_handler_expected_arg = NULL;
    if (rc == SQLITE_OK) return 1;
    fprintf(stderr, "sqlite3_busy_handler(clear %s) failed: rc=%d: %s\n",
            label, rc, sqlite3_errmsg(db));
    return 0;
}

static int assert_busy_handler_probe_fires(
    sqlite3 *db,
    const char *path,
    const char *label,
    const char *contended_sql,
    busy_handler_probe *probe
) {
    sqlite3 *locker = NULL;
    char *err = NULL;
    int before_calls = probe->calls;
    int before_unexpected_arg_count = g_busy_handler_unexpected_arg_count;
    int lock_held = 0;
    int rc;
    int ok = 1;

    if (!open_db(path, RW_FLAGS, &locker, label)) ok = 0;
    if (ok && exec_sql(locker, label, "BEGIN IMMEDIATE;")) {
        lock_held = 1;
    } else {
        ok = 0;
    }
    if (ok) {
        rc = sqlite3_exec(db, contended_sql, NULL, NULL, &err);
        if (rc != SQLITE_BUSY) {
            fprintf(stderr,
                    "FAIL [%s]: contended statement rc=%d expected SQLITE_BUSY: %s\n",
                    label, rc, err ? err : sqlite3_errmsg(db));
            ok = 0;
        }
        sqlite3_free(err);
        err = NULL;
    }
    if (ok && probe->calls <= before_calls) {
        fprintf(stderr,
                "FAIL [%s]: busy handler calls=%d expected >%d for arg %p\n",
                label, probe->calls, before_calls, (void*)probe);
        ok = 0;
    }
    if (ok && g_busy_handler_unexpected_arg_count != before_unexpected_arg_count) {
        fprintf(stderr,
                "FAIL [%s]: busy handler unexpected arg count advanced from %d to %d\n",
                label, before_unexpected_arg_count, g_busy_handler_unexpected_arg_count);
        ok = 0;
    }
    if (ok && probe->arg_matches <= 0) {
        fprintf(stderr,
                "FAIL [%s]: busy handler arg_matches=%d expected >0 for arg %p\n",
                label, probe->arg_matches, (void*)probe);
        ok = 0;
    }
    if (ok && probe->last_prior_call_count != 0) {
        fprintf(stderr,
                "FAIL [%s]: first restored busy handler nBusy=%d expected 0\n",
                label, probe->last_prior_call_count);
        ok = 0;
    }
    if (ok) {
        printf("PASS [%s]: busy handler calls advanced from %d to %d with arg %p nBusy=%d\n",
               label, before_calls, probe->calls, (void*)probe,
               probe->last_prior_call_count);
    }
    if (lock_held && !exec_sql(locker, label, "ROLLBACK;")) ok = 0;
    if (locker && !close_db(locker, label)) ok = 0;
    if (!clear_busy_handler_probe(db, label)) ok = 0;
    return ok;
}

static int assert_busy_handler_probe_unchanged(
    const char *label,
    const busy_handler_probe *probe,
    int before_calls,
    int before_unexpected_arg_count
) {
    int ok = 1;

    if (probe->calls != before_calls) {
        fprintf(stderr,
                "FAIL [%s]: app busy handler calls advanced from %d to %d "
                "during inline optimize\n",
                label, before_calls, probe->calls);
        ok = 0;
    }
    if (g_busy_handler_unexpected_arg_count != before_unexpected_arg_count) {
        fprintf(stderr,
                "FAIL [%s]: busy handler unexpected arg count advanced from %d to %d "
                "during inline optimize\n",
                label,
                before_unexpected_arg_count,
                g_busy_handler_unexpected_arg_count);
        ok = 0;
    }
    if (ok) {
        printf("PASS [%s]: app busy handler stayed at calls=%d during inline optimize\n",
               label, probe->calls);
    }
    return ok;
}

static int step_to_done(sqlite3 *db, sqlite3_stmt *stmt, const char *label) {
    int rc;
    do {
        rc = sqlite3_step(stmt);
    } while (rc == SQLITE_ROW);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite3_step(%s) expected SQLITE_DONE boundary, rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int trigger_inline_sql_done(sqlite3 *db, const char *label, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    int ok = 1;
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare inline step %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    if (!step_to_done(db, stmt, label)) ok = 0;
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize inline step %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        ok = 0;
    }
    return ok;
}

static int trigger_inline_step_done(sqlite3 *db, const char *label) {
    return trigger_inline_sql_done(
        db,
        label,
        "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;"
    );
}

static int trigger_inline_reset(sqlite3 *db, const char *label) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
        -1,
        &stmt,
        NULL
    );
    int ok = 1;
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare inline reset %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step inline reset %s expected SQLITE_ROW, rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        ok = 0;
    }
    rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "reset inline %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        ok = 0;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize inline reset %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        ok = 0;
    }
    return ok;
}

static int trigger_inline_finalize(sqlite3 *db, const char *label) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare inline finalize %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step inline finalize %s expected SQLITE_ROW, rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize inline %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int icu_root_collation(void *ctx, int a_len, const void *a, int b_len, const void *b) {
    int min_len = a_len < b_len ? a_len : b_len;
    int cmp;
    (void)ctx;
    cmp = memcmp(a, b, (size_t)min_len);
    if (cmp != 0) return cmp;
    return (a_len > b_len) - (a_len < b_len);
}

static int register_icu_root_collation(sqlite3 *db) {
    int rc = sqlite3_create_collation(db, "icu_root", SQLITE_UTF8, NULL, icu_root_collation);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_create_collation(icu_root) failed: rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int seed_icu_work(sqlite3 *db, const char *label, int rows) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int i;

    if (!register_icu_root_collation(db)) return 0;
    if (!exec_sql(db, label,
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE runtime_optimize_icu(name TEXT COLLATE icu_root);"
            "CREATE INDEX runtime_optimize_icu_idx ON runtime_optimize_icu(name);"
            "BEGIN;")) {
        return 0;
    }

    rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO runtime_optimize_icu(name) VALUES(?1);",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare ICU seed %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    for (i = 0; i < rows; i++) {
        char name[64];
        snprintf(name, sizeof(name), "icu-name-%05d", i);
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "step ICU seed %s failed at row %d: rc=%d: %s\n",
                    label, i, rc, sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "finalize ICU seed %s failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
        return 0;
    }
    return exec_sql(db, label, "COMMIT;");
}

static int create_icu_seeded_db_without_runtime_optimize(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA")) ok = 0;
    if (!clean_db(path)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !seed_icu_work(db, label, 3000)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
}

static int count_optimize_trace_cb(unsigned trace, void *ctx, void *p, void *x) {
    int *count = (int*)ctx;
    const char *sql;
    (void)p;
    if (trace != SQLITE_TRACE_STMT) return 0;
    sql = (const char*)x;
    if (sql && strcmp(sql, "ANALYZE main;") == 0) (*count)++;
    return 0;
}

typedef struct {
    int seq;
    int first_analysis_limit_zero_seq;
    int analysis_limit_zero_count;
    int analysis_limit_1024_count;
    int bare_optimize_count;
    int limited_optimize_count;
    int analyze_main_count;
    int tier_sql_seq;
} optimize_trace_counts;

static int collect_optimize_trace_cb(unsigned trace, void *ctx, void *p, void *x) {
    optimize_trace_counts *counts = (optimize_trace_counts*)ctx;
    const char *sql;

    (void)p;
    if (trace != SQLITE_TRACE_STMT) return 0;
    sql = (const char*)x;
    if (!sql) return 0;
    counts->seq++;
    if (strcmp(sql, "PRAGMA main.analysis_limit=0;") == 0) {
        counts->analysis_limit_zero_count++;
        if (counts->first_analysis_limit_zero_seq == 0) {
            counts->first_analysis_limit_zero_seq = counts->seq;
        }
    } else if (strcmp(sql, "PRAGMA main.analysis_limit=1024;") == 0) {
        counts->analysis_limit_1024_count++;
    } else if (strcmp(sql, "PRAGMA main.optimize;") == 0) {
        counts->bare_optimize_count++;
    } else if (strcmp(sql, "PRAGMA main.optimize=0x10002;") == 0) {
        counts->limited_optimize_count++;
        counts->tier_sql_seq = counts->seq;
    } else if (strcmp(sql, "ANALYZE main;") == 0) {
        counts->analyze_main_count++;
        counts->tier_sql_seq = counts->seq;
    }
    return 0;
}

static int assert_trace_tier_shape(
    const char *label,
    const optimize_trace_counts *counts,
    const char *tier
) {
    int ok = 1;
    int expect_full = strcmp(tier, "full") == 0;
    int tier_count = expect_full ? counts->analyze_main_count : counts->limited_optimize_count;
    int other_count = expect_full ? counts->limited_optimize_count : counts->analyze_main_count;
    const char *tier_sql = expect_full ? "ANALYZE main;" : "PRAGMA main.optimize=0x10002;";

    if (counts->analysis_limit_zero_count < 1 ||
        counts->first_analysis_limit_zero_seq == 0 ||
        counts->tier_sql_seq == 0 ||
        counts->first_analysis_limit_zero_seq > counts->tier_sql_seq) {
        fprintf(stderr,
                "FAIL [%s]: expected PRAGMA main.analysis_limit=0 before %s; "
                "analysis_limit_zero_count=%d first_seq=%d tier_seq=%d\n",
                label, tier_sql, counts->analysis_limit_zero_count,
                counts->first_analysis_limit_zero_seq, counts->tier_sql_seq);
        ok = 0;
    }
    if (tier_count != 1 || other_count != 0) {
        fprintf(stderr,
                "FAIL [%s]: expected exactly one %s and zero other tier SQL; "
                "limited=%d analyze_main=%d\n",
                label, tier_sql, counts->limited_optimize_count, counts->analyze_main_count);
        ok = 0;
    }
    if (counts->analysis_limit_1024_count != 0) {
        fprintf(stderr, "FAIL [%s]: saw PRAGMA main.analysis_limit=1024 %d times\n",
                label, counts->analysis_limit_1024_count);
        ok = 0;
    }
    if (counts->bare_optimize_count != 0) {
        fprintf(stderr, "FAIL [%s]: saw bare PRAGMA main.optimize; %d times\n",
                label, counts->bare_optimize_count);
        ok = 0;
    }
    if (ok) {
        printf("PASS [%s]: tier=%s sql=%s analysis_limit_zero_count=%d\n",
               label, tier, tier_sql, counts->analysis_limit_zero_count);
    }
    return ok;
}

static int progress_sentinel_cb(void *ctx) {
    int *count = (int*)ctx;
    (*count)++;
    return 0;
}

static int capture_stderr_begin(FILE **capture, int *saved_fd, const char *label) {
    *capture = tmpfile();
    if (!*capture) {
        fprintf(stderr, "tmpfile(%s) failed: %s\n", label, strerror(errno));
        return 0;
    }

    fflush(stderr);
    *saved_fd = dup(STDERR_FILENO);
    if (*saved_fd < 0) {
        fprintf(stderr, "dup(stderr %s) failed: %s\n", label, strerror(errno));
        fclose(*capture);
        *capture = NULL;
        return 0;
    }
    if (dup2(fileno(*capture), STDERR_FILENO) < 0) {
        fprintf(stderr, "dup2(stderr %s) failed: %s\n", label, strerror(errno));
        close(*saved_fd);
        fclose(*capture);
        *capture = NULL;
        *saved_fd = -1;
        return 0;
    }
    return 1;
}

static int capture_stderr_end(
    FILE *capture,
    int saved_fd,
    char *out,
    size_t out_n,
    const char *label
) {
    size_t n;
    int ok = 1;

    fflush(stderr);
    if (dup2(saved_fd, STDERR_FILENO) < 0) {
        ok = 0;
    }
    close(saved_fd);
    fflush(capture);
    rewind(capture);
    n = fread(out, 1, out_n - 1, capture);
    if (ferror(capture)) {
        fprintf(stderr, "fread(stderr capture %s) failed\n", label);
        ok = 0;
    }
    out[n] = 0;
    fclose(capture);
    if (!ok) {
        fprintf(stderr, "restore stderr capture %s failed: %s\n", label, strerror(errno));
    }
    return ok;
}

static int assert_log_contains(const char *label, const char *output, const char *needle) {
    if (strstr(output, needle)) return 1;
    fprintf(stderr, "FAIL [%s]: missing log fragment `%s` in:\n%s\n",
            label, needle, output);
    return 0;
}

static int count_log_occurrences(const char *output, const char *needle) {
    int count = 0;
    const char *p = output;
    size_t needle_n = strlen(needle);

    if (needle_n == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_n;
    }
    return count;
}

static int assert_log_count(
    const char *label,
    const char *output,
    const char *needle,
    int expected
) {
    int actual = count_log_occurrences(output, needle);
    if (actual == expected) return 1;
    fprintf(stderr,
            "FAIL [%s]: log fragment `%s` count=%d expected=%d in:\n%s\n",
            label, needle, actual, expected, output);
    return 0;
}

static int span_contains(const char *start, const char *end, const char *needle) {
    size_t n = strlen(needle);
    const char *p;

    if (n == 0) return 1;
    for (p = start; p + n <= end; p++) {
        if (memcmp(p, needle, n) == 0) return 1;
    }
    return 0;
}

static int count_log_lines_with_pair(const char *output, const char *a, const char *b) {
    int count = 0;
    const char *line = output;

    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (span_contains(line, end, a) && span_contains(line, end, b)) count++;
        line = *end == '\n' ? end + 1 : end;
    }
    return count;
}

static int assert_no_library_log_sql(
    const char *label,
    const char *output,
    const char *log_fn,
    const char *sql
) {
    int actual = count_log_lines_with_pair(output, log_fn, sql);
    if (actual == 0) return 1;
    fprintf(stderr,
            "FAIL [%s]: %s lines contained internal SQL `%s` %d times in:\n%s\n",
            label, log_fn, sql, actual, output);
    return 0;
}

static int assert_internal_sql_suppressed(const char *label, const char *output) {
    static const char *const log_fns[] = {
        "trace_stmt",
        "slow_query",
        "slow_query_expanded",
    };
    static const char *const internal_sql[] = {
        "PRAGMA main.analysis_limit=0;",
        "PRAGMA main.optimize=0x10002;",
        "ANALYZE main;",
    };
    int ok = 1;
    size_t i;
    size_t j;

    for (i = 0; i < sizeof(log_fns) / sizeof(log_fns[0]); i++) {
        for (j = 0; j < sizeof(internal_sql) / sizeof(internal_sql[0]); j++) {
            if (!assert_no_library_log_sql(label, output, log_fns[i], internal_sql[j])) {
                ok = 0;
            }
        }
    }
    return ok;
}

static int assert_optimize_log_fields(
    const char *label,
    const char *output,
    const char *event,
    const char *tier
) {
    int ok = 1;
    char event_fragment[64];
    char tier_fragment[64];

    snprintf(event_fragment, sizeof(event_fragment), "event=%s", event);
    snprintf(tier_fragment, sizeof(tier_fragment), "tier=%s", tier);
    if (!assert_log_contains(label, output, event_fragment)) ok = 0;
    if (!assert_log_contains(label, output, tier_fragment)) ok = 0;
    if (!assert_log_contains(label, output, "elapsed_ms=")) ok = 0;
    if (!assert_log_contains(label, output, "stat_rows=")) ok = 0;
    if (!assert_log_contains(label, output, "since_last_ms=")) ok = 0;
    return ok;
}

static int capture_progress_cadence_probe_count(
    sqlite3 *db,
    int *count,
    const char *label,
    int *observed_delta
) {
    int before = *count;
    int delta;

    if (!exec_sql(db, label, "SELECT 1;")) {
        return 0;
    }
    delta = *count - before;
    *observed_delta = delta;
    if (delta > 0) {
        printf("PASS [%s]: progress cadence probe delta=%d before=%d after=%d\n",
               label, delta, before, *count);
        return 1;
    }
    fprintf(stderr,
            "FAIL [%s]: progress callback did not run for a tiny statement; "
            "before=%d after=%d\n",
            label, before, *count);
    return 0;
}

static int assert_progress_cadence_probe_count(
    sqlite3 *db,
    int *count,
    const char *label,
    int expected_delta
) {
    int before = *count;
    int observed_delta;

    if (!exec_sql(db, label, "SELECT 1;")) {
        return 0;
    }
    observed_delta = *count - before;
    if (observed_delta == expected_delta) {
        printf("PASS [%s]: progress cadence probe delta=%d matched baseline\n",
               label, observed_delta);
        return 1;
    }
    fprintf(stderr,
            "FAIL [%s]: progress cadence probe delta=%d expected exact baseline %d; "
            "before=%d after=%d\n",
            label, observed_delta, expected_delta, before, *count);
    return 0;
}

static int run_full_writes_stat4_path_case(const char *path, const char *label);

static int run_full_writes_stat4_case(void) {
    return run_full_writes_stat4_path_case(
        "/tmp/runtime-optimize-full-close-library.db",
        "full-close"
    );
}

static int run_full_writes_stat4_path_case(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, label)) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, label, STATS_EXPECT_ANALYZED);
}

static int run_limited_analyzed_case(void) {
    const char *path = "/tmp/runtime-optimize-limited-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS", "1")) ok = 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS", "86400")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "limited-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "limited-full")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "limited-full")) ok = 0;
    if (db && !close_db(db, "limited-full")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "limited-full", STATS_EXPECT_ANALYZED)) ok = 0;
    if (ok && !clear_stats_with_runtime_disabled(path, "limited-clear-stats")) ok = 0;
    sleep(2);
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "limited-second")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "limited-second")) ok = 0;
    if (db && !close_db(db, "limited-second")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "limited-second", STATS_EXPECT_ANALYZED);
}

static int run_fast_inline_success_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "inline-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "inline-without-close")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "inline-without-close")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "inline-without-close")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "inline-without-close")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "inline-without-close-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "inline-without-close-done", output, "optimize_done", "full")) ok = 0;
    if (ok && !assert_stats_db(db, "inline-without-close-before-close", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "inline-without-close")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_enabled_tracking_inline_hot_skip_case(void) {
    const char *path = "/tmp/runtime-optimize-enabled-tracking-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int i;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "enabled-tracking-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "enabled-tracking")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "enabled-tracking-prime")) ok = 0;
    if (ok && !assert_stats_db(db, "enabled-tracking-prime-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "enabled-tracking-hot-skip")) ok = 0;
    for (i = 0; ok && i < ENABLED_TRACKING_INLINE_ITERATIONS; i++) {
        char label[64];
        snprintf(label, sizeof(label), "enabled-tracking-hot-skip-%d", i);
        if (!trigger_inline_step_done(db, label)) ok = 0;
    }
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "enabled-tracking-hot-skip")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count(
            "enabled-tracking-no-reoptimize",
            output,
            "event=optimize_start",
            0)) ok = 0;
    if (ok) {
        printf("PASS [enabled-tracking-hot-skip]: iterations=%d emitted no optimize_start\n",
               ENABLED_TRACKING_INLINE_ITERATIONS);
    }
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "enabled-tracking")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_change_counter_inline_restore_case(void) {
    const char *path = "/tmp/runtime-optimize-change-counter-inline-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "change-counter-inline-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "change-counter-inline")) ok = 0;
    if (ok && !assert_runtime_optimize_seeded(db, "change-counter-inline-seeded")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !exec_sql(db, "change-counter-inline-caller-write",
            "INSERT INTO runtime_optimize_data(bucket, name) "
            "VALUES(42, 'change-counter-inline-caller-write');")) ok = 0;
    if (ok && !assert_change_counters(
            db,
            "change-counter-inline-before",
            1,
            1)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "change-counter-inline")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "change-counter-inline")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "change-counter-inline")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "change-counter-inline-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "change-counter-inline-done", output, "optimize_done", "full")) ok = 0;
    if (ok && !assert_log_count(
            "change-counter-inline-no-skip",
            output,
            "event=optimize_skipped",
            0)) ok = 0;
    if (ok && !assert_change_counters(
            db,
            "change-counter-inline-after",
            1,
            1)) ok = 0;
    if (ok && !assert_stats_db(db, "change-counter-inline-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "change-counter-inline")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_change_counter_close_exercise_case(void) {
    const char *path = "/tmp/runtime-optimize-change-counter-close-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "change-counter-close-seed")) return 0;
    if (!disable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "change-counter-close")) ok = 0;
    if (ok && !exec_sql(db, "change-counter-close-caller-write",
            "INSERT INTO runtime_optimize_data(bucket, name) "
            "VALUES(42, 'change-counter-close-caller-write');")) ok = 0;
    if (ok && !assert_change_counters(
            db,
            "change-counter-close-before",
            1,
            1)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "change-counter-close")) ok = 0;
    if (db && !close_db(db, "change-counter-close")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "change-counter-close")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "change-counter-close-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "change-counter-close-done", output, "optimize_done", "full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "change-counter-close-stats", STATS_EXPECT_ANALYZED);
}

static int run_change_counter_restore_case(void) {
    int ok = 1;

    if (!run_change_counter_inline_restore_case()) ok = 0;
    if (!run_change_counter_close_exercise_case()) ok = 0;
    return ok;
}

static int run_inline_full_latency_bound_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-full-latency-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    sqlite3_int64 start_ns = 0;
    sqlite3_int64 end_ns = 0;
    sqlite3_int64 elapsed_ns = 0;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize_rows(
            path,
            "inline-full-latency-seed",
            INLINE_FULL_LATENCY_ROWS)) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "inline-full-latency")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "inline-full-latency")) ok = 0;
    if (ok && !monotonic_now_ns(&start_ns, "inline-full-latency-start")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "inline-full-latency")) ok = 0;
    if (ok && !monotonic_now_ns(&end_ns, "inline-full-latency-end")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "inline-full-latency")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && end_ns < start_ns) {
        fprintf(stderr,
                "FAIL [inline-full-latency]: monotonic end=%lld before start=%lld\n",
                (long long)end_ns,
                (long long)start_ns);
        ok = 0;
    }
    if (ok) {
        elapsed_ns = end_ns - start_ns;
        if (elapsed_ns > RUNTIME_OPTIMIZE_FULL_DEADLINE_NS) {
            fprintf(stderr,
                    "FAIL [inline-full-latency]: elapsed_ms=%.3f exceeds full "
                    "deadline_ms=%.3f rows=%d\n",
                    (double)elapsed_ns / 1000000.0,
                    (double)RUNTIME_OPTIMIZE_FULL_DEADLINE_NS / 1000000.0,
                    INLINE_FULL_LATENCY_ROWS);
            ok = 0;
        } else {
            printf("PASS [inline-full-latency]: elapsed_ms=%.3f deadline_ms=%.3f rows=%d\n",
                   (double)elapsed_ns / 1000000.0,
                   (double)RUNTIME_OPTIMIZE_FULL_DEADLINE_NS / 1000000.0,
                   INLINE_FULL_LATENCY_ROWS);
        }
    }
    if (ok && !assert_optimize_log_fields(
            "inline-full-latency-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "inline-full-latency-done", output, "optimize_done", "full")) ok = 0;
    if (ok && !assert_stats_db(db, "inline-full-latency-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "inline-full-latency")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_inline_reset_enabled_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-reset-library.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "inline-reset-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "inline-reset")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
            -1,
            &stmt,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare inline reset enabled failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "step inline reset enabled expected SQLITE_ROW, rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_reset(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "reset inline enabled failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !assert_stats_db(db, "inline-reset-before-close", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize inline reset cleanup failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (db && !close_db(db, "inline-reset")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_finalize_defer_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-finalize-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "inline-finalize-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "inline-finalize")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
            -1,
            &stmt,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare inline finalize enabled failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "step inline finalize enabled expected SQLITE_ROW, rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "inline-finalize")) ok = 0;
    if (ok) {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize inline enabled failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "inline-finalize")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count("inline-finalize-no-inline", output, "event=optimize_start", 0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(
            db, "inline-finalize-before-close", STATS_EXPECT_NONE)) ok = 0;
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) ok = 0;
    }
    if (db && !close_db(db, "inline-finalize")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "inline-finalize-after-close", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_restore_after_success_case(void) {
    const char *path = "/tmp/runtime-optimize-restore-success-library.db";
    sqlite3 *db = NULL;
    int progress_count = 0;
    int progress_baseline = 0;
    int limit = 0;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "restore-success-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "restore-success")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !set_analysis_limit(db, 777)) ok = 0;
    if (ok) sqlite3_progress_handler(db, 1, progress_sentinel_cb, &progress_count);
    progress_count = 0;
    if (ok && !capture_progress_cadence_probe_count(
            db,
            &progress_count,
            "restore-success-progress-baseline",
            &progress_baseline)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !trigger_inline_step_done(db, "restore-success")) ok = 0;
    if (ok && !read_analysis_limit(db, &limit)) ok = 0;
    if (ok && limit != 777) {
        fprintf(stderr, "FAIL [restore-success]: analysis_limit=%d expected 777\n", limit);
        ok = 0;
    }
    progress_count = 0;
    if (ok && !assert_progress_cadence_probe_count(
            db,
            &progress_count,
            "restore-success-progress-cadence",
            progress_baseline)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "restore-success")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_restore_after_failure_case(void) {
    const char *path = "/tmp/runtime-optimize-restore-failure-library.db";
    sqlite3 *db = NULL;
    int progress_count = 0;
    int progress_baseline = 0;
    int limit = 0;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_icu_seeded_db_without_runtime_optimize(path, "restore-failure-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "restore-failure")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !set_analysis_limit(db, 777)) ok = 0;
    if (ok) sqlite3_progress_handler(db, 1, progress_sentinel_cb, &progress_count);
    progress_count = 0;
    if (ok && !capture_progress_cadence_probe_count(
            db,
            &progress_count,
            "restore-failure-progress-baseline",
            &progress_baseline)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !trigger_inline_sql_done(db, "restore-failure",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (ok && !read_analysis_limit(db, &limit)) ok = 0;
    if (ok && limit != 777) {
        fprintf(stderr, "FAIL [restore-failure]: analysis_limit=%d expected 777\n", limit);
        ok = 0;
    }
    progress_count = 0;
    if (ok && !assert_progress_cadence_probe_count(
            db,
            &progress_count,
            "restore-failure-progress-cadence",
            progress_baseline)) ok = 0;
    if (ok && !assert_stats_db(db, "restore-failure-stats", STATS_EXPECT_NONE)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "restore-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_handler_preserved_success_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-handler-success-library.db";
    sqlite3 *db = NULL;
    busy_handler_probe probe;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "busy-handler-success-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-handler-success")) ok = 0;
    if (ok && !install_busy_handler_probe(db, "busy-handler-success", &probe)) ok = 0;
    if (ok && !trigger_inline_step_done(db, "busy-handler-success-inline")) ok = 0;
    if (ok && !assert_stats_db(db, "busy-handler-success-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !assert_busy_handler_probe_fires(
            db,
            path,
            "busy-handler-success-contention",
            "INSERT INTO runtime_optimize_data(bucket, name) "
            "VALUES(42, 'busy-handler-success-contention');",
            &probe)) {
        ok = 0;
    }
    if (db && g_busy_handler_expected_arg == &probe) {
        if (!clear_busy_handler_probe(db, "busy-handler-success-cleanup")) ok = 0;
    }
    if (db && !close_db(db, "busy-handler-success")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_handler_preserved_failure_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-handler-failure-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    busy_handler_probe probe;
    int saved_fd = -1;
    int ok = 1;

    output[0] = 0;
    if (!reset_runtime_env()) return 0;
    if (!create_icu_seeded_db_without_runtime_optimize(path, "busy-handler-failure-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-handler-failure")) ok = 0;
    if (ok && !install_busy_handler_probe(db, "busy-handler-failure", &probe)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "busy-handler-failure-inline")) ok = 0;
    if (ok && !trigger_inline_sql_done(db, "busy-handler-failure-inline",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (capture && !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "busy-handler-failure-inline")) {
        ok = 0;
    }
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "busy-handler-failure-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "busy-handler-failure-failed", output, "optimize_failed", "full")) ok = 0;
    if (ok && strstr(output, "event=optimize_done")) {
        fprintf(stderr, "FAIL [busy-handler-failure]: unexpected optimize_done in:\n%s\n", output);
        ok = 0;
    }
    if (ok && !assert_stats_db(db, "busy-handler-failure-stats", STATS_EXPECT_NONE)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !register_icu_root_collation(db)) ok = 0;
    if (ok && !assert_busy_handler_probe_fires(
            db,
            path,
            "busy-handler-failure-contention",
            "INSERT INTO runtime_optimize_icu(name) "
            "VALUES('busy-handler-failure-contention');",
            &probe)) {
        ok = 0;
    }
    if (capture && !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "busy-handler-failure-inline-cleanup")) {
        ok = 0;
    }
    if (db && g_busy_handler_expected_arg == &probe) {
        if (!clear_busy_handler_probe(db, "busy-handler-failure-cleanup")) ok = 0;
    }
    if (db && !close_db(db, "busy-handler-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_handler_inline_swap_contention_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-handler-inline-swap-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    sqlite3 *locker = NULL;
    FILE *capture = NULL;
    busy_handler_probe probe;
    int saved_fd = -1;
    int lock_held = 0;
    int before_calls = 0;
    int before_unexpected_arg_count = 0;
    int ok = 1;

    output[0] = 0;
    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "busy-handler-inline-swap-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-handler-inline-swap")) ok = 0;
    if (ok && !install_busy_handler_probe(db, "busy-handler-inline-swap", &probe)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &locker, "busy-handler-inline-swap-locker")) ok = 0;
    if (ok && exec_sql(locker, "busy-handler-inline-swap-locker", "BEGIN IMMEDIATE;")) {
        lock_held = 1;
    } else {
        ok = 0;
    }
    before_calls = probe.calls;
    before_unexpected_arg_count = g_busy_handler_unexpected_arg_count;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "busy-handler-inline-swap")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "busy-handler-inline-swap")) ok = 0;
    if (capture && !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "busy-handler-inline-swap")) {
        ok = 0;
    }
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_busy_handler_probe_unchanged(
            "busy-handler-inline-swap",
            &probe,
            before_calls,
            before_unexpected_arg_count)) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "busy-handler-inline-swap-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "busy-handler-inline-swap-failed", output, "optimize_failed", "full")) ok = 0;
    if (ok && strstr(output, "event=optimize_done")) {
        fprintf(stderr, "FAIL [busy-handler-inline-swap]: unexpected optimize_done in:\n%s\n",
                output);
        ok = 0;
    }
    if (ok && !assert_stats_db_with_runtime_disabled(
            db,
            "busy-handler-inline-swap-stats",
            STATS_EXPECT_NONE)) ok = 0;
    if (lock_held && !exec_sql(locker, "busy-handler-inline-swap-locker", "ROLLBACK;")) ok = 0;
    lock_held = 0;
    if (locker && !close_db(locker, "busy-handler-inline-swap-locker")) ok = 0;
    locker = NULL;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !assert_busy_handler_probe_fires(
            db,
            path,
            "busy-handler-inline-swap-restored",
            "INSERT INTO runtime_optimize_data(bucket, name) "
            "VALUES(42, 'busy-handler-inline-swap-restored');",
            &probe)) {
        ok = 0;
    }
    if (lock_held && !exec_sql(locker, "busy-handler-inline-swap-locker", "ROLLBACK;")) ok = 0;
    if (locker && !close_db(locker, "busy-handler-inline-swap-locker")) ok = 0;
    if (db && g_busy_handler_expected_arg == &probe) {
        if (!clear_busy_handler_probe(db, "busy-handler-inline-swap-cleanup")) ok = 0;
    }
    if (db && !close_db(db, "busy-handler-inline-swap")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_timeout_restored_success_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-timeout-success-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "busy-timeout-success-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-timeout-success")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !set_busy_timeout(
            db,
            "busy-timeout-success-seed-timeout",
            SEEDED_BUSY_TIMEOUT_MS)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !trigger_inline_step_done(db, "busy-timeout-success-inline")) ok = 0;
    if (ok && !assert_busy_timeout(
            db,
            "busy-timeout-success-restored",
            SEEDED_BUSY_TIMEOUT_MS)) ok = 0;
    if (ok && !assert_stats_db(db, "busy-timeout-success-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "busy-timeout-success")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_timeout_restored_failure_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-timeout-failure-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    output[0] = 0;
    if (!reset_runtime_env()) return 0;
    if (!create_icu_seeded_db_without_runtime_optimize(path, "busy-timeout-failure-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-timeout-failure")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !set_busy_timeout(
            db,
            "busy-timeout-failure-seed-timeout",
            SEEDED_BUSY_TIMEOUT_MS)) ok = 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "busy-timeout-failure-inline")) ok = 0;
    if (ok && !trigger_inline_sql_done(db, "busy-timeout-failure-inline",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (capture && !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "busy-timeout-failure-inline")) {
        ok = 0;
    }
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "busy-timeout-failure-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "busy-timeout-failure-failed", output, "optimize_failed", "full")) ok = 0;
    if (ok && strstr(output, "event=optimize_done")) {
        fprintf(stderr, "FAIL [busy-timeout-failure]: unexpected optimize_done in:\n%s\n", output);
        ok = 0;
    }
    if (ok && !assert_busy_timeout(
            db,
            "busy-timeout-failure-restored",
            SEEDED_BUSY_TIMEOUT_MS)) ok = 0;
    if (ok && !assert_stats_db(db, "busy-timeout-failure-stats", STATS_EXPECT_NONE)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "busy-timeout-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_handler_preserved_case(void) {
    int ok = 1;

    if (!run_busy_timeout_restored_success_case()) ok = 0;
    if (!run_busy_timeout_restored_failure_case()) ok = 0;
    if (!run_busy_handler_inline_swap_contention_case()) ok = 0;
    if (!run_busy_handler_preserved_success_case()) ok = 0;
    if (!run_busy_handler_preserved_failure_case()) ok = 0;
    return ok;
}

static int run_error_state_preserved_after_failure_case(void) {
    const char *path = "/tmp/runtime-optimize-error-state-library.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int errcode;
    const char *errmsg;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_icu_seeded_db_without_runtime_optimize(path, "error-state-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "error-state")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(db, "SELECT 1 WHERE 0;", -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare error-state trigger failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "step error-state trigger expected SQLITE_DONE, rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        errcode = sqlite3_errcode(db);
        errmsg = sqlite3_errmsg(db);
        if (errcode != SQLITE_DONE ||
            (errmsg && strstr(errmsg, "icu_root") != NULL)) {
            fprintf(stderr,
                    "FAIL [error-state]: errcode=%d expected SQLITE_DONE, errmsg=%s\n",
                    errcode, errmsg ? errmsg : "(null)");
            ok = 0;
        } else {
            printf("PASS [error-state]: errcode=%d errmsg=%s\n",
                   errcode, errmsg ? errmsg : "(null)");
        }
    }
    if (!disable_runtime_optimize()) ok = 0;
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize error-state trigger failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (db && !close_db(db, "error-state")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_optimize_logging_case(void) {
    const char *success_path = "/tmp/runtime-optimize-log-success-library.db";
    const char *failure_path = "/tmp/runtime-optimize-log-failure-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY")) return 0;
    if (!create_seeded_db_without_runtime_optimize(success_path, "log-success-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(success_path, RW_FLAGS, &db, "log-success")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "log-success")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "log-success")) ok = 0;
    if (capture && !capture_stderr_end(capture, saved_fd, output, sizeof(output), "log-success")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields("log-success-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields("log-success-done", output, "optimize_done", "full")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "log-success")) ok = 0;
    db = NULL;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;

    if (!create_icu_seeded_db_without_runtime_optimize(failure_path, "log-failure-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(failure_path, RW_FLAGS, &db, "log-failure")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "log-failure")) ok = 0;
    if (ok && !trigger_inline_sql_done(db, "log-failure",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (capture && !capture_stderr_end(capture, saved_fd, output, sizeof(output), "log-failure")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields("log-failure-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields("log-failure-failed", output, "optimize_failed", "full")) ok = 0;
    if (ok && strstr(output, "event=optimize_done")) {
        fprintf(stderr, "FAIL [log-failure]: unexpected optimize_done in:\n%s\n", output);
        ok = 0;
    }
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "log-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_tier_sql_shape_case(void) {
    const char *path = "/tmp/runtime-optimize-tier-sql-library.db";
    sqlite3 *db = NULL;
    optimize_trace_counts counts;
    char output[32768];
    FILE *capture = NULL;
    int saved_fd = -1;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS", "1")) ok = 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS", "86400")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "tier-sql-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "tier-sql-full")) ok = 0;
    memset(&counts, 0, sizeof(counts));
    if (ok) {
        rc = sqlite3_trace_v2(db, SQLITE_TRACE_STMT, collect_optimize_trace_cb, &counts);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "sqlite3_trace_v2(tier-sql-full) failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "tier-sql-full")) ok = 0;
    if (db && !close_db(db, "tier-sql-full")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "tier-sql-full")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_trace_tier_shape("tier-sql-full", &counts, "full")) ok = 0;
    if (ok && !assert_optimize_log_fields("tier-sql-full-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields("tier-sql-full-done", output, "optimize_done", "full")) ok = 0;

    if (ok && !clear_stats_with_runtime_disabled(path, "tier-sql-clear")) ok = 0;
    sleep(2);
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "tier-sql-limited")) ok = 0;
    memset(&counts, 0, sizeof(counts));
    if (ok) {
        rc = sqlite3_trace_v2(db, SQLITE_TRACE_STMT, collect_optimize_trace_cb, &counts);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "sqlite3_trace_v2(tier-sql-limited) failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "tier-sql-limited")) ok = 0;
    if (db && !close_db(db, "tier-sql-limited")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "tier-sql-limited")) ok = 0;
    if (ok && !assert_trace_tier_shape("tier-sql-limited", &counts, "limited")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "tier-sql-limited-start", output, "optimize_start", "limited")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "tier-sql-limited-done", output, "optimize_done", "limited")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_seeded_bad_stat1_full_repair_case(void) {
    const char *path = "/tmp/runtime-optimize-bad-stat1-repair-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "bad-stat1-seed")) return 0;
    if (!force_bad_stat1_with_runtime_disabled(path, "bad-stat1-force")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "bad-stat1-full")) ok = 0;
    if (db && !close_db(db, "bad-stat1-full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return assert_runtime_index_fanout_repaired(path, "bad-stat1-repaired");
}

static int run_reentrancy_case(void) {
    const char *path = "/tmp/runtime-optimize-reentrant-library.db";
    sqlite3 *db = NULL;
    int optimize_count = 0;
    int ok = 1;
    int rc;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "reentrant-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "reentrant")) ok = 0;
    if (ok) {
        rc = sqlite3_trace_v2(db, SQLITE_TRACE_STMT, count_optimize_trace_cb, &optimize_count);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "sqlite3_trace_v2(reentrant) failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (db && !close_db(db, "reentrant")) ok = 0;
    db = NULL;
    if (ok && optimize_count != 1) {
        fprintf(stderr, "FAIL [reentrant-full]: ANALYZE main trace count=%d expected 1\n",
                optimize_count);
        ok = 0;
    }
    if (ok && !inspect_stats(path, "reentrant-stats", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_icu_failsafe_retry_case(void) {
    const char *path = "/tmp/runtime-optimize-icu-retry-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS", "1")) ok = 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS", "86400")) ok = 0;
    if (ok && !create_icu_seeded_db_without_runtime_optimize(path, "icu-retry-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "icu-retry")) ok = 0;
    if (ok && !trigger_inline_sql_done(db, "icu-retry-failure",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (ok && !assert_stats_db(db, "icu-retry-after-failure", STATS_EXPECT_NONE)) ok = 0;
    sleep(2);
    if (ok && !register_icu_root_collation(db)) ok = 0;
    /* WHY: The failed FULL attempt arms the one-shot fairness rotation, so
     * the first post-failure reservation goes to LIMITED (bounded: stat1
     * only). This is the starvation fix under test, not a regression. */
    if (ok && !trigger_inline_sql_done(db, "icu-retry-success",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (ok && !assert_stats_db(db, "icu-retry-after-rotation", STATS_EXPECT_STAT1_ONLY)) ok = 0;
    /* The rotation is consumed; the close hook (which bypasses the inline
     * blocked-rearm cache) retries FULL, which now succeeds with the
     * collation registered and writes full-shape stats. */
    if (db && !close_db(db, "icu-retry")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "icu-retry-after-success", STATS_EXPECT_ANALYZED)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_env_shortened_cadence_case(void) {
    const char *path = "/tmp/runtime-optimize-short-cadence-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS", "1")) ok = 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS", "86400")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "short-cadence-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "short-cadence-full")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "short-cadence-full")) ok = 0;
    if (db && !close_db(db, "short-cadence-full")) ok = 0;
    db = NULL;
    if (ok && !clear_stats_with_runtime_disabled(path, "short-cadence-clear")) ok = 0;
    sleep(2);
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "short-cadence-limited")) ok = 0;
    if (ok && !trigger_inline_reset(db, "short-cadence-limited")) ok = 0;
    if (db && !close_db(db, "short-cadence-limited")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    /* WHY: LIMITED runs PRAGMA optimize under SQLite's bounded temporary
     * analysis limit (mask bit 0x10), which populates sqlite_stat1 but not
     * sqlite_stat4 -- full-shape stats are the FULL tier's job. */
    return inspect_stats(path, "short-cadence-limited", STATS_EXPECT_STAT1_ONLY);
}

static int run_full_deadline_gate_case(void) {
    const char *path = "/tmp/runtime-optimize-deadline-gate-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    /* WHY: A 1ms FULL deadline guarantees the seeded ANALYZE (about 10ms) is
     * interrupted by OUR progress deadline, exercising the
     * SQLITE_INTERRUPT+deadline_reached classification deterministically. */
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_DEADLINE_MS", "1")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "deadline-gate-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "deadline-gate")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "deadline-gate-full")) ok = 0;
    /* The interrupted ANALYZE rolled back: no stats. */
    if (ok && !assert_stats_db(db, "deadline-gate-after-interrupt", STATS_EXPECT_NONE)) ok = 0;
    /* WHY: The close hook is the core gate assertion. FULL is deadline-gated
     * on its cadence, so close must NOT retry it (stat4 would appear if it
     * did); LIMITED is due and no shared backoff was set by the deadline, so
     * the previously-starved tier runs instead (stat1 only). */
    if (db && !close_db(db, "deadline-gate")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "deadline-gate-close", STATS_EXPECT_STAT1_ONLY)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_full_kill_switch_tier_case(void) {
    const char *path = "/tmp/runtime-optimize-full-kill-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE_FULL", "1")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "full-kill-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "full-kill")) ok = 0;
    /* WHY: With FULL disabled, the very first reservation goes to LIMITED
     * (previously FULL always won the first turn); LIMITED's bounded analyze
     * writes stat1 only, so any stat4 row means FULL ran despite the switch. */
    if (ok && !trigger_inline_step_done(db, "full-kill-limited")) ok = 0;
    if (db && !close_db(db, "full-kill")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "full-kill-stats", STATS_EXPECT_STAT1_ONLY)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_busy_peer_inline_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-peer-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    sqlite3_stmt *peer = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int rc;
    int i;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY")) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "busy-peer-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-peer")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT id FROM runtime_optimize_data ORDER BY id;",
            -1,
            &peer,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare busy peer failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_step(peer);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "step busy peer expected SQLITE_ROW, rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "busy-peer-skip")) ok = 0;
    for (i = 0; ok && i < 5; i++) {
        char label[64];
        snprintf(label, sizeof(label), "busy-peer-skip-%d", i);
        if (!trigger_inline_step_done(db, label)) ok = 0;
    }
    if (capture && !capture_stderr_end(capture, saved_fd, output, sizeof(output), "busy-peer-skip")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count("busy-peer-skip-count", output, "event=optimize_skipped", 1)) ok = 0;
    if (ok && !assert_log_contains("busy-peer-skip-reason", output, "reason=busy_peer")) ok = 0;
    if (ok && !assert_log_count("busy-peer-no-optimize", output, "event=optimize_start", 0)) ok = 0;
    if (ok && !assert_stats_db(db, "busy-peer-after-skip", STATS_EXPECT_NONE)) ok = 0;
    if (ok) {
        rc = sqlite3_reset(peer);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "reset busy peer failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !trigger_inline_step_done(db, "busy-peer-rearmed")) ok = 0;
    if (ok && !assert_stats_db(db, "busy-peer-after-rearm", STATS_EXPECT_NONE)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (peer) {
        rc = sqlite3_finalize(peer);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize busy peer failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (db && !close_db(db, "busy-peer")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_forced_defer_child_case(void) {
    const char *path = "/tmp/runtime-optimize-forced-defer-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "forced-defer-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "forced-defer")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "forced-defer-inline")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "forced-defer-inline")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "forced-defer-inline")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count("forced-defer-no-inline", output, "event=optimize_start", 0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(
            db, "forced-defer-before-close", STATS_EXPECT_NONE)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "forced-defer-close")) ok = 0;
    if (db && !close_db(db, "forced-defer")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "forced-defer-close")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "forced-defer-close-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "forced-defer-close-done", output, "optimize_done", "full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "forced-defer-after-close", STATS_EXPECT_ANALYZED);
}

static int run_telemetry_off_defer_child_case(void) {
    const char *path = "/tmp/runtime-optimize-telemetry-off-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "telemetry-off-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "telemetry-off")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "telemetry-off-inline")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "telemetry-off-inline")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "telemetry-off-inline")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count("telemetry-off-no-inline", output, "event=optimize_start", 0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(
            db, "telemetry-off-before-close", STATS_EXPECT_NONE)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "telemetry-off-close")) ok = 0;
    if (db && !close_db(db, "telemetry-off")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "telemetry-off-close")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "telemetry-off-close-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "telemetry-off-close-done", output, "optimize_done", "full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "telemetry-off-after-close", STATS_EXPECT_ANALYZED);
}

static int run_sql_defer_to_close_case(
    const char *path,
    const char *label,
    const char *sql
) {
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, label)) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, label)) ok = 0;
    if (ok && !exec_sql(db, label, sql)) ok = 0;
    if (capture && !capture_stderr_end(capture, saved_fd, output, sizeof(output), label)) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count(label, output, "event=optimize_start", 0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(db, label, STATS_EXPECT_NONE)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    db = NULL;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, label, STATS_EXPECT_ANALYZED);
}

static int run_commit_defer_case(void) {
    const char *path = "/tmp/runtime-optimize-commit-defer-library.db";
    const char *control_path = "/tmp/runtime-optimize-commit-defer-read-control-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    sqlite3 *control_db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(
            control_path,
            "commit-defer-read-control-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(control_path, RW_FLAGS, &control_db, "commit-defer-read-control")) ok = 0;
    if (ok && !capture_stderr_begin(
            &capture,
            &saved_fd,
            "commit-defer-read-control")) ok = 0;
    if (ok && !trigger_inline_step_done(
            control_db,
            "commit-defer-read-control")) ok = 0;
    if (capture &&
        !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "commit-defer-read-control")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "commit-defer-read-control-start",
            output,
            "optimize_start",
            "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "commit-defer-read-control-done",
            output,
            "optimize_done",
            "full")) ok = 0;
    if (control_db && !close_db(control_db, "commit-defer-read-control")) ok = 0;
    control_db = NULL;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;

    if (!create_seeded_db_without_runtime_optimize(path, "commit-defer-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "commit-defer")) ok = 0;
    if (ok && !assert_runtime_optimize_seeded(db, "commit-defer-seeded")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !exec_sql(db, "commit-defer-setup",
            "BEGIN;"
            "INSERT INTO runtime_optimize_data(bucket, name) "
            "VALUES(42, 'commit-defer');")) ok = 0;
    if (ok && sqlite3_get_autocommit(db) != 0) {
        fprintf(stderr, "FAIL [commit-defer-setup]: expected open transaction before COMMIT\n");
        ok = 0;
    }
    if (!enable_runtime_optimize()) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "commit-defer-inline")) ok = 0;
    if (ok && !exec_sql(db, "commit-defer-inline", "COMMIT;")) ok = 0;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "commit-defer-inline")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count(
            "commit-defer-skip-count",
            output,
            "event=optimize_skipped",
            1)) ok = 0;
    if (ok && !assert_log_contains(
            "commit-defer-skip-reason",
            output,
            "reason=not_idle_read")) ok = 0;
    if (ok && !assert_log_count(
            "commit-defer-no-inline",
            output,
            "event=optimize_start",
            0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(
            db,
            "commit-defer-before-close",
            STATS_EXPECT_NONE)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "commit-defer-close")) ok = 0;
    if (db && !close_db(db, "commit-defer")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "commit-defer-close")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "commit-defer-close-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "commit-defer-close-done", output, "optimize_done", "full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "commit-defer-after-close", STATS_EXPECT_ANALYZED);
}

static int run_write_and_commit_defer_case(void) {
    int ok = 1;

    if (!run_sql_defer_to_close_case(
            "/tmp/runtime-optimize-write-defer-library.db",
            "write-defer",
            "INSERT INTO runtime_optimize_data(bucket, name) VALUES(42, 'write-defer');")) {
        ok = 0;
    }
    if (!run_commit_defer_case()) ok = 0;
    return ok;
}

static int run_readonly_pragma_defer_case(void) {
    const char *path = "/tmp/runtime-optimize-readonly-pragma-defer-library.db";
    char output[32768];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "readonly-pragma-defer-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "readonly-pragma-defer")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "readonly-pragma-defer-inline")) ok = 0;
    if (ok && !trigger_inline_sql_done(
            db,
            "readonly-pragma-defer-inline",
            "PRAGMA main.table_info(runtime_optimize_data);")) ok = 0;
    if (capture &&
        !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "readonly-pragma-defer-inline")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_log_count(
            "readonly-pragma-defer-skip-count",
            output,
            "event=optimize_skipped",
            1)) ok = 0;
    if (ok && !assert_log_contains(
            "readonly-pragma-defer-skip-reason",
            output,
            "reason=not_idle_read")) ok = 0;
    if (ok && !assert_log_count(
            "readonly-pragma-defer-no-inline",
            output,
            "event=optimize_start",
            0)) ok = 0;
    if (ok && !assert_stats_db_with_runtime_disabled(
            db,
            "readonly-pragma-defer-before-close",
            STATS_EXPECT_NONE)) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "readonly-pragma-defer-close")) ok = 0;
    if (db && !close_db(db, "readonly-pragma-defer")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(
            capture,
            saved_fd,
            output,
            sizeof(output),
            "readonly-pragma-defer-close")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "readonly-pragma-defer-close-start",
            output,
            "optimize_start",
            "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "readonly-pragma-defer-close-done",
            output,
            "optimize_done",
            "full")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "readonly-pragma-defer-after-close", STATS_EXPECT_ANALYZED);
}

static int run_internal_trace_suppression_child_case(void) {
    const char *path = "/tmp/runtime-optimize-internal-suppression-library.db";
    char output[65536];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS", "1")) ok = 0;
    if (!safe_setenv("SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS", "86400")) ok = 0;
    if (ok && !create_seeded_db_without_runtime_optimize(path, "internal-suppression-seed")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "internal-suppression-full")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "internal-suppression-full")) ok = 0;
    if (db && !close_db(db, "internal-suppression-full")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "internal-suppression-full")) ok = 0;
    capture = NULL;
    saved_fd = -1;
    if (ok && !assert_optimize_log_fields(
            "internal-suppression-full-start", output, "optimize_start", "full")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "internal-suppression-full-done", output, "optimize_done", "full")) ok = 0;
    if (ok && !assert_internal_sql_suppressed("internal-suppression-full", output)) ok = 0;

    if (ok && !clear_stats_with_runtime_disabled(path, "internal-suppression-clear")) ok = 0;
    sleep(2);
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "internal-suppression-limited")) ok = 0;
    if (ok && !capture_stderr_begin(&capture, &saved_fd, "internal-suppression-limited")) ok = 0;
    if (db && !close_db(db, "internal-suppression-limited")) ok = 0;
    db = NULL;
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), "internal-suppression-limited")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "internal-suppression-limited-start", output, "optimize_start", "limited")) ok = 0;
    if (ok && !assert_optimize_log_fields(
            "internal-suppression-limited-done", output, "optimize_done", "limited")) ok = 0;
    if (ok && !assert_internal_sql_suppressed("internal-suppression-limited", output)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_non_target_case(void) {
    const char *path = "/tmp/runtime-optimize-nontarget.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!clean_db(path)) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "non-target")) ok = 0;
    if (ok && !seed_work(db, "non-target", 3000)) ok = 0;
    if (db && !close_db(db, "non-target")) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "non-target", STATS_EXPECT_NONE);
}

static int run_runtime_kill_switch_case(void) {
    const char *path = "/tmp/runtime-optimize-runtime-kill-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "runtime-kill-seed")) return 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "runtime-kill")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "runtime-kill-step")) ok = 0;
    if (ok && !trigger_inline_reset(db, "runtime-kill-reset")) ok = 0;
    if (ok && !trigger_inline_finalize(db, "runtime-kill-finalize")) ok = 0;
    if (ok && !assert_stats_db(db, "runtime-kill-before-close", STATS_EXPECT_NONE)) ok = 0;
    if (db && !close_db(db, "runtime-kill")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "runtime-kill-switch", STATS_EXPECT_NONE);
}

static int run_runtime_disabled_value_case(const char *label, const char *value) {
    char path[256];
    sqlite3 *db = NULL;
    int ok = 1;

    if (snprintf(path, sizeof(path), "/tmp/runtime-optimize-%s-library.db", label) >=
        (int)sizeof(path)) {
        fprintf(stderr, "runtime disabled case path too long for %s\n", label);
        return 0;
    }

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, label)) return 0;
    if (value && !safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", value)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !trigger_inline_step_done(db, label)) ok = 0;
    if (ok && !trigger_inline_reset(db, label)) ok = 0;
    if (ok && !trigger_inline_finalize(db, label)) ok = 0;
    if (ok && !assert_stats_db(db, label, STATS_EXPECT_NONE)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, label, STATS_EXPECT_NONE);
}

static int run_autopragma_kill_switch_case(void) {
    const char *path = "/tmp/runtime-optimize-autopragma-kill-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!enable_runtime_optimize()) ok = 0;
    if (!safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1")) ok = 0;
    if (ok && !clean_db(path)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "autopragma-kill-switch")) ok = 0;
    if (ok && !seed_work(db, "autopragma-kill-switch", 3000)) ok = 0;
    if (db && !close_db(db, "autopragma-kill-switch")) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA")) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "autopragma-kill-switch", STATS_EXPECT_NONE);
}

static int run_readonly_immutable_case(void) {
    const char *path = "/tmp/runtime-optimize-immutable-library.db";
    char uri[512];
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "immutable-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    snprintf(uri, sizeof(uri), "file:%s?immutable=1", path);
    if (!open_db(uri, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, &db, "immutable")) ok = 0;
    if (ok && !exec_sql(db, "immutable",
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;")) ok = 0;
    if (db && !close_db(db, "immutable")) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "immutable", STATS_EXPECT_NONE);
}

static int run_non_autocommit_case(void) {
    const char *path = "/tmp/runtime-optimize-transaction-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "transaction-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "transaction")) ok = 0;
    if (ok && !exec_sql(db, "transaction",
            "BEGIN;"
            "INSERT INTO runtime_optimize_data(bucket, name) VALUES(42, 'txn-open');"
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;")) ok = 0;
    if (db && !close_db(db, "transaction")) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "transaction", STATS_EXPECT_NONE);
}

static int run_busy_close_case(void) {
    const char *path = "/tmp/runtime-optimize-busy-library.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "busy-close-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "busy-close")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
            -1,
            &stmt,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare busy-close statement failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_close(db);
        if (rc != SQLITE_BUSY) {
            fprintf(stderr, "FAIL [busy-close]: sqlite3_close rc=%d expected SQLITE_BUSY\n", rc);
            ok = 0;
        } else {
            printf("PASS [busy-close-rc]: sqlite3_close returned SQLITE_BUSY\n");
        }
    }
    if (!disable_runtime_optimize()) ok = 0;
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize busy-close statement failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (db && !close_db(db, "busy-close-final")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "busy-close", STATS_EXPECT_NONE);
}

static int run_zombie_close_v2_case(void) {
    const char *path = "/tmp/runtime-optimize-zombie-library.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "zombie-close-v2-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "zombie-close-v2")) ok = 0;
    if (ok) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;",
            -1,
            &stmt,
            NULL
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "prepare zombie statement failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok) {
        rc = sqlite3_close_v2(db);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "FAIL [zombie-close-v2]: sqlite3_close_v2 rc=%d expected SQLITE_OK\n", rc);
            ok = 0;
        } else {
            printf("PASS [zombie-close-v2-rc]: sqlite3_close_v2 returned SQLITE_OK\n");
        }
    }
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize zombie statement failed: rc=%d\n", rc);
            ok = 0;
        }
    }
    if (!ok) return 0;
    return inspect_stats(path, "zombie-close-v2", STATS_EXPECT_NONE);
}

static int run_throttle_case(void) {
    const char *path = "/tmp/runtime-optimize-throttle-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "throttle-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "throttle-first")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "throttle-first")) ok = 0;
    if (db && !close_db(db, "throttle-first")) ok = 0;
    db = NULL;
    if (ok && !inspect_stats(path, "throttle-first", STATS_EXPECT_ANALYZED)) ok = 0;
    if (ok && !clear_stats_with_runtime_disabled(path, "throttle-clear-stats")) ok = 0;
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "throttle-second")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "throttle-second")) ok = 0;
    if (db && !close_db(db, "throttle-second")) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "throttle-second", STATS_EXPECT_NONE);
}

static int run_shutdown_reinit_case(void) {
    int rc;

    if (!reset_runtime_env()) return 0;
    rc = sqlite3_shutdown();
    if (rc != SQLITE_OK) {
        fprintf(stderr, "FAIL [shutdown-reinit]: sqlite3_shutdown rc=%d expected SQLITE_OK\n", rc);
        return 0;
    }
    printf("PASS [shutdown-reinit-shutdown]: sqlite3_shutdown rc=0\n");
    return run_full_writes_stat4_path_case(
        "/tmp/runtime-optimize-shutdown-reinit-library.db",
        "shutdown-reinit"
    );
}

static int run_child_process_case(
    const char *self_path,
    const char *case_name,
    const child_env *env,
    size_t env_n
) {
    pid_t pid = fork();
    int status;
    size_t i;

    if (pid < 0) {
        fprintf(stderr, "fork(%s) failed: %s\n", case_name, strerror(errno));
        return 0;
    }
    if (pid == 0) {
        for (i = 0; i < env_n; i++) {
            if (env[i].value) {
                if (setenv(env[i].name, env[i].value, 1) != 0) {
                    fprintf(stderr, "child setenv(%s=%s) failed: %s\n",
                            env[i].name, env[i].value, strerror(errno));
                    _exit(126);
                }
            } else if (unsetenv(env[i].name) != 0) {
                fprintf(stderr, "child unsetenv(%s) failed: %s\n",
                        env[i].name, strerror(errno));
                _exit(126);
            }
        }
        execlp(self_path, self_path, "--case", case_name, (char*)NULL);
        fprintf(stderr, "execlp(%s --case %s) failed: %s\n",
                self_path, case_name, strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid) {
        fprintf(stderr, "waitpid(%s) failed: %s\n", case_name, strerror(errno));
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS [%s-child]: exit=0\n", case_name);
        return 1;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "FAIL [%s-child]: exit=%d\n", case_name, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL [%s-child]: signal=%d\n", case_name, WTERMSIG(status));
    } else {
        fprintf(stderr, "FAIL [%s-child]: unexpected wait status=%d\n", case_name, status);
    }
    return 0;
}

static int run_forced_defer_case(const char *self_path) {
    const child_env env[] = {
        {"SQLITE3_SLOW_QUERY_THRESHOLD_MS", "0"},
        {"SQLITE3_DISABLE_OBSERVABILITY", NULL},
        {"SQLITE3_DISABLE_SLOW_QUERY", NULL},
        {"SQLITE3_DISABLE_STMT_TRACE", NULL},
    };
    return run_child_process_case(
        self_path,
        "forced-defer-threshold-zero",
        env,
        sizeof(env) / sizeof(env[0])
    );
}

static int run_telemetry_off_defer_case(const char *self_path) {
    const child_env env[] = {
        {"SQLITE3_DISABLE_SLOW_QUERY", "1"},
        {"SQLITE3_DISABLE_OBSERVABILITY", NULL},
        {"SQLITE3_DISABLE_STMT_TRACE", NULL},
        {"SQLITE3_SLOW_QUERY_THRESHOLD_MS", NULL},
    };
    return run_child_process_case(
        self_path,
        "telemetry-off-defer",
        env,
        sizeof(env) / sizeof(env[0])
    );
}

static int run_internal_trace_suppression_case(const char *self_path) {
    const child_env env[] = {
        {"SQLITE3_DISABLE_STMT_TRACE", "0"},
        {"SQLITE3_SLOW_QUERY_THRESHOLD_MS", "0"},
        {"SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS", "0"},
        {"SQLITE3_DISABLE_OBSERVABILITY", NULL},
        {"SQLITE3_DISABLE_SLOW_QUERY", NULL},
        {"SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL", NULL},
    };
    return run_child_process_case(
        self_path,
        "internal-trace-suppression",
        env,
        sizeof(env) / sizeof(env[0])
    );
}

static int run_named_child_case(const char *case_name) {
    if (strcmp(case_name, "forced-defer-threshold-zero") == 0) {
        return run_forced_defer_child_case();
    }
    if (strcmp(case_name, "telemetry-off-defer") == 0) {
        return run_telemetry_off_defer_child_case();
    }
    if (strcmp(case_name, "internal-trace-suppression") == 0) {
        return run_internal_trace_suppression_child_case();
    }
    fprintf(stderr, "unknown child case: %s\n", case_name);
    return 0;
}

int main(int argc, char **argv) {
    int failures = 0;

    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        return run_named_child_case(argv[2]) ? 0 : 1;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--case name]\n", argv[0]);
        return 1;
    }

    if (!run_full_writes_stat4_case()) failures++;
    if (!run_limited_analyzed_case()) failures++;
    if (!run_tier_sql_shape_case()) failures++;
    if (!run_seeded_bad_stat1_full_repair_case()) failures++;
    if (!run_fast_inline_success_case()) failures++;
    if (!run_enabled_tracking_inline_hot_skip_case()) failures++;
    if (!run_change_counter_restore_case()) failures++;
    if (!run_inline_full_latency_bound_case()) failures++;
    if (!run_forced_defer_case(argv[0])) failures++;
    if (!run_inline_reset_enabled_case()) failures++;
    if (!run_finalize_defer_case()) failures++;
    if (!run_restore_after_success_case()) failures++;
    if (!run_restore_after_failure_case()) failures++;
    if (!run_busy_handler_preserved_case()) failures++;
    if (!run_error_state_preserved_after_failure_case()) failures++;
    if (!run_optimize_logging_case()) failures++;
    if (!run_reentrancy_case()) failures++;
    if (!run_icu_failsafe_retry_case()) failures++;
    if (!run_env_shortened_cadence_case()) failures++;
    if (!run_full_deadline_gate_case()) failures++;
    if (!run_full_kill_switch_tier_case()) failures++;
    if (!run_busy_peer_inline_case()) failures++;
    if (!run_telemetry_off_defer_case(argv[0])) failures++;
    if (!run_write_and_commit_defer_case()) failures++;
    if (!run_readonly_pragma_defer_case()) failures++;
    if (!run_internal_trace_suppression_case(argv[0])) failures++;
    if (!run_non_target_case()) failures++;
    if (!run_runtime_kill_switch_case()) failures++;
    if (!run_runtime_disabled_value_case("runtime-unset-disabled", NULL)) failures++;
    if (!run_runtime_disabled_value_case("runtime-other-disabled", "false")) failures++;
    if (!run_autopragma_kill_switch_case()) failures++;
    if (!run_readonly_immutable_case()) failures++;
    if (!run_non_autocommit_case()) failures++;
    if (!run_busy_close_case()) failures++;
    if (!run_zombie_close_v2_case()) failures++;
    if (!run_throttle_case()) failures++;
    if (!run_shutdown_reinit_case()) failures++;

    return failures == 0 ? 0 : 1;
}
