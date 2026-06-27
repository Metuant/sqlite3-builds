#include "sqlite3.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void auto_extension_register_for_open(void);
void slow_query_test_disable_atexit_dump(void);

struct fake_db {
    const char *filename;
};

struct fake_stmt {
    struct fake_db *db;
    const char *sql;
};

static unsigned g_trace_mask;
static int (*g_trace_cb)(unsigned, void*, void*, void*);
static unsigned g_trace_registrations;
static int (*g_autoext_cb)(sqlite3*, char**, const sqlite3_api_routines*);

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *read_all_fd(int fd) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) failf("FATAL: malloc failed");
    for (;;) {
        ssize_t n;
        if (len + 2048 + 1 > cap) {
            char *next;
            cap *= 2;
            next = realloc(buf, cap);
            if (!next) failf("FATAL: realloc failed");
            buf = next;
        }
        n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            failf("FATAL: read failed errno=%d", errno);
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    return buf;
}

static char *run_child_capture(const char *self, const char *mode, char *const envp[]) {
    int pipefd[2];
    pid_t pid;
    int status;
    char *out;
    char *const argv_child[] = { (char *)self, (char *)mode, NULL };

    if (pipe(pipefd) != 0) failf("FATAL: pipe failed errno=%d", errno);
    pid = fork();
    if (pid < 0) failf("FATAL: fork failed errno=%d", errno);
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execve(self, argv_child, envp);
        _exit(127);
    }
    close(pipefd[1]);
    out = read_all_fd(pipefd[0]);
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid failed errno=%d", errno);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d output=%s", mode, status, out);
    }
    return out;
}

static void reset_trace_state(void) {
    g_trace_mask = 0;
    g_trace_cb = NULL;
    g_trace_registrations = 0;
    g_autoext_cb = NULL;
}

static void require_contains(const char *label, const char *haystack, const char *needle) {
    if (!strstr(haystack, needle)) {
        failf("FATAL: %s missing \"%s\": %s", label, needle, haystack);
    }
}

static void require_absent(const char *label, const char *haystack, const char *needle) {
    if (strstr(haystack, needle)) {
        failf("FATAL: %s unexpectedly found \"%s\": %s", label, needle, haystack);
    }
}

static void run_trace_registration_scenario(const char *label, unsigned want_mask) {
    struct fake_db db = { .filename = "/tmp/stmt-trace-smoke.db" };
    struct fake_stmt stmt = { .db = &db, .sql = "SELECT trace_stmt_smoke" };
    sqlite3_int64 elapsed_ns = 1000000000LL;
    int rc;

    reset_trace_state();
    auto_extension_register_for_open();
    if (!g_autoext_cb) failf("FATAL: %s missing auto-extension callback registration", label);

    rc = g_autoext_cb((sqlite3 *)&db, NULL, NULL);
    if (rc != SQLITE_OK) {
        failf("FATAL: %s autopragma callback rc=%d", label, rc);
    }
    if (g_trace_registrations != 1u) {
        failf("FATAL: %s trace registrations=%u want=1", label, g_trace_registrations);
    }
    if (g_trace_mask != want_mask) {
        failf("FATAL: %s trace_mask=%u want=%u", label, g_trace_mask, want_mask);
    }
    if (!g_trace_cb) failf("FATAL: %s missing registered trace callback", label);

    if (g_trace_mask & SQLITE_TRACE_STMT) {
        g_trace_cb(SQLITE_TRACE_STMT, NULL, &stmt, (void *)stmt.sql);
    }
    if (g_trace_mask & SQLITE_TRACE_PROFILE) {
        g_trace_cb(SQLITE_TRACE_PROFILE, NULL, &stmt, &elapsed_ns);
    }
    slow_query_test_disable_atexit_dump();
}

static int child_default(void) {
    run_trace_registration_scenario("default", SQLITE_TRACE_PROFILE);
    return 0;
}

static int child_stmt_enabled(void) {
    run_trace_registration_scenario(
        "stmt-enabled",
        (unsigned)(SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE)
    );
    return 0;
}

static int child_stmt_disabled(void) {
    run_trace_registration_scenario("stmt-disabled", SQLITE_TRACE_PROFILE);
    return 0;
}

static int child_stmt_other(void) {
    run_trace_registration_scenario("stmt-other", SQLITE_TRACE_PROFILE);
    return 0;
}

int sqlite3_initialize_real(void) {
    return SQLITE_OK;
}

int sqlite3_config_real(int op, ...) {
    (void)op;
    return SQLITE_OK;
}

int sqlite3_db_config_real(sqlite3 *db, int op, ...) {
    (void)db;
    (void)op;
    return SQLITE_OK;
}

int sqlite3_open_real(const char *filename, sqlite3 **ppDb) {
    (void)filename;
    if (ppDb) *ppDb = NULL;
    return SQLITE_OK;
}

int sqlite3_open_v2_real(
    const char *filename, sqlite3 **ppDb, int flags, const char *zVfs
) {
    (void)filename;
    (void)flags;
    (void)zVfs;
    if (ppDb) *ppDb = NULL;
    return SQLITE_OK;
}

int sqlite3_open16_real(const void *filename, sqlite3 **ppDb) {
    (void)filename;
    if (ppDb) *ppDb = NULL;
    return SQLITE_OK;
}

int sqlite3_auto_extension(void (*xEntryPoint)(void)) {
    g_autoext_cb = (int (*)(sqlite3*, char**, const sqlite3_api_routines*))xEntryPoint;
    return SQLITE_OK;
}

int sqlite3_trace_v2(
    sqlite3 *db,
    unsigned mask,
    int (*xCallback)(unsigned, void*, void*, void*),
    void *pCtx
) {
    (void)db;
    (void)pCtx;
    g_trace_registrations++;
    g_trace_mask = mask;
    g_trace_cb = xCallback;
    return SQLITE_OK;
}

const char *sqlite3_db_filename(sqlite3 *db, const char *zDbName) {
    struct fake_db *fake = (struct fake_db *)db;
    (void)zDbName;
    return fake ? fake->filename : NULL;
}

int sqlite3_db_readonly(sqlite3 *db, const char *zDbName) {
    (void)db;
    (void)zDbName;
    return 0;
}

sqlite3 *sqlite3_db_handle(sqlite3_stmt *pStmt) {
    struct fake_stmt *fake = (struct fake_stmt *)pStmt;
    return fake ? (sqlite3 *)fake->db : NULL;
}

const char *sqlite3_sql(sqlite3_stmt *pStmt) {
    struct fake_stmt *fake = (struct fake_stmt *)pStmt;
    return fake ? fake->sql : NULL;
}

int sqlite3_exec(
    sqlite3 *db,
    const char *sql,
    int (*callback)(void*, int, char**, char**),
    void *arg,
    char **errmsg
) {
    (void)db;
    (void)sql;
    (void)callback;
    (void)arg;
    if (errmsg) *errmsg = NULL;
    return SQLITE_OK;
}

void sqlite3_free(void *p) {
    free(p);
}

void sqlite3_log(int iErrCode, const char *zFormat, ...) {
    (void)iErrCode;
    (void)zFormat;
}

void runtime_optimize_seed_path(sqlite3 *db, const char *raw_fn) {
    (void)db;
    (void)raw_fn;
    abort();
}

int main(int argc, char **argv) {
    char *env_default[] = { NULL };
    char *env_stmt_enabled[] = { "SQLITE3_DISABLE_STMT_TRACE=0", NULL };
    char *env_stmt_disabled[] = { "SQLITE3_DISABLE_STMT_TRACE=1", NULL };
    char *env_stmt_other[] = { "SQLITE3_DISABLE_STMT_TRACE=false", NULL };
    char *out;

    if (argc == 2 && strcmp(argv[1], "child-default") == 0) return child_default();
    if (argc == 2 && strcmp(argv[1], "child-stmt-enabled") == 0) return child_stmt_enabled();
    if (argc == 2 && strcmp(argv[1], "child-stmt-disabled") == 0) return child_stmt_disabled();
    if (argc == 2 && strcmp(argv[1], "child-stmt-other") == 0) return child_stmt_other();

    out = run_child_capture(argv[0], "child-default", env_default);
    require_absent("default trace output", out, " trace_stmt ");
    require_absent("default trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("default slow_query output", out, " slow_query ");
    require_contains("default slow_query output", out, "sql=\"SELECT trace_stmt_smoke\"");
    free(out);

    out = run_child_capture(argv[0], "child-stmt-enabled", env_stmt_enabled);
    require_contains("stmt-enabled trace output", out, " trace_stmt ");
    require_contains("stmt-enabled trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("stmt-enabled slow_query output", out, " slow_query ");
    require_contains("stmt-enabled slow_query output", out, "sql=\"SELECT trace_stmt_smoke\"");
    free(out);

    out = run_child_capture(argv[0], "child-stmt-disabled", env_stmt_disabled);
    require_absent("stmt-disabled trace output", out, " trace_stmt ");
    require_absent("stmt-disabled trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("stmt-disabled slow_query output", out, " slow_query ");
    require_contains("stmt-disabled slow_query output", out, "sql=\"SELECT trace_stmt_smoke\"");
    free(out);

    out = run_child_capture(argv[0], "child-stmt-other", env_stmt_other);
    require_absent("stmt-other trace output", out, " trace_stmt ");
    require_absent("stmt-other trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("stmt-other slow_query output", out, " slow_query ");
    require_contains("stmt-other slow_query output", out, "sql=\"SELECT trace_stmt_smoke\"");
    free(out);

    printf("stmt trace smoke passed\n");
    return 0;
}
