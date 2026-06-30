#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
};

enum { PRAGMA_CHECK_COUNT = (int)(sizeof(PRAGMA_CHECKS) / sizeof(PRAGMA_CHECKS[0])) };

typedef struct {
    const char *name;
    const char *value;
} child_env;

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

static int set_autopragma_disable(void) {
    if (setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1", 1) == 0) return 1;

    fprintf(stderr, "setenv(SQLITE3_DISABLE_AUTOPRAGMA=1) failed: %s\n", strerror(errno));
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
    if (dup2(saved_fd, STDERR_FILENO) < 0) ok = 0;
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

static int assert_analysis_limit_trace(
    const char *label,
    const char *output,
    int expected_present
) {
    const char *needle = "sql=\"PRAGMA analysis_limit=0;\"";
    int present = strstr(output, needle) != NULL;

    if (present == expected_present) {
        printf("PASS [%s]: open-time trace %s PRAGMA analysis_limit=0\n",
               label, expected_present ? "emitted" : "did not emit");
        return 1;
    }
    fprintf(stderr,
            "FAIL [%s]: open-time trace %s `%s` in:\n%s\n",
            label,
            expected_present ? "missing" : "unexpectedly contained",
            needle,
            output);
    return 0;
}

static int capture_analysis_limit_open_trace(
    const char *path,
    const char *label,
    int expected_present
) {
    char output[65536];
    sqlite3 *db = NULL;
    FILE *capture = NULL;
    int saved_fd = -1;
    int rc;
    int ok = 1;

    if (!capture_stderr_begin(&capture, &saved_fd, label)) return 0;
    rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (capture &&
        !capture_stderr_end(capture, saved_fd, output, sizeof(output), label)) {
        ok = 0;
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2(%s) failed in %s: rc=%d",
                path, label, rc);
        if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
        fprintf(stderr, "\n");
        ok = 0;
    }
    if (ok && !assert_analysis_limit_trace(label, output, expected_present)) ok = 0;
    if (db && close_db(db, label) != SQLITE_OK) ok = 0;
    return ok;
}

static int run_analysis_limit_reopen_trace_child_case(
    const char *label,
    const char *path,
    int expected_present
) {
    int rc;
    int ok = 1;

    if (!clean_db(path)) return 0;
    if (!unset_autopragma_disable()) return 0;
    if (!capture_analysis_limit_open_trace(path, "analysis-limit-first-open-trace", 1)) ok = 0;
    rc = sqlite3_shutdown();
    if (rc != SQLITE_OK) {
        fprintf(stderr, "FAIL [%s]: sqlite3_shutdown rc=%d expected SQLITE_OK\n", label, rc);
        ok = 0;
    }
    if (ok) {
        if (expected_present) {
            if (!unset_autopragma_disable()) ok = 0;
        } else if (!set_autopragma_disable()) {
            ok = 0;
        }
    }
    if (ok && !capture_analysis_limit_open_trace(path, label, expected_present)) ok = 0;
    if (!unset_autopragma_disable()) ok = 0;
    return ok;
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

static int run_analysis_limit_trace_case(const char *self_path, const char *case_name) {
    const child_env env[] = {
        {"SQLITE3_DISABLE_STMT_TRACE", "0"},
        {"SQLITE3_DISABLE_AUTOPRAGMA", NULL},
        {"SQLITE3_DISABLE_OBSERVABILITY", NULL},
        {"SQLITE3_DISABLE_SLOW_QUERY", "1"},
        {"SQLITE3_DISABLE_RUNTIME_OPTIMIZE", NULL},
    };
    return run_child_process_case(
        self_path,
        case_name,
        env,
        sizeof(env) / sizeof(env[0])
    );
}

static int run_named_child_case(const char *case_name) {
    if (strcmp(case_name, "analysis-limit-reopen-trace-active") == 0) {
        return run_analysis_limit_reopen_trace_child_case(
            "analysis-limit-reopen-trace-active",
            "/tmp/shutdown-reinit-analysis-limit-active-library.db",
            1
        );
    }
    if (strcmp(case_name, "analysis-limit-reopen-trace-disabled") == 0) {
        return run_analysis_limit_reopen_trace_child_case(
            "analysis-limit-reopen-trace-disabled",
            "/tmp/shutdown-reinit-analysis-limit-disabled-library.db",
            0
        );
    }
    fprintf(stderr, "unknown child case: %s\n", case_name);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "/tmp/library.db";
    int rc;
    int failures = 0;

    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        return run_named_child_case(argv[2]) ? 0 : 1;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--case name]\n", argv[0]);
        return 1;
    }

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
    if (!run_analysis_limit_trace_case(argv[0], "analysis-limit-reopen-trace-active")) failures++;
    if (!run_analysis_limit_trace_case(argv[0], "analysis-limit-reopen-trace-disabled")) failures++;
    return failures == 0 ? 0 : 1;
}
