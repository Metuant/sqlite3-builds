#include "sqlite3.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int slow_query_test_record_sql(const char *sql, sqlite3_int64 elapsed_ns);
int slow_query_test_record_sql_for_db(
    const char *db_filename, const char *sql, sqlite3_int64 elapsed_ns, int with_stmt
);
void slow_query_test_dump(void);
uint32_t slow_query_test_entries_used(void);
uint32_t slow_query_test_max_entries(void);
uint32_t slow_query_test_sql_cap(void);
uint32_t slow_query_test_expanded_sql_cap(void);
uint32_t slow_query_test_count_for_sql(const char *sql);
uint64_t slow_query_test_parse_threshold_ns(const char *value);
uint64_t slow_query_test_parse_expanded_threshold_ns(
    const char *slow_value, const char *expanded_value
);
int slow_query_test_expanded_sql_enabled_value(const char *value);
int slow_query_test_path_is_target(const char *path);
void slow_query_test_set_expanded_sql_provider(char *(*provider)(sqlite3_stmt *));
void slow_query_test_reset_expanded_sql_provider(void);
uint32_t slow_query_test_expanded_sql_free_count(void);
uint32_t slow_query_test_expanded_sql_call_count(void);
void slow_query_test_force_detail_alloc_failure(int enabled);
void slow_query_test_disable_atexit_dump(void);

void obs_logf(const char *fn, const char *fmt, ...);

int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    (void)ctx;
    (void)p;
    (void)x;
    if (trace == SQLITE_TRACE_STMT) {
        obs_logf("trace_stmt", "event=SQLITE_TRACE_STMT sql=\"SELECT ?1\"");
    }
    return 0;
}

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void assert_u64(const char *label, uint64_t got, uint64_t want) {
    if (got != want) {
        failf("FATAL: %s got=%" PRIu64 " want=%" PRIu64, label, got, want);
    }
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

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;
    size_t n = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += n;
    }
    return count;
}

static uint64_t test_fnv1a_prefix(const char *sql, size_t cap, uint32_t *len_out) {
    const unsigned char *p = (const unsigned char *)(sql ? sql : "");
    uint64_t h = 1469598103934665603ull;
    uint32_t n = 0;

    while (*p && n < cap) {
        h ^= (uint64_t)*p++;
        h *= 1099511628211ull;
        n++;
    }
    *len_out = n;
    return h;
}

static void require_expanded_hash(
    const char *label, const char *out, const char *sql, size_t cap
) {
    uint32_t len;
    uint64_t hash = test_fnv1a_prefix(sql, cap, &len);
    char field[96];
    snprintf(field, sizeof(field), "sql_expanded_len=%" PRIu32, len);
    require_contains(label, out, field);
    snprintf(field, sizeof(field), "sql_expanded_hash=%016" PRIx64, hash);
    require_contains(label, out, field);
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

static int child_positive(void) {
    char long_sql[2600];
    memset(long_sql, 'A', sizeof(long_sql) - 1);
    long_sql[sizeof(long_sql) - 1] = 0;

    slow_query_test_record_sql("SELECT ?1", 1000000000LL);
    slow_query_test_record_sql(long_sql, 1000000000LL);
    slow_query_test_dump();
    return 0;
}

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1u;
    char *p = malloc(n);
    if (!p) failf("FATAL: malloc failed");
    memcpy(p, s, n);
    return p;
}

static char *provider_search(sqlite3_stmt *stmt) {
    (void)stmt;
    return dup_cstr(
        "SELECT * FROM MediaItems WHERE SearchTerm = 'quoted search' "
        "AND RawTerm = 'unquoted' AND note = '"
        "\xC3" "\xA9"
        " %s%n event=forged [sqlite3-builds-obs]'"
    );
}

static char *provider_controls(sqlite3_stmt *stmt) {
    (void)stmt;
    return dup_cstr(
        "SELECT 'valid "
        "\xC3" "\xA9"
        " controls "
        "\x01" "\x7F" "\x85"
        " quote \" slash \\ cr \r lf \n tab \t %s%n "
        "event=forged [sqlite3-builds-obs]'"
    );
}

static char *provider_null(sqlite3_stmt *stmt) {
    (void)stmt;
    return NULL;
}

static char *provider_overcap(sqlite3_stmt *stmt) {
    uint32_t cap = slow_query_test_expanded_sql_cap();
    char *p;
    (void)stmt;
    p = malloc((size_t)cap + 32u);
    if (!p) failf("FATAL: malloc failed");
    memset(p, 'Z', (size_t)cap + 20u);
    p[(size_t)cap + 20u] = 0;
    return p;
}

static char *provider_secret(sqlite3_stmt *stmt) {
    (void)stmt;
    return dup_cstr("SELECT 'SECRET_EXPANDED_VALUE'");
}

static int child_expanded_success(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT * FROM MediaItems WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 1u) {
        failf("FATAL: expanded provider calls=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_call_count());
    }
    if (slow_query_test_expanded_sql_free_count() != 1u) {
        failf("FATAL: expanded free count=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_free_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_default_on(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT default_on WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 1u) {
        failf("FATAL: default-on expanded provider calls=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_disabled(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT disabled WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 0u) {
        failf("FATAL: disabled expanded provider calls=%" PRIu32 " want=0",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_below_threshold(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT below_threshold WHERE SearchTerm = @SearchTerm",
        2499000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 0u) {
        failf("FATAL: below-threshold expanded provider calls=%" PRIu32 " want=0",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_equal_threshold(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT equal_threshold WHERE SearchTerm = @SearchTerm",
        2500000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 1u) {
        failf("FATAL: equal-threshold expanded provider calls=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_clamp(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT clamp_below WHERE SearchTerm = @SearchTerm",
        999000000LL,
        1
    );
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT clamp_equal WHERE SearchTerm = @SearchTerm",
        1000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 1u) {
        failf("FATAL: clamp expanded provider calls=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_non_target(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/tmp/not-media.db",
        "SELECT non_target WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 0u) {
        failf("FATAL: non-target expanded provider calls=%" PRIu32 " want=0",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_targets(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/Library/Application Support/Plex Media Server/Plug-in Support/Databases/com.plexapp.plugins.library.db",
        "SELECT plex_target WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT emby_target WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    slow_query_test_record_sql_for_db(
        "/config/data/jellyfin.db",
        "SELECT jf_target WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 3u) {
        failf("FATAL: target expanded provider calls=%" PRIu32 " want=3",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_template_fallback(void) {
    slow_query_test_set_expanded_sql_provider(provider_null);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT fallback WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_call_count() != 1u) {
        failf("FATAL: fallback expanded provider calls=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_call_count());
    }
    if (slow_query_test_expanded_sql_free_count() != 0u) {
        failf("FATAL: fallback free count=%" PRIu32 " want=0",
              slow_query_test_expanded_sql_free_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_no_stmt_fallback(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT no_stmt WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        0
    );
    if (slow_query_test_expanded_sql_call_count() != 0u) {
        failf("FATAL: no-stmt expanded provider calls=%" PRIu32 " want=0",
              slow_query_test_expanded_sql_call_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_overcap(void) {
    slow_query_test_set_expanded_sql_provider(provider_overcap);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT overcap WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    if (slow_query_test_expanded_sql_free_count() != 1u) {
        failf("FATAL: overcap free count=%" PRIu32 " want=1",
              slow_query_test_expanded_sql_free_count());
    }
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_escaping(void) {
    slow_query_test_set_expanded_sql_provider(provider_controls);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT escaping WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_alloc_failure(void) {
    slow_query_test_set_expanded_sql_provider(provider_search);
    slow_query_test_force_detail_alloc_failure(1);
    slow_query_test_record_sql_for_db(
        "/config/data/library.db",
        "SELECT alloc_failure WHERE SearchTerm = @SearchTerm",
        3000000000LL,
        1
    );
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_expanded_not_retained(void) {
    int i;
    slow_query_test_set_expanded_sql_provider(provider_secret);
    for (i = 0; i < 5; i++) {
        slow_query_test_record_sql_for_db(
            "/config/data/library.db",
            "SELECT retained_template WHERE SearchTerm = @SearchTerm",
            3000000000LL,
            1
        );
    }
    slow_query_test_dump();
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_kill(void) {
    obs_trace_stmt_cb(SQLITE_TRACE_STMT, NULL, NULL, NULL);
    slow_query_test_record_sql("SELECT ?1", 1000000000LL);
    slow_query_test_dump();
    return 0;
}

static int child_fast_shape(void) {
    int i;
    for (i = 0; i < 5; i++) {
        slow_query_test_record_sql("SELECT fast_shape", 50000000LL);
    }
    slow_query_test_dump();
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_slow_low_count(void) {
    int i;
    for (i = 0; i < 4; i++) {
        slow_query_test_record_sql("SELECT slow_low_count", 200000000LL);
    }
    slow_query_test_dump();
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_single_slow_event(void) {
    slow_query_test_record_sql("SELECT single_slow_event", 200000000LL);
    slow_query_test_dump();
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_hierarchy(void) {
    slow_query_test_record_sql("SELECT ?1", 1000000000LL);
    slow_query_test_dump();
    return 0;
}

static int child_stats_frequent(void) {
    int i;
    for (i = 0; i < 5; i++) {
        slow_query_test_record_sql("SELECT stats_frequent", 1000000000LL);
    }
    slow_query_test_dump();
    slow_query_test_disable_atexit_dump();
    return 0;
}

static int child_accumulator(void) {
    int i;
    for (i = 0; i < 20; i++) slow_query_test_record_sql("SELECT accumulator_guard", 1000000000LL);
    slow_query_test_dump();
    return 0;
}

static void test_parser_cases(void) {
    assert_u64("parser unset/default", slow_query_test_parse_threshold_ns(NULL), 500000000ULL);
    assert_u64("parser empty/default", slow_query_test_parse_threshold_ns(""), 500000000ULL);
    assert_u64("parser zero", slow_query_test_parse_threshold_ns("0"), 0ULL);
    assert_u64("parser leading sign/default", slow_query_test_parse_threshold_ns("-1"), 500000000ULL);
    assert_u64("parser nondigit/default", slow_query_test_parse_threshold_ns("abc"), 500000000ULL);
    assert_u64("parser trailing junk/default", slow_query_test_parse_threshold_ns("12ms"), 500000000ULL);
    assert_u64("parser uint64max-overflow-guard/default", slow_query_test_parse_threshold_ns("18446744073709551615"), 500000000ULL);
    assert_u64("parser erange/default", slow_query_test_parse_threshold_ns("18446744073709551616"), 500000000ULL);
    assert_u64("parser overflow/default", slow_query_test_parse_threshold_ns("18446744073710"), 500000000ULL);

    if (slow_query_test_expanded_sql_enabled_value(NULL) != 1) failf("FATAL: expanded flag unset got=0 want=1");
    if (slow_query_test_expanded_sql_enabled_value("") != 1) failf("FATAL: expanded flag empty got=0 want=1");
    if (slow_query_test_expanded_sql_enabled_value("0") != 1) failf("FATAL: expanded flag 0 got=0 want=1");
    if (slow_query_test_expanded_sql_enabled_value("1") != 0) failf("FATAL: expanded flag 1 got=1 want=0");
    if (slow_query_test_expanded_sql_enabled_value("false") != 1) failf("FATAL: expanded flag false got=0 want=1");
    if (slow_query_test_expanded_sql_enabled_value("yes") != 1) failf("FATAL: expanded flag yes got=0 want=1");
    if (slow_query_test_expanded_sql_enabled_value("junk") != 1) failf("FATAL: expanded flag junk got=0 want=1");

    assert_u64("expanded parser default", slow_query_test_parse_expanded_threshold_ns(NULL, NULL), 2500000000ULL);
    assert_u64("expanded parser explicit", slow_query_test_parse_expanded_threshold_ns("500", "1000"), 1000000000ULL);
    assert_u64("expanded parser zero clamp", slow_query_test_parse_expanded_threshold_ns("500", "0"), 500000000ULL);
    assert_u64("expanded parser below slow clamp", slow_query_test_parse_expanded_threshold_ns("1000", "500"), 1000000000ULL);
    assert_u64("expanded parser invalid default", slow_query_test_parse_expanded_threshold_ns("500", "abc"), 2500000000ULL);
    assert_u64("expanded parser invalid default clamp", slow_query_test_parse_expanded_threshold_ns("3000", "abc"), 3000000000ULL);

    if (slow_query_test_sql_cap() != 2048u) {
        failf("FATAL: SLOW_QUERY_SQL_CAP got=%" PRIu32 " want=2048", slow_query_test_sql_cap());
    }
    if (slow_query_test_expanded_sql_cap() != 65536u) {
        failf("FATAL: SLOW_QUERY_EXPANDED_SQL_CAP got=%" PRIu32 " want=65536",
              slow_query_test_expanded_sql_cap());
    }
    if (slow_query_test_max_entries() != 4096u) {
        failf("FATAL: SLOW_QUERY_MAX_ENTRIES got=%" PRIu32 " want=4096",
              slow_query_test_max_entries());
    }

    if (!slow_query_test_path_is_target("/config/data/library.db")) {
        failf("FATAL: target helper missed Emby library.db");
    }
    if (!slow_query_test_path_is_target("/config/data/jellyfin.db?immutable=1")) {
        failf("FATAL: target helper missed legacy JF path with query suffix");
    }
    if (!slow_query_test_path_is_target("/config/com.plexapp.plugins.library.db")) {
        failf("FATAL: target helper missed Plex path");
    }
    if (slow_query_test_path_is_target("/tmp/not-media.db")) {
        failf("FATAL: target helper matched non-target path");
    }
}

static void test_lru_and_accumulator(void) {
    char sql[64];
    uint32_t max_entries = slow_query_test_max_entries();
    uint32_t i;
    for (i = 0; i < max_entries + 6u; i++) {
        snprintf(sql, sizeof(sql), "SELECT %" PRIu32, i);
        slow_query_test_record_sql(sql, 1000);
    }
    if (slow_query_test_entries_used() > max_entries) {
        failf("FATAL: entries_used=%" PRIu32 " want<=%" PRIu32,
              slow_query_test_entries_used(), max_entries);
    }
    slow_query_test_record_sql("SELECT 0", 2000);
    slow_query_test_dump();
    if (slow_query_test_count_for_sql("SELECT 0") != 1u) {
        failf("FATAL: SELECT 0 was not evicted and reallocated with count=1");
    }
}

int main(int argc, char **argv) {
    /* WHY: Dockerfile invokes this smoke under LD_LIBRARY_PATH=$LIB_DIR so
     * the freshly-built libsqlite3.so resolves. execve() below replaces the
     * child env wholesale, so we must propagate LD_LIBRARY_PATH explicitly
     * or the child fails in the dynamic loader before main(). */
    static char ld_lib_path[2048];
    const char *parent_ld = getenv("LD_LIBRARY_PATH");
    snprintf(ld_lib_path, sizeof(ld_lib_path), "LD_LIBRARY_PATH=%s",
             parent_ld ? parent_ld : "");

    char *env_positive[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0", ld_lib_path, NULL };
    char *env_kill[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0", "SQLITE3_DISABLE_SLOW_QUERY=1", ld_lib_path, NULL };
    char *env_kill_expanded[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0",
        "SQLITE3_DISABLE_SLOW_QUERY=1",
        "SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=0",
        ld_lib_path,
        NULL
    };
    char *env_hierarchy[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0", "SQLITE3_DISABLE_OBSERVABILITY=1", ld_lib_path, NULL };
    char *env_hierarchy_expanded[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0",
        "SQLITE3_DISABLE_OBSERVABILITY=1",
        "SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=0",
        ld_lib_path,
        NULL
    };
    char *env_threshold[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=100", ld_lib_path, NULL };
    char *env_expanded[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=500",
        "SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=0",
        ld_lib_path,
        NULL
    };
    char *env_expanded_disabled[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=100",
        "SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=1",
        ld_lib_path,
        NULL
    };
    char *env_expanded_threshold[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=100",
        "SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS=2500",
        ld_lib_path,
        NULL
    };
    char *env_expanded_clamp[] = {
        "SQLITE3_SLOW_QUERY_THRESHOLD_MS=1000",
        "SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS=500",
        ld_lib_path,
        NULL
    };
    char *out;
    int expanded_detail_count;
    int forged_physical_count;

    if (argc == 2 && strcmp(argv[1], "child-positive") == 0) return child_positive();
    if (argc == 2 && strcmp(argv[1], "child-expanded-success") == 0) return child_expanded_success();
    if (argc == 2 && strcmp(argv[1], "child-expanded-default-on") == 0) return child_expanded_default_on();
    if (argc == 2 && strcmp(argv[1], "child-expanded-disabled") == 0) return child_expanded_disabled();
    if (argc == 2 && strcmp(argv[1], "child-expanded-below-threshold") == 0) return child_expanded_below_threshold();
    if (argc == 2 && strcmp(argv[1], "child-expanded-equal-threshold") == 0) return child_expanded_equal_threshold();
    if (argc == 2 && strcmp(argv[1], "child-expanded-clamp") == 0) return child_expanded_clamp();
    if (argc == 2 && strcmp(argv[1], "child-expanded-non-target") == 0) return child_expanded_non_target();
    if (argc == 2 && strcmp(argv[1], "child-expanded-targets") == 0) return child_expanded_targets();
    if (argc == 2 && strcmp(argv[1], "child-expanded-template-fallback") == 0) return child_expanded_template_fallback();
    if (argc == 2 && strcmp(argv[1], "child-expanded-no-stmt-fallback") == 0) return child_expanded_no_stmt_fallback();
    if (argc == 2 && strcmp(argv[1], "child-expanded-overcap") == 0) return child_expanded_overcap();
    if (argc == 2 && strcmp(argv[1], "child-expanded-escaping") == 0) return child_expanded_escaping();
    if (argc == 2 && strcmp(argv[1], "child-expanded-alloc-failure") == 0) return child_expanded_alloc_failure();
    if (argc == 2 && strcmp(argv[1], "child-expanded-not-retained") == 0) return child_expanded_not_retained();
    if (argc == 2 && strcmp(argv[1], "child-kill") == 0) return child_kill();
    if (argc == 2 && strcmp(argv[1], "child-fast-shape") == 0) return child_fast_shape();
    if (argc == 2 && strcmp(argv[1], "child-slow-low-count") == 0) return child_slow_low_count();
    if (argc == 2 && strcmp(argv[1], "child-single-slow-event") == 0) return child_single_slow_event();
    if (argc == 2 && strcmp(argv[1], "child-hierarchy") == 0) return child_hierarchy();
    if (argc == 2 && strcmp(argv[1], "child-stats-frequent") == 0) return child_stats_frequent();
    if (argc == 2 && strcmp(argv[1], "child-accumulator") == 0) return child_accumulator();

    test_parser_cases();

    out = run_child_capture(argv[0], "child-positive", env_positive);
    if (!strstr(out, "slow_query ") || !strstr(out, "sql=\"SELECT ?1\"")) {
        failf("FATAL: positive trigger missing slow_query parameterized SQL: %s", out);
    }
    if (!strstr(out, "...[TRUNC]")) {
        failf("FATAL: 2048-byte cap/truncation marker missing: %s", out);
    }
    printf("%s", out);
    free(out);

    out = run_child_capture(argv[0], "child-expanded-default-on", env_threshold);
    require_contains("expanded default-on base line", out, " slow_query ");
    require_contains("expanded default-on detail line", out, " slow_query_expanded ");
    require_contains("expanded default-on provider value", out, "quoted search");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-disabled", env_expanded_disabled);
    require_contains("expanded disabled base line", out, " slow_query ");
    require_absent("expanded disabled detail line", out, " slow_query_expanded ");
    require_absent("expanded disabled provider value", out, "quoted search");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-success", env_expanded);
    require_contains("expanded success base line", out, " slow_query ");
    require_contains("expanded success detail line", out, " slow_query_expanded ");
    require_contains("expanded success source", out, "sql_expanded_source=expanded");
    require_contains("expanded success unquoted bind", out, "unquoted");
    require_contains("expanded success quoted bind", out, "quoted search");
    require_contains("expanded success valid utf8", out, "\xC3" "\xA9");
    require_contains("expanded success format literal", out, "%s%n");
    require_contains("expanded success trunc flag", out, "sql_expanded_truncated=0");
    require_absent("expanded flag does not enable STMT trace", out, " trace_stmt ");
    require_expanded_hash(
        "expanded success hash",
        out,
        "SELECT * FROM MediaItems WHERE SearchTerm = 'quoted search' "
        "AND RawTerm = 'unquoted' AND note = '"
        "\xC3" "\xA9"
        " %s%n event=forged [sqlite3-builds-obs]'",
        slow_query_test_expanded_sql_cap()
    );
    free(out);

    out = run_child_capture(argv[0], "child-expanded-below-threshold", env_expanded_threshold);
    require_contains("expanded below-threshold base line", out, " slow_query ");
    require_absent("expanded below-threshold detail line", out, " slow_query_expanded ");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-equal-threshold", env_expanded_threshold);
    require_contains("expanded equal-threshold detail line", out, " slow_query_expanded ");
    require_contains("expanded equal-threshold source", out, "sql_expanded_source=expanded");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-clamp", env_expanded_clamp);
    require_absent("expanded clamp below-slow detail", out, "SELECT clamp_below");
    require_contains("expanded clamp equal detail", out, "SELECT clamp_equal");
    require_contains("expanded clamp detail source", out, "sql_expanded_source=expanded");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-non-target", env_expanded);
    require_contains("expanded non-target base line", out, " slow_query ");
    require_absent("expanded non-target detail line", out, " slow_query_expanded ");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-targets", env_expanded);
    if (count_occurrences(out, " slow_query_expanded ") != 3) {
        failf("FATAL: target detail lines got=%d want=3 output=%s",
              count_occurrences(out, " slow_query_expanded "), out);
    }
    require_contains("expanded Plex target", out, "SELECT plex_target");
    require_contains("expanded Emby target", out, "SELECT emby_target");
    require_contains("expanded legacy JF target", out, "SELECT jf_target");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-template-fallback", env_expanded);
    require_contains("expanded NULL fallback detail line", out, " slow_query_expanded ");
    require_contains("expanded NULL fallback source", out, "sql_expanded_source=template");
    require_contains("expanded NULL fallback template", out, "SELECT fallback WHERE SearchTerm = @SearchTerm");
    require_expanded_hash(
        "expanded NULL fallback hash",
        out,
        "SELECT fallback WHERE SearchTerm = @SearchTerm",
        slow_query_test_expanded_sql_cap()
    );
    free(out);

    out = run_child_capture(argv[0], "child-expanded-no-stmt-fallback", env_expanded);
    require_contains("expanded no-stmt fallback detail line", out, " slow_query_expanded ");
    require_contains("expanded no-stmt fallback source", out, "sql_expanded_source=template");
    require_contains("expanded no-stmt fallback template", out, "SELECT no_stmt WHERE SearchTerm = @SearchTerm");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-overcap", env_expanded);
    require_contains("expanded overcap detail line", out, " slow_query_expanded ");
    require_contains("expanded overcap source", out, "sql_expanded_source=expanded");
    require_contains("expanded overcap len", out, "sql_expanded_len=65536");
    require_contains("expanded overcap trunc flag", out, "sql_expanded_truncated=1");
    require_contains("expanded overcap tail", out, "...[TRUNC]");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-escaping", env_expanded);
    require_contains("expanded escaping valid utf8", out, "\xC3" "\xA9");
    require_contains("expanded escaping C0", out, "\\x01");
    require_contains("expanded escaping DEL", out, "\\x7F");
    require_contains("expanded escaping C1", out, "\\x85");
    require_contains("expanded escaping quote", out, "\\\"");
    require_contains("expanded escaping backslash", out, "\\\\");
    require_contains("expanded escaping CR", out, "\\r");
    require_contains("expanded escaping LF", out, "\\n");
    require_contains("expanded escaping TAB", out, "\\t");
    expanded_detail_count = count_occurrences(out, " slow_query_expanded ");
    forged_physical_count =
        count_occurrences(out, "\n tab \\t %s%n event=forged [sqlite3-builds-obs]") +
        count_occurrences(out, "\n tab \t %s%n event=forged [sqlite3-builds-obs]");
    if (expanded_detail_count != 1 || forged_physical_count != 0) {
        failf("FATAL: expanded escaping forged physical log line counts got detail=%d forged=%d want detail=1 forged=0 output=%s",
              expanded_detail_count, forged_physical_count, out);
    }
    free(out);

    out = run_child_capture(argv[0], "child-expanded-alloc-failure", env_expanded);
    require_contains("expanded alloc failure base line", out, " slow_query ");
    require_contains("expanded alloc failure detail status", out, "sql_expanded_source=alloc_failed");
    require_absent("expanded alloc failure no sensitive detail", out, "quoted search");
    require_absent("expanded alloc failure no sql_expanded field", out, "sql_expanded=\"");
    free(out);

    out = run_child_capture(argv[0], "child-expanded-not-retained", env_expanded);
    require_contains("expanded not retained detail", out, "SECRET_EXPANDED_VALUE");
    require_contains("expanded not retained stats template", out, "slow_query_stats sql=\"SELECT retained_template WHERE SearchTerm = @SearchTerm\"");
    require_absent("expanded not retained stats secret", out, "slow_query_stats sql=\"SELECT 'SECRET_EXPANDED_VALUE");
    free(out);

    out = run_child_capture(argv[0], "child-fast-shape", env_threshold);
    require_absent("fast shape per-statement slow_query", out, " slow_query ");
    require_absent("fast shape stats", out, "slow_query_stats");
    free(out);

    out = run_child_capture(argv[0], "child-slow-low-count", env_threshold);
    require_contains("slow low-count per-statement slow_query", out, " slow_query ");
    require_absent("slow low-count stats", out, "slow_query_stats");
    free(out);

    out = run_child_capture(argv[0], "child-single-slow-event", env_threshold);
    require_contains("single slow execution per-statement slow_query", out, " slow_query ");
    require_contains("single slow execution SQL", out, "sql=\"SELECT single_slow_event\"");
    require_absent("single slow execution stats", out, "slow_query_stats");
    free(out);

    out = run_child_capture(argv[0], "child-kill", env_kill);
    if (strstr(out, "slow_query")) failf("FATAL: slow-query kill switch emitted output: %s", out);
    free(out);

    out = run_child_capture(argv[0], "child-kill", env_kill_expanded);
    if (strstr(out, "slow_query")) {
        failf("FATAL: slow-query kill switch with expanded flag emitted output: %s", out);
    }
    free(out);

    out = run_child_capture(argv[0], "child-hierarchy", env_hierarchy);
    if (strstr(out, "[sqlite3-builds-obs]")) {
        failf("FATAL: observability hierarchy emitted output: %s", out);
    }
    free(out);

    out = run_child_capture(argv[0], "child-hierarchy", env_hierarchy_expanded);
    if (strstr(out, "[sqlite3-builds-obs]")) {
        failf("FATAL: observability hierarchy with expanded flag emitted output: %s", out);
    }
    free(out);

    out = run_child_capture(argv[0], "child-stats-frequent", env_threshold);
    require_contains("frequent slow stats", out, "slow_query_stats");
    require_contains("frequent slow stats count", out, "count=5");
    require_contains("frequent slow stats mean precision", out, "mean_ms=1000.000000");
    require_contains("frequent slow stats stddev precision", out, "stddev_ms=0.000000");
    require_contains("frequent slow stats min precision", out, "min_ms=1000.000000");
    require_contains("frequent slow stats max precision", out, "max_ms=1000.000000");
    if (strstr(out, "stddev_ms=-") || strstr(out, "nan") || strstr(out, "inf")) {
        failf("FATAL: stats dump has invalid stddev: %s", out);
    }
    free(out);

    out = run_child_capture(argv[0], "child-accumulator", env_positive);
    if (!strstr(out, "count=20") || strstr(out, "stddev_ms=-") ||
        strstr(out, "nan") || strstr(out, "inf")) {
        failf("FATAL: accumulator guard failed: %s", out);
    }
    free(out);

    test_lru_and_accumulator();
    slow_query_test_disable_atexit_dump();
    printf("slow query smoke passed\n");
    return 0;
}
