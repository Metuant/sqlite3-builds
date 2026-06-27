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
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

typedef enum {
    STATS_EXPECT_NONE = 0,
    STATS_EXPECT_LIMITED = 1,
    STATS_EXPECT_FULL = 2
} stats_expectation;

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

static int create_seeded_db_without_runtime_optimize(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int ok = 1;

    if (!disable_runtime_optimize()) return 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA")) ok = 0;
    if (!clean_db(path)) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, label)) ok = 0;
    if (ok && !seed_work(db, label, 3000)) ok = 0;
    if (db && !close_db(db, label)) ok = 0;
    if (!safe_unsetenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE")) ok = 0;
    return ok;
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

static int assert_stats_db(sqlite3 *db, const char *label, stats_expectation expect) {
    sqlite3_int64 stat1 = 0;
    sqlite3_int64 stat4 = 0;
    int pass = 0;

    if (!stat_table_count(db, "sqlite_stat1", &stat1)) return 0;
    if (!stat_table_count(db, "sqlite_stat4", &stat4)) return 0;

    if (expect == STATS_EXPECT_NONE) pass = stat1 == 0 && stat4 == 0;
    else if (expect == STATS_EXPECT_LIMITED) pass = stat1 > 0 && stat4 == 0;
    else if (expect == STATS_EXPECT_FULL) pass = stat1 > 0 && stat4 > 0;

    if (pass) {
        printf("PASS [%s]: sqlite_stat1=%lld sqlite_stat4=%lld\n",
               label, (long long)stat1, (long long)stat4);
        return 1;
    }

    fprintf(stderr,
            "FAIL [%s]: expected stats mode %d, observed sqlite_stat1=%lld sqlite_stat4=%lld\n",
            label, expect, (long long)stat1, (long long)stat4);
    return 0;
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
    if (sql && strstr(sql, "PRAGMA main.optimize") != NULL) (*count)++;
    return 0;
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

static int assert_optimize_log_fields(
    const char *label,
    const char *output,
    const char *event
) {
    int ok = 1;
    char event_fragment[64];

    snprintf(event_fragment, sizeof(event_fragment), "event=%s", event);
    if (!assert_log_contains(label, output, event_fragment)) ok = 0;
    if (!assert_log_contains(label, output, "tier=full")) ok = 0;
    if (!assert_log_contains(label, output, "elapsed_ms=")) ok = 0;
    if (!assert_log_contains(label, output, "stat_rows=")) ok = 0;
    if (!assert_log_contains(label, output, "since_last_ms=-1")) ok = 0;
    return ok;
}

static int run_progress_heavy_statement(sqlite3 *db, int *count, const char *label) {
    int before = *count;
    if (!exec_sql(db, label,
            "WITH RECURSIVE c(x) AS ("
            "VALUES(0) UNION ALL SELECT x+1 FROM c WHERE x<5000"
            ") SELECT sum(x) FROM c;")) {
        return 0;
    }
    if (*count > before) {
        printf("PASS [%s]: progress callback count advanced from %d to %d\n",
               label, before, *count);
        return 1;
    }
    fprintf(stderr, "FAIL [%s]: progress callback did not run; before=%d after=%d\n",
            label, before, *count);
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
    return inspect_stats(path, label, STATS_EXPECT_FULL);
}

static int run_limited_stat1_only_case(void) {
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
    if (ok && !inspect_stats(path, "limited-full", STATS_EXPECT_FULL)) ok = 0;
    if (ok && !clear_stats_with_runtime_disabled(path, "limited-clear-stats")) ok = 0;
    sleep(2);
    if (ok && !enable_runtime_optimize()) ok = 0;
    if (ok && !open_db(path, RW_FLAGS, &db, "limited-second")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "limited-second")) ok = 0;
    if (db && !close_db(db, "limited-second")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    if (!ok) return 0;
    return inspect_stats(path, "limited-second", STATS_EXPECT_LIMITED);
}

static int run_inline_without_close_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-library.db";
    sqlite3 *db = NULL;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "inline-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "inline-without-close")) ok = 0;
    if (ok && !trigger_inline_step_done(db, "inline-without-close")) ok = 0;
    if (ok && !assert_stats_db(db, "inline-without-close-before-close", STATS_EXPECT_FULL)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "inline-without-close")) ok = 0;
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
    if (ok && !assert_stats_db(db, "inline-reset-before-close", STATS_EXPECT_FULL)) ok = 0;
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

static int run_inline_finalize_enabled_case(void) {
    const char *path = "/tmp/runtime-optimize-inline-finalize-library.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
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
    if (ok) {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_OK) {
            fprintf(stderr, "finalize inline enabled failed: rc=%d: %s\n",
                    rc, sqlite3_errmsg(db));
            ok = 0;
        }
    }
    if (ok && !assert_stats_db(db, "inline-finalize-before-close", STATS_EXPECT_FULL)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (stmt) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) ok = 0;
    }
    if (db && !close_db(db, "inline-finalize")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_restore_after_success_case(void) {
    const char *path = "/tmp/runtime-optimize-restore-success-library.db";
    sqlite3 *db = NULL;
    int progress_count = 0;
    int limit = 0;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_seeded_db_without_runtime_optimize(path, "restore-success-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "restore-success")) ok = 0;
    if (ok && !set_analysis_limit(db, 777)) ok = 0;
    sqlite3_progress_handler(db, 1, progress_sentinel_cb, &progress_count);
    if (ok && !trigger_inline_step_done(db, "restore-success")) ok = 0;
    if (ok && !read_analysis_limit(db, &limit)) ok = 0;
    if (ok && limit != 777) {
        fprintf(stderr, "FAIL [restore-success]: analysis_limit=%d expected 777\n", limit);
        ok = 0;
    }
    progress_count = 0;
    if (ok && !run_progress_heavy_statement(db, &progress_count, "restore-success-progress")) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "restore-success")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
}

static int run_restore_after_failure_case(void) {
    const char *path = "/tmp/runtime-optimize-restore-failure-library.db";
    sqlite3 *db = NULL;
    int progress_count = 0;
    int limit = 0;
    int ok = 1;

    if (!reset_runtime_env()) return 0;
    if (!create_icu_seeded_db_without_runtime_optimize(path, "restore-failure-seed")) return 0;
    if (!enable_runtime_optimize()) return 0;
    if (!open_db(path, RW_FLAGS, &db, "restore-failure")) ok = 0;
    if (ok && !set_analysis_limit(db, 777)) ok = 0;
    sqlite3_progress_handler(db, 1, progress_sentinel_cb, &progress_count);
    if (ok && !trigger_inline_sql_done(db, "restore-failure",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (ok && !read_analysis_limit(db, &limit)) ok = 0;
    if (ok && limit != 777) {
        fprintf(stderr, "FAIL [restore-failure]: analysis_limit=%d expected 777\n", limit);
        ok = 0;
    }
    progress_count = 0;
    if (ok && !run_progress_heavy_statement(db, &progress_count, "restore-failure-progress")) ok = 0;
    if (ok && !assert_stats_db(db, "restore-failure-stats", STATS_EXPECT_NONE)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "restore-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
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
    if (ok && !assert_optimize_log_fields("log-success-start", output, "optimize_start")) ok = 0;
    if (ok && !assert_optimize_log_fields("log-success-done", output, "optimize_done")) ok = 0;
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
    if (ok && !assert_optimize_log_fields("log-failure-start", output, "optimize_start")) ok = 0;
    if (ok && !assert_optimize_log_fields("log-failure-failed", output, "optimize_failed")) ok = 0;
    if (ok && strstr(output, "event=optimize_done")) {
        fprintf(stderr, "FAIL [log-failure]: unexpected optimize_done in:\n%s\n", output);
        ok = 0;
    }
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "log-failure")) ok = 0;
    if (!reset_runtime_env()) ok = 0;
    return ok;
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
    if (ok && !trigger_inline_step_done(db, "reentrant")) ok = 0;
    if (ok && optimize_count != 1) {
        fprintf(stderr, "FAIL [reentrant]: optimize trace count=%d expected 1\n", optimize_count);
        ok = 0;
    }
    if (ok && !exec_sql(db, "reentrant-usable",
            "SELECT count(*) FROM runtime_optimize_data WHERE bucket=42;")) ok = 0;
    if (ok && !assert_stats_db(db, "reentrant-stats", STATS_EXPECT_FULL)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "reentrant")) ok = 0;
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
    if (ok && !trigger_inline_sql_done(db, "icu-retry-success",
            "SELECT count(*) FROM runtime_optimize_icu;")) ok = 0;
    if (ok && !assert_stats_db(db, "icu-retry-after-success", STATS_EXPECT_FULL)) ok = 0;
    if (!disable_runtime_optimize()) ok = 0;
    if (db && !close_db(db, "icu-retry")) ok = 0;
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
    return inspect_stats(path, "short-cadence-limited", STATS_EXPECT_LIMITED);
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
    if (ok && !inspect_stats(path, "throttle-first", STATS_EXPECT_FULL)) ok = 0;
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

int main(void) {
    int failures = 0;

    if (!run_full_writes_stat4_case()) failures++;
    if (!run_limited_stat1_only_case()) failures++;
    if (!run_inline_without_close_case()) failures++;
    if (!run_inline_reset_enabled_case()) failures++;
    if (!run_inline_finalize_enabled_case()) failures++;
    if (!run_restore_after_success_case()) failures++;
    if (!run_restore_after_failure_case()) failures++;
    if (!run_error_state_preserved_after_failure_case()) failures++;
    if (!run_optimize_logging_case()) failures++;
    if (!run_reentrancy_case()) failures++;
    if (!run_icu_failsafe_retry_case()) failures++;
    if (!run_env_shortened_cadence_case()) failures++;
    if (!run_busy_peer_inline_case()) failures++;
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
