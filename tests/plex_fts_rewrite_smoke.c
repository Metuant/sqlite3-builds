#include "sqlite3.h"

#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

static const char *MATCH_SQL_INT =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_INT_REWRITTEN =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_LEAN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6";
static const char *MATCH_SQL_LEAN_REWRITTEN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)";
static const char *MATCH_SQL_QUOTED =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type='6' and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_PARAM =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=? and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *PROJECTED_TAG_TYPE_SQL =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 order by tag_type, tags.id";
static const char *PROJECTED_TAG_TYPE_SQL_REWRITTEN =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6) order by tag_type, tags.id";
static const char *BOUNDARY_PLUS_INT_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6+1 order by tags.id";
static const char *BOUNDARY_PLUS_PARAM_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=?+1 order by tags.id";
static const char *BOUNDARY_HEX_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=0x1f order by tags.id";
static const char *NONMATCH_SQL = "select 1";
static const char *NO_FTS_SQL =
    "select tags.id from tags where tag_type=6";
static const char *NO_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tags.id=10";
static const char *DUPLICATE_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 and tag_type=1";
static const char *CROSS_SCOPE_CTE_SQL =
    "with matched as (select rowid from fts4_tag_titles_icu where fts4_tag_titles_icu.tag match 'Django*') select tags.id from tags where tag_type=6 and tags.id in (select rowid from matched)";
static const char *CROSS_SCOPE_SUBQUERY_SQL =
    "select tags.id from tags where tag_type=6 and exists (select 1 from fts4_tag_titles_icu where fts4_tag_titles_icu.rowid=tags.id and fts4_tag_titles_icu.tag match 'Django*')";
static const char *PROJECTION_ONLY_TAG_TYPE_EQ_SQL =
    "select (tag_type=6) from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*'";
static const char *ORDER_ONLY_TAG_TYPE_EQ_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' order by tag_type=6";

typedef struct auth_probe {
    int unlikely_calls;
    int deny_unlikely;
} auth_probe;

typedef struct digest_result {
    int rows;
    uint64_t hash;
} digest_result;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void require_int(const char *label, int got, int want) {
    if (got != want) failf("FAIL [%s]: got=%d want=%d", label, got, want);
}

static void require_str_eq(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        failf("FAIL [%s]: got=\"%s\" want=\"%s\"", label, got ? got : "(null)", want);
    }
}

static void require_contains(const char *label, const char *got, const char *needle) {
    if (!got || !strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" missing=\"%s\"", label, got ? got : "(null)", needle);
    }
}

static int safe_setenv(const char *name, const char *value) {
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "setenv(%s=%s) failed: %s\n", name, value, strerror(errno));
        return 0;
    }
    return 1;
}

static int safe_unsetenv(const char *name) {
    if (unsetenv(name) != 0) {
        fprintf(stderr, "unsetenv(%s) failed: %s\n", name, strerror(errno));
        return 0;
    }
    return 1;
}

static void configure_env(const char *rewrite_value) {
    if (!safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_OBSERVABILITY", "1")) exit(1);
    if (rewrite_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", rewrite_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE")) exit(1);
    }
}

static void temp_path(char *buf, size_t n, const char *basename) {
    int rc = snprintf(buf, n, "/tmp/plex-fts-rewrite-smoke-%ld/%s", (long)getpid(), basename);
    if (rc < 0 || (size_t)rc >= n) failf("FATAL: temp path too long for %s", basename);
}

static void make_temp_dir(void) {
    char dir[256];
    int rc = snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(dir)) failf("FATAL: temp dir too long");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        failf("FATAL: mkdir(%s) failed: %s", dir, strerror(errno));
    }
}

static void cleanup_temp_dir(void) {
    char dir[256];
    char path[512];
    const char *names[] = {
        "com.plexapp.plugins.library.db", "library.db", "jellyfin.db", "not-target.db"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        temp_path(path, sizeof(path), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), names[i]);
        unlink(path);
    }
    snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    rmdir(dir);
}

static void exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: sqlite3_exec rc=%d err=%s", label, rc, err ? err : sqlite3_errmsg(db));
    }
    sqlite3_free(err);
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [open]: path=%s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    }
    return db;
}

static void setup_schema(sqlite3 *db) {
    exec_sql(db, "schema",
        "CREATE TABLE metadata_items(id INTEGER PRIMARY KEY, library_section_id INTEGER NOT NULL, metadata_type INTEGER NOT NULL);"
        "CREATE TABLE tags(id INTEGER PRIMARY KEY, tag_type INTEGER NOT NULL);"
        "CREATE TABLE taggings(metadata_item_id INTEGER NOT NULL, tag_id INTEGER NOT NULL);"
        "CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);"
        "INSERT INTO metadata_items VALUES(1,1,1),(2,1,1),(3,1,1),(4,2,1),(5,1,1),(6,1,1);"
        "INSERT INTO tags VALUES(10,6),(11,6),(12,1),(13,6),(14,7),(15,31);"
        "INSERT INTO taggings VALUES(1,10),(2,11),(3,12),(4,13),(5,14),(6,15);"
        "INSERT INTO fts4_tag_titles_icu(rowid, tag) VALUES"
        "(10,'Django Alpha'),(11,'Django Beta'),(12,'Django Other'),(13,'Django Section Two'),"
        "(14,'Django Seven'),(15,'Django Hex');"
    );
}

static int authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    auth_probe *probe = (auth_probe *)ctx;
    (void)db;
    (void)trigger;
    if (action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "unlikely") == 0) || (p2 && strcmp(p2, "unlikely") == 0))) {
        probe->unlikely_calls++;
        if (probe->deny_unlikely) return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static sqlite3_stmt *prepare_entry(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (entry == 3) {
        rc = sqlite3_prepare_v3(db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, &stmt, tail);
    } else if (entry == 2) {
        rc = sqlite3_prepare_v2(db, sql, nbyte, &stmt, tail);
    } else {
        rc = sqlite3_prepare(db, sql, nbyte, &stmt, tail);
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=%d rc=%d err=%s", label, entry, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static void expect_saved_sql(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char *want_sql
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, entry, &tail);
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_saved_sql_contains(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *needle
) {
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, NULL);
    require_contains(label, sqlite3_sql(stmt), needle);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_legacy_authorizer(sqlite3 *db, int expect_rewrite) {
    auth_probe probe = {0, 0};
    sqlite3_stmt *stmt = NULL;

    require_int("legacy-authorizer/set", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    stmt = prepare_entry(db, "legacy-authorizer", MATCH_SQL_INT, -1, 1, NULL);
    require_int("legacy-authorizer/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("legacy-authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite && probe.unlikely_calls < 1) {
        failf("FAIL [legacy-authorizer]: expected unlikely authorizer call, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite && probe.unlikely_calls != 0) {
        failf("FAIL [legacy-authorizer]: unexpected unlikely authorizer calls=%d", probe.unlikely_calls);
    }
}

static void expect_tail_null_ok(sqlite3 *db, const char *label, const char *want_sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [%s]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_clean_original_single_id(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int bind_param,
    int bind_value,
    int want_id
) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;
    int got_id;

    require_int("clean-original/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    require_int("clean-original/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (rc != SQLITE_OK) failf("FAIL [%s]: prepare rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (probe.unlikely_calls != 0) {
        failf("FAIL [%s]: rewrite attempt count=%d want=0", label, probe.unlikely_calls);
    }
    if (bind_param) {
        require_int("clean-original/bind", sqlite3_bind_int(stmt, 1, bind_value), SQLITE_OK);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        failf("FAIL [%s]: first step rc=%d want=SQLITE_ROW id=%d err=%s",
              label, rc, want_id, sqlite3_errmsg(db));
    }
    got_id = sqlite3_column_int(stmt, 0);
    if (got_id != want_id) failf("FAIL [%s]: got_id=%d want_id=%d", label, got_id, want_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) failf("FAIL [%s]: second step rc=%d want=SQLITE_DONE", label, rc);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static uint64_t fnv1a_u64(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (unsigned char)(v & 0xffu);
        h *= 1099511628211ULL;
        v >>= 8;
    }
    return h;
}

static digest_result digest_grouped(sqlite3 *db, const char *sql, int bind_mode) {
    sqlite3_stmt *stmt = NULL;
    digest_result result = {0, 1469598103934665603ULL};
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [digest/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, "Django*", -1, SQLITE_STATIC);
    if (bind_mode == 0) sqlite3_bind_int(stmt, 2, 6);
    else if (bind_mode == 1) sqlite3_bind_text(stmt, 2, "6", -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, 1);
    sqlite3_bind_int(stmt, 4, 1);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.rows++;
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 0));
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 1));
    }
    if (rc != SQLITE_DONE) failf("FAIL [digest/step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_int("digest/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return result;
}

static void expect_grouped_digest_identity(sqlite3 *db) {
    static const char *original =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and tag_type=? and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    static const char *rewritten =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and unlikely(tag_type=?) and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    int mode;
    for (mode = 0; mode < 3; mode++) {
        digest_result a = digest_grouped(db, original, mode);
        digest_result b = digest_grouped(db, rewritten, mode);
        if (a.rows != b.rows || a.hash != b.hash) {
            failf("FAIL [grouped-digest mode=%d]: original rows=%d hash=%llu rewritten rows=%d hash=%llu",
                  mode, a.rows, (unsigned long long)a.hash, b.rows, (unsigned long long)b.hash);
        }
    }
}

static void expect_fail_open(sqlite3 *db, int expect_rewrite_attempt) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;

    require_int("fail-open/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [fail-open]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_str_eq("fail-open/sql", sqlite3_sql(stmt), MATCH_SQL_INT);
    require_int("fail-open/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("fail-open/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite_attempt && probe.unlikely_calls < 1) {
        failf("FAIL [fail-open]: expected denied unlikely attempt, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite_attempt && probe.unlikely_calls != 0) {
        failf("FAIL [fail-open]: unexpected denied unlikely attempts=%d", probe.unlikely_calls);
    }
}

static sqlite3 *open_seeded_temp(const char *basename) {
    char path[512];
    temp_path(path, sizeof(path), basename);
    unlink(path);
    sqlite3 *db = open_db(path);
    setup_schema(db);
    return db;
}

static int child_positive(void) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env("0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, "v2-int", MATCH_SQL_INT, -1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "v3-int", MATCH_SQL_INT, -1, 3,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "lean-no-metadata-group-limit", MATCH_SQL_LEAN, -1, 2,
                     expect_rewrite ? MATCH_SQL_LEAN_REWRITTEN : MATCH_SQL_LEAN);
    expect_saved_sql(db, "projected-tag-type", PROJECTED_TAG_TYPE_SQL, -1, 2,
                     expect_rewrite ? PROJECTED_TAG_TYPE_SQL_REWRITTEN : PROJECTED_TAG_TYPE_SQL);
    expect_saved_sql(db, "nbyte-positive-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT) + 1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "nbyte-positive-no-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT), 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_tail_null_ok(db, "pztail-null", expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    if (expect_rewrite) {
        expect_saved_sql_contains(db, "quoted", MATCH_SQL_QUOTED, "unlikely(tag_type='6')");
        expect_saved_sql_contains(db, "param", MATCH_SQL_PARAM, "unlikely(tag_type=?)");
    } else {
        expect_saved_sql(db, "quoted", MATCH_SQL_QUOTED, -1, 2, MATCH_SQL_QUOTED);
        expect_saved_sql(db, "param", MATCH_SQL_PARAM, -1, 2, MATCH_SQL_PARAM);
    }
    expect_legacy_authorizer(db, expect_rewrite);
    expect_grouped_digest_identity(db);
    exec_sql(db, "exec-miss", "PRAGMA user_version;");
    require_int("positive/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [positive]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_env_off(const char *value, const char *label) {
    sqlite3 *db;
    configure_env(value);
    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, label, MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
    expect_legacy_authorizer(db, 0);
    require_int("env-off/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_path_negative(void) {
    const char *names[] = {"library.db", "jellyfin.db", "not-target.db"};
    size_t i;
    configure_env("0");
    make_temp_dir();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3 *db = open_seeded_temp(names[i]);
        expect_saved_sql(db, names[i], MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/close", sqlite3_close(db), SQLITE_OK);
    }
    {
        sqlite3 *db = open_db(":memory:");
        setup_schema(db);
        expect_saved_sql(db, "memory", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/memory-close", sqlite3_close(db), SQLITE_OK);
    }
    cleanup_temp_dir();
    printf("PASS [path-negative]\n");
    return 0;
}

static int child_nonmatch(void) {
    sqlite3 *db;
    configure_env("0");
    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, "nonmatch-select", NONMATCH_SQL, -1, 2, NONMATCH_SQL);
    expect_saved_sql(db, "no-fts-table", NO_FTS_SQL, -1, 2, NO_FTS_SQL);
    expect_saved_sql(db, "no-target-column", NO_TARGET_SQL, -1, 2, NO_TARGET_SQL);
    expect_saved_sql(db, "duplicate-target-column", DUPLICATE_TARGET_SQL, -1, 2,
                     DUPLICATE_TARGET_SQL);
    expect_saved_sql(db, "cross-scope-cte", CROSS_SCOPE_CTE_SQL, -1, 2, CROSS_SCOPE_CTE_SQL);
    expect_saved_sql(db, "cross-scope-subquery", CROSS_SCOPE_SUBQUERY_SQL, -1, 2,
                     CROSS_SCOPE_SUBQUERY_SQL);
    expect_saved_sql(db, "projection-only-target-column", PROJECTION_ONLY_TAG_TYPE_EQ_SQL, -1,
                     2, PROJECTION_ONLY_TAG_TYPE_EQ_SQL);
    expect_saved_sql(db, "order-only-target-column", ORDER_ONLY_TAG_TYPE_EQ_SQL, -1, 2,
                     ORDER_ONLY_TAG_TYPE_EQ_SQL);
    expect_clean_original_single_id(db, "boundary-plus-int", BOUNDARY_PLUS_INT_SQL, 0, 0, 14);
    expect_clean_original_single_id(db, "boundary-plus-param", BOUNDARY_PLUS_PARAM_SQL, 1, 6, 14);
    expect_clean_original_single_id(db, "boundary-hex", BOUNDARY_HEX_SQL, 0, 0, 15);
    expect_fail_open(db, sqlite3_compileoption_used("ENABLE_ICU") != 0);
    require_int("nonmatch/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [nonmatch]\n");
    return 0;
}

static void run_child(const char *name) {
    pid_t pid = fork();
    int status;
    if (pid < 0) failf("FATAL: fork(%s) failed: %s", name, strerror(errno));
    if (pid == 0) {
        if (strcmp(name, "positive") == 0) _exit(child_positive());
        if (strcmp(name, "env-unset") == 0) _exit(child_env_off(NULL, "env-unset"));
        if (strcmp(name, "env-one") == 0) _exit(child_env_off("1", "env-one"));
        if (strcmp(name, "env-garbage") == 0) _exit(child_env_off("false", "env-garbage"));
        if (strcmp(name, "path-negative") == 0) _exit(child_path_negative());
        if (strcmp(name, "nonmatch") == 0) _exit(child_nonmatch());
        failf("FATAL: unknown child %s", name);
    }
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid(%s) failed: %s", name, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d", name, status);
    }
}

int main(void) {
    run_child("positive");
    run_child("env-unset");
    run_child("env-one");
    run_child("env-garbage");
    run_child("path-negative");
    run_child("nonmatch");
    printf("plex fts rewrite smoke passed\n");
    return 0;
}
