#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long run_single_mode(const char *mode) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int i;
    long long start;
    long long elapsed;

    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "FATAL: sqlite3_open failed: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    if (sqlite3_prepare_v2(db, "SELECT 1", -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "FATAL: prepare failed: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    start = mono_ns();
    for (i = 0; i < 100000; i++) {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "FATAL: step[%d] rc=%d\n", i, rc);
            exit(1);
        }
        sqlite3_reset(stmt);
    }
    elapsed = mono_ns() - start;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (strcmp(mode, "disabled") == 0) {
        printf("mode=disabled elapsed_ns=%lld\n", elapsed);
    } else {
        printf("mode=enabled elapsed_ns=%lld\n", elapsed);
    }
    return elapsed;
}

static char *read_all_fd(int fd) {
    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "FATAL: malloc failed\n");
        exit(1);
    }
    for (;;) {
        ssize_t n;
        if (len + 512 + 1 > cap) {
            char *next;
            cap *= 2;
            next = realloc(buf, cap);
            if (!next) {
                fprintf(stderr, "FATAL: realloc failed\n");
                exit(1);
            }
            buf = next;
        }
        n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "FATAL: read failed errno=%d\n", errno);
            exit(1);
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    return buf;
}

static long long parse_elapsed(const char *mode, const char *out) {
    const char *p = strstr(out, "elapsed_ns=");
    char *end = NULL;
    long long elapsed;
    if (!p) {
        fprintf(stderr, "FATAL: child %s missing elapsed_ns: %s\n", mode, out);
        exit(1);
    }
    p += strlen("elapsed_ns=");
    elapsed = strtoll(p, &end, 10);
    if (end == p || elapsed < 0) {
        fprintf(stderr, "FATAL: child %s invalid elapsed_ns: %s\n", mode, out);
        exit(1);
    }
    return elapsed;
}

static long long run_child_mode(const char *self, const char *mode) {
    int pipefd[2];
    pid_t pid;
    int status;
    char *out;
    long long elapsed;
    char *const argv_child[] = { (char *)self, NULL };

    if (pipe(pipefd) != 0) {
        fprintf(stderr, "FATAL: pipe failed errno=%d\n", errno);
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "FATAL: fork failed errno=%d\n", errno);
        exit(1);
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        setenv("SLOW_QUERY_BENCH_MODE", mode, 1);
        if (strcmp(mode, "disabled") == 0) {
            setenv("SQLITE3_DISABLE_SLOW_QUERY", "1", 1);
        } else {
            unsetenv("SQLITE3_DISABLE_SLOW_QUERY");
        }
        execv(self, argv_child);
        _exit(127);
    }
    close(pipefd[1]);
    out = read_all_fd(pipefd[0]);
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "FATAL: waitpid failed errno=%d\n", errno);
        exit(1);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FATAL: child %s failed status=%d output=%s\n", mode, status, out);
        exit(1);
    }
    fputs(out, stdout);
    elapsed = parse_elapsed(mode, out);
    free(out);
    return elapsed;
}

int main(int argc, char **argv) {
    const char *mode;
    long long disabled_ns;
    long long enabled_ns;

    (void)argc;
    mode = getenv("SLOW_QUERY_BENCH_MODE");
    if (mode && mode[0]) {
        if (strcmp(mode, "disabled") != 0 && strcmp(mode, "enabled") != 0) {
            fprintf(stderr, "FATAL: unknown mode=%s\n", mode);
            return 1;
        }
        run_single_mode(mode);
        return 0;
    }

    disabled_ns = run_child_mode(argv[0], "disabled");
    enabled_ns = run_child_mode(argv[0], "enabled");
    printf("disabled_ns=%lld enabled_ns=%lld delta_ns=%lld\n",
           disabled_ns, enabled_ns, enabled_ns - disabled_ns);
    return 0;
}
