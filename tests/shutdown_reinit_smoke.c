#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

typedef struct {
    const char *name;
    const char *sql;
    sqlite3_int64 expected;
} pragma_check;

static const pragma_check PRAGMA_CHECKS[] = {
    {"busy_timeout", "PRAGMA busy_timeout;", 10000},
    {"cache_size", "PRAGMA cache_size;", -1048576},
    {"mmap_size", "PRAGMA mmap_size;", 34359738368LL},
    {"wal_autocheckpoint", "PRAGMA wal_autocheckpoint;", 16000},
    {"journal_size_limit", "PRAGMA journal_size_limit;", 67108864},
    {"threads", "PRAGMA threads;", 8},
    /* SQLITE_TEMP_STORE=3 does not override the per-connection PRAGMA value
     * set by the auto-extension. PRAGMA temp_store returns 2 independent of
     * the compile default. */
    {"temp_store", "PRAGMA temp_store;", 2},
    {"analysis_limit", "PRAGMA analysis_limit;", 1024},
};

enum { PRAGMA_CHECK_COUNT = (int)(sizeof(PRAGMA_CHECKS) / sizeof(PRAGMA_CHECKS[0])) };

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

static int unset_autopragma_disable(void) {
    if (unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") == 0) return 1;

    fprintf(stderr, "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA) failed: %s\n", strerror(errno));
    return 0;
}

static int read_integer_pragma(
    sqlite3 *db,
    const char *name,
    const char *sql,
    sqlite3_int64 *value
) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_prepare_v2(%s) failed: rc=%d: %s\n",
                name, rc, sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *value = sqlite3_column_int64(stmt, 0);
        rc = SQLITE_OK;
    } else {
        fprintf(stderr, "sqlite3_step(%s) failed: rc=%d: %s\n",
                name, rc, sqlite3_errmsg(db));
        if (rc == SQLITE_DONE) rc = SQLITE_ERROR;
    }

    int finalize_rc = sqlite3_finalize(stmt);
    if (finalize_rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_finalize(%s) failed: rc=%d: %s\n",
                name, finalize_rc, sqlite3_errmsg(db));
        if (rc == SQLITE_OK) rc = finalize_rc;
    }
    return rc;
}

static int assert_active_profile(sqlite3 *db, const char *label) {
    int ok = 1;
    int i;

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        sqlite3_int64 observed = 0;
        int rc = read_integer_pragma(
            db,
            PRAGMA_CHECKS[i].name,
            PRAGMA_CHECKS[i].sql,
            &observed
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "FAIL [%s]: %s read rc=%d expected=%lld\n",
                    label,
                    PRAGMA_CHECKS[i].name,
                    rc,
                    (long long)PRAGMA_CHECKS[i].expected);
            ok = 0;
        } else if (observed != PRAGMA_CHECKS[i].expected) {
            fprintf(stderr, "FAIL [%s]: %s observed=%lld expected=%lld\n",
                    label,
                    PRAGMA_CHECKS[i].name,
                    (long long)observed,
                    (long long)PRAGMA_CHECKS[i].expected);
            ok = 0;
        }
    }

    if (ok) {
        printf("PASS [%s]: active PRAGMA profile observed\n", label);
        return 1;
    }
    return 0;
}

static int close_db(sqlite3 *db, const char *label) {
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_close(%s) failed: rc=%d: %s\n",
                label, rc, sqlite3_errmsg(db));
    }
    return rc;
}

static int open_with_sqlite3_open(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int rc;
    int ok = 1;

    rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open(%s) failed in %s: rc=%d",
                path, label, rc);
        if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
        fprintf(stderr, "\n");
        ok = 0;
    } else if (!assert_active_profile(db, label)) {
        ok = 0;
    }

    if (db && close_db(db, label) != SQLITE_OK) ok = 0;
    return ok;
}

static int open_with_sqlite3_open_v2(const char *path, const char *label) {
    sqlite3 *db = NULL;
    int rc;
    int ok = 1;

    rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2(%s) failed in %s: rc=%d",
                path, label, rc);
        if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
        fprintf(stderr, "\n");
        ok = 0;
    } else if (!assert_active_profile(db, label)) {
        ok = 0;
    }

    if (db && close_db(db, label) != SQLITE_OK) ok = 0;
    return ok;
}

int main(void) {
    const char *path = "/tmp/library.db";
    int rc;
    int failures = 0;

    if (!unset_autopragma_disable()) failures++;
    if (!clean_db(path)) failures++;

    if (failures == 0 && !open_with_sqlite3_open(path, "first-open")) failures++;

    rc = sqlite3_shutdown();
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_shutdown failed: rc=%d expected=%d\n", rc, SQLITE_OK);
        failures++;
    } else {
        printf("PASS [shutdown]: rc=0\n");
    }

    /* sqlite3_open16 uses the same helper ordering at src/observability.c:542-544.
     * This shutdown-boundary smoke leaves direct open16 coverage to
     * config_after_dlopen_smoke.c intentionally. */
    if (!open_with_sqlite3_open_v2(path, "reopen-after-shutdown")) failures++;
    return failures == 0 ? 0 : 1;
}
