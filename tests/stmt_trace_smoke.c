#include "sqlite3.h"
#include "fts_lex.h"
#include "observability.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void auto_extension_register_for_open(void);
void slow_query_test_disable_atexit_dump(void);

#define FAKE_CLIENTDATA_SLOTS 4

struct fake_clientdata {
    const char *key;
    void *data;
    void (*xDestroy)(void *);
};

struct fake_db {
    const char *filename;
    struct fake_clientdata clientdata[FAKE_CLIENTDATA_SLOTS];
};

struct fake_stmt {
    struct fake_db *db;
    const char *sql;
};

static unsigned g_trace_mask;
static int (*g_trace_cb)(unsigned, void*, void*, void*);
static unsigned g_trace_registrations;
static int (*g_autoext_cb)(sqlite3*, char**, const sqlite3_api_routines*);
static unsigned g_sqlite_malloc_calls;
static unsigned g_sqlite_malloc_fail_call;
static unsigned g_set_clientdata_calls;
static unsigned g_set_clientdata_fail_call;
static int g_prepare_capture_miss;
static pthread_mutex_t g_fake_clientdata_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_creation_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_creation_race_cond = PTHREAD_COND_INITIALIZER;
static unsigned g_creation_race_gets;
static unsigned g_connection_state_overwrites;
static int g_connection_creation_race;
static void *g_deferred_destroy_data;
static void (*g_deferred_destroy)(void *);

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

static void require_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) {
        failf("FATAL: %s text=(null)", label);
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            return;
        }
        if (!end) break;
        line = end + 1;
    }
    failf("FATAL: %s has no line containing \"%s\" and \"%s\": %s",
          label, first, second, text);
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;

    if (needle_len == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static void require_occurrences(
    const char *label,
    const char *haystack,
    const char *needle,
    int want
) {
    int got = count_occurrences(haystack, needle);
    if (got != want) {
        failf("FATAL: %s occurrences=%d want=%d needle=\"%s\": %s",
              label, got, want, needle, haystack);
    }
}

static void reset_fault_state(void) {
    g_sqlite_malloc_calls = 0;
    g_sqlite_malloc_fail_call = 0;
    g_set_clientdata_calls = 0;
    g_set_clientdata_fail_call = 0;
    g_creation_race_gets = 0;
    g_connection_state_overwrites = 0;
    g_connection_creation_race = 0;
    g_deferred_destroy_data = NULL;
    g_deferred_destroy = NULL;
}

static void free_fake_clientdata(struct fake_db *db) {
    unsigned i;
    for (i = 0; i < FAKE_CLIENTDATA_SLOTS; i++) {
        if (db->clientdata[i].xDestroy && db->clientdata[i].data) {
            db->clientdata[i].xDestroy(db->clientdata[i].data);
        }
        memset(&db->clientdata[i], 0, sizeof(db->clientdata[i]));
    }
}

static char *make_long_capture_sql(void) {
    static const char prefix[] = "SELECT '";
    static const char suffix[] = "CAPTURE_END_SENTINEL'";
    size_t payload_len = 9000;
    size_t sql_len = sizeof(prefix) - 1 + payload_len + sizeof(suffix) - 1;
    char *sql = malloc(sql_len + 1);
    if (!sql) failf("FATAL: long capture SQL allocation failed");
    memcpy(sql, prefix, sizeof(prefix) - 1);
    memset(sql + sizeof(prefix) - 1, 'x', payload_len);
    memcpy(sql + sizeof(prefix) - 1 + payload_len, suffix, sizeof(suffix));
    return sql;
}

static char *make_long_log_value(void) {
    static const char sentinel[] = "EXPANDED_END_SENTINEL";
    size_t value_len = 65536u;
    char *value = malloc(value_len + 1u);
    if (!value) failf("FATAL: long log value allocation failed");
    memset(value, 'Z', value_len);
    memcpy(
        value + value_len - (sizeof(sentinel) - 1u),
        sentinel,
        sizeof(sentinel)
    );
    return value;
}

static void run_trace_registration_scenario(
    const char *label,
    unsigned want_mask,
    unsigned stmt_callbacks
) {
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
        unsigned i;
        for (i = 0; i < stmt_callbacks; i++) {
            const char *callback_sql =
                (i == 1u || i == 2u) ? "SELECT trace_stmt_distinct" : stmt.sql;
            g_trace_cb(SQLITE_TRACE_STMT, NULL, &stmt, (void *)callback_sql);
        }
    }
    if (g_trace_mask & SQLITE_TRACE_PROFILE) {
        g_trace_cb(SQLITE_TRACE_PROFILE, NULL, &stmt, &elapsed_ns);
    }
    slow_query_test_disable_atexit_dump();
}

static int child_default(void) {
    run_trace_registration_scenario("default", SQLITE_TRACE_PROFILE, 1025u);
    return 0;
}

static int child_stmt_enabled(void) {
    run_trace_registration_scenario(
        "stmt-enabled",
        (unsigned)(SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE),
        1025u
    );
    return 0;
}

static int child_stmt_disabled(void) {
    run_trace_registration_scenario("stmt-disabled", SQLITE_TRACE_PROFILE, 1025u);
    return 0;
}

static int child_stmt_other(void) {
    run_trace_registration_scenario("stmt-other", SQLITE_TRACE_PROFILE, 1025u);
    return 0;
}

static int child_stmt_full(void) {
    run_trace_registration_scenario(
        "stmt-full",
        (unsigned)(SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE),
        1025u
    );
    return 0;
}

static int child_capture_miss(int fail_alloc) {
    struct fake_db db = { .filename = "/tmp/capture-miss-smoke.db" };
    sqlite3_stmt *stmt = (sqlite3_stmt *)(uintptr_t)1;
    const char *tail = NULL;
    char *sql = make_long_capture_sql();
    int rc;

    reset_fault_state();
    if (fail_alloc) g_sqlite_malloc_fail_call = 1u;
    g_prepare_capture_miss = 1;
    rc = sqlite3_prepare_v2((sqlite3 *)&db, sql, -1, &stmt, &tail);
    g_prepare_capture_miss = 0;
    if (rc != SQLITE_OK || stmt != NULL || tail != sql + strlen(sql)) {
        failf(
            "FATAL: capture-miss prepare rc=%d stmt=%p tail_offset=%ld want=%ld",
            rc, (void *)stmt, tail ? (long)(tail - sql) : -1L, (long)strlen(sql)
        );
    }
    free_fake_clientdata(&db);
    free(sql);
    return 0;
}

static void log_test_rewrite_miss(
    const char *mode,
    obs_miss_reason reason,
    const char *sub_reason,
    const char *sql
) {
    uint64_t shape = 0;
    size_t len = strlen(sql);

    if (!obs_is_disabled()) shape = fts_lex_shape_key(sql, len);
    obs_log_rewrite_miss(
        "emby", mode, reason, sub_reason, NULL, sql, len, shape
    );
}

static int child_miss_sampling(void) {
    static const char sql_a[] = "SELECT miss_a FROM source_a";
    static const char sql_b[] = "SELECT miss_b, extra_b FROM source_b";
    unsigned i;

    for (i = 1u; i <= 1025u; i++) {
        const char *sql = (i == 2u || i == 3u) ? sql_b : sql_a;
        log_test_rewrite_miss(
            "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection", sql
        );
    }
    return 0;
}

static int child_miss_guid_dedup(void) {
    char sql[192];
    unsigned i;

    for (i = 0u; i < 32u; i++) {
        int rc = snprintf(
            sql, sizeof(sql),
            "SELECT id FROM metadata_items WHERE guid='plex://show/%024x'",
            i
        );
        if (rc < 0 || (size_t)rc >= sizeof(sql)) {
            failf("FATAL: GUID miss SQL overflow");
        }
        log_test_rewrite_miss(
            "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection", sql
        );
    }
    return 0;
}

static int child_miss_list_dedup(void) {
    static const char list_two[] = "SELECT id FROM items WHERE id IN (1,2)";
    static const char list_three[] = "SELECT id FROM items WHERE id IN (1,2,3)";

    log_test_rewrite_miss(
        "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection", list_two
    );
    log_test_rewrite_miss(
        "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection", list_three
    );
    return 0;
}

static int child_miss_shape_cap(void) {
    char sql[128];
    unsigned i;

    for (i = 0u; i < 513u; i++) {
        int rc = snprintf(
            sql, sizeof(sql), "SELECT column_%u FROM table_%u", i, i
        );
        if (rc < 0 || (size_t)rc >= sizeof(sql)) {
            failf("FATAL: shape-cap SQL overflow");
        }
        log_test_rewrite_miss(
            "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection", sql
        );
    }
    for (i = 513u; i < 1024u; i++) {
        log_test_rewrite_miss(
            "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection",
            "SELECT column_0 FROM table_0"
        );
    }
    return 0;
}

static int child_miss_lexer_error(void) {
    log_test_rewrite_miss(
        "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection",
        "SELECT 'unterminated_a"
    );
    log_test_rewrite_miss(
        "dashboard+episodes_latest", OBS_MISS_CAPTURE, "projection",
        "SELECT /* unterminated_b"
    );
    return 0;
}

static int child_miss_unknown_mode(void) {
    log_test_rewrite_miss(
        "unknown-mode", OBS_MISS_CAPTURE, "projection", "SELECT unknown_mode"
    );
    return 0;
}

static int child_applied_sampling(void) {
    static const char source_a[] = "SELECT source_a";
    static const char rewritten_a[] = "SELECT rewritten_a";
    static const char source_b[] = "SELECT source_b";
    static const char rewritten_b[] = "SELECT rewritten_b";
    struct fake_db db = { .filename = "/tmp/applied-sampling-smoke.db" };
    unsigned i;

    reset_fault_state();
    for (i = 1; i <= 1025u; i++) {
        const char *source = (i == 2u || i == 3u) ? source_b : source_a;
        const char *rewritten = (i == 2u || i == 3u) ? rewritten_b : rewritten_a;
        obs_log_rewrite_applied(
            "emby", "dashboard+episodes_latest", (sqlite3 *)&db,
            source, strlen(source), rewritten, strlen(rewritten)
        );
    }
    free_fake_clientdata(&db);
    return 0;
}

static int child_applied_corr_cap(void) {
    struct fake_db db = { .filename = "/tmp/applied-corr-cap-smoke.db" };
    char source[64];
    char rewritten[64];
    unsigned i;

    reset_fault_state();
    for (i = 0; i < 513u; i++) {
        snprintf(source, sizeof(source), "SELECT source_%u", i);
        snprintf(rewritten, sizeof(rewritten), "SELECT rewritten_%u", i);
        obs_log_rewrite_applied(
            "emby", "dashboard+episodes_latest", (sqlite3 *)&db,
            source, strlen(source), rewritten, strlen(rewritten)
        );
    }
    for (i = 513u; i < 1024u; i++) {
        obs_log_rewrite_applied(
            "emby", "dashboard+episodes_latest", (sqlite3 *)&db,
            "SELECT source_0", strlen("SELECT source_0"),
            "SELECT rewritten_0", strlen("SELECT rewritten_0")
        );
    }
    free_fake_clientdata(&db);
    return 0;
}

static int child_applied_corr_failure(int fail_clientdata) {
    struct fake_db db = { .filename = "/tmp/applied-corr-failure-smoke.db" };
    unsigned i;

    reset_fault_state();
    if (fail_clientdata) {
        g_set_clientdata_fail_call = 2u;
    } else {
        g_sqlite_malloc_fail_call = 2u;
    }
    for (i = 0; i < 1024u; i++) {
        obs_log_rewrite_applied(
            "emby", "dashboard+episodes_latest", (sqlite3 *)&db,
            "SELECT failure_source", strlen("SELECT failure_source"),
            "SELECT failure_rewritten", strlen("SELECT failure_rewritten")
        );
    }
    free_fake_clientdata(&db);
    return 0;
}

static int child_long_obs_log(int fail_alloc) {
    char *value = make_long_log_value();

    reset_fault_state();
    if (fail_alloc) g_sqlite_malloc_fail_call = 1u;
    obs_logf(
        "slow_query_expanded",
        "sql_expanded_len=65536 sql_expanded_truncated=0 sql_expanded=\"%s\"",
        value
    );
    free(value);
    return 0;
}

static int child_connection_state_failure(int stmt_stream, int fail_clientdata) {
    static const char source[] = "SELECT first_state_failure_source";
    static const char rewritten[] = "SELECT first_state_failure_rewritten";
    struct fake_db db = { .filename = "/tmp/first-state-failure-smoke.db" };
    struct fake_stmt stmt = { .db = &db, .sql = source };
    unsigned i;

    reset_fault_state();
    if (fail_clientdata) {
        g_set_clientdata_fail_call = 1u;
    } else {
        g_sqlite_malloc_fail_call = 1u;
    }
    for (i = 0; i < 1025u; i++) {
        if (stmt_stream) {
            obs_trace_stmt_cb(SQLITE_TRACE_STMT, NULL, &stmt, (void *)source);
        } else {
            obs_log_rewrite_applied(
                "emby", "dashboard+episodes_latest", (sqlite3 *)&db,
                source, strlen(source), rewritten, strlen(rewritten)
            );
        }
    }
    free_fake_clientdata(&db);
    return 0;
}

struct creation_race_arg {
    struct fake_db *db;
    const char *source;
    const char *rewritten;
};

static void *connection_creation_race_thread(void *arg) {
    struct creation_race_arg *race = (struct creation_race_arg *)arg;
    obs_log_rewrite_applied(
        "emby", "dashboard+episodes_latest", (sqlite3 *)race->db,
        race->source, strlen(race->source),
        race->rewritten, strlen(race->rewritten)
    );
    return NULL;
}

static int child_connection_creation_race(void) {
    struct fake_db db = { .filename = "/tmp/connection-creation-race-smoke.db" };
    struct creation_race_arg args[2] = {
        {&db, "SELECT race_source_a", "SELECT race_rewritten_a"},
        {&db, "SELECT race_source_b", "SELECT race_rewritten_b"}
    };
    pthread_t threads[2];
    unsigned i;

    reset_fault_state();
    g_connection_creation_race = 1;
    for (i = 0; i < 2u; i++) {
        if (pthread_create(
                &threads[i], NULL, connection_creation_race_thread, &args[i]
            ) != 0) {
            failf("FATAL: connection creation race pthread_create failed");
        }
    }
    for (i = 0; i < 2u; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            failf("FATAL: connection creation race pthread_join failed");
        }
    }
    g_connection_creation_race = 0;
    if (g_connection_state_overwrites != 0u) {
        failf(
            "FATAL: connection creation overwrites=%u want=0",
            g_connection_state_overwrites
        );
    }
    free_fake_clientdata(&db);
    if (g_deferred_destroy && g_deferred_destroy_data) {
        g_deferred_destroy(g_deferred_destroy_data);
    }
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

int sqlite3_prepare_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    (void)db;
    (void)nByte;
    if (ppStmt) *ppStmt = NULL;
    if (pzTail) *pzTail = zSql ? zSql + strlen(zSql) : NULL;
    return SQLITE_OK;
}

int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
}

int sqlite3_prepare_v3_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    (void)prepFlags;
    return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
}

int plex_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    int kind
) {
    if (kind == 2) {
        return sqlite3_prepare_v3_real(db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (kind == 1) {
        return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
    }
    return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
}

int emby_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    int kind
) {
    if (g_prepare_capture_miss) {
        size_t scan_len = zSql ? (nByte < 0 ? strlen(zSql) : (size_t)nByte) : 0;
        uint64_t shape = 0;

        if (!obs_is_disabled()) shape = fts_lex_shape_key(zSql, scan_len);
        obs_log_rewrite_miss(
            "emby", "dashboard+episodes_latest", OBS_MISS_CAPTURE,
            "projection", db, zSql, scan_len, shape
        );
    }
    return plex_fts_rewrite_prepare(db, zSql, nByte, prepFlags, ppStmt, pzTail, kind);
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

char *sqlite3_expanded_sql(sqlite3_stmt *pStmt) {
    struct fake_stmt *fake = (struct fake_stmt *)pStmt;
    const char *sql = fake ? fake->sql : NULL;
    char *copy;
    size_t n;

    if (!sql) return NULL;
    n = strlen(sql) + 1;
    copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, sql, n);
    return copy;
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

void *sqlite3_malloc64(sqlite3_uint64 n) {
    int fail;

    pthread_mutex_lock(&g_fake_clientdata_mutex);
    g_sqlite_malloc_calls++;
    fail = g_sqlite_malloc_fail_call != 0u &&
        g_sqlite_malloc_calls == g_sqlite_malloc_fail_call;
    pthread_mutex_unlock(&g_fake_clientdata_mutex);
    if (fail) return NULL;
    if (n > (sqlite3_uint64)SIZE_MAX) return NULL;
    return malloc((size_t)n);
}

void *sqlite3_get_clientdata(sqlite3 *db, const char *key) {
    struct fake_db *fake = (struct fake_db *)db;
    void *data = NULL;
    unsigned i;
    if (!fake || !key) return NULL;
    pthread_mutex_lock(&g_fake_clientdata_mutex);
    for (i = 0; i < FAKE_CLIENTDATA_SLOTS; i++) {
        if (fake->clientdata[i].key &&
            strcmp(fake->clientdata[i].key, key) == 0) {
            data = fake->clientdata[i].data;
            break;
        }
    }
    pthread_mutex_unlock(&g_fake_clientdata_mutex);
    if (g_connection_creation_race && !data &&
        strcmp(key, "sqlite3-builds-observability") == 0) {
        pthread_mutex_lock(&g_creation_race_mutex);
        if (g_creation_race_gets < 2u) {
            g_creation_race_gets++;
            if (g_creation_race_gets == 2u) {
                pthread_cond_broadcast(&g_creation_race_cond);
            } else {
                while (g_creation_race_gets < 2u) {
                    pthread_cond_wait(
                        &g_creation_race_cond, &g_creation_race_mutex
                    );
                }
            }
        }
        pthread_mutex_unlock(&g_creation_race_mutex);
    }
    return data;
}

int sqlite3_set_clientdata(
    sqlite3 *db,
    const char *key,
    void *data,
    void (*xDestroy)(void *)
) {
    struct fake_db *fake = (struct fake_db *)db;
    struct fake_clientdata *slot = NULL;
    void *destroy_data = NULL;
    void (*destroy)(void *) = NULL;
    unsigned i;
    if (!fake || !key) return SQLITE_MISUSE;
    pthread_mutex_lock(&g_fake_clientdata_mutex);
    g_set_clientdata_calls++;
    if (g_set_clientdata_fail_call != 0u &&
        g_set_clientdata_calls == g_set_clientdata_fail_call) {
        pthread_mutex_unlock(&g_fake_clientdata_mutex);
        return SQLITE_NOMEM;
    }
    for (i = 0; i < FAKE_CLIENTDATA_SLOTS; i++) {
        if (fake->clientdata[i].key &&
            strcmp(fake->clientdata[i].key, key) == 0) {
            slot = &fake->clientdata[i];
            break;
        }
        if (!slot && !fake->clientdata[i].key) slot = &fake->clientdata[i];
    }
    if (!slot) {
        pthread_mutex_unlock(&g_fake_clientdata_mutex);
        return SQLITE_NOMEM;
    }
    if (slot->xDestroy && slot->data) {
        if (g_connection_creation_race &&
            strcmp(key, "sqlite3-builds-observability") == 0) {
            g_connection_state_overwrites++;
            g_deferred_destroy_data = slot->data;
            g_deferred_destroy = slot->xDestroy;
        } else {
            destroy_data = slot->data;
            destroy = slot->xDestroy;
        }
    }
    slot->key = key;
    slot->data = data;
    slot->xDestroy = xDestroy;
    pthread_mutex_unlock(&g_fake_clientdata_mutex);
    if (destroy && destroy_data) destroy(destroy_data);
    return SQLITE_OK;
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
    char *env_sampling_only[] = {
        "SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1",
        NULL
    };
    char *env_stmt_full[] = {
        "SQLITE3_DISABLE_STMT_TRACE=0",
        "SQLITE3_DISABLE_STMT_TRACE_SAMPLING=1",
        NULL
    };
    char *env_obs_disabled[] = {
        "SQLITE3_DISABLE_OBSERVABILITY=1",
        NULL
    };
    char *out;

    if (argc == 2 && strcmp(argv[1], "child-default") == 0) return child_default();
    if (argc == 2 && strcmp(argv[1], "child-stmt-enabled") == 0) return child_stmt_enabled();
    if (argc == 2 && strcmp(argv[1], "child-stmt-disabled") == 0) return child_stmt_disabled();
    if (argc == 2 && strcmp(argv[1], "child-stmt-other") == 0) return child_stmt_other();
    if (argc == 2 && strcmp(argv[1], "child-stmt-full") == 0) return child_stmt_full();
    if (argc == 2 && strcmp(argv[1], "child-capture-full") == 0) {
        return child_capture_miss(0);
    }
    if (argc == 2 && strcmp(argv[1], "child-capture-alloc-failure") == 0) {
        return child_capture_miss(1);
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-sampling") == 0) {
        return child_miss_sampling();
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-guid-dedup") == 0) {
        return child_miss_guid_dedup();
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-list-dedup") == 0) {
        return child_miss_list_dedup();
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-shape-cap") == 0) {
        return child_miss_shape_cap();
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-lexer-error") == 0) {
        return child_miss_lexer_error();
    }
    if (argc == 2 && strcmp(argv[1], "child-miss-unknown-mode") == 0) {
        return child_miss_unknown_mode();
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-sampling") == 0) {
        return child_applied_sampling();
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-corr-cap") == 0) {
        return child_applied_corr_cap();
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-corr-alloc-failure") == 0) {
        return child_applied_corr_failure(0);
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-corr-clientdata-failure") == 0) {
        return child_applied_corr_failure(1);
    }
    if (argc == 2 && strcmp(argv[1], "child-long-log-full") == 0) {
        return child_long_obs_log(0);
    }
    if (argc == 2 && strcmp(argv[1], "child-long-log-alloc-failure") == 0) {
        return child_long_obs_log(1);
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-state-alloc-failure") == 0) {
        return child_connection_state_failure(0, 0);
    }
    if (argc == 2 && strcmp(argv[1], "child-applied-state-clientdata-failure") == 0) {
        return child_connection_state_failure(0, 1);
    }
    if (argc == 2 && strcmp(argv[1], "child-stmt-state-alloc-failure") == 0) {
        return child_connection_state_failure(1, 0);
    }
    if (argc == 2 && strcmp(argv[1], "child-stmt-state-clientdata-failure") == 0) {
        return child_connection_state_failure(1, 1);
    }
    if (argc == 2 && strcmp(argv[1], "child-connection-creation-race") == 0) {
        return child_connection_creation_race();
    }

    out = run_child_capture(argv[0], "child-long-log-full", env_default);
    require_contains("long obs_logf source tail", out, "EXPANDED_END_SENTINEL\"");
    require_absent("long obs_logf truncation", out, "...[TRUNC]");
    free(out);

    out = run_child_capture(
        argv[0], "child-long-log-alloc-failure", env_default
    );
    require_contains("long obs_logf allocation fallback", out, "...[TRUNC]");
    require_absent(
        "long obs_logf allocation fallback tail", out, "EXPANDED_END_SENTINEL"
    );
    free(out);

    out = run_child_capture(argv[0], "child-capture-full", env_default);
    require_contains("capture full source tail", out, "CAPTURE_END_SENTINEL'");
    require_absent("capture full truncation", out, "...[TRUNC]");
    free(out);

    out = run_child_capture(argv[0], "child-capture-alloc-failure", env_default);
    require_contains("capture allocation fallback", out, "...[TRUNC]");
    require_absent("capture allocation fallback tail", out, "CAPTURE_END_SENTINEL'");
    free(out);

    out = run_child_capture(argv[0], "child-miss-sampling", env_default);
    require_occurrences(
        "miss sampling output", out,
        "event=rewrite_skipped target=emby reason=capture_miss", 3
    );
    require_contains("miss sampling first", out, "sample=first count=1");
    require_contains("miss sampling new", out, "sample=new count=2");
    require_same_line(
        "miss sampling new SQL", out, "sample=new count=2",
        "sql=\"SELECT miss_b, extra_b FROM source_b\""
    );
    require_absent("miss sampling repeated new", out, "sample=new count=3");
    require_absent("miss sampling first-shape re-emission", out, "sample=new count=4");
    require_contains("miss sampling periodic", out, "sample=periodic count=1024");
    require_absent("miss sampling suppressed tail", out, "count=1025");
    free(out);

    out = run_child_capture(argv[0], "child-miss-guid-dedup", env_default);
    require_occurrences(
        "miss GUID shape dedup", out,
        "event=rewrite_skipped target=emby reason=capture_miss", 1
    );
    require_contains(
        "miss GUID first exemplar", out,
        "plex://show/000000000000000000000000"
    );
    require_absent("miss GUID no novel literal shape", out, "sample=new");
    free(out);

    out = run_child_capture(argv[0], "child-miss-list-dedup", env_default);
    require_occurrences(
        "miss list cardinality dedup", out,
        "event=rewrite_skipped target=emby reason=capture_miss", 1
    );
    require_contains("miss list first exemplar", out, "id IN (1,2)");
    require_absent("miss list cardinality not novel", out, "id IN (1,2,3)");
    free(out);

    out = run_child_capture(argv[0], "child-miss-shape-cap", env_default);
    require_occurrences(
        "miss shape cap output", out,
        "event=rewrite_skipped target=emby reason=capture_miss", 513
    );
    require_contains("miss shape cap last inserted", out, "column_511");
    require_absent("miss shape cap overflow suppressed", out, "column_512");
    require_contains("miss shape cap periodic", out, "sample=periodic count=1024");
    free(out);

    out = run_child_capture(argv[0], "child-miss-lexer-error", env_default);
    require_occurrences(
        "miss lexer error fallback", out,
        "event=rewrite_skipped target=emby reason=capture_miss", 1
    );
    require_contains("miss lexer error first", out, "unterminated_a");
    require_absent("miss lexer error fewer records", out, "unterminated_b");
    require_contains("miss lexer error shape unavailable", out, "shape=0000000000000000");
    free(out);

    out = run_child_capture(argv[0], "child-miss-guid-dedup", env_obs_disabled);
    require_absent("miss master disable", out, "event=rewrite_skipped");
    free(out);

    out = run_child_capture(argv[0], "child-miss-unknown-mode", env_default);
    require_absent("miss unknown mode suppresses", out, "event=rewrite_skipped");
    free(out);

    out = run_child_capture(argv[0], "child-default", env_default);
    require_absent("default trace output", out, " trace_stmt ");
    require_absent("default trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("default slow_query output", out, " slow_query ");
    require_contains("default slow_query output", out, "sql=\"SELECT trace_stmt_smoke\"");
    free(out);

    out = run_child_capture(argv[0], "child-default", env_sampling_only);
    require_absent("sampling-only trace output", out, "event=SQLITE_TRACE_STMT");
    require_contains("sampling-only slow_query output", out, " slow_query ");
    free(out);

    out = run_child_capture(argv[0], "child-stmt-enabled", env_stmt_enabled);
    require_occurrences("stmt-enabled trace output", out, "event=SQLITE_TRACE_STMT", 3);
    require_contains("stmt-enabled first sample", out, "sample=first count=1");
    require_contains("stmt-enabled new sample", out, "sample=new count=2");
    require_same_line(
        "stmt-enabled new SQL", out,
        "event=SQLITE_TRACE_STMT", "sql=\"SELECT trace_stmt_distinct\""
    );
    require_absent("stmt-enabled repeated new SQL", out, "sample=new count=3");
    require_same_line(
        "stmt-enabled periodic sample", out,
        "event=SQLITE_TRACE_STMT", "sample=periodic count=1024"
    );
    require_absent("stmt-enabled suppressed tail", out, "count=1025");
    require_contains("stmt-enabled correlation", out, "sql_len=23 corr=");
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

    out = run_child_capture(argv[0], "child-stmt-full", env_stmt_full);
    require_occurrences("stmt-full trace output", out, "event=SQLITE_TRACE_STMT", 1025);
    require_contains("stmt-full first count", out, "sample=full count=1");
    require_contains("stmt-full final count", out, "sample=full count=1025");
    free(out);

    out = run_child_capture(argv[0], "child-applied-sampling", env_default);
    require_occurrences("applied sampling output", out, "event=rewrite_applied", 3);
    require_contains("applied first sample", out, "sample=first count=1");
    require_contains("applied new sample", out, "sample=new count=2");
    require_contains("applied new source", out, "source_sql=\"SELECT source_b\"");
    require_contains("applied new rewritten", out, "sql=\"SELECT rewritten_b\"");
    require_absent("applied repeated corr", out, "sample=new count=3");
    require_contains("applied periodic sample", out, "sample=periodic count=1024");
    require_contains("applied periodic source", out, "source_sql=\"SELECT source_a\"");
    require_contains("applied periodic rewritten", out, "sql=\"SELECT rewritten_a\"");
    free(out);

    out = run_child_capture(argv[0], "child-applied-corr-cap", env_default);
    require_occurrences("applied corr cap output", out, "event=rewrite_applied", 513);
    require_contains("applied corr cap last inserted", out, "sample=new count=512");
    require_absent("applied corr cap fallback", out, "SELECT rewritten_512");
    require_contains("applied corr cap periodic", out, "sample=periodic count=1024");
    free(out);

    out = run_child_capture(
        argv[0], "child-applied-corr-alloc-failure", env_default
    );
    require_occurrences("applied corr alloc fallback", out, "event=rewrite_applied", 2);
    require_contains("applied corr alloc first", out, "sample=first count=1");
    require_contains("applied corr alloc periodic", out, "sample=periodic count=1024");
    free(out);

    out = run_child_capture(
        argv[0], "child-applied-corr-clientdata-failure", env_default
    );
    require_occurrences("applied corr clientdata fallback", out, "event=rewrite_applied", 2);
    require_contains("applied corr clientdata first", out, "sample=first count=1");
    require_contains(
        "applied corr clientdata periodic", out, "sample=periodic count=1024"
    );
    free(out);

    out = run_child_capture(
        argv[0], "child-applied-state-alloc-failure", env_default
    );
    require_occurrences(
        "applied initial state alloc fallback", out, "event=rewrite_applied", 2
    );
    require_contains(
        "applied initial state alloc first", out, "sample=first count=1"
    );
    require_contains(
        "applied initial state alloc periodic", out, "sample=periodic count=1024"
    );
    free(out);

    out = run_child_capture(
        argv[0], "child-applied-state-clientdata-failure", env_default
    );
    require_occurrences(
        "applied initial state clientdata fallback", out,
        "event=rewrite_applied", 2
    );
    require_contains(
        "applied initial state clientdata first", out, "sample=first count=1"
    );
    require_contains(
        "applied initial state clientdata periodic", out,
        "sample=periodic count=1024"
    );
    free(out);

    out = run_child_capture(
        argv[0], "child-stmt-state-alloc-failure", env_default
    );
    require_occurrences(
        "STMT initial state alloc fallback", out, "event=SQLITE_TRACE_STMT", 2
    );
    require_contains(
        "STMT initial state alloc first", out, "sample=first count=1"
    );
    require_contains(
        "STMT initial state alloc periodic", out, "sample=periodic count=1024"
    );
    free(out);

    out = run_child_capture(
        argv[0], "child-stmt-state-clientdata-failure", env_default
    );
    require_occurrences(
        "STMT initial state clientdata fallback", out,
        "event=SQLITE_TRACE_STMT", 2
    );
    require_contains(
        "STMT initial state clientdata first", out, "sample=first count=1"
    );
    require_contains(
        "STMT initial state clientdata periodic", out,
        "sample=periodic count=1024"
    );
    free(out);

    out = run_child_capture(
        argv[0], "child-connection-creation-race", env_default
    );
    require_absent("connection creation race", out, "FATAL:");
    free(out);

    printf("stmt trace smoke passed\n");
    return 0;
}
