#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

typedef struct {
    const char *name;
    const char *sql;
    sqlite3_int64 expected_active;
    sqlite3_int64 expected_disabled;
} pragma_check;

static const pragma_check PRAGMA_CHECKS[] = {
    /* WHY: These PRAGMAs expose both callback-only values and compile-default
     * mirrors, so the smoke can distinguish active, disabled, and filter-miss paths. */
    {"busy_timeout", "PRAGMA busy_timeout;", 10000, 0},
    {"cache_size", "PRAGMA cache_size;", -1048576, -1048576},
    {"mmap_size", "PRAGMA mmap_size;", 8589934592LL, 8589934592LL},
    {"wal_autocheckpoint", "PRAGMA wal_autocheckpoint;", 16000, 16000},
    {"journal_size_limit", "PRAGMA journal_size_limit;", 67108864, 67108864},
    {"threads", "PRAGMA threads;", 8, 8},
    {"temp_store", "PRAGMA temp_store;", 2, 0},
    {"analysis_limit", "PRAGMA analysis_limit;", 1024, 0},
};

enum { PRAGMA_CHECK_COUNT = (int)(sizeof(PRAGMA_CHECKS) / sizeof(PRAGMA_CHECKS[0])) };

extern int auto_extension_path_is_target(const char *raw_fn);

typedef enum {
    EXPECT_ACTIVE,
    EXPECT_DISABLED
} expected_profile;

typedef struct {
    sqlite3_int64 observed[PRAGMA_CHECK_COUNT];
    int status[PRAGMA_CHECK_COUNT];
    int close_rc;
    const char *setup_failure;
} pragma_result;

static void init_result(pragma_result *result) {
    int i;

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        result->observed[i] = 0;
        result->status[i] = SQLITE_MISUSE;
    }
    result->close_rc = SQLITE_OK;
    result->setup_failure = NULL;
}

static int safe_setenv(const char *name, const char *value, int overwrite) {
    /* WHY: Environment mutation failures need errno in output or kill-switch
     * tests can fail before SQLite is exercised. */
    errno = 0;
    if (setenv(name, value, overwrite) != 0) {
        fprintf(stderr, "setenv(%s) failed: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

static int safe_unsetenv(const char *name) {
    errno = 0;
    if (unsetenv(name) != 0) {
        fprintf(stderr, "unsetenv(%s) failed: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
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

static int read_pragmas(sqlite3 *db, pragma_result *result) {
    int rc = SQLITE_OK;
    int i;

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        result->status[i] = read_integer_pragma(
            db,
            PRAGMA_CHECKS[i].name,
            PRAGMA_CHECKS[i].sql,
            &result->observed[i]
        );
        if (result->status[i] != SQLITE_OK && rc == SQLITE_OK) {
            rc = result->status[i];
        }
    }
    return rc;
}

static void print_observed_values(FILE *stream, const pragma_result *result) {
    int i;

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        if (result->status[i] == SQLITE_OK) {
            fprintf(stream, " %s=%lld",
                    PRAGMA_CHECKS[i].name, (long long)result->observed[i]);
        } else {
            fprintf(stream, " %s=<error rc=%d>",
                    PRAGMA_CHECKS[i].name, result->status[i]);
        }
    }
}

static sqlite3_int64 expected_value(int index, expected_profile profile) {
    if (profile == EXPECT_ACTIVE) return PRAGMA_CHECKS[index].expected_active;
    return PRAGMA_CHECKS[index].expected_disabled;
}

static int print_result(
    const char *label,
    const pragma_result *result,
    expected_profile profile
) {
    int ok = 1;
    int i;

    if (result->setup_failure) ok = 0;
    if (result->close_rc != SQLITE_OK) ok = 0;

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        if (result->status[i] != SQLITE_OK ||
            result->observed[i] != expected_value(i, profile)) {
            ok = 0;
        }
    }

    if (ok) {
        printf("PASS [%s]:", label);
        print_observed_values(stdout, result);
        printf("\n");
        return 1;
    }

    fprintf(stderr, "FAIL [%s]:", label);
    print_observed_values(stderr, result);
    if (result->setup_failure) {
        fprintf(stderr, " setup_failure=%s", result->setup_failure);
    }
    if (result->close_rc != SQLITE_OK) {
        fprintf(stderr, " close_rc=%d", result->close_rc);
    }
    fprintf(stderr, "\n");

    for (i = 0; i < PRAGMA_CHECK_COUNT; i++) {
        sqlite3_int64 expected = expected_value(i, profile);
        if (result->status[i] != SQLITE_OK) {
            fprintf(stderr, "  %s read failed: rc=%d (expected %lld)\n",
                    PRAGMA_CHECKS[i].name,
                    result->status[i],
                    (long long)expected);
        } else if (result->observed[i] != expected) {
            fprintf(stderr, "  %s mismatch: observed %lld expected %lld\n",
                    PRAGMA_CHECKS[i].name,
                    (long long)result->observed[i],
                    (long long)expected);
        }
    }
    return 0;
}

static int close_db(sqlite3 *db) {
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_close failed: rc=%d: %s\n", rc, sqlite3_errmsg(db));
    }
    return rc;
}

static int open_and_read_pragmas(const char *path, int flags, pragma_result *result) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2(%s) failed: rc=%d", path, rc);
        if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
        fprintf(stderr, "\n");
        if (db) result->close_rc = close_db(db);
        return rc;
    }

    rc = read_pragmas(db, result);
    result->close_rc = close_db(db);
    if (rc == SQLITE_OK && result->close_rc != SQLITE_OK) rc = result->close_rc;
    return rc;
}

static int run_rw_case(
    const char *label,
    const char *path,
    expected_profile profile
) {
    pragma_result result;
    init_result(&result);

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) {
        result.setup_failure = "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    } else if (!clean_db(path)) {
        result.setup_failure = "clean_db";
    } else {
        (void)open_and_read_pragmas(path, RW_FLAGS, &result);
    }

    return print_result(label, &result, profile);
}

static int run_kill_switch_1(void) {
    pragma_result result;
    const char *path = "/tmp/library.db";

    /* WHY: Literal "1" should leave only compile defaults visible because the
     * callback exits before emitting its PRAGMA block. */
    init_result(&result);
    if (!clean_db(path)) {
        result.setup_failure = "clean_db";
    } else if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) {
        result.setup_failure = "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    } else if (safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1", 1) != 0) {
        result.setup_failure = "setenv(SQLITE3_DISABLE_AUTOPRAGMA=1)";
    } else {
        (void)open_and_read_pragmas(path, RW_FLAGS, &result);
    }

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0 && !result.setup_failure) {
        result.setup_failure = "cleanup unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    }
    return print_result("kill-switch-1", &result, EXPECT_DISABLED);
}

static int run_kill_switch_non_1(void) {
    pragma_result result;
    const char *path = "/tmp/library.db";

    init_result(&result);
    if (!clean_db(path)) {
        result.setup_failure = "clean_db";
    } else if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) {
        result.setup_failure = "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    } else if (safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "true", 1) != 0) {
        result.setup_failure = "setenv(SQLITE3_DISABLE_AUTOPRAGMA=true)";
    } else {
        (void)open_and_read_pragmas(path, RW_FLAGS, &result);
    }

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0 && !result.setup_failure) {
        result.setup_failure = "cleanup unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    }
    return print_result("kill-switch-non-1", &result, EXPECT_ACTIVE);
}

static int seed_empty_db(const char *path) {
    sqlite3 *db = NULL;
    char *err = NULL;

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) return 0;

    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2(%s) for seed failed: rc=%d", path, rc);
        if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
        fprintf(stderr, "\n");
    } else {
        rc = sqlite3_exec(
            db,
            "CREATE TABLE IF NOT EXISTS auto_extension_smoke_seed(x INTEGER);"
            "DROP TABLE auto_extension_smoke_seed;",
            NULL,
            NULL,
            &err
        );
        if (rc != SQLITE_OK) {
            fprintf(stderr, "sqlite3_exec(seed) failed: rc=%d: %s\n",
                    rc, err ? err : sqlite3_errmsg(db));
        }
    }

    sqlite3_free(err);
    if (db) {
        int close_rc = close_db(db);
        if (rc == SQLITE_OK && close_rc != SQLITE_OK) rc = close_rc;
    }

    return rc == SQLITE_OK;
}

static int run_ro_case(
    const char *label,
    const char *path,
    expected_profile profile
) {
    pragma_result result;
    init_result(&result);

    if (!clean_db(path)) {
        result.setup_failure = "clean_db";
    } else if (!seed_empty_db(path)) {
        result.setup_failure = "seed_empty_db";
    } else if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) {
        result.setup_failure = "unsetenv(SQLITE3_DISABLE_AUTOPRAGMA)";
    } else {
        (void)open_and_read_pragmas(path, SQLITE_OPEN_READONLY, &result);
    }

    return print_result(label, &result, profile);
}

static int check_filter_case(const char *label, const char *path, int expected) {
    int observed = auto_extension_path_is_target(path);

    if (observed == expected) {
        printf("PASS [filter-unit/%s]: observed=%d expected=%d\n",
               label, observed, expected);
        return 1;
    }

    fprintf(stderr, "FAIL [filter-unit/%s]: observed=%d expected=%d\n",
            label, observed, expected);
    return 0;
}

static int run_path_filter_unit_cases(void) {
    static const char suffix[] = "/library.db";
    enum { LONG_PATH_LEN = 3000 };
    int ok = 1;
    char *long_path;

    ok &= check_filter_case(
        "plex-hit",
        "/var/lib/plex/Databases/com.plexapp.plugins.library.db",
        1
    );
    ok &= check_filter_case("emby-hit", "/opt/emby/data/library.db", 1);
    ok &= check_filter_case("jf-hit", "/var/lib/jellyfin/data/jellyfin.db", 1);
    ok &= check_filter_case("nontarget-miss", "/tmp/not-a-target.db", 0);
    ok &= check_filter_case("empty-miss", "", 0);
    ok &= check_filter_case("null-miss", NULL, 0);
    ok &= check_filter_case(
        "uri-query-stripped-hit",
        "/opt/emby/data/library.db?vfs=unix",
        1
    );

    long_path = malloc(LONG_PATH_LEN + 1);
    if (!long_path) {
        fprintf(stderr, "FAIL [filter-unit/long-truncation]: malloc failed\n");
        return 0;
    }
    memset(long_path, 'a', LONG_PATH_LEN);
    memcpy(long_path + LONG_PATH_LEN - strlen(suffix), suffix, strlen(suffix));
    long_path[LONG_PATH_LEN] = '\0';
    ok &= check_filter_case("long-truncation-miss", long_path, 0);
    free(long_path);

    return ok;
}

/* WHY: Exposed by src/auto_extension.c so the smoke can prove the constructor's
 * runtime sqlite3_config(SORTERREF_SIZE) landed before sqlite3_initialize().
 * Log capture is infeasible because the constructor runs at libsqlite3.so
 * dlopen time, before main() can install a log handler. */
extern int auto_extension_sorterref_cfg_rc(void);

static int run_sorterref_config_case(void) {
    int rc = auto_extension_sorterref_cfg_rc();
    if (rc == SQLITE_OK) {
        printf("PASS [sorterref-config]: cfg_rc=0\n");
        return 1;
    }
    fprintf(stderr,
            "FAIL [sorterref-config]: cfg_rc=%d "
            "(SORTER_REFERENCES feature would be inert)\n", rc);
    return 0;
}

int main(void) {
    int failures = 0;

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) failures++;

    failures += !run_sorterref_config_case();
    failures += !run_rw_case("filter-hit-emby", "/tmp/library.db", EXPECT_ACTIVE);
    failures += !run_rw_case("filter-hit-jf", "/tmp/jellyfin.db", EXPECT_ACTIVE);
    failures += !run_rw_case(
        "filter-hit-plex",
        "/tmp/com.plexapp.plugins.library.db",
        EXPECT_ACTIVE
    );
    failures += !run_rw_case("filter-miss", "/tmp/not-a-target.db", EXPECT_DISABLED);
    failures += !run_kill_switch_1();
    failures += !run_kill_switch_non_1();
    failures += !run_ro_case("ro-skip", "/tmp/library.db", EXPECT_DISABLED);
    failures += !run_path_filter_unit_cases();

    if (safe_unsetenv("SQLITE3_DISABLE_AUTOPRAGMA") != 0) failures++;
    return failures == 0 ? 0 : 1;
}
