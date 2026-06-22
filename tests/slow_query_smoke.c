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
void slow_query_test_dump(void);
uint32_t slow_query_test_entries_used(void);
uint32_t slow_query_test_max_entries(void);
uint32_t slow_query_test_count_for_sql(const char *sql);
uint64_t slow_query_test_parse_threshold_ns(const char *value);
void slow_query_test_disable_atexit_dump(void);

void obs_logf(const char *fn, const char *fmt, ...);

int obs_is_disabled(void) {
    const char *v = getenv("SQLITE3_DISABLE_OBSERVABILITY");
    return v && strcmp(v, "1") == 0;
}

int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    (void)ctx;
    (void)p;
    (void)x;
    if (trace == SQLITE_TRACE_STMT) {
        obs_logf("trace_stmt", "event=SQLITE_TRACE_STMT sql=\"SELECT ?1\"");
    }
    return 0;
}

void obs_logf(const char *fn, const char *fmt, ...) {
    va_list ap;
    if (obs_is_disabled()) return;
    fprintf(stderr, "[sqlite3-builds-obs] test 0 0 %s", fn);
    if (fmt && fmt[0]) {
        fputc(' ', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
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
    char long_sql[1400];
    memset(long_sql, 'A', sizeof(long_sql) - 1);
    long_sql[sizeof(long_sql) - 1] = 0;

    slow_query_test_record_sql("SELECT ?1", 1000000000LL);
    slow_query_test_record_sql(long_sql, 1000000000LL);
    slow_query_test_dump();
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
    char *env_hierarchy[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=0", "SQLITE3_DISABLE_OBSERVABILITY=1", ld_lib_path, NULL };
    char *env_threshold[] = { "SQLITE3_SLOW_QUERY_THRESHOLD_MS=100", ld_lib_path, NULL };
    char *out;

    if (argc == 2 && strcmp(argv[1], "child-positive") == 0) return child_positive();
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
        failf("FATAL: 1024-byte cap/truncation marker missing: %s", out);
    }
    printf("%s", out);
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

    out = run_child_capture(argv[0], "child-hierarchy", env_hierarchy);
    if (strstr(out, "[sqlite3-builds-obs]")) {
        failf("FATAL: observability hierarchy emitted output: %s", out);
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
