#include "sqlite3.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

int auto_extension_sorterref_cfg_rc(void);
int auto_extension_pmasz_cfg_rc(void);

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

typedef struct {
    const char *label;
    const char *path;
    int (*open_fn)(const char*, const char*);
    int rc;
} thread_case;

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

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return 1;
    if (errno == EEXIST) return 1;

    fprintf(stderr, "mkdir(%s) failed: %s\n", path, strerror(errno));
    return 0;
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

static int path_to_utf16(const char *path, uint16_t *path16, size_t count) {
    size_t i;
    size_t len = strlen(path);

    if (len + 1 > count) {
        fprintf(stderr, "sqlite3_open16 path too long: %s\n", path);
        return 0;
    }
    for (i = 0; i <= len; i++) path16[i] = (uint16_t)(unsigned char)path[i];
    return 1;
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

static int open_with_sqlite3_open16(const char *path, const char *label) {
    uint16_t path16[512];
    sqlite3 *db = NULL;
    int rc;
    int ok = 1;

    if (!path_to_utf16(path, path16, sizeof(path16) / sizeof(path16[0]))) {
        return 0;
    }

    rc = sqlite3_open16((const void*)path16, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open16(%s) failed in %s: rc=%d",
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

static int run_pre_open_config_case(void) {
    int rc;

    if (!unset_autopragma_disable()) return 0;
    if (!ensure_dir("/tmp/config_after_dlopen_open")) return 0;
    if (!ensure_dir("/tmp/config_after_dlopen_open_v2")) return 0;
    if (!ensure_dir("/tmp/config_after_dlopen_open16")) return 0;
    if (!clean_db("/tmp/config_after_dlopen_open/library.db")) return 0;
    if (!clean_db("/tmp/config_after_dlopen_open_v2/library.db")) return 0;
    if (!clean_db("/tmp/config_after_dlopen_open16/library.db")) return 0;

    rc = auto_extension_sorterref_cfg_rc();
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "FAIL [pre-open-config]: SORTERREF_SIZE constructor rc=%d expected=%d\n",
                rc, SQLITE_OK);
        return 0;
    }
    printf("PASS [pre-open-config]: SORTERREF_SIZE constructor rc=0\n");

    rc = auto_extension_pmasz_cfg_rc();
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "FAIL [pre-open-config]: PMASZ constructor rc=%d expected=%d\n",
                rc, SQLITE_OK);
        return 0;
    }
    printf("PASS [pre-open-config]: PMASZ constructor rc=0\n");

    rc = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "FAIL [pre-open-config]: SQLITE_CONFIG_MULTITHREAD rc=%d expected=%d\n",
                rc, SQLITE_OK);
        return 0;
    }
    printf("PASS [pre-open-config]: SQLITE_CONFIG_MULTITHREAD rc=0\n");

    rc = sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1);
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "FAIL [pre-open-config]: SQLITE_CONFIG_MEMSTATUS rc=%d expected=%d\n",
                rc, SQLITE_OK);
        return 0;
    }
    printf("PASS [pre-open-config]: SQLITE_CONFIG_MEMSTATUS rc=0\n");

    if (!open_with_sqlite3_open(
            "/tmp/config_after_dlopen_open/library.db",
            "post-config-open")) {
        return 0;
    }
    if (!open_with_sqlite3_open_v2(
            "/tmp/config_after_dlopen_open_v2/library.db",
            "post-config-open-v2")) {
        return 0;
    }
    return open_with_sqlite3_open16(
        "/tmp/config_after_dlopen_open16/library.db",
        "post-config-open16");
}

static void *thread_open(void *arg) {
    thread_case *tc = (thread_case*)arg;
    tc->rc = tc->open_fn(tc->path, tc->label) ? SQLITE_OK : SQLITE_ERROR;
    return NULL;
}

static int run_concurrent_first_open_child(void) {
    pthread_t t1;
    pthread_t t2;
    pthread_t t3;
    thread_case a = {
        "concurrent-first-open-a",
        "/tmp/config_after_dlopen_thread_a/library.db",
        open_with_sqlite3_open,
        SQLITE_ERROR
    };
    thread_case b = {
        "concurrent-first-open-b",
        "/tmp/config_after_dlopen_thread_b/library.db",
        open_with_sqlite3_open_v2,
        SQLITE_ERROR
    };
    thread_case c = {
        "concurrent-first-open-c",
        "/tmp/config_after_dlopen_thread_c/library.db",
        open_with_sqlite3_open16,
        SQLITE_ERROR
    };
    int ok = 1;

    if (!unset_autopragma_disable()) return 1;
    if (!ensure_dir("/tmp/config_after_dlopen_thread_a")) return 1;
    if (!ensure_dir("/tmp/config_after_dlopen_thread_b")) return 1;
    if (!ensure_dir("/tmp/config_after_dlopen_thread_c")) return 1;
    if (!clean_db(a.path)) return 1;
    if (!clean_db(b.path)) return 1;
    if (!clean_db(c.path)) return 1;

    if (pthread_create(&t1, NULL, thread_open, &a) != 0) {
        fprintf(stderr, "pthread_create(%s) failed\n", a.label);
        return 1;
    }
    if (pthread_create(&t2, NULL, thread_open, &b) != 0) {
        fprintf(stderr, "pthread_create(%s) failed\n", b.label);
        pthread_join(t1, NULL);
        return 1;
    }
    if (pthread_create(&t3, NULL, thread_open, &c) != 0) {
        fprintf(stderr, "pthread_create(%s) failed\n", c.label);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        return 1;
    }

    if (pthread_join(t1, NULL) != 0) {
        fprintf(stderr, "pthread_join(%s) failed\n", a.label);
        ok = 0;
    }
    if (pthread_join(t2, NULL) != 0) {
        fprintf(stderr, "pthread_join(%s) failed\n", b.label);
        ok = 0;
    }
    if (pthread_join(t3, NULL) != 0) {
        fprintf(stderr, "pthread_join(%s) failed\n", c.label);
        ok = 0;
    }
    if (a.rc != SQLITE_OK || b.rc != SQLITE_OK || c.rc != SQLITE_OK) {
        fprintf(stderr,
                "FAIL [concurrent-first-open]: rc_a=%d rc_b=%d rc_c=%d expected=0\n",
                a.rc, b.rc, c.rc);
        ok = 0;
    }
    if (ok) printf("PASS [concurrent-first-open]: all opens received active profile\n");
    return ok ? 0 : 1;
}

static int run_concurrent_first_open_exec(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;
    char *child_argv[] = { argv[0], "--concurrent-first-open", NULL };

    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 0;
    }
    if (pid == 0) {
        execv(argv[0], child_argv);
        fprintf(stderr, "execv(%s) failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL [concurrent-first-open]: child status=%d\n", status);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int failures = 0;

    if (argc == 2 && strcmp(argv[1], "--concurrent-first-open") == 0) {
        return run_concurrent_first_open_child();
    }

    failures += !run_pre_open_config_case();
    failures += !run_concurrent_first_open_exec(argv);
    return failures == 0 ? 0 : 1;
}
