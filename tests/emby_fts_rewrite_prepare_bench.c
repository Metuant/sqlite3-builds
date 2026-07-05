#include "sqlite3.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 4
#define ITERATIONS_PER_THREAD 300
#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
#define ADVISORY_LOW_PREPARE_P50_NS 30000ULL
#define ADVISORY_LOW_PREPARE_P95_NS 150000ULL

static const char *MATCH_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select count(*) OVER() AS TotalRecordCount,A.type,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where "
    "A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) "
    "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) "
    "AND A.ExtraType is null AND "
    "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (100)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) "
    "Group by A.Type ORDER BY Rank ASC LIMIT 50";

static const char *LARGE_MISS_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
    "select count(*) OVER() AS TotalRecordCount,A.type,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where "
    "A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) "
    "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) "
    "AND A.ExtraType is null AND "
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid=A.Id)) "
    "Group by A.Type ORDER BY Rank ASC LIMIT 50";

typedef enum bench_work {
    WORK_PREPARE_SELECT,
    WORK_PREPARE_LARGE_MISS,
    WORK_PREPARE_MATCH,
    WORK_EXEC_PRAGMA
} bench_work;

typedef enum bench_assertion {
    ASSERT_NONE = 0,
    ASSERT_ADVISORY_LOW_PREPARE_COST = 1
} bench_assertion;

typedef struct bench_thread {
    const char *db_path;
    bench_work work;
    uint64_t *samples;
    int sample_count;
} bench_thread;

typedef struct bench_stats {
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    uint64_t max;
} bench_stats;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) failf("clock_gettime failed: %s", strerror(errno));
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static void configure_env(const char *fts, const char *fanout, const char *dashboard) {
    if (setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1", 1) != 0) failf("setenv AUTOPRAGMA failed");
    if (setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1", 1) != 0) failf("setenv RUNTIME failed");
    if (setenv("SQLITE3_DISABLE_OBSERVABILITY", "1", 1) != 0) failf("setenv OBS failed");
    if (setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1", 1) != 0) failf("setenv PLEX failed");
    if (fts) {
        if (setenv("SQLITE3_DISABLE_EMBY_FTS_REWRITE", fts, 1) != 0) {
            failf("setenv EMBY FTS failed");
        }
    } else if (unsetenv("SQLITE3_DISABLE_EMBY_FTS_REWRITE") != 0) {
        failf("unsetenv EMBY FTS failed");
    }
    if (fanout) {
        if (setenv("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE", fanout, 1) != 0) {
            failf("setenv EMBY FANOUT failed");
        }
    } else if (unsetenv("SQLITE3_DISABLE_EMBY_FANOUT_REWRITE") != 0) {
        failf("unsetenv EMBY FANOUT failed");
    }
    if (dashboard) {
        if (setenv("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", dashboard, 1) != 0) {
            failf("setenv EMBY DASHBOARD failed");
        }
    } else if (unsetenv("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE") != 0) {
        failf("unsetenv EMBY DASHBOARD failed");
    }
}

static void temp_path(char *buf, size_t n, const char *basename) {
    int rc = snprintf(buf, n, "/tmp/emby-fts-rewrite-bench-%ld/%s", (long)getpid(), basename);
    if (rc < 0 || (size_t)rc >= n) failf("temp path too long");
}

static void make_temp_dir(void) {
    char dir[256];
    int rc = snprintf(dir, sizeof(dir), "/tmp/emby-fts-rewrite-bench-%ld", (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(dir)) failf("temp dir too long");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        failf("mkdir(%s) failed: %s", dir, strerror(errno));
    }
}

static void cleanup_temp_dir(void) {
    char dir[256];
    char path[512];
    const char *names[] = {"library.db", "com.plexapp.plugins.library.db"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        temp_path(path, sizeof(path), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/emby-fts-rewrite-bench-%ld/%s-wal", (long)getpid(), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/emby-fts-rewrite-bench-%ld/%s-shm", (long)getpid(), names[i]);
        unlink(path);
    }
    snprintf(dir, sizeof(dir), "/tmp/emby-fts-rewrite-bench-%ld", (long)getpid());
    rmdir(dir);
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) failf("open %s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    return db;
}

static void exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) failf("%s rc=%d err=%s", label, rc, err ? err : sqlite3_errmsg(db));
    sqlite3_free(err);
}

static void setup_schema(const char *path) {
    sqlite3 *db = open_db(path);
    exec_sql(db, "schema",
        "CREATE TABLE mediaitems(Id INTEGER PRIMARY KEY, Type INTEGER, UserDataKeyId INTEGER, IsPublic INTEGER, ExtraType INTEGER);"
        "CREATE TABLE AncestorIds2(itemid INTEGER, AncestorId INTEGER, Distance INTEGER);"
        "CREATE TABLE ItemLinks2(ItemId INTEGER, LinkedId INTEGER, Type INTEGER);"
        "CREATE TABLE itemPeople2(ItemId INTEGER, PersonId INTEGER);"
        "CREATE TABLE ListItems(ListId INTEGER, ListItemId INTEGER);"
        "CREATE TABLE UserItemShares(UserId INTEGER, ItemId INTEGER, ShareLevel INTEGER);"
        "CREATE VIRTUAL TABLE fts_search9 USING fts5(title);"
        "INSERT INTO mediaitems VALUES(1,1,1,1,NULL);"
        "INSERT INTO AncestorIds2 VALUES(1,100,0);"
        "INSERT INTO fts_search9(rowid,title) VALUES(1,'alpha');"
    );
    if (sqlite3_close(db) != SQLITE_OK) failf("schema close failed");
}

static void *bench_worker(void *arg) {
    bench_thread *bt = (bench_thread *)arg;
    sqlite3 *db = open_db(bt->db_path);
    int i;
    for (i = 0; i < bt->sample_count; i++) {
        uint64_t start = now_ns();
        if (bt->work == WORK_EXEC_PRAGMA) {
            exec_sql(db, "exec pragma", "PRAGMA user_version;");
        } else {
            sqlite3_stmt *stmt = NULL;
            const char *sql = "SELECT 1";
            if (bt->work == WORK_PREPARE_MATCH) sql = MATCH_SQL;
            else if (bt->work == WORK_PREPARE_LARGE_MISS) sql = LARGE_MISS_SQL;
            int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            if (rc != SQLITE_OK) failf("prepare rc=%d err=%s", rc, sqlite3_errmsg(db));
            rc = sqlite3_finalize(stmt);
            if (rc != SQLITE_OK) failf("finalize rc=%d err=%s", rc, sqlite3_errmsg(db));
        }
        bt->samples[i] = now_ns() - start;
    }
    if (sqlite3_close(db) != SQLITE_OK) failf("worker close failed");
    return NULL;
}

static bench_stats run_parallel(const char *db_path, bench_work work, uint64_t *storage) {
    pthread_t threads[THREADS];
    bench_thread args[THREADS];
    int i;
    int total = THREADS * ITERATIONS_PER_THREAD;
    bench_stats stats;

    for (i = 0; i < THREADS; i++) {
        args[i].db_path = db_path;
        args[i].work = work;
        args[i].samples = storage + (i * ITERATIONS_PER_THREAD);
        args[i].sample_count = ITERATIONS_PER_THREAD;
        if (pthread_create(&threads[i], NULL, bench_worker, &args[i]) != 0) {
            failf("pthread_create failed");
        }
    }
    for (i = 0; i < THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) failf("pthread_join failed");
    }
    qsort(storage, (size_t)total, sizeof(storage[0]), cmp_u64);
    stats.p50 = storage[(total * 50) / 100];
    stats.p95 = storage[(total * 95) / 100];
    stats.p99 = storage[(total * 99) / 100];
    stats.max = storage[total - 1];
    return stats;
}

static void print_stats(const char *label, bench_stats baseline, bench_stats target) {
    long long delta = (long long)target.p50 - (long long)baseline.p50;
    printf("BENCH [%s]: threads=%d samples=%d baseline_p50_ns=%llu target_p50_ns=%llu delta_p50_ns=%lld target_p95_ns=%llu target_p99_ns=%llu target_max_ns=%llu\n",
           label,
           THREADS,
           THREADS * ITERATIONS_PER_THREAD,
           (unsigned long long)baseline.p50,
           (unsigned long long)target.p50,
           delta,
           (unsigned long long)target.p95,
           (unsigned long long)target.p99,
           (unsigned long long)target.max);
}

static void report_low_prepare_cost(const char *label, bench_stats target) {
    if (target.p50 > ADVISORY_LOW_PREPARE_P50_NS ||
        target.p95 > ADVISORY_LOW_PREPARE_P95_NS) {
        printf("BENCH ADVISORY FAIL [%s]: target_p50_ns=%llu limit=%llu target_p95_ns=%llu limit=%llu\n",
               label,
               (unsigned long long)target.p50,
               (unsigned long long)ADVISORY_LOW_PREPARE_P50_NS,
               (unsigned long long)target.p95,
               (unsigned long long)ADVISORY_LOW_PREPARE_P95_NS);
    } else {
        printf("BENCH ADVISORY PASS [%s]: target_p50_ns=%llu limit=%llu target_p95_ns=%llu limit=%llu\n",
               label,
               (unsigned long long)target.p50,
               (unsigned long long)ADVISORY_LOW_PREPARE_P50_NS,
               (unsigned long long)target.p95,
               (unsigned long long)ADVISORY_LOW_PREPARE_P95_NS);
    }
}

static int child_case(
    const char *label,
    const char *fts,
    const char *fanout,
    const char *dashboard,
    const char *basename,
    bench_work work,
    int seed,
    bench_assertion assertion
) {
    char path[512];
    uint64_t baseline_samples[THREADS * ITERATIONS_PER_THREAD];
    uint64_t target_samples[THREADS * ITERATIONS_PER_THREAD];
    bench_stats baseline;
    bench_stats target;

    configure_env(fts, fanout, dashboard);
    make_temp_dir();
    temp_path(path, sizeof(path), basename);
    unlink(path);
    if (seed) setup_schema(path);
    else {
        sqlite3 *db = open_db(path);
        if (sqlite3_close(db) != SQLITE_OK) failf("empty close failed");
    }
    baseline = run_parallel(path, WORK_PREPARE_SELECT, baseline_samples);
    target = run_parallel(path, work, target_samples);
    print_stats(label, baseline, target);
    fflush(stdout);
    if (assertion == ASSERT_ADVISORY_LOW_PREPARE_COST) report_low_prepare_cost(label, target);
    fflush(stdout);
    cleanup_temp_dir();
    return 0;
}

static void run_child(
    const char *label,
    const char *fts,
    const char *fanout,
    const char *dashboard,
    const char *basename,
    bench_work work,
    int seed,
    bench_assertion assertion
) {
    pid_t pid = fork();
    int status;
    if (pid < 0) failf("fork(%s) failed: %s", label, strerror(errno));
    if (pid == 0) _exit(child_case(label, fts, fanout, dashboard, basename, work, seed, assertion));
    if (waitpid(pid, &status, 0) < 0) failf("waitpid(%s) failed: %s", label, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failf("child %s failed status=%d", label, status);
}

int main(void) {
    run_child("default-on-select", NULL, NULL, NULL, "library.db", WORK_PREPARE_SELECT, 0, ASSERT_NONE);
    run_child("enabled-nontarget-select", "0", NULL, NULL, "com.plexapp.plugins.library.db",
              WORK_PREPARE_SELECT, 0, ASSERT_ADVISORY_LOW_PREPARE_COST);
    run_child("enabled-emby-miss", "0", NULL, NULL, "library.db", WORK_PREPARE_SELECT, 1,
              ASSERT_ADVISORY_LOW_PREPARE_COST);
    run_child("enabled-emby-large-miss", "0", NULL, NULL, "library.db", WORK_PREPARE_LARGE_MISS, 1,
              ASSERT_ADVISORY_LOW_PREPARE_COST);
    run_child("enabled-all-large-miss", "0", "0", "0", "library.db", WORK_PREPARE_LARGE_MISS, 1,
              ASSERT_ADVISORY_LOW_PREPARE_COST);
    run_child("enabled-emby-match", "0", NULL, NULL, "library.db", WORK_PREPARE_MATCH, 1, ASSERT_NONE);
    run_child("enabled-emby-exec-miss", "0", NULL, NULL, "library.db", WORK_EXEC_PRAGMA, 1, ASSERT_NONE);
    printf("emby fts rewrite prepare bench completed\n");
    return 0;
}
