#include "rewrite_smoke_harness.h"
#include "sqlite3.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
#include "emby_fts_rewrite.h"
#include "observability.h"

enum direct_fault_mode {
    DIRECT_FAULT_NONE = 0,
    DIRECT_FAULT_CANDIDATE_ERROR,
    DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT,
    DIRECT_FAULT_CANDIDATE_WRONG_TAIL,
    DIRECT_FAULT_MIXED_BIND_MISMATCH,
    DIRECT_FAULT_MIXED_COLUMN_MISMATCH,
    DIRECT_FAULT_MIXED_INDEX_MISMATCH
};

static const char *direct_original_sql;
static enum direct_fault_mode direct_fault;
static int direct_candidate_calls;
static int direct_original_calls;
static size_t direct_candidate_input_len;
static int direct_candidate_had_stmt;
static int direct_candidate_bind_count;
static int direct_candidate_column_count;
static int direct_candidate_outer_index_count;
static int direct_candidate_inner_index_count;
static int direct_skipped_calls;
static const char *direct_skipped_reason;
static obs_rewrite_mode direct_skipped_mode;

static int direct_count_bytes(const char *text, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *p = text;

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
}

__attribute__((visibility("hidden"))) int plex_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
) {
    int candidate = direct_original_sql && zSql != direct_original_sql;
    const char *prepare_sql = zSql;
    int prepare_nbyte = nByte;
    int rc;
    if (candidate) {
        direct_candidate_calls++;
        direct_candidate_input_len = strlen(zSql);
        direct_candidate_outer_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        direct_candidate_inner_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_gk_dc"
        );
        if (direct_fault == DIRECT_FAULT_CANDIDATE_ERROR) {
            *ppStmt = NULL;
            if (pzTail) *pzTail = zSql;
            return SQLITE_ERROR;
        }
        if (direct_fault == DIRECT_FAULT_MIXED_BIND_MISMATCH) {
            prepare_sql = "SELECT ?";
            prepare_nbyte = -1;
        } else if (direct_fault == DIRECT_FAULT_MIXED_COLUMN_MISMATCH) {
            prepare_sql = "SELECT 1";
            prepare_nbyte = -1;
        }
    } else if (direct_original_sql && zSql == direct_original_sql) {
        direct_original_calls++;
    }
    if (kind == FTS_REWRITE_PREPARE_V3) {
        rc = sqlite3_prepare_v3(db, prepare_sql, prepare_nbyte, prepFlags, ppStmt, pzTail);
    } else if (kind == FTS_REWRITE_PREPARE_V2) {
        rc = sqlite3_prepare_v2(db, prepare_sql, prepare_nbyte, ppStmt, pzTail);
    } else {
        (void)prepFlags;
        rc = sqlite3_prepare(db, prepare_sql, prepare_nbyte, ppStmt, pzTail);
    }
    if (candidate && prepare_sql != zSql && rc == SQLITE_OK && pzTail) {
        *pzTail = zSql + strlen(zSql);
    }
    if (candidate && direct_fault == DIRECT_FAULT_MIXED_INDEX_MISMATCH &&
        rc == SQLITE_OK) {
        char *index_name = strstr(
            (char *)zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        if (index_name) index_name[0] = 'X';
        direct_candidate_outer_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_dcn_gk"
        );
        direct_candidate_inner_index_count = direct_count_bytes(
            zSql, "idx_dshadow_emby_latest_mixed_gk_dc"
        );
    }
    if (candidate) {
        direct_candidate_had_stmt = ppStmt && *ppStmt != NULL;
        if (direct_candidate_had_stmt) {
            direct_candidate_bind_count = sqlite3_bind_parameter_count(*ppStmt);
            direct_candidate_column_count = sqlite3_column_count(*ppStmt);
        }
    }
    if (candidate && direct_fault == DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT &&
        rc == SQLITE_OK) {
        return SQLITE_ERROR;
    }
    if (candidate && direct_fault == DIRECT_FAULT_CANDIDATE_WRONG_TAIL &&
        rc == SQLITE_OK && pzTail) {
        *pzTail = zSql;
    }
    return rc;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...) {
    (void)fn;
    (void)fmt;
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void) {
    return 0;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_escape_sql(
    const char *src,
    char *dst,
    size_t dst_n
) {
    size_t i;
    if (dst_n == 0) return;
    for (i = 0; i + 1 < dst_n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_miss(
    obs_rewrite_mode mode,
    obs_miss_reason reason,
    const char *sub_reason,
    sqlite3 *db,
    const char *sql,
    size_t len,
    uint64_t shape
) {
    (void)mode;
    (void)reason;
    (void)sub_reason;
    (void)db;
    (void)sql;
    (void)len;
    (void)shape;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_index_missing(
    sqlite3 *db,
    obs_rewrite_mode mode
) {
    (void)db;
    (void)mode;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_applied(
    obs_rewrite_mode mode,
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len
) {
    (void)mode;
    (void)db;
    (void)source;
    (void)source_len;
    (void)rewritten;
    (void)rewritten_len;
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_skipped(
    sqlite3 *db,
    const char *reason,
    obs_rewrite_mode mode
) {
    (void)db;
    direct_skipped_calls++;
    direct_skipped_reason = reason;
    direct_skipped_mode = mode;
}
#endif

#ifdef EMBY_FTS_REWRITE_TEST_API
int emby_fts_rewrite_test_scalar_calls(void);
void emby_fts_rewrite_test_reset_scalar_calls(void);
int emby_fts_rewrite_test_duplicate_anchor_guard(void);
int emby_fts_rewrite_test_string_anchor_immunity(void);
#endif

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

typedef struct emby_auth_probe {
    int scalar_calls;
    int deny_scalar;
} emby_auth_probe;

typedef struct sqlite_master_auth_probe {
    int reads;
} sqlite_master_auth_probe;

static void seed_movies_latest_rows(sqlite3 *db);
static void seed_movies_latest_expanded_rows(sqlite3 *db);

static int scalar_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
);

static int active_stderr_capture_fd = -1;
static const rsh_suite_spec emby_suite_spec;

static void failf(const char *fmt, ...) {
    va_list ap;

    if (active_stderr_capture_fd >= 0) {
        fflush(stderr);
        if (dup2(active_stderr_capture_fd, STDERR_FILENO) >= 0) {
            close(active_stderr_capture_fd);
            active_stderr_capture_fd = -1;
            clearerr(stderr);
        }
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

#include "contract_parity.h"

static int rsh_public_prepare(
    sqlite3 *db,
    const char *sql,
    int nbyte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
) {
    if (kind == RSH_PREPARE_V3) {
        return sqlite3_prepare_v3(
            db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, stmt_out, tail_out
        );
    }
    if (kind == RSH_PREPARE_V2) {
        return sqlite3_prepare_v2(db, sql, nbyte, stmt_out, tail_out);
    }
    return sqlite3_prepare(db, sql, nbyte, stmt_out, tail_out);
}

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
static int rsh_direct_prepare(
    sqlite3 *db,
    const char *sql,
    int nbyte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
) {
    fts_rewrite_prepare_kind engine_kind = FTS_REWRITE_PREPARE_LEGACY;
    unsigned int flags = 0;

    if (kind == RSH_PREPARE_V3) {
        engine_kind = FTS_REWRITE_PREPARE_V3;
        flags = SQLITE_PREPARE_PERSISTENT;
    } else if (kind == RSH_PREPARE_V2) {
        engine_kind = FTS_REWRITE_PREPARE_V2;
    }
    return emby_fts_rewrite_prepare(
        db, sql, nbyte, flags, stmt_out, tail_out, engine_kind
    );
}
#endif

static char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;
    char *buf;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) failf("FATAL: vsnprintf sizing failed");
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) failf("FATAL: malloc failed");
    if (vsnprintf(buf, (size_t)n + 1, fmt, ap2) != n) failf("FATAL: vsnprintf write failed");
    va_end(ap2);
    return buf;
}

static char *read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp) failf("FATAL: open %s failed: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0) failf("FATAL: seek end %s failed", path);
    size = ftell(fp);
    if (size < 0) failf("FATAL: tell %s failed", path);
    if (fseek(fp, 0, SEEK_SET) != 0) failf("FATAL: seek start %s failed", path);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) failf("FATAL: malloc file buffer failed");
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        failf("FATAL: read %s failed", path);
    }
    if (fclose(fp) != 0) failf("FATAL: close %s failed", path);
    while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) size--;
    buf[size] = 0;
    return buf;
}

static FILE *begin_stderr_capture(int *saved_stderr_fd) {
    FILE *fp = tmpfile();

    if (!fp) failf("FATAL: tmpfile for stderr capture failed: %s", strerror(errno));
    fflush(stderr);
    *saved_stderr_fd = dup(STDERR_FILENO);
    if (*saved_stderr_fd < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    if (dup2(fileno(fp), STDERR_FILENO) < 0) {
        failf("FATAL: redirect stderr failed: %s", strerror(errno));
    }
    active_stderr_capture_fd = *saved_stderr_fd;
    clearerr(stderr);
    return fp;
}

static char *end_stderr_capture(FILE *fp, int saved_stderr_fd) {
    long size;
    char *buf;

    fflush(stderr);
    if (dup2(saved_stderr_fd, STDERR_FILENO) < 0) {
        failf("FATAL: restore stderr failed: %s", strerror(errno));
    }
    active_stderr_capture_fd = -1;
    close(saved_stderr_fd);
    clearerr(stderr);
    if (fseek(fp, 0, SEEK_END) != 0) failf("FATAL: seek capture end failed");
    size = ftell(fp);
    if (size < 0) failf("FATAL: tell capture failed");
    if (fseek(fp, 0, SEEK_SET) != 0) failf("FATAL: seek capture start failed");
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) failf("FATAL: malloc capture buffer failed");
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        failf("FATAL: read capture failed");
    }
    if (fclose(fp) != 0) failf("FATAL: close capture failed");
    buf[size] = 0;
    return buf;
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

static void require_absent(const char *label, const char *got, const char *needle) {
    if (got && strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" unexpected=\"%s\"", label, got, needle);
    }
}

static void require_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) failf("FAIL [%s]: text=(null)", label);
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
    failf(
        "FAIL [%s]: no line contains \"%s\" and \"%s\" in \"%s\"",
        label, first, second, text
    );
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;

    if (!haystack || !needle || !*needle) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

static uint64_t sql_corr_key(const char *sql, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)sql[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void configure_env_common(
    const char *fts,
    const char *fanout,
    const char *dashboard,
    int disable_observability
) {
    if (setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1", 1) != 0) failf("setenv AUTOPRAGMA failed");
    if (setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1", 1) != 0) failf("setenv RUNTIME failed");
    if (disable_observability) {
        if (setenv("SQLITE3_DISABLE_OBSERVABILITY", "1", 1) != 0) failf("setenv OBS failed");
    } else if (unsetenv("SQLITE3_DISABLE_OBSERVABILITY") != 0) {
        failf("unsetenv OBS failed");
    }
    if (unsetenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE") != 0) failf("unsetenv PLEX failed");
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

static void configure_env(const char *fts, const char *fanout, const char *dashboard) {
    configure_env_common(fts, fanout, dashboard, 1);
}

static void configure_env_observable(const char *fts, const char *fanout, const char *dashboard) {
    configure_env_common(fts, fanout, dashboard, 0);
}

static void temp_path(char *buf, size_t n, const char *basename) {
    int rc = snprintf(buf, n, "/tmp/emby-fts-rewrite-smoke-%ld/%s", (long)getpid(), basename);
    if (rc < 0 || (size_t)rc >= n) failf("FATAL: temp path too long for %s", basename);
}

static void make_temp_dir(void) {
    char dir[256];
    int rc = snprintf(dir, sizeof(dir), "/tmp/emby-fts-rewrite-smoke-%ld", (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(dir)) failf("FATAL: temp dir too long");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        failf("FATAL: mkdir(%s) failed: %s", dir, strerror(errno));
    }
}

static void cleanup_temp_dir(void) {
    char dir[256];
    char path[512];
    const char *names[] = {
        "library.db", "com.plexapp.plugins.library.db", "jellyfin.db", "not-target.db"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        temp_path(path, sizeof(path), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/emby-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/emby-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), names[i]);
        unlink(path);
    }
    snprintf(dir, sizeof(dir), "/tmp/emby-fts-rewrite-smoke-%ld", (long)getpid());
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

static sqlite3 *open_db_path(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [open]: path=%s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    }
    return db;
}

static int naturalsort_cmp(void *ctx, int left_n, const void *left, int right_n, const void *right) {
    int min_n = left_n < right_n ? left_n : right_n;
    int cmp;
    (void)ctx;
    cmp = memcmp(left, right, (size_t)min_n);
    if (cmp != 0) return cmp;
    return (left_n > right_n) - (left_n < right_n);
}

static int rsh_base_seed(sqlite3 *db, void *suite_ctx) {
    (void)suite_ctx;
    require_int("schema/collation",
                sqlite3_create_collation(db, "NATURALSORT", SQLITE_UTF8, NULL, naturalsort_cmp),
                SQLITE_OK);
    exec_sql(db, "schema",
        "CREATE TABLE MediaItems("
        "Id INTEGER PRIMARY KEY, Type INTEGER, EndDate TEXT, IndexNumber INTEGER, Name TEXT,"
        "SortName TEXT, DateCreated INTEGER, EpisodeAbsoluteIndexNumber INTEGER,"
        "Path TEXT, ParentIndexNumber INTEGER, ProductionYear INTEGER, RunTimeTicks INTEGER,"
        "ParentId INTEGER, SeriesName TEXT, AlbumId INTEGER, SeriesId INTEGER,"
        "SeriesPresentationUniqueKey TEXT, PresentationUniqueKey TEXT, Images TEXT, Status TEXT, SortIndexNumber INTEGER,"
        "SortParentIndexNumber INTEGER, IndexNumberEnd INTEGER, UserDataKeyId INTEGER,"
        "IsPublic INTEGER, ExtraType INTEGER, CommunityRating REAL, PremiereDate INTEGER,"
        "OfficialRating TEXT, guid TEXT, CriticRating REAL);"
        "CREATE TABLE AncestorIds2(itemid INTEGER, AncestorId INTEGER, Distance INTEGER);"
        "CREATE TABLE ItemLinks2(ItemId INTEGER, LinkedId INTEGER, Type INTEGER);"
        "CREATE TABLE itemPeople2(ItemId INTEGER, PersonId INTEGER);"
        "CREATE TABLE ListItems(ListId INTEGER, ListItemId INTEGER);"
        "CREATE TABLE UserItemShares(UserId INTEGER, ItemId INTEGER, ShareLevel INTEGER);"
        "CREATE TABLE UserDatas(UserDataKeyId INTEGER, UserId INTEGER, IsFavorite INTEGER,"
        "Played INTEGER, PlaybackPositionTicks INTEGER, AudioStreamIndex INTEGER,"
        "SubtitleStreamIndex INTEGER, HideFromResume INTEGER, LastPlayedDateInt INTEGER,"
        "PRIMARY KEY (UserDataKeyId, UserId)) WITHOUT ROWID;"
        "CREATE VIRTUAL TABLE fts_search9 USING fts5(title);"
        "INSERT INTO mediaitems("
        "Id, Type, Name, SortName, DateCreated, EpisodeAbsoluteIndexNumber,"
        "SeriesName, SeriesPresentationUniqueKey, PresentationUniqueKey,"
        "UserDataKeyId, IsPublic, ExtraType"
        ") VALUES"
        "(1,1,'direct','direct',100,1,NULL,NULL,'p1',1,1,NULL),"
        "(2,2,'list','list',101,2,NULL,NULL,'p2',2,1,NULL),"
        "(3,5,'person','person',102,3,NULL,NULL,'p3',3,1,NULL),"
        "(4,6,'link1','link1',103,4,NULL,NULL,'p4',4,1,NULL),"
        "(5,8,'link2','link2',104,5,'series z','latest-g1','p5',5,1,NULL),"
        "(6,8,'latest old','latest old',200,6,'series a','latest-g1','p6',6,1,NULL),"
        "(7,8,'latest new','latest new',300,7,'series a','latest-g1','p7',7,1,NULL),"
        "(8,8,'latest other','latest other',250,8,'series b',NULL,'latest-g2',8,1,NULL),"
        "(9,9,'browse','browse',120,9,NULL,NULL,'p9',9,1,NULL),"
        "(10,8,'latest null','latest null',350,10,'series c',NULL,NULL,10,1,NULL);"
        "INSERT INTO fts_search9(rowid,title) VALUES"
        "(1,'alpha direct'),(2,'alpha list'),(3,'alpha person'),"
        "(4,'alpha link'),(5,'alpha two');"
        "INSERT INTO AncestorIds2 VALUES(1,100,0),(6,100,0),(7,100,0),(8,100,0),(9,100,0),(10,100,0),(200,100,0),(300,100,0),(400,100,0),(502,100,0);"
        "INSERT INTO ListItems VALUES(2,200);"
        "INSERT INTO itemPeople2 VALUES(300,3);"
        "INSERT INTO ItemLinks2 VALUES(400,4,6),(501,5,0),(502,501,7);"
        "INSERT INTO UserItemShares VALUES(1,1,1),(1,2,1),(1,3,1),(1,4,1),(1,5,1);"
        "INSERT INTO UserDatas("
        "UserDataKeyId, UserId, IsFavorite, Played, PlaybackPositionTicks,"
        "AudioStreamIndex, SubtitleStreamIndex, HideFromResume, LastPlayedDateInt"
        ") VALUES"
        "(1,1,0,0,0,0,0,0,10),(2,1,0,0,0,0,0,0,20),(3,1,0,0,0,0,0,0,30),"
        "(4,1,0,0,0,0,0,0,40),(5,1,0,0,0,0,0,0,50),"
        "(6,42,0,0,0,0,0,0,60),(7,42,0,0,0,0,0,0,70),(8,42,0,0,0,0,0,0,80),"
        "(9,1,1,0,0,0,0,0,90),(10,42,0,0,0,0,0,0,100);"
    );
    return SQLITE_OK;
}

static int rsh_open(const char *path, sqlite3 **db_out, void *suite_ctx) {
    (void)suite_ctx;
    *db_out = open_db_path(path);
    return SQLITE_OK;
}

static rsh_apply_profile_fn rsh_resolve_setup_profile(
    int profile,
    void *suite_ctx
) {
    (void)profile;
    (void)suite_ctx;
    return NULL;
}

static sqlite3 *open_seeded_temp(const char *basename) {
    rsh_db_spec db_spec;
    rsh_db_handle handle;
    int rc;

    memset(&db_spec, 0, sizeof(db_spec));
    memset(&handle, 0, sizeof(handle));
    db_spec.role = "legacy";
    rc = snprintf(
        db_spec.relative_path, sizeof(db_spec.relative_path), "%s", basename
    );
    if (rc < 0 || (size_t)rc >= sizeof(db_spec.relative_path)) {
        failf("FATAL: seeded relative path too long for %s", basename);
    }
    db_spec.kind = strcmp(basename, emby_suite_spec.target_basename) == 0
        ? RSH_DB_CANDIDATE
        : strcmp(basename, emby_suite_spec.vendor_basename) == 0
            ? RSH_DB_VENDOR : RSH_DB_AUXILIARY;
    db_spec.storage = RSH_DB_RELATIVE;
    rsh_validate_db_specs(
        &emby_suite_spec, "legacy", "seeded-open", &db_spec, 1
    );
    rsh_open_seeded_db(
        &emby_suite_spec, getpid(), NULL, "legacy", "seeded-open",
        &db_spec, &handle
    );
    return handle.db;
}

static sqlite3 *open_seeded_temp_with_latest_indexes(
    const char *basename,
    int episodes_gk_dc_index,
    int episodes_dcn_gk_index
) {
    sqlite3 *db = open_seeded_temp(basename);
    if (episodes_gk_dc_index) {
        exec_sql(db, "latest-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    if (episodes_dcn_gk_index) {
        exec_sql(db, "latest-episodes-date-index",
            "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    return db;
}

static sqlite3 *open_seeded_temp_with_dashboard_indexes(
    const char *basename,
    int episodes_gk_dc_index,
    int episodes_dcn_gk_index,
    int movies_outer_index,
    int movies_inner_index
) {
    sqlite3 *db = open_seeded_temp_with_latest_indexes(
        basename, episodes_gk_dc_index, episodes_dcn_gk_index
    );
    if (movies_outer_index) {
        exec_sql(db, "movies-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    }
    if (movies_inner_index) {
        exec_sql(db, "movies-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
            "ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) "
            "WHERE Type = 5;"
        );
    }
    return db;
}

static sqlite3 *open_seeded_temp_with_mixed_indexes(
    const char *basename,
    int mixed_outer_index,
    int mixed_inner_index
) {
    sqlite3 *db = open_seeded_temp_with_dashboard_indexes(
        basename, 1, 1, 1, 1
    );
    if (mixed_outer_index) {
        exec_sql(db, "mixed-outer-index",
            "CREATE INDEX idx_dshadow_emby_latest_mixed_dcn_gk "
            "ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) "
            "WHERE Type IN (8,5);"
        );
    }
    if (mixed_inner_index) {
        exec_sql(db, "mixed-inner-index",
            "CREATE INDEX idx_dshadow_emby_latest_mixed_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type IN (8,5);"
        );
    }
    return db;
}

static void seed_mixed_latest_identity_rows(sqlite3 *db) {
    exec_sql(db, "mixed-identity-seed",
        "INSERT INTO MediaItems(Id,Type,Name,DateCreated,SeriesPresentationUniqueKey,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(2000,8,'cross-old',800,'g-cross','p-cross-8',2000),"
        "(2001,5,'cross-new',900,'g-cross','p-cross-5',2001),"
        "(2002,8,'null-gk-new',850,NULL,NULL,2002),"
        "(2003,5,'null-gk-null-date',NULL,NULL,NULL,2003),"
        "(2004,8,'null-played',700,'g-null-played','p-null-played',2004),"
        "(2005,5,'absent-userdata',600,'g-absent','p-absent',2005),"
        "(2010,8,'all-null-low-id',NULL,'g-all-null','p-all-null-1',2010),"
        "(2011,5,'all-null-high-id',NULL,'g-all-null','p-all-null-2',2011),"
        "(2020,8,'played-newer',950,'g-played','p-played-new',2020),"
        "(2021,5,'unplayed-older',500,'g-played','p-played-old',2021),"
        "(2030,5,'invisible-newer',990,'g-hidden','p-hidden-new',2030),"
        "(2031,8,'visible-older',400,'g-hidden','p-hidden-old',2031),"
        "(2040,8,'same-date-high-id',300,'g-same-id','p-same-1',2040),"
        "(2039,5,'same-date-low-id',300,'g-same-id','p-same-2',2039),"
        "(2050,8,'boundary-d',250,'g-d','p-d',2050),"
        "(2051,5,'boundary-a',250,'g-a','p-a',2051),"
        "(2052,8,'boundary-c',250,'g-c','p-c',2052),"
        "(2053,5,'boundary-b',250,'g-b','p-b',2053);"
        "INSERT INTO AncestorIds2(itemid,AncestorId,Distance) VALUES"
        "(2000,100,0),(2001,100,0),(2002,100,0),(2003,100,0),(2004,100,0),(2005,100,0),"
        "(2010,101,0),(2011,101,0),"
        "(2020,102,0),(2021,102,0),"
        "(2031,103,0),"
        "(2040,105,0),(2039,105,0),"
        "(2050,104,0),(2051,104,0),(2052,104,0),(2053,104,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,Played) VALUES"
        "(2000,42,0),(2001,42,0),(2002,42,0),(2003,42,0),"
        "(2004,42,NULL),(2010,42,0),(2011,42,0),"
        "(2020,42,1),(2021,42,0),(2030,42,0),(2031,42,0),"
        "(2040,42,0),(2039,42,0),(2050,42,0),(2051,42,0),(2052,42,0),(2053,42,0);"
    );
}

static char *make_emby_sql(int presentation, const char *l1, const char *l2,
                           const char *t1, const char *t2, int user_id) {
    const char *select_type =
        "select count(*) OVER() AS TotalRecordCount,A.type,"
        "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from";
    const char *select_presentation =
        "select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.PresentationUniqueKey,A.Images,A.Status,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex,"
        "(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=1 and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from";
    const char *join_presentation =
        " left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%d";
    char *left_join = presentation ? xasprintf(join_presentation, user_id) : xasprintf("%s", "");
    char *sql = xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (%s) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (%s)))"
        "%s mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm%s where "
        "A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) "
        "AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) "
        "AND A.ExtraType is null AND "
        "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (%s)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) "
        "Group by %s ORDER BY Rank ASC LIMIT 50",
        l1, t1, t2, presentation ? select_presentation : select_type,
        left_join, l2, presentation ? "A.PresentationUniqueKey" : "A.Type"
    );
    free(left_join);
    return sql;
}

static char *make_exists_arms(const char *l1, const char *l2, const char *t1, const char *t2) {
    return xasprintf(
        "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))"
        " OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (%s) where ListItems.ListId=A.Id)"
        " OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (%s))"
        " OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (%s))"
        " OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (%s)) and ItemLinks2TwoLevel.LinkedId=A.Id))",
        l1, l2, l1, l1, t1, l1, t2
    );
}

static char *replace_once(const char *sql, const char *old, const char *new_text) {
    const char *p = strstr(sql, old);
    size_t prefix;
    size_t out_len;
    char *out;

    if (!p) failf("FATAL: replace_once missing old=\"%s\"", old);
    if (strstr(p + strlen(old), old)) failf("FATAL: replace_once ambiguous old=\"%s\"", old);
    prefix = (size_t)(p - sql);
    out_len = strlen(sql) - strlen(old) + strlen(new_text);
    out = (char *)malloc(out_len + 1);
    if (!out) failf("FATAL: malloc failed");
    memcpy(out, sql, prefix);
    memcpy(out + prefix, new_text, strlen(new_text));
    strcpy(out + prefix + strlen(new_text), p + strlen(old));
    return out;
}

static char *add_anchor_literal_predicate(const char *sql, const char *anchor) {
    char *injected = xasprintf(" AND '%s'='%s' Group by ", anchor, anchor);
    char *out = replace_once(sql, " Group by ", injected);
    free(injected);
    return out;
}

static char *make_numeric_list(size_t min_len) {
    char *buf = (char *)malloc(min_len + 4);
    size_t used = 0;
    if (!buf) failf("FATAL: malloc numeric list failed");
    while (used + 2 < min_len) {
        buf[used++] = '1';
        buf[used++] = ',';
    }
    buf[used++] = '1';
    buf[used] = 0;
    return buf;
}

static char *make_expected_sql(int presentation, const char *l1, const char *l2,
                               const char *t1, const char *t2, int user_id) {
    char *original = make_emby_sql(presentation, l1, l2, t1, t2, user_id);
    char *old_membership = xasprintf(
        "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (%s)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)",
        l2
    );
    char *arms = make_exists_arms(l1, l2, t1, t2);
    char *with_membership = replace_once(original, old_membership, arms);
    char *expected = replace_once(
        with_membership,
        "fts_search9 match @SearchTerm",
        "fts_search9 match dshadow_emby_fts_rewrite(@SearchTerm)"
    );
    free(original);
    free(old_membership);
    free(arms);
    free(with_membership);
    return expected;
}

static char *make_ancestor_exists_splice(const char *l1) {
    return xasprintf(
        "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))",
        l1
    );
}

static char *make_complex_resume_watched_progress_conjunct(const char *user_id) {
    return xasprintf(
        " AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=%s AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=%s WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))",
        user_id, user_id
    );
}

static char *make_exists_people(const char *l1) {
    return xasprintf(
        "EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (%s))",
        l1
    );
}

static char *make_exists_links_one(const char *l1, const char *t1) {
    return xasprintf(
        "Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (%s))",
        l1, t1
    );
}

static char *make_exists_links_two(const char *l1, const char *t2) {
    return xasprintf(
        "Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (%s)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (%s)) and ItemLinks2TwoLevel.LinkedId=A.Id)",
        l1, t2
    );
}

static char *make_resume_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks,"
        "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
        "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=1 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
        "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where ((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null)) "
        "AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12"
    );
}

static char *make_resume_0065_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.DateCreated,A.ParentId,A.SeriesName,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,"
        "((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End)) EpisodeAbsoluteIndexNumber "
        "from mediaitems A left join (Select N.SeriesPresentationUniqueKey,((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End)) AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=13 where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
        "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=13 where ((UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null) "
        "AND Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0 AND A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=13 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 30"
    );
}

static char *make_resume_expected(const char *sql, const char *l1, const char *user_id) {
    char *ancestor_exists_splice = make_ancestor_exists_splice(l1);
    char *with_ancestor_exists_splice =
        replace_once(sql, "A.Id in WithAncestors", ancestor_exists_splice);
    char *watched_progress_conjunct =
        make_complex_resume_watched_progress_conjunct(user_id);
    char *group_with_conjunct =
        xasprintf("%s Group by coalesce(", watched_progress_conjunct);
    char *expected = replace_once(
        with_ancestor_exists_splice, " Group by coalesce(", group_with_conjunct
    );

    free(ancestor_exists_splice);
    free(with_ancestor_exists_splice);
    free(watched_progress_conjunct);
    free(group_with_conjunct);
    return expected;
}

static char *make_resume_simple_sql(int count_projection, const char *limit) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )%s%s "
        "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 "
        "where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=1 and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT %s",
        count_projection ? "select count(*) OVER() AS TotalRecordCount," : "select ",
        count_projection ? "A.type,A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks" : "A.Id,A.SeriesPresentationUniqueKey,UserDatas.PlaybackPositionTicks",
        limit
    );
}

static char *make_resume_simple_expected(const char *sql) {
    char *ancestor = make_ancestor_exists_splice("100");
    char *expected = replace_once(sql, "A.Id in WithAncestors", ancestor);

    free(ancestor);
    return expected;
}

static const char *EMBY_RESUME_SIMPLE_LEFT_JOIN_UNPLAYED_SHAPE_07_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (9,10,11,12,13,14,101,102,103) )select A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12;";

static char *make_similar_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "SimB_Ids AS (SELECT DISTINCT ItemLinks2SimB.LinkedId FROM ItemLinks2 ItemLinks2SimB WHERE ItemLinks2SimB.Type in (2,7) AND ItemLinks2SimB.ItemId=1),"
        "LinkedCounts AS (SELECT A.Id AS AId, COUNT(ItemLinks2SimA.LinkedId) AS LinkedCount FROM MediaItems A JOIN ItemLinks2 ItemLinks2SimA ON ItemLinks2SimA.ItemId = A.Id JOIN SimB_Ids ON SimB_Ids.LinkedId = ItemLinks2SimA.LinkedId WHERE ItemLinks2SimA.Type in (2,7) GROUP BY A.Id)"
        "select A.type,A.Id,A.PresentationUniqueKey,(LinkedCounts_Joined.LinkedCount * 15) as SimilarityScore from mediaitems A join LinkedCounts LinkedCounts_Joined ON A.Id = LinkedCounts_Joined.AId left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where SimilarityScore > 19 AND A.Type in (5,6) AND A.Id<>1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY SimilarityScore DESC,RANDOM() ASC LIMIT 16"
    );
}

static char *make_similar_expected(const char *sql) {
    char *ancestor = make_ancestor_exists_splice("100");
    char *expected = replace_once(sql, "A.Id in WithAncestors", ancestor);

    free(ancestor);
    return expected;
}

static const char *EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (15,16,17,18,19,20,107) ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,3,2) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,2)))select A.Type from mediaitems A where A.Type in (34,9,29,21) AND A.Id in WithItemLinkItemIds Group by A.Type";

static char *make_link_type_count_shape_05_expected(void) {
    char *one = make_exists_links_one("15,16,17,18,19,20,107", "6,4,3,2");
    char *two = make_exists_links_two("15,16,17,18,19,20,107", "7,0,1,5,6,2");
    char *replacement = xasprintf("(%s OR %s)", one, two);
    char *expected = replace_once(
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        "A.Id in WithItemLinkItemIds",
        replacement
    );

    free(one);
    free(two);
    free(replacement);
    return expected;
}

static void seed_link_type_count_shape_05_rows(sqlite3 *db) {
    exec_sql(db, "links-two-level-seed",
        "INSERT INTO MediaItems(Id,Type,Name) VALUES"
        "(1001,34,'l1-only'),(1002,9,'l2-only'),"
        "(1003,29,'both-and-duplicates'),(1004,21,'no-hit');"
        "INSERT INTO AncestorIds2 VALUES"
        "(2001,15,0),(2002,15,0),(2003,15,0),(2004,15,0);"
        "INSERT INTO ItemLinks2 VALUES"
        "(2001,1001,6),"
        "(2002,2102,7),(2102,1002,99),"
        "(2003,1003,6),(2003,1003,6),"
        "(2003,2103,7),(2103,1003,99),(2103,1003,99),"
        "(2004,NULL,6);"
    );
}

static char *make_favorites_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6))"
        "select A.Id,A.PresentationUniqueKey from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where (A.Id in WithAncestors OR A.Id in WithItemLinkItemIds) Group by A.PresentationUniqueKey ORDER BY Coalesce(UserDatas.IsFavorite, 0) DESC,RANDOM() ASC LIMIT 12"
    );
}

static char *make_browse_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type=6 union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7)))"
        "select A.Id,A.SortName from mediaitems A where A.Id in WithItemLinkItemIds ORDER BY A.SortName collate NATURALSORT ASC LIMIT 12"
    );
}

static char *make_people_sql(void) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) ORDER BY Rank ASC LIMIT 12"
    );
}

static char *make_links_search_sql(const char *types) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
        "WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (%s))"
        "select A.Id,A.Name from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match @SearchTerm where A.Id in WithItemLinkItemIds ORDER BY Rank ASC LIMIT 12",
        types
    );
}

static const char EMBY_MOVIES_LATEST_COMPACT_PROJECTION[] =
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_MOVIES_LATEST_WIDE_PROJECTION[] =
    "A.Id,A.EndDate,A.CommunityRating,A.Name,A.Path,A.PremiereDate,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_EPISODES_LATEST_WIDE_PROJECTION[] =
    "A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";
static const char EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION[] =
    "A.Id,A.Name,A.DateCreated,A.SeriesPresentationUniqueKey,A.PresentationUniqueKey,UserDatas.IsFavorite,UserDatas.Played ";
static const char EMBY_MIXED_LATEST_PROJECTION[] =
    "A.type,A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ";

static char *make_movies_latest_sql_form(
    int played_guard,
    const char *ancestor_ids,
    int scalar_ancestor,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId%s%s%s )select "
        "%s"
        "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s "
        "where A.Type=5 %sA.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT %s",
        scalar_ancestor ? "=" : " in (", ancestor_ids, scalar_ancestor ? "" : ")",
        projection, user_id,
        played_guard ? "AND Coalesce(UserDatas.played, 0)=0 AND " : "AND ", limit
    );
}

static char *make_movies_latest_sql(int played_guard, const char *ancestor_ids,
                                    const char *user_id, const char *limit) {
    return make_movies_latest_sql_form(
        played_guard, ancestor_ids, 0, EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
        user_id, limit
    );
}

static char *make_movies_latest_expected_projection(
    const char *ancestor_ids,
    const char *projection,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "WITH ranked(id, dc, puk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, A.PresentationUniqueKey "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_movies_dcn_puk "
        "  WHERE A.Type = 5 "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = %s AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_movies_puk_dc_cover "
        "      WHERE B.Type = 5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = %s AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, A.PresentationUniqueKey ASC LIMIT %s ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = %s "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.puk ASC LIMIT %s",
        ancestor_ids, user_id, ancestor_ids, user_id, limit, projection, user_id, limit
    );
}

static char *make_movies_latest_expected(const char *ancestor_ids,
                                         const char *user_id, const char *limit) {
    return make_movies_latest_expected_projection(
        ancestor_ids, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, user_id, limit
    );
}

static char *make_latest_sql_form(
    const char *projection,
    const char *ancestor_ids,
    int scalar_ancestor,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId%s%s%s )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
        "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT %s",
        scalar_ancestor ? "=" : " in (", ancestor_ids, scalar_ancestor ? "" : ")",
        projection, limit
    );
}

static char *make_latest_sql(const char *projection, const char *limit) {
    return make_latest_sql_form(projection, "100", 0, limit);
}

static char *make_latest_expected_form(
    const char *projection,
    const char *ancestor_ids,
    const char *limit
) {
    return xasprintf(
        "WITH ranked(id, dc, gk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk "
        "  WHERE A.Type = 8 "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = 42 AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_gk_dc "
        "      WHERE B.Type = 8 AND coalesce(B.SeriesPresentationUniqueKey, B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = 42 AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ASC LIMIT %s ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.gk ASC LIMIT %s",
        ancestor_ids, ancestor_ids, limit, projection, limit
    );
}

static char *make_latest_expected(const char *projection, const char *limit) {
    return make_latest_expected_form(projection, "100", limit);
}

static char *make_mixed_latest_sql_form(
    const char *ancestors,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %s"
        "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s "
        "where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT %s",
        ancestors, EMBY_MIXED_LATEST_PROJECTION, user_id, limit
    );
}

static char *make_mixed_latest_sql(const char *user_id, const char *limit) {
    return make_mixed_latest_sql_form("100", user_id, limit);
}

static char *make_mixed_latest_expected_form(
    const char *ancestors,
    const char *user_id,
    const char *limit
) {
    return xasprintf(
        "WITH mixed_latest_args(user_id, row_limit) AS MATERIALIZED ( VALUES (%s, %s) ), "
        "ranked(id, dc, gk) AS MATERIALIZED ("
        "  SELECT A.Id, A.DateCreated, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_mixed_dcn_gk "
        "  CROSS JOIN mixed_latest_args AS Args "
        "  WHERE A.Type IN (8,5) AND EXISTS ( SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A.Id AND X.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId = A.UserDataKeyId AND U0.UserId = Args.user_id AND U0.played <> 0 ) "
        "AND NOT EXISTS (      SELECT 1 "
        "      FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_mixed_gk_dc "
        "      WHERE B.Type IN (8,5) AND coalesce(B.SeriesPresentationUniqueKey, B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) "
        "AND ( (B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated > A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id < A.Id) ) "
        "AND EXISTS ( SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId = B.Id AND XB.AncestorId IN (%s) ) "
        "AND NOT EXISTS ( SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId = B.UserDataKeyId AND UB.UserId = Args.user_id AND UB.played <> 0 ) ) "
        "ORDER BY (A.DateCreated IS NULL) ASC, A.DateCreated DESC, coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ASC LIMIT (SELECT row_limit FROM mixed_latest_args) ) "
        "SELECT %s"
        "FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id CROSS JOIN mixed_latest_args AS Args LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = Args.user_id "
        "ORDER BY (R.dc IS NULL) ASC, R.dc DESC, R.gk ASC LIMIT (SELECT row_limit FROM mixed_latest_args)",
        user_id, limit, ancestors, ancestors, EMBY_MIXED_LATEST_PROJECTION
    );
}

static char *make_mixed_latest_expected(const char *user_id, const char *limit) {
    return make_mixed_latest_expected_form("100", user_id, limit);
}

static sqlite3_stmt *prepare_entry(sqlite3 *db, const char *label, const char *sql,
                                   int nbyte, int entry, const char **tail) {
    sqlite3_stmt *stmt = NULL;
    int rc;

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    if (entry == 3) {
        rc = emby_fts_rewrite_prepare(
            db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, &stmt, tail, FTS_REWRITE_PREPARE_V3
        );
    } else if (entry == 2) {
        rc = emby_fts_rewrite_prepare(db, sql, nbyte, 0, &stmt, tail, FTS_REWRITE_PREPARE_V2);
    } else {
        rc = emby_fts_rewrite_prepare(db, sql, nbyte, 0, &stmt, tail, FTS_REWRITE_PREPARE_LEGACY);
    }
#else
    if (entry == 3) {
        rc = sqlite3_prepare_v3(db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, &stmt, tail);
    } else if (entry == 2) {
        rc = sqlite3_prepare_v2(db, sql, nbyte, &stmt, tail);
    } else {
        rc = sqlite3_prepare(db, sql, nbyte, &stmt, tail);
    }
#endif
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=%d rc=%d err=%s", label, entry, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static sqlite3_stmt *contract_prepare_v2(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char **tail
) {
    return prepare_entry(db, label, sql, -1, 2, tail);
}

static void bind_search_term(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    const char *search_term = (const char *)ctx;
    int index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");
    int rc;
    if (index == 0) {
        failf("FAIL [%s/%s-bind-index]: @SearchTerm missing", label, side);
    }
    rc = sqlite3_bind_text(stmt, index, search_term, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind]: rc=%d", label, side, rc);
    }
}

static void expect_sql(sqlite3 *db, const char *label, const char *sql, int nbyte,
                       int entry, const char *want_sql, int want_rewrite) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, entry, &tail);
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (want_rewrite) {
        if (strstr(want_sql, "@SearchTerm")) {
            require_int("bind-index", sqlite3_bind_parameter_index(stmt, "@SearchTerm") > 0, 1);
        }
        require_contains(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    } else {
        require_absent(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_dashboard_negative(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *boundary_needle,
    int boundary_count
) {
    require_int(label, count_occurrences(sql, boundary_needle), boundary_count);
    expect_sql(db, label, sql, -1, 2, sql, 0);
}

static void expect_mixed_latest_sql(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *want_sql,
    int user_bound,
    int limit_bound
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, &tail);
    int expected_binds = user_bound + limit_bound;
    int i;
    int parameter = 1;
    int rc;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    require_int(label, sqlite3_bind_parameter_count(stmt), expected_binds);
    require_int(label, sqlite3_column_count(stmt), 19);
    require_int(label, count_occurrences(
        sqlite3_sql(stmt), "INDEXED BY idx_dshadow_emby_latest_mixed_dcn_gk"
    ), 1);
    require_int(label, count_occurrences(
        sqlite3_sql(stmt), "INDEXED BY idx_dshadow_emby_latest_mixed_gk_dc"
    ), 1);
    require_absent(label, sqlite3_sql(stmt), " Group by ");
    require_absent(label, sqlite3_sql(stmt), "MAX(A.DateCreated)");
    for (i = 1; i <= expected_binds; i++) {
        if (sqlite3_bind_parameter_name(stmt, i) != NULL) {
            failf("FAIL [%s/bind-name-%d]: expected anonymous parameter actual=%s",
                  label, i, sqlite3_bind_parameter_name(stmt, i));
        }
    }
    if (user_bound) {
        require_int(label, sqlite3_bind_int(stmt, parameter++, 42), SQLITE_OK);
    }
    if (limit_bound) {
        require_int(label, sqlite3_bind_int(stmt, parameter++, 3), SQLITE_OK);
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
    require_int(label, rc, SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_mixed_latest_negative(
    sqlite3 *db,
    const char *label,
    const char *sql
) {
    size_t sql_len = strlen(sql);
    uint16_t *vendor_sql = (uint16_t *)malloc(
        (sql_len + 1) * sizeof(*vendor_sql)
    );
    sqlite3_stmt *vendor = NULL;
    const void *tail = NULL;
    size_t i;

    if (!vendor_sql) failf("FATAL: malloc vendor SQL failed");
    for (i = 0; i <= sql_len; i++) {
        unsigned char byte = (unsigned char)sql[i];
        if (byte > 0x7f) {
            free(vendor_sql);
            failf("FAIL [%s/vendor-sql-ascii]: offset=%lu byte=0x%02x",
                  label, (unsigned long)i, (unsigned int)byte);
        }
        vendor_sql[i] = (uint16_t)byte;
    }
    require_int(label, sqlite3_prepare16_v2(db, vendor_sql, -1, &vendor, &tail), SQLITE_OK);
    require_int(label, tail == vendor_sql + sql_len, 1);
    require_int(label, sqlite3_finalize(vendor), SQLITE_OK);
    free(vendor_sql);
    expect_sql(db, label, sql, -1, 2, sql, 0);
}

static void expect_legacy_authorizer(sqlite3 *db, const char *label, const char *sql,
                                     int expect_rewrite) {
    emby_auth_probe probe = {0, 0};
    const char *tail = NULL;
    sqlite3_stmt *stmt;

    require_int("legacy-authorizer/set",
                sqlite3_set_authorizer(db, scalar_authorizer_cb, &probe), SQLITE_OK);
    stmt = prepare_entry(db, label, sql, -1, 1, &tail);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
    require_int("legacy-authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (expect_rewrite && probe.scalar_calls < 1) {
        failf("FAIL [%s]: expected scalar authorizer call, got=%d", label, probe.scalar_calls);
    }
    if (!expect_rewrite && probe.scalar_calls != 0) {
        failf("FAIL [%s]: unexpected scalar authorizer calls=%d", label, probe.scalar_calls);
    }
}

static int g_collision_scalar_calls;

static void scalar_collision_tripwire(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    g_collision_scalar_calls++;
    sqlite3_result_error(ctx, "collision scalar must not execute", -1);
}

static void scalar_owner_spoof(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    sqlite3_result_text(ctx, "__dshadow_emby_owner_ok__", -1, SQLITE_STATIC);
}

static int deny_pragma_function_list(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    (void)ctx;
    (void)action;
    (void)db;
    (void)trigger;
    if ((p1 && strcmp(p1, "pragma_function_list") == 0) ||
        (p2 && strcmp(p2, "pragma_function_list") == 0)) {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static int deny_sqlite_master_read(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    (void)ctx;
    (void)p2;
    (void)db;
    (void)trigger;
    if (action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static int count_sqlite_master_read(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    sqlite_master_auth_probe *probe = (sqlite_master_auth_probe *)ctx;
    (void)p2;
    (void)db;
    (void)trigger;
    if (action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        probe->reads++;
    }
    return SQLITE_OK;
}

static int scalar_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    emby_auth_probe *probe = (emby_auth_probe *)ctx;
    (void)db;
    (void)trigger;
    if (action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "dshadow_emby_fts_rewrite") == 0) ||
         (p2 && strcmp(p2, "dshadow_emby_fts_rewrite") == 0))) {
        probe->scalar_calls++;
        if (probe->deny_scalar) return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static void expect_scalar(sqlite3 *db, const char *input, const char *want) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT dshadow_emby_fts_rewrite(?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [scalar/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    if (input) require_int("scalar/bind", sqlite3_bind_text(stmt, 1, input, -1, SQLITE_STATIC), SQLITE_OK);
    else require_int("scalar/bind-null", sqlite3_bind_null(stmt, 1), SQLITE_OK);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) failf("FAIL [scalar/step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    if (want) require_str_eq("scalar/value", (const char *)sqlite3_column_text(stmt, 0), want);
    else require_int("scalar/null", sqlite3_column_type(stmt, 0), SQLITE_NULL);
    require_int("scalar/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_scalar_type(sqlite3 *db, const char *label, int bind_blob) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT dshadow_emby_fts_rewrite(?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [%s/prepare]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    if (bind_blob) {
        static const unsigned char blob[] = {0x01, 0x02, 0x03};
        require_int("scalar-type/bind-blob", sqlite3_bind_blob(stmt, 1, blob, sizeof(blob), SQLITE_STATIC), SQLITE_OK);
    } else {
        require_int("scalar-type/bind-int", sqlite3_bind_int(stmt, 1, 42), SQLITE_OK);
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_int(label, sqlite3_column_type(stmt, 0), bind_blob ? SQLITE_BLOB : SQLITE_INTEGER);
    require_int("scalar-type/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

#ifdef EMBY_FTS_REWRITE_TEST_API
static void expect_scalar_counter(sqlite3 *db, const char *sql) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, "scalar-counter", sql, -1, 2, &tail);
    int index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");
    int rc;
    int calls;

    require_int("scalar-counter/bind-index", index > 0, 1);
    require_int("scalar-counter/bind1",
                sqlite3_bind_text(stmt, index, "(\"alpha\"*) OR (\"direct\"*)", -1, SQLITE_STATIC),
                SQLITE_OK);
    emby_fts_rewrite_test_reset_scalar_calls();
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        failf("FAIL [scalar-counter/first-step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    calls = emby_fts_rewrite_test_scalar_calls();
    if (calls <= 0) failf("FAIL [scalar-counter/first-calls]: got=%d want>0", calls);

    require_int("scalar-counter/reset", sqlite3_reset(stmt), SQLITE_OK);
    require_int("scalar-counter/clear", sqlite3_clear_bindings(stmt), SQLITE_OK);
    require_int("scalar-counter/bind2",
                sqlite3_bind_text(stmt, index, "(\"alpha\"*) OR (\"missing\"*)", -1, SQLITE_STATIC),
                SQLITE_OK);
    emby_fts_rewrite_test_reset_scalar_calls();
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        failf("FAIL [scalar-counter/second-step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    calls = emby_fts_rewrite_test_scalar_calls();
    if (calls <= 0) failf("FAIL [scalar-counter/second-calls]: got=%d want>0", calls);
    require_int("scalar-counter/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}
#endif

static void expect_fixture(sqlite3 *db, const char *name) {
    char *input_path = xasprintf("tests/fixtures/emby-fts-rewrite/%s.sql", name);
    char *expected_path = xasprintf("tests/fixtures/emby-fts-rewrite/%s.expected.sql", name);
    char *input = read_text_file(input_path);
    char *expected = read_text_file(expected_path);
    int want_rewrite = strstr(expected, "dshadow_emby_fts_rewrite(") != NULL;

    expect_sql(db, name, input, -1, 2, expected, want_rewrite);
    free(input_path);
    free(expected_path);
    free(input);
    free(expected);
}

static unsigned int collect_fts_id_mask(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int want_rewrite,
    const char *search_term
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, 2, &tail);
    int index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");
    int rc;
    int rows = 0;
    unsigned int mask = 0;

    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (want_rewrite) {
        require_contains(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    } else {
        require_absent(label, sqlite3_sql(stmt), "dshadow_emby_fts_rewrite(");
    }
    require_int("row-parity/bind-index", index > 0, 1);
    require_int("row-parity/bind",
                sqlite3_bind_text(stmt, index, search_term, -1, SQLITE_STATIC),
                SQLITE_OK);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 2);
        unsigned int bit;
        if (sqlite3_column_type(stmt, 2) != SQLITE_INTEGER || id < 1 || id > 5) {
            failf("FAIL [%s/id]: type=%d got=%lld want=1..5", label,
                  sqlite3_column_type(stmt, 2), (long long)id);
        }
        bit = 1u << (unsigned int)(id - 1);
        if ((mask & bit) != 0) {
            failf("FAIL [%s/duplicate]: id=%lld mask=0x%x", label,
                  (long long)id, mask);
        }
        mask |= bit;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int(label, rows, 5);
    require_int("row-parity/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return mask;
}

static int accept_emby_fts_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    (void)label;
    (void)row;
    (void)ctx;
    if (sqlite3_column_type(vendor, 2) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 2) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 2);
    candidate_id = sqlite3_column_int64(candidate, 2);
    return vendor_id >= 1 && vendor_id <= 5 &&
        candidate_id >= 1 && candidate_id <= 5;
}

static void require_emby_fts_ordered_contract(
    sqlite3 *vendor_db,
    sqlite3 *candidate_db,
    const char *label,
    const char *vendor_sql,
    const char *candidate_sql,
    const char *search_term
) {
    char *ordered_vendor_sql = xasprintf("SELECT * FROM (%s) ORDER BY 3", vendor_sql);
    char *ordered_candidate_sql = xasprintf("SELECT * FROM (%s) ORDER BY 3", candidate_sql);
    sqlite3_stmt *vendor = NULL;
    sqlite3_stmt *candidate = NULL;
    int row;
    int columns;

    require_int(label, sqlite3_prepare_v2(
        vendor_db, ordered_vendor_sql, -1, &vendor, NULL
    ), SQLITE_OK);
    require_int(label, sqlite3_prepare_v2(
        candidate_db, ordered_candidate_sql, -1, &candidate, NULL
    ), SQLITE_OK);
    columns = sqlite3_column_count(vendor);
    require_int(label, sqlite3_column_count(candidate), columns);
    bind_search_term(vendor, "vendor-ordered", label, (void *)search_term);
    bind_search_term(candidate, "candidate-ordered", label, (void *)search_term);
    for (row = 0; row < 5; row++) {
        int col;
        require_int(label, sqlite3_step(vendor), SQLITE_ROW);
        require_int(label, sqlite3_step(candidate), SQLITE_ROW);
        require_int(label, sqlite3_column_int(vendor, 2), row + 1);
        require_int(label, sqlite3_column_int(candidate, 2), row + 1);
        for (col = 0; col < columns; col++) {
            if (!contract_parity_cell_equal(vendor, candidate, col)) {
                failf("FAIL [%s/ordered-row-%d-column-%d]: vendor_type=%d "
                      "candidate_type=%d vendor_bytes=%d candidate_bytes=%d",
                      label, row, col, sqlite3_column_type(vendor, col),
                      sqlite3_column_type(candidate, col),
                      sqlite3_column_bytes(vendor, col),
                      sqlite3_column_bytes(candidate, col));
            }
        }
    }
    require_int(label, sqlite3_step(vendor), SQLITE_DONE);
    require_int(label, sqlite3_step(candidate), SQLITE_DONE);
    require_int(label, sqlite3_finalize(vendor), SQLITE_OK);
    require_int(label, sqlite3_finalize(candidate), SQLITE_OK);
    free(ordered_vendor_sql);
    free(ordered_candidate_sql);
}

static void collect_first_ints(sqlite3 *db, const char *label, const char *sql,
                               const char *want_sql, char *out, size_t out_n) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, &tail);
    int rc;
    size_t used = 0;
    int rows = 0;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n = snprintf(out + used, out_n - used, "%d,", sqlite3_column_int(stmt, 0));
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: output buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("first-ints/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void collect_int_column(sqlite3 *db, const char *label, const char *sql,
                               const char *want_sql, int nbyte, int column,
                               char *out, size_t out_n) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, 2, &tail);
    int rc;
    size_t used = 0;
    int rows = 0;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (sqlite3_column_count(stmt) <= column) {
        failf("FAIL [%s]: column=%d count=%d", label, column, sqlite3_column_count(stmt));
    }
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n = snprintf(out + used, out_n - used, "%d,", sqlite3_column_int(stmt, column));
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: output buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("int-column/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

typedef struct typed_rows {
    unsigned char *bytes;
    size_t len;
    size_t cap;
    int columns;
    int rows;
} typed_rows;

static void typed_rows_append(typed_rows *rows, const void *src, size_t n) {
    if (rows->len + n + 1 > rows->cap) {
        size_t cap = rows->cap ? rows->cap : 256;
        unsigned char *next;
        while (cap < rows->len + n + 1) cap *= 2;
        next = (unsigned char *)realloc(rows->bytes, cap);
        if (!next) failf("FATAL: typed row realloc failed");
        rows->bytes = next;
        rows->cap = cap;
    }
    memcpy(rows->bytes + rows->len, src, n);
    rows->len += n;
    rows->bytes[rows->len] = 0;
}

static void typed_rows_appendf(typed_rows *rows, const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;
    char *buf;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) failf("FATAL: typed row formatting failed");
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) failf("FATAL: typed row formatting alloc failed");
    if (vsnprintf(buf, (size_t)n + 1, fmt, ap2) != n) {
        failf("FATAL: typed row formatting write failed");
    }
    va_end(ap2);
    typed_rows_append(rows, buf, (size_t)n);
    free(buf);
}

static void typed_rows_append_hex(typed_rows *rows, const unsigned char *src, int n) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        unsigned char out[2];
        out[0] = (unsigned char)hex[src[i] >> 4];
        out[1] = (unsigned char)hex[src[i] & 15];
        typed_rows_append(rows, out, sizeof(out));
    }
}

static typed_rows collect_typed_rows(sqlite3 *db, const char *label,
                                     const char *sql, const char *want_sql) {
    typed_rows body = {0};
    typed_rows out = {0};
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, &tail);
    int rc;
    int col;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    body.columns = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        typed_rows_append(&body, "R", 1);
        body.rows++;
        for (col = 0; col < body.columns; col++) {
            int type = sqlite3_column_type(stmt, col);
            if (type == SQLITE_NULL) {
                typed_rows_append(&body, "N;", 2);
            } else if (type == SQLITE_INTEGER) {
                typed_rows_appendf(&body, "I:%lld;", (long long)sqlite3_column_int64(stmt, col));
            } else if (type == SQLITE_FLOAT) {
                typed_rows_appendf(&body, "F:%.17g;", sqlite3_column_double(stmt, col));
            } else {
                const unsigned char *value = type == SQLITE_TEXT
                    ? sqlite3_column_text(stmt, col)
                    : (const unsigned char *)sqlite3_column_blob(stmt, col);
                int n = sqlite3_column_bytes(stmt, col);
                typed_rows_appendf(&body, "%c:%d:", type == SQLITE_TEXT ? 'T' : 'B', n);
                if (n > 0) typed_rows_append_hex(&body, value, n);
                typed_rows_append(&body, ";", 1);
            }
        }
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int("typed-rows/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    out.columns = body.columns;
    out.rows = body.rows;
    typed_rows_appendf(&out, "C%d;N%d;", out.columns, out.rows);
    typed_rows_append(&out, body.bytes ? body.bytes : (const unsigned char *)"", body.len);
    free(body.bytes);
    return out;
}

typedef enum mixed_limit_bind_case {
    MIXED_LIMIT_ZERO = 0,
    MIXED_LIMIT_NEGATIVE,
    MIXED_LIMIT_NULL,
    MIXED_LIMIT_TEXT_THREE,
    MIXED_LIMIT_TEXT_GARBAGE
} mixed_limit_bind_case;

static typed_rows collect_bound_mixed_rows(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *want_sql,
    mixed_limit_bind_case limit_case,
    int *step_result
) {
    typed_rows body = {0};
    typed_rows out = {0};
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, &tail);
    int rc;
    int col;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, tail == sql + strlen(sql), 1);
    require_int(label, sqlite3_bind_parameter_count(stmt), 2);
    require_int(label, sqlite3_bind_int(stmt, 1, 42), SQLITE_OK);
    switch (limit_case) {
        case MIXED_LIMIT_ZERO:
            rc = sqlite3_bind_int(stmt, 2, 0);
            break;
        case MIXED_LIMIT_NEGATIVE:
            rc = sqlite3_bind_int(stmt, 2, -1);
            break;
        case MIXED_LIMIT_NULL:
            rc = sqlite3_bind_null(stmt, 2);
            break;
        case MIXED_LIMIT_TEXT_THREE:
            rc = sqlite3_bind_text(stmt, 2, "3", -1, SQLITE_STATIC);
            break;
        default:
            rc = sqlite3_bind_text(stmt, 2, "garbage", -1, SQLITE_STATIC);
            break;
    }
    require_int(label, rc, SQLITE_OK);
    body.columns = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        typed_rows_append(&body, "R", 1);
        body.rows++;
        for (col = 0; col < body.columns; col++) {
            int type = sqlite3_column_type(stmt, col);
            if (type == SQLITE_NULL) {
                typed_rows_append(&body, "N;", 2);
            } else if (type == SQLITE_INTEGER) {
                typed_rows_appendf(&body, "I:%lld;", (long long)sqlite3_column_int64(stmt, col));
            } else if (type == SQLITE_FLOAT) {
                typed_rows_appendf(&body, "F:%.17g;", sqlite3_column_double(stmt, col));
            } else {
                const unsigned char *value = type == SQLITE_TEXT
                    ? sqlite3_column_text(stmt, col)
                    : (const unsigned char *)sqlite3_column_blob(stmt, col);
                int n = sqlite3_column_bytes(stmt, col);
                typed_rows_appendf(&body, "%c:%d:", type == SQLITE_TEXT ? 'T' : 'B', n);
                if (n > 0) typed_rows_append_hex(&body, value, n);
                typed_rows_append(&body, ";", 1);
            }
        }
    }
    *step_result = rc;
    require_int(label, sqlite3_finalize(stmt), rc == SQLITE_DONE ? SQLITE_OK : rc);
    out.columns = body.columns;
    out.rows = body.rows;
    typed_rows_appendf(&out, "C%d;N%d;RC%d;", out.columns, out.rows, rc);
    typed_rows_append(&out, body.bytes ? body.bytes : (const unsigned char *)"", body.len);
    free(body.bytes);
    return out;
}

static void require_ordered_full_row_identity(const char *label,
                                              const typed_rows *vendor,
                                              const typed_rows *candidate) {
    size_t i;
    if (vendor->columns != candidate->columns || vendor->rows != candidate->rows ||
        vendor->len != candidate->len ||
        memcmp(vendor->bytes, candidate->bytes, vendor->len) != 0) {
        size_t max = vendor->len < candidate->len ? vendor->len : candidate->len;
        for (i = 0; i < max && vendor->bytes[i] == candidate->bytes[i]; i++) {}
        failf("FAIL [%s]: first_diff=%lu vendor_columns=%d candidate_columns=%d "
              "vendor_rows=%d candidate_rows=%d vendor_len=%lu candidate_len=%lu "
              "vendor=\"%s\" candidate=\"%s\"",
              label, (unsigned long)i, vendor->columns, candidate->columns,
              vendor->rows, candidate->rows, (unsigned long)vendor->len,
              (unsigned long)candidate->len, (const char *)vendor->bytes,
              (const char *)candidate->bytes);
    }
}

typedef struct movies_latest_contract {
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;
    const char *projection;
} movies_latest_contract;

static void require_dashboard_latest_fixture_row(
    const char *label,
    const char *side,
    sqlite3 *db,
    sqlite3_stmt *actual,
    const char *projection,
    sqlite3_int64 id
) {
    char *sql = xasprintf(
        "SELECT %sFROM MediaItems AS A LEFT JOIN UserDatas "
        "ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 "
        "WHERE A.Id = ?",
        projection
    );
    sqlite3_stmt *expected = NULL;
    int columns;
    int col;
    int rc;

    rc = sqlite3_prepare_v2(db, sql, -1, &expected, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-fixture-prepare]: id=%lld rc=%d err=%s", label,
              side, (long long)id, rc, sqlite3_errmsg(db));
    }
    require_int(label, sqlite3_bind_int64(expected, 1, id), SQLITE_OK);
    rc = sqlite3_step(expected);
    if (rc != SQLITE_ROW) {
        failf("FAIL [%s/%s-fixture-row]: id=%lld rc=%d want=%d err=%s", label,
              side, (long long)id, rc, SQLITE_ROW, sqlite3_errmsg(db));
    }
    columns = sqlite3_column_count(actual);
    if (sqlite3_column_count(expected) != columns) {
        failf("FAIL [%s/%s-fixture-columns]: id=%lld actual=%d expected=%d",
              label, side, (long long)id, columns, sqlite3_column_count(expected));
    }
    for (col = 0; col < columns; col++) {
        if (!contract_parity_cell_equal(actual, expected, col)) {
            failf("FAIL [%s/%s-fixture-column-%d]: id=%lld actual_type=%d "
                  "expected_type=%d actual_bytes=%d expected_bytes=%d",
                  label, side, col, (long long)id,
                  sqlite3_column_type(actual, col), sqlite3_column_type(expected, col),
                  sqlite3_column_bytes(actual, col), sqlite3_column_bytes(expected, col));
        }
    }
    require_int(label, sqlite3_step(expected), SQLITE_DONE);
    require_int(label, sqlite3_finalize(expected), SQLITE_OK);
    free(sql);
}

static unsigned int movies_latest_vendor_group_bit(sqlite3_int64 id) {
    if (id >= 9301 && id <= 9303) return 1u;
    if (id >= 9311 && id <= 9312) return 2u;
    if (id >= 9321 && id <= 9322) return 4u;
    return 0;
}

static int accept_movies_latest_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    static const sqlite3_int64 candidate_ids[] = {9303, 9312, 9321};
    movies_latest_contract *contract = (movies_latest_contract *)ctx;
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    if (row < 0 || row >= (int)(sizeof(candidate_ids) / sizeof(candidate_ids[0])) ||
        sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    if (movies_latest_vendor_group_bit(vendor_id) == 0 ||
        candidate_id != candidate_ids[row]) {
        return 0;
    }
    require_dashboard_latest_fixture_row(
        label, "vendor", contract->vendor_db, vendor,
        contract->projection, vendor_id
    );
    require_dashboard_latest_fixture_row(
        label, "candidate", contract->candidate_db, candidate,
        contract->projection, candidate_id
    );
    return 1;
}

static void require_movies_latest_vendor_rows(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *projection
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, &tail);
    unsigned int group_mask = 0;
    int rows = 0;
    int rc;

    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/vendor-tail]: got=%ld want=%ld", label,
              (long)(tail - sql), (long)strlen(sql));
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        unsigned int bit;
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            (bit = movies_latest_vendor_group_bit(id)) == 0) {
            failf("FAIL [%s/vendor-id]: type=%d got=%lld", label,
                  sqlite3_column_type(stmt, 0), (long long)id);
        }
        if ((group_mask & bit) != 0) {
            failf("FAIL [%s/vendor-duplicate-group]: id=%lld mask=0x%x bit=0x%x",
                  label, (long long)id, group_mask, bit);
        }
        require_dashboard_latest_fixture_row(
            label, "vendor", db, stmt, projection, id
        );
        group_mask |= bit;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/vendor-step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_int(label, rows, 3);
    require_int(label, (int)group_mask, 7);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void require_movies_latest_candidate_rows(
    sqlite3 *db,
    const char *label,
    const char *source_sql,
    const char *expected_sql,
    const char *projection,
    const sqlite3_int64 *expected_ids,
    int expected_count
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, source_sql, -1, 2, &tail);
    int row;

    require_str_eq(label, sqlite3_sql(stmt), expected_sql);
    if (tail != source_sql + strlen(source_sql)) {
        failf("FAIL [%s/tail]: got=%ld want=%ld", label,
              (long)(tail - source_sql), (long)strlen(source_sql));
    }
    for (row = 0; row < expected_count; row++) {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [%s/row-%d]: rc=%d want=%d err=%s", label, row, rc,
                  SQLITE_ROW, sqlite3_errmsg(db));
        }
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            sqlite3_column_int64(stmt, 0) != expected_ids[row]) {
            failf("FAIL [%s/id-row-%d]: type=%d got=%lld want_type=%d want=%lld",
                  label, row, sqlite3_column_type(stmt, 0),
                  (long long)sqlite3_column_int64(stmt, 0), SQLITE_INTEGER,
                  (long long)expected_ids[row]);
        }
        require_dashboard_latest_fixture_row(
            label, "candidate", db, stmt, projection, expected_ids[row]
        );
    }
    require_int(label, sqlite3_step(stmt), SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void require_typed_rows_differ(const char *label,
                                      const typed_rows *left,
                                      const typed_rows *right) {
    if (left->columns == right->columns && left->rows == right->rows &&
        left->len == right->len && memcmp(left->bytes, right->bytes, left->len) == 0) {
        failf("FAIL [%s]: typed streams unexpectedly equal: %s", label,
              (const char *)left->bytes);
    }
}

static void free_typed_rows(typed_rows *rows) {
    free(rows->bytes);
    memset(rows, 0, sizeof(*rows));
}

static void seed_complex_resume_watched_progress_parity_rows(sqlite3 *db) {
    exec_sql(db, "complex-resume-watched-progress-parity-seed",
        "INSERT INTO MediaItems("
        "Id, Type, Name, SortName, DateCreated, EpisodeAbsoluteIndexNumber,"
        "ParentIndexNumber, SortIndexNumber, SortParentIndexNumber, IndexNumber,"
        "SeriesName, SeriesPresentationUniqueKey, PresentationUniqueKey,"
        "UserDataKeyId, IsPublic, ExtraType"
        ") VALUES"
        "(100,5,'resume movie','resume movie',800,1,NULL,NULL,NULL,NULL,NULL,NULL,'resume-movie',100,1,NULL),"
        "(101,8,'resume watched','resume watched',900,1,1,1,1,1,'resume series','resume-series','resume-episode-1',101,1,NULL),"
        "(102,8,'resume next','resume next',901,1000002,1,2,1,2,'resume series','resume-series','resume-episode-2',102,1,NULL);"
        "INSERT INTO AncestorIds2 VALUES(100,100,0),(102,100,0);"
        "INSERT INTO UserDatas("
        "UserDataKeyId, UserId, IsFavorite, Played, PlaybackPositionTicks,"
        "AudioStreamIndex, SubtitleStreamIndex, HideFromResume, LastPlayedDateInt"
        ") VALUES"
        "(100,1,0,0,500,0,0,0,800),"
        "(101,1,0,1,0,0,0,0,900),"
        "(102,1,0,0,0,0,0,0,0);"
    );
}

static int child_positive(void) {
    sqlite3 *db;
    char *type_sql;
    char *numbered_sql;
    char *presentation_sql;
    char *type_expected;
    char *numbered_expected;
    char *presentation_expected;

    configure_env(NULL, "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    type_sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    numbered_sql = replace_once(type_sql, "fts_search9 match @SearchTerm", "fts_search9 match ?1");
    presentation_sql = make_emby_sql(1, "100", "100", "6", "7", 42);
    type_expected = make_expected_sql(0, "100", "100", "6", "7", 1);
    numbered_expected = replace_once(
        type_expected,
        "fts_search9 match dshadow_emby_fts_rewrite(@SearchTerm)",
        "fts_search9 match dshadow_emby_fts_rewrite(?1)"
    );
    presentation_expected = make_expected_sql(1, "100", "100", "6", "7", 42);

    expect_sql(db, "type-v2", type_sql, -1, 2, type_expected, 1);
    expect_sql(db, "type-v3", type_sql, -1, 3, type_expected, 1);
    expect_legacy_authorizer(db, "type-legacy", type_sql, 1);
    expect_sql(db, "type-numbered-param", numbered_sql, -1, 2, numbered_expected, 1);
    expect_sql(db, "type-nbyte-no-nul", type_sql, (int)strlen(type_sql), 2, type_expected, 1);
    expect_sql(db, "type-nbyte-with-nul", type_sql, (int)strlen(type_sql) + 1, 2, type_expected, 1);
    expect_sql(db, "presentation-user42", presentation_sql, -1, 2, presentation_expected, 1);

    expect_scalar(db, "(\"Star\"*) OR (\"Wars\"*)", "(\"Star\"*) AND (\"Wars\"*)");
    expect_scalar(db, "(\"a\"*) OR (\"b\"*) OR (\"c\"*)", "(\"a\"*) AND (\"b\"*) AND (\"c\"*)");
    expect_scalar(db, "(\"a\"*) AND (\"b\"*)", "(\"a\"*) AND (\"b\"*)");
    expect_scalar(db, "\"star wars\"", "\"star wars\"");
    expect_scalar(db, "(\"a\"*) OR \"b\"*", "(\"a\"*) OR \"b\"*");
    expect_scalar(db, "(\"a\"*) or (\"b\"*)", "(\"a\"*) or (\"b\"*)");
    expect_scalar(db, "(\"caf\303\251\"*) OR (\"b\"*)", "(\"caf\303\251\"*) OR (\"b\"*)");
    expect_scalar(db, "", "");
    expect_scalar(db, NULL, NULL);
    expect_scalar_type(db, "scalar-integer", 0);
    expect_scalar_type(db, "scalar-blob", 1);
#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    {
        char *literal_sql = replace_once(
            type_sql,
            "fts_search9 match @SearchTerm",
            "fts_search9 match '(\"alpha\"*) OR (\"direct\"*)'"
        );
        char *literal_expected = replace_once(
            type_expected,
            "fts_search9 match dshadow_emby_fts_rewrite(@SearchTerm)",
            "fts_search9 match dshadow_emby_fts_rewrite('(\"alpha\"*) OR (\"direct\"*)')"
        );
        expect_sql(db, "literal-rhs-positive", literal_sql, -1, 2, literal_expected, 1);
        free(literal_sql);
        free(literal_expected);
    }
#endif
#ifdef EMBY_FTS_REWRITE_TEST_API
    expect_scalar_counter(db, type_sql);
#endif

    free(type_sql);
    free(numbered_sql);
    free(presentation_sql);
    free(type_expected);
    free(numbered_expected);
    free(presentation_expected);
    require_int("positive/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [positive]\n");
    return 0;
}

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
static int child_direct_test_api(void) {
    sqlite3 *db;
    char *type_sql;

    configure_env("0", "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    type_sql = make_emby_sql(0, "100", "100", "6", "7", 1);

#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    {
        char *type_expected = make_expected_sql(0, "100", "100", "6", "7", 1);
        char *literal_sql = replace_once(
            type_sql,
            "fts_search9 match @SearchTerm",
            "fts_search9 match '(\"alpha\"*) OR (\"direct\"*)'"
        );
        char *literal_expected = replace_once(
            type_expected,
            "fts_search9 match dshadow_emby_fts_rewrite(@SearchTerm)",
            "fts_search9 match dshadow_emby_fts_rewrite('(\"alpha\"*) OR (\"direct\"*)')"
        );
        expect_sql(db, "direct-literal-rhs-positive", literal_sql, -1, 2, literal_expected, 1);
        free(type_expected);
        free(literal_sql);
        free(literal_expected);
    }
#endif
#ifdef EMBY_FTS_REWRITE_TEST_API
    require_int("direct-duplicate-anchor-guard",
                emby_fts_rewrite_test_duplicate_anchor_guard(), 0);
    require_int("direct-string-anchor-immunity",
                emby_fts_rewrite_test_string_anchor_immunity(), 1);
    expect_scalar_counter(db, type_sql);
#endif

    free(type_sql);
    require_int("direct-test-api/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [direct-test-api]\n");
    return 0;
}

static int child_direct_fail_open_family(int movies) {
    sqlite3 *db;
    char *raw;
    enum direct_fault_mode faults[] = {
        DIRECT_FAULT_CANDIDATE_ERROR,
        DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT,
        DIRECT_FAULT_CANDIDATE_WRONG_TAIL
    };
    size_t i;
    sqlite3_stmt *baseline[64];
    int baseline_n = 0;
    sqlite3_stmt *bs;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
    seed_movies_latest_rows(db);
    raw = movies ? make_movies_latest_sql(1, "100", "42", "12")
                 : make_latest_sql("A.Id ", "12");
    for (bs = sqlite3_next_stmt(db, NULL); bs; bs = sqlite3_next_stmt(db, bs)) {
        if (baseline_n >= (int)(sizeof(baseline) / sizeof(baseline[0]))) {
            failf("FAIL [direct-fail-open/baseline-overflow]: too many open statements");
        }
        baseline[baseline_n++] = bs;
    }
    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        int rc;
        direct_original_sql = raw;
        direct_fault = faults[i];
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        rc = emby_fts_rewrite_prepare(db, raw, -1, 0, &stmt, &tail,
                                      FTS_REWRITE_PREPARE_V2);
        require_int("direct-fail-open/rc", rc, SQLITE_OK);
        require_int("direct-fail-open/candidate-calls", direct_candidate_calls, 1);
        require_int("direct-fail-open/original-calls", direct_original_calls, 1);
        require_str_eq("direct-fail-open/sql", sqlite3_sql(stmt), raw);
        if (tail != raw + strlen(raw)) {
            failf("FAIL [direct-fail-open/tail]: got=%ld want=%ld",
                  (long)(tail - raw), (long)strlen(raw));
        }
        {
            sqlite3_stmt *os;
            int found = 0;
            for (os = sqlite3_next_stmt(db, NULL); os; os = sqlite3_next_stmt(db, os)) {
                int in_baseline = 0;
                int k;
                if (os == stmt) {
                    found = 1;
                    continue;
                }
                for (k = 0; k < baseline_n; k++) {
                    if (os == baseline[k]) {
                        in_baseline = 1;
                        break;
                    }
                }
                if (!in_baseline) {
                    failf("FAIL [direct-fail-open/next-stmt]: candidate statement leaked");
                }
            }
            if (!found) {
                failf("FAIL [direct-fail-open/next-stmt]: expected statement missing");
            }
        }
        require_int("direct-fail-open/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    }
    {
        char *invalid = make_latest_sql("A.NoSuchColumn ", "12");
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        int rc;
        direct_original_sql = invalid;
        direct_fault = DIRECT_FAULT_CANDIDATE_ERROR;
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        rc = emby_fts_rewrite_prepare(db, invalid, -1, 0, &stmt, &tail,
                                      FTS_REWRITE_PREPARE_V2);
        require_int("direct-invalid-original/rc", rc, SQLITE_ERROR);
        require_int("direct-invalid-original/candidate-calls", direct_candidate_calls, 1);
        require_int("direct-invalid-original/original-calls", direct_original_calls, 1);
        if (stmt != NULL) failf("FAIL [direct-invalid-original/stmt]: expected NULL");
        free(invalid);
    }
    direct_original_sql = NULL;
    direct_fault = DIRECT_FAULT_NONE;
    free(raw);
    require_int("direct-fail-open/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [direct-fail-open-%s]\n", movies ? "movies" : "episodes");
    return 0;
}

static int child_direct_fail_open_episodes(void) {
    return child_direct_fail_open_family(0);
}

static int child_direct_fail_open_movies(void) {
    return child_direct_fail_open_family(1);
}

typedef struct direct_mixed_fail_open_case {
    const char *name;
    enum direct_fault_mode fault;
    int constrain_sql_limit;
    int candidate_calls;
    const char *skip_reason;
    int candidate_had_stmt;
    int candidate_bind_count;
    int candidate_column_count;
    int candidate_outer_index_count;
    int candidate_inner_index_count;
} direct_mixed_fail_open_case;

static int direct_stmt_is_baseline(
    sqlite3_stmt *stmt,
    sqlite3_stmt **baseline,
    int baseline_n
) {
    int i;
    for (i = 0; i < baseline_n; i++) {
        if (stmt == baseline[i]) return 1;
    }
    return 0;
}

static void require_direct_mixed_stmt_set(
    const char *case_name,
    sqlite3 *db,
    sqlite3_stmt **baseline,
    int baseline_n,
    sqlite3_stmt *result_stmt
) {
    sqlite3_stmt *open_stmt;
    int result_count = 0;

    for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
         open_stmt = sqlite3_next_stmt(db, open_stmt)) {
        if (open_stmt == result_stmt) {
            result_count++;
        } else if (!direct_stmt_is_baseline(open_stmt, baseline, baseline_n)) {
            failf("FAIL [direct-fail-open-mixed/%s/next-stmt]: candidate statement leaked",
                  case_name);
        }
    }
    if (result_count != 1) {
        failf("FAIL [direct-fail-open-mixed/%s/next-stmt]: got result_count=%d want=1",
              case_name, result_count);
    }
}

static void require_direct_mixed_baseline_only(
    const char *case_name,
    sqlite3 *db,
    sqlite3_stmt **baseline,
    int baseline_n
) {
    sqlite3_stmt *open_stmt;

    for (open_stmt = sqlite3_next_stmt(db, NULL); open_stmt;
         open_stmt = sqlite3_next_stmt(db, open_stmt)) {
        if (!direct_stmt_is_baseline(open_stmt, baseline, baseline_n)) {
            failf("FAIL [direct-fail-open-mixed/%s/post-finalize]: statement leaked",
                  case_name);
        }
    }
}

static int child_direct_fail_open_mixed(void) {
    static const direct_mixed_fail_open_case cases[] = {
        {"build-failure", DIRECT_FAULT_NONE, 1, 0, "build_failed", 0, -1, -1, -1, -1},
        {"rewritten-prepare-failure", DIRECT_FAULT_CANDIDATE_ERROR, 0, 1,
         "rewritten_prepare_failed", 0, -1, -1, 1, 1},
        {"partial-candidate", DIRECT_FAULT_CANDIDATE_ERROR_WITH_STMT, 0, 1,
         "rewritten_prepare_failed", 1, 0, 19, 1, 1},
        {"tail-mismatch", DIRECT_FAULT_CANDIDATE_WRONG_TAIL, 0, 1,
         "tail_mismatch", 1, 0, 19, 1, 1},
        {"accept-bind-mismatch", DIRECT_FAULT_MIXED_BIND_MISMATCH, 0, 1,
         "candidate_bind_mismatch", 1, 1, 1, 1, 1},
        {"accept-column-mismatch", DIRECT_FAULT_MIXED_COLUMN_MISMATCH, 0, 1,
         "candidate_column_mismatch", 1, 0, 1, 1, 1},
        {"accept-index-mismatch", DIRECT_FAULT_MIXED_INDEX_MISMATCH, 0, 1,
         "candidate_index_mismatch", 1, 0, 19, 0, 1}
    };
    sqlite3 *db;
    sqlite3_stmt *baseline[64];
    int baseline_n = 0;
    sqlite3_stmt *baseline_stmt;
    char *raw;
    char *expected;
    int prior_sql_limit;
    size_t i;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_mixed_latest_identity_rows(db);
    raw = make_mixed_latest_sql("42", "3");
    expected = make_mixed_latest_expected("42", "3");
    require_int("direct-fail-open-mixed/rewrite-longer-than-source",
                strlen(expected) > strlen(raw), 1);
    require_int("direct-fail-open-mixed/rewrite-length-fits-int",
                strlen(expected) <= (size_t)INT_MAX, 1);
    prior_sql_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    for (baseline_stmt = sqlite3_next_stmt(db, NULL); baseline_stmt;
         baseline_stmt = sqlite3_next_stmt(db, baseline_stmt)) {
        if (baseline_n >= (int)(sizeof(baseline) / sizeof(baseline[0]))) {
            failf("FAIL [direct-fail-open-mixed/baseline-overflow]: too many open statements");
        }
        baseline[baseline_n++] = baseline_stmt;
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const direct_mixed_fail_open_case *test_case = &cases[i];
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        char label[160];
        int rc;

        direct_original_sql = raw;
        direct_fault = test_case->fault;
        direct_candidate_calls = 0;
        direct_original_calls = 0;
        direct_candidate_input_len = 0;
        direct_candidate_had_stmt = 0;
        direct_candidate_bind_count = -1;
        direct_candidate_column_count = -1;
        direct_candidate_outer_index_count = -1;
        direct_candidate_inner_index_count = -1;
        direct_skipped_calls = 0;
        direct_skipped_reason = NULL;
        direct_skipped_mode = OBS_MODE_NONE;

        if (test_case->constrain_sql_limit) {
            int constrained_limit = (int)strlen(expected) - 1;
            require_int("direct-fail-open-mixed/build/source-fits-limit",
                        strlen(raw) <= (size_t)constrained_limit, 1);
            sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, constrained_limit);
            require_int("direct-fail-open-mixed/build/sql-limit",
                        sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1),
                        constrained_limit);
        }

        rc = emby_fts_rewrite_prepare(db, raw, -1, 0, &stmt, &tail,
                                      FTS_REWRITE_PREPARE_V2);
        if (test_case->constrain_sql_limit) {
            sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, prior_sql_limit);
        }

        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/rc", test_case->name);
        require_int(label, rc, SQLITE_OK);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/result-stmt",
                 test_case->name);
        require_int(label, stmt != NULL, 1);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/candidate-calls",
                 test_case->name);
        require_int(label, direct_candidate_calls, test_case->candidate_calls);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/original-calls",
                 test_case->name);
        require_int(label, direct_original_calls, 1);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/skipped-calls",
                 test_case->name);
        require_int(label, direct_skipped_calls, 1);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/skipped-reason",
                 test_case->name);
        require_str_eq(label, direct_skipped_reason, test_case->skip_reason);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/skipped-mode",
                 test_case->name);
        require_int(label, direct_skipped_mode, OBS_MODE_EMBY_MIXED_LATEST);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/candidate-length",
                 test_case->name);
        require_int(label, direct_candidate_input_len,
                    test_case->candidate_calls ? strlen(expected) : 0);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/candidate-had-stmt",
                 test_case->name);
        require_int(label, direct_candidate_had_stmt, test_case->candidate_had_stmt);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/candidate-binds",
                 test_case->name);
        require_int(label, direct_candidate_bind_count, test_case->candidate_bind_count);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/candidate-columns",
                 test_case->name);
        require_int(label, direct_candidate_column_count, test_case->candidate_column_count);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/outer-index-count",
                 test_case->name);
        require_int(label, direct_candidate_outer_index_count,
                    test_case->candidate_outer_index_count);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/inner-index-count",
                 test_case->name);
        require_int(label, direct_candidate_inner_index_count,
                    test_case->candidate_inner_index_count);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/source-sql",
                 test_case->name);
        require_str_eq(label, sqlite3_sql(stmt), raw);
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/source-tail",
                 test_case->name);
        require_int(label, tail == raw + strlen(raw), 1);
        require_direct_mixed_stmt_set(
            test_case->name, db, baseline, baseline_n, stmt
        );
        snprintf(label, sizeof(label), "direct-fail-open-mixed/%s/finalize",
                 test_case->name);
        require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
        require_direct_mixed_baseline_only(
            test_case->name, db, baseline, baseline_n
        );
    }

    direct_original_sql = NULL;
    direct_fault = DIRECT_FAULT_NONE;
    free(raw);
    free(expected);
    require_int("direct-fail-open-mixed/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [direct-fail-open-mixed]\n");
    return 0;
}
#endif

static int child_fts_env(const char *value, const char *label, int want_rewrite) {
    sqlite3 *db;
    char *sql;
    char *expected;

    configure_env(value, "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    expected = want_rewrite ? make_expected_sql(0, "100", "100", "6", "7", 1) : xasprintf("%s", sql);
    expect_sql(db, label, sql, -1, 2, expected, want_rewrite);
    free(sql);
    free(expected);
    require_int("env-off/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_path_negative(void) {
    const char *names[] = {"com.plexapp.plugins.library.db", "jellyfin.db", "not-target.db"};
    char non_ascii_name[256];
    char non_ascii_dir[512];
    char non_ascii_path[768];
    char *sql;
    size_t i;

    configure_env("0", "1", "1");
    make_temp_dir();
    sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3 *db = open_seeded_temp(names[i]);
        expect_sql(db, names[i], sql, -1, 2, sql, 0);
        require_int("path-negative/close", sqlite3_close(db), SQLITE_OK);
    }
    {
        sqlite3 *db = open_db_path(":memory:");
        exec_sql(db, "schema-memory", "CREATE TABLE t(x);");
        expect_sql(db, "memory", "SELECT 1", -1, 2, "SELECT 1", 0);
        require_int("memory/close", sqlite3_close(db), SQLITE_OK);
    }
    {
        sqlite3 *db;
        int rc = snprintf(non_ascii_dir, sizeof(non_ascii_dir),
                          "/tmp/emby-fts-rewrite-smoke-%ld/emb\303\251",
                          (long)getpid());
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_dir)) {
            failf("FATAL: non-ASCII temp dir too long");
        }
        if (mkdir(non_ascii_dir, 0700) != 0 && errno != EEXIST) {
            failf("FATAL: mkdir(%s) failed: %s", non_ascii_dir, strerror(errno));
        }
        rc = snprintf(non_ascii_name, sizeof(non_ascii_name), "emb\303\251/library.db");
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_name)) {
            failf("FATAL: non-ASCII temp name too long");
        }
        db = open_seeded_temp(non_ascii_name);
        expect_sql(db, "non-ascii-path", sql, -1, 2, sql, 0);
        require_int("path-negative/non-ascii-close", sqlite3_close(db), SQLITE_OK);
        temp_path(non_ascii_path, sizeof(non_ascii_path), non_ascii_name);
        unlink(non_ascii_path);
        snprintf(non_ascii_path, sizeof(non_ascii_path),
                 "/tmp/emby-fts-rewrite-smoke-%ld/emb\303\251/library.db-wal",
                 (long)getpid());
        unlink(non_ascii_path);
        snprintf(non_ascii_path, sizeof(non_ascii_path),
                 "/tmp/emby-fts-rewrite-smoke-%ld/emb\303\251/library.db-shm",
                 (long)getpid());
        unlink(non_ascii_path);
        rmdir(non_ascii_dir);
    }
    free(sql);
    cleanup_temp_dir();
    printf("PASS [path-negative]\n");
    return 0;
}

static int child_nonmatch(void) {
    sqlite3 *db;
    char *sql;
    char *type_expected;
    char *fast;
    char *duplicate;
    char *literal;
    char *concat_rhs;
    char *named_colon_rhs;
    char *named_func_rhs;
    char *over_cap;
    char *over_cap_sql;
    char *ambiguous_after_l1;
    char *ambiguous_after_t1;
    char *ambiguous_after_t2;
    char *ambiguous_after_l2;
    char *ambiguous_after_l1_expected;
    char *ambiguous_after_t1_expected;
    char *ambiguous_after_t2_expected;
    char *ambiguous_after_l2_expected;
    char *embedded;
    static const char after_l1[] =
        ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t1[] =
        ") union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t2[] =
        ")))select";
    static const char after_l2[] =
        ")) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)";
    const char *tail = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;

    configure_env("0", "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    type_expected = make_expected_sql(0, "100", "100", "6", "7", 1);
    fast = replace_once(sql, "A.Id in WithAncestors", "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid=A.Id)");
    duplicate = replace_once(sql, "fts_search9 match @SearchTerm", "fts_search9 match @SearchTerm AND fts_search9 match @SearchTerm2");
    literal = replace_once(sql, "fts_search9 match @SearchTerm", "fts_search9 match 'alpha'");
    concat_rhs = replace_once(sql, "fts_search9 match @SearchTerm", "fts_search9 match @SearchTerm || ''");
    named_colon_rhs = replace_once(sql, "fts_search9 match @SearchTerm", "fts_search9 match $SearchTerm::suffix");
    named_func_rhs = replace_once(sql, "fts_search9 match @SearchTerm", "fts_search9 match $SearchTerm(extra)");
    over_cap = make_numeric_list(66000);
    over_cap_sql = make_emby_sql(0, over_cap, over_cap, "6", "7", 1);
    ambiguous_after_l1 = add_anchor_literal_predicate(sql, after_l1);
    ambiguous_after_t1 = add_anchor_literal_predicate(sql, after_t1);
    ambiguous_after_t2 = add_anchor_literal_predicate(sql, after_t2);
    ambiguous_after_l2 = add_anchor_literal_predicate(sql, after_l2);
    ambiguous_after_l1_expected = add_anchor_literal_predicate(type_expected, after_l1);
    ambiguous_after_t1_expected = add_anchor_literal_predicate(type_expected, after_t1);
    ambiguous_after_t2_expected = add_anchor_literal_predicate(type_expected, after_t2);
    ambiguous_after_l2_expected = add_anchor_literal_predicate(type_expected, after_l2);
    expect_sql(db, "fast-form", fast, -1, 2, fast, 0);
    expect_sql(db, "duplicate-fts", duplicate, -1, 2, duplicate, 0);
#ifndef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
    expect_sql(db, "literal-rhs", literal, -1, 2, literal, 0);
#endif
    expect_sql(db, "match-rhs-concat", concat_rhs, -1, 2, concat_rhs, 0);
    expect_sql(db, "match-rhs-named-colon", named_colon_rhs, -1, 2, named_colon_rhs, 0);
    expect_sql(db, "match-rhs-named-func", named_func_rhs, -1, 2, named_func_rhs, 0);
    expect_sql(db, "over-cap-slot", over_cap_sql, -1, 2, over_cap_sql, 0);
    expect_sql(db, "ambiguous-after-l1", ambiguous_after_l1, -1, 2, ambiguous_after_l1_expected, 1);
    expect_sql(db, "ambiguous-after-t1", ambiguous_after_t1, -1, 2, ambiguous_after_t1_expected, 1);
    expect_sql(db, "ambiguous-after-t2", ambiguous_after_t2, -1, 2, ambiguous_after_t2_expected, 1);
    expect_sql(db, "ambiguous-after-l2", ambiguous_after_l2, -1, 2, ambiguous_after_l2_expected, 1);
    stmt = prepare_entry(db, "semicolon", "SELECT 1; SELECT 2", -1, 2, &tail);
    require_str_eq("semicolon/sql", sqlite3_sql(stmt), "SELECT 1;");
    if (!tail || strcmp(tail, " SELECT 2") != 0) {
        failf("FAIL [semicolon/tail]: tail=\"%s\"", tail ? tail : "(null)");
    }
    require_int("semicolon/finalize", sqlite3_finalize(stmt), SQLITE_OK);

    embedded = xasprintf("SELECT 1%c SELECT 2", '\0');
    rc = sqlite3_prepare_v2(db, embedded, (int)strlen("SELECT 1") + 1 + (int)strlen(" SELECT 2"),
                            &stmt, &tail);
    require_int("embedded-nul/rc", rc, SQLITE_OK);
    require_str_eq("embedded-nul/sql", sqlite3_sql(stmt), "SELECT 1");
    require_int("embedded-nul/finalize", sqlite3_finalize(stmt), SQLITE_OK);

    free(sql);
    free(type_expected);
    free(fast);
    free(duplicate);
    free(literal);
    free(concat_rhs);
    free(named_colon_rhs);
    free(named_func_rhs);
    free(over_cap);
    free(over_cap_sql);
    free(ambiguous_after_l1);
    free(ambiguous_after_t1);
    free(ambiguous_after_t2);
    free(ambiguous_after_l2);
    free(ambiguous_after_l1_expected);
    free(ambiguous_after_t1_expected);
    free(ambiguous_after_t2_expected);
    free(ambiguous_after_l2_expected);
    free(embedded);
    require_int("nonmatch/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [nonmatch]\n");
    return 0;
}

static int child_fixture_canary(void) {
    sqlite3 *db;
    static const char *fixtures[] = {
        "slow-type",
        "slow-presentation",
        "presentation-user42",
        "l1-l2-mismatch",
        "mutated-type-slots",
        "fast-exists",
        "string-anchor-passthrough",
        "comment-anchor-passthrough",
        "fanout-people",
        "fanout-links-search",
        "fanout-browse",
        "fanout-favorites",
        "latest-limit12",
        "latest-limit16",
        "latest-movies-played-limit12",
        "latest-star-projection-negative",
        "latest-capture-miss-negative",
        "latest-aggregate-projection-negative",
        "latest-over-negative",
        "latest-series-browse-negative",
        "latest-explain-prefix-negative"
    };
    size_t i;

    configure_env("0", "0", "0");
    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
    seed_movies_latest_rows(db);
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        expect_fixture(db, fixtures[i]);
    }
    require_int("fixture-canary/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [fixture-canary]\n");
    return 0;
}

static int child_row_parity(void) {
    sqlite3 *original_db;
    sqlite3 *rewritten_db;
    char *sql;
    char *expected;
    unsigned int original_mask;
    unsigned int rewritten_mask;

    configure_env("0", "1", "1");
    make_temp_dir();
    original_db = open_seeded_temp("not-target.db");
    rewritten_db = open_seeded_temp("library.db");
    sql = make_emby_sql(1, "100", "100", "6", "7", 1);
    expected = make_expected_sql(1, "100", "100", "6", "7", 1);

    contract_parity_require(
        original_db, rewritten_db, contract_prepare_v2, "emby-fts-contract",
        sql, sql, expected, bind_search_term, (void *)"(\"alpha\"*)",
        accept_emby_fts_legal_difference, NULL
    );

    original_mask = collect_fts_id_mask(
        original_db, "row-parity-original", sql, -1, 0, "(\"alpha\"*)"
    );
    rewritten_mask = collect_fts_id_mask(
        rewritten_db, "row-parity-rewritten", sql, (int)strlen(sql) + 1, 1,
        "(\"alpha\"*)"
    );
    require_int("row-parity/original-membership", (int)original_mask, 0x1f);
    require_int("row-parity/candidate-membership", (int)rewritten_mask, 0x1f);
    require_emby_fts_ordered_contract(
        original_db, rewritten_db, "row-parity/ordered-contract", sql, expected,
        "(\"alpha\"*)"
    );
    original_mask = collect_fts_id_mask(
        original_db, "row-parity-or-original", sql, -1, 0,
        "(\"alpha\"*) OR (\"alpha\"*)"
    );
    rewritten_mask = collect_fts_id_mask(
        rewritten_db, "row-parity-or-rewritten", sql,
        (int)strlen(sql) + 1, 1, "(\"alpha\"*) OR (\"alpha\"*)"
    );
    require_int("row-parity-or/original-membership", (int)original_mask, 0x1f);
    require_int("row-parity-or/candidate-membership", (int)rewritten_mask, 0x1f);
    require_emby_fts_ordered_contract(
        original_db, rewritten_db, "row-parity-or/ordered-contract", sql, expected,
        "(\"alpha\"*) OR (\"alpha\"*)"
    );

    free(expected);
    free(sql);
    require_int("row-parity/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("row-parity/rewrite-close", sqlite3_close(rewritten_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [row-parity]\n");
    return 0;
}

static int child_complex_resume_watched_progress_row_parity(void) {
    sqlite3 *original_db;
    sqlite3 *rewritten_db;
    char *sql;
    char *expected;
    char original_ids[128];
    char rewritten_ids[128];

    configure_env("1", "0", NULL);
    make_temp_dir();
    original_db = open_seeded_temp("not-target.db");
    rewritten_db = open_seeded_temp("library.db");
    seed_complex_resume_watched_progress_parity_rows(original_db);
    seed_complex_resume_watched_progress_parity_rows(rewritten_db);
    sql = make_resume_sql();
    expected = make_resume_expected(sql, "100", "1");

    contract_parity_require(
        original_db, rewritten_db, contract_prepare_v2, "emby-fanout-contract",
        sql, sql, expected, NULL, NULL, NULL, NULL
    );

    collect_int_column(original_db, "complex-resume-watched-progress-row-original", sql, sql, -1, 2,
                       original_ids, sizeof(original_ids));
    collect_int_column(rewritten_db, "complex-resume-watched-progress-row-rewritten", sql, expected,
                       (int)strlen(sql) + 1, 2, rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("complex-resume-watched-progress-row-parity/ids", rewritten_ids, original_ids);
    require_str_eq("complex-resume-watched-progress-row-parity/expected", rewritten_ids, "102,100,");

    free(sql);
    free(expected);
    require_int("complex-resume-watched-progress-row-parity/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("complex-resume-watched-progress-row-parity/rewrite-close", sqlite3_close(rewritten_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [complex-resume-watched-progress-row-parity]\n");
    return 0;
}

static int child_fanout_env(
    const char *value,
    const char *label,
    int env_enables_rewrite
) {
    sqlite3 *db;
    char *browse_sql;
    char *browse_one;
    char *browse_two;
    char *browse_repl;
    char *browse_expected;

    configure_env("1", value, "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    browse_sql = make_browse_sql();
    browse_one = make_exists_links_one("100", "6");
    browse_two = make_exists_links_two("100", "7");
    browse_repl = xasprintf("(%s OR %s)", browse_one, browse_two);
    browse_expected = replace_once(
        browse_sql, "A.Id in WithItemLinkItemIds", browse_repl
    );
    expect_sql(
        db, label, browse_sql, -1, 2,
        env_enables_rewrite ? browse_expected : browse_sql, 0
    );
    free(browse_expected);
    free(browse_repl);
    free(browse_two);
    free(browse_one);
    free(browse_sql);
    require_int("fanout-env/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_fanout_matrix(void) {
    sqlite3 *db;
    char *browse_sql;
    char *browse_one;
    char *browse_two;
    char *browse_repl;
    char *browse_expected;
    char *favorites_sql;
    char *favorites_ancestor;
    char *favorites_one;
    char *favorites_repl;
    char *favorites_expected;
    char *resume_sql;
    char *resume_expected;
    char *people_sql;
    char *people_repl;
    char *people_expected;
    char *links_sql;
    char *links_repl;
    char *links_expected;
    char *fts_sql;

    configure_env("1", "0", NULL);
    make_temp_dir();
    db = open_seeded_temp("library.db");
    browse_sql = make_browse_sql();
    browse_one = make_exists_links_one("100", "6");
    browse_two = make_exists_links_two("100", "7");
    browse_repl = xasprintf("(%s OR %s)", browse_one, browse_two);
    browse_expected = replace_once(browse_sql, "A.Id in WithItemLinkItemIds", browse_repl);
    expect_sql(db, "fanout-browse", browse_sql, -1, 2, browse_expected, 0);
    expect_sql(db, "fanout-browse-v3", browse_sql, -1, 3, browse_expected, 0);

    favorites_sql = make_favorites_sql();
    favorites_ancestor = make_ancestor_exists_splice("100");
    favorites_one = make_exists_links_one("100", "6");
    favorites_repl = xasprintf("(%s OR %s)", favorites_ancestor, favorites_one);
    favorites_expected = replace_once(favorites_sql, "(A.Id in WithAncestors OR A.Id in WithItemLinkItemIds)", favorites_repl);
    expect_sql(db, "fanout-favorites", favorites_sql, -1, 2, favorites_expected, 0);

    resume_sql = make_resume_sql();
    resume_expected = make_resume_expected(resume_sql, "100", "1");
    expect_sql(db, "fanout-resume", resume_sql, -1, 2, resume_expected, 0);

    people_sql = make_people_sql();
    people_repl = make_exists_people("100");
    people_expected = replace_once(people_sql, "A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)", people_repl);
    expect_sql(db, "fanout-people", people_sql, -1, 2, people_expected, 0);

    links_sql = make_links_search_sql("2,3");
    links_repl = make_exists_links_one("100", "2,3");
    links_expected = replace_once(links_sql, "A.Id in WithItemLinkItemIds", links_repl);
    expect_sql(db, "fanout-links-search", links_sql, -1, 2, links_expected, 0);

    fts_sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    expect_sql(db, "fanout-search-shape-nomisfire", fts_sql, -1, 2, fts_sql, 0);

    free(browse_sql);
    free(browse_one);
    free(browse_two);
    free(browse_repl);
    free(browse_expected);
    free(favorites_sql);
    free(favorites_ancestor);
    free(favorites_one);
    free(favorites_repl);
    free(favorites_expected);
    free(resume_sql);
    free(resume_expected);
    free(people_sql);
    free(people_repl);
    free(people_expected);
    free(links_sql);
    free(links_repl);
    free(links_expected);
    free(fts_sql);
    require_int("fanout-matrix/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [fanout-matrix]\n");
    return 0;
}

static int child_emby_slow_search_matrix(void) {
    sqlite3 *db;
    char *resume_simple_count_sql;
    char *resume_simple_count_expected;
    char *resume_simple_bare_sql;
    char *resume_simple_bare_expected;
    char *resume_unplayed_sql;
    char *resume_bad_l1_sql;
    char *resume_bad_limit_sql;
    char *resume_simple_string_sql;
    char *resume_simple_string_expected;
    char *resume_simple_string_only_sql;
    char *resume_simple_duplicate_sql;
    char *resume_complex_sql;
    char *resume_complex_expected;
    char *resume_complex_no_not_null_sql;
    char *resume_complex_comment_sql;
    char *resume_complex_comment_expected;
    char *resume_0065_sql;
    char *resume_0065_expected;
    char *resume_bound_user_sql;
    char *resume_named_user_sql;
    char *resume_derived_param_sql;
    char *resume_conflict_user_sql;
    char *resume_duplicate_sql;
    char *similar_sql;
    char *similar_expected;
    char *similar_comment_sql;
    char *similar_comment_expected;
    char *similar_comment_only_sql;
    char *people_sql;
    char *people_repl;
    char *people_expected;
    char *links_sql;
    char *links_repl;
    char *links_expected;
    char *fts_sql;
    char *latest_sql;

    configure_env("1", "0", NULL);
    make_temp_dir();
    db = open_seeded_temp("library.db");

    resume_simple_count_sql = make_resume_simple_sql(1, "12");
    resume_simple_count_expected = make_resume_simple_expected(resume_simple_count_sql);
    expect_sql(db, "resume-simple-count", resume_simple_count_sql, -1, 2,
               resume_simple_count_expected, 0);

    resume_simple_bare_sql = make_resume_simple_sql(0, "24");
    resume_simple_bare_expected = make_resume_simple_expected(resume_simple_bare_sql);
    expect_sql(db, "resume-simple-bare", resume_simple_bare_sql, -1, 2,
               resume_simple_bare_expected, 0);

    resume_unplayed_sql = replace_once(
        resume_simple_count_sql,
        "UserDatas.playbackPositionTicks > 0",
        "Coalesce(UserDatas.played, 0)=0"
    );
    expect_sql(db, "resume-simple-unplayed-negative", resume_unplayed_sql, -1, 2,
               resume_unplayed_sql, 0);
    expect_sql(db, "resume-simple-shape-07-547bab3e-negative",
               EMBY_RESUME_SIMPLE_LEFT_JOIN_UNPLAYED_SHAPE_07_SQL, -1, 2,
               EMBY_RESUME_SIMPLE_LEFT_JOIN_UNPLAYED_SHAPE_07_SQL, 0);

    resume_bad_l1_sql = replace_once(resume_simple_count_sql, "AncestorId in (100)",
                                     "AncestorId in (@AncestorId)");
    expect_sql(db, "resume-simple-bad-l1-negative", resume_bad_l1_sql, -1, 2,
               resume_bad_l1_sql, 0);

    resume_bad_limit_sql = replace_once(resume_simple_count_sql, "LIMIT 12", "LIMIT @Limit");
    expect_sql(db, "resume-simple-bad-limit-negative", resume_bad_limit_sql, -1, 2,
               resume_bad_limit_sql, 0);

    resume_simple_string_sql = replace_once(
        resume_simple_count_sql,
        "AND A.Id in WithAncestors",
        "AND 'A.Id in WithAncestors' IS NOT NULL AND A.Id in WithAncestors"
    );
    resume_simple_string_expected = replace_once(
        resume_simple_count_expected,
        "AND EXISTS (SELECT 1 FROM AncestorIds2",
        "AND 'A.Id in WithAncestors' IS NOT NULL AND EXISTS (SELECT 1 FROM AncestorIds2"
    );
    expect_sql(db, "resume-simple-string-embedded-immunity", resume_simple_string_sql,
               -1, 2, resume_simple_string_expected, 0);

    resume_simple_string_only_sql = replace_once(
        resume_simple_count_sql,
        "AND A.Id in WithAncestors",
        "AND 'A.Id in WithAncestors' IS NOT NULL"
    );
    expect_sql(db, "resume-simple-string-only-negative", resume_simple_string_only_sql,
               -1, 2, resume_simple_string_only_sql, 0);

    resume_simple_duplicate_sql = replace_once(
        resume_simple_count_sql,
        "AND A.Id in WithAncestors",
        "AND A.Id in WithAncestors AND A.Id in WithAncestors"
    );
    expect_sql(db, "resume-simple-duplicate-negative", resume_simple_duplicate_sql, -1, 2,
               resume_simple_duplicate_sql, 0);

    resume_complex_sql = make_resume_sql();
    resume_complex_expected = make_resume_expected(resume_complex_sql, "100", "1");
    expect_sql(db, "resume-complex-count", resume_complex_sql, -1, 2,
               resume_complex_expected, 0);

    resume_complex_no_not_null_sql = replace_once(
        resume_complex_sql,
        " AND LastWatchedEpisodes.LastPlayedDateInt not null",
        ""
    );
    expect_sql(db, "resume-complex-missing-lastwatched-not-null-negative",
               resume_complex_no_not_null_sql, -1, 2,
               resume_complex_no_not_null_sql, 0);

    resume_complex_comment_sql = replace_once(
        resume_complex_sql,
        "AND A.Id in WithAncestors Group by",
        "AND /* A.Id in WithAncestors */ A.Id in WithAncestors Group by"
    );
    resume_complex_comment_expected = replace_once(
        resume_complex_expected,
        "AND EXISTS (SELECT 1 FROM AncestorIds2",
        "AND /* A.Id in WithAncestors */ EXISTS (SELECT 1 FROM AncestorIds2"
    );
    expect_sql(db, "resume-complex-comment-embedded-immunity",
               resume_complex_comment_sql, -1, 2, resume_complex_comment_expected, 0);

    resume_0065_sql = make_resume_0065_sql();
    resume_0065_expected = make_resume_expected(resume_0065_sql, "100", "13");
    expect_sql(db, "resume-complex-0065", resume_0065_sql, -1, 2,
               resume_0065_expected, 0);

    resume_bound_user_sql = replace_once(resume_complex_sql, "UserDatas.UserId=1",
                                         "UserDatas.UserId=?1");
    expect_sql(db, "resume-complex-bound-user-negative", resume_bound_user_sql, -1, 2,
               resume_bound_user_sql, 0);

    resume_named_user_sql = replace_once(resume_complex_sql, "userdatas.userid=1",
                                         "userdatas.userid=@UserId");
    expect_sql(db, "resume-complex-named-user-negative", resume_named_user_sql, -1, 2,
               resume_named_user_sql, 0);

    resume_derived_param_sql = replace_once(resume_complex_sql, "UserDatas_N.UserId=1",
                                            "UserDatas_N.UserId=:UserId");
    expect_sql(db, "resume-complex-derived-param-negative", resume_derived_param_sql, -1, 2,
               resume_derived_param_sql, 0);

    resume_conflict_user_sql = replace_once(resume_complex_sql, "userdatas.userid=1",
                                            "userdatas.userid=2");
    expect_sql(db, "resume-complex-conflicting-user-negative", resume_conflict_user_sql, -1, 2,
               resume_conflict_user_sql, 0);

    resume_duplicate_sql = replace_once(
        resume_complex_sql,
        "AND A.Id in WithAncestors",
        "AND A.Id in WithAncestors AND A.Id in WithAncestors"
    );
    expect_sql(db, "resume-complex-duplicate-negative", resume_duplicate_sql, -1, 2,
               resume_duplicate_sql, 0);

    similar_sql = make_similar_sql();
    similar_expected = make_similar_expected(similar_sql);
    expect_sql(db, "fanout-similar", similar_sql, -1, 2, similar_expected, 0);

    similar_comment_sql = replace_once(
        similar_sql,
        "AND A.Id in WithAncestors",
        "AND /* A.Id in WithAncestors */ A.Id in WithAncestors"
    );
    similar_comment_expected = replace_once(
        similar_expected,
        "AND EXISTS (SELECT 1 FROM AncestorIds2",
        "AND /* A.Id in WithAncestors */ EXISTS (SELECT 1 FROM AncestorIds2"
    );
    expect_sql(db, "similar-comment-embedded-immunity", similar_comment_sql,
               -1, 2, similar_comment_expected, 0);

    similar_comment_only_sql = replace_once(
        similar_sql,
        "AND A.Id in WithAncestors",
        "AND /* A.Id in WithAncestors */ 1=1"
    );
    expect_sql(db, "similar-comment-only-negative", similar_comment_only_sql, -1, 2,
               similar_comment_only_sql, 0);

    people_sql = make_people_sql();
    people_repl = make_exists_people("100");
    people_expected = replace_once(
        people_sql,
        "A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)",
        people_repl
    );
    expect_sql(db, "similar-people-negative", people_sql, -1, 2, people_expected, 0);

    links_sql = make_links_search_sql("2,3");
    links_repl = make_exists_links_one("100", "2,3");
    links_expected = replace_once(links_sql, "A.Id in WithItemLinkItemIds", links_repl);
    expect_sql(db, "similar-links-search-negative", links_sql, -1, 2, links_expected, 0);
    /* Similar must not claim this shape on its SimB_Ids guard; links_search owns
       the exact two-level links/type-count membership splice. */
    {
        char *type_count_expected = make_link_type_count_shape_05_expected();
        char *bad_t1 = replace_once(
            EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "Type in (6,4,3,2) union",
            "Type in (6,'bad') union"
        );
        char *bad_t2 = replace_once(
            EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "Type in (7,0,1,5,6,2)))select",
            "Type in (7,'bad')))select"
        );
        char *duplicate_membership = replace_once(
            EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
            "AND A.Id in WithItemLinkItemIds Group by",
            "AND A.Id in WithItemLinkItemIds AND A.Id in WithItemLinkItemIds Group by"
        );

        expect_sql(db, "links-type-count-shape-05-two-level",
                   EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL, -1, 2,
                   type_count_expected, 0);
        expect_sql(db, "links-type-count-shape-05-bad-t1",
                   bad_t1, -1, 2, bad_t1, 0);
        expect_sql(db, "links-type-count-shape-05-bad-t2",
                   bad_t2, -1, 2, bad_t2, 0);
        expect_sql(db, "links-type-count-shape-05-duplicate-membership",
                   duplicate_membership, -1, 2, duplicate_membership, 0);
        free(type_count_expected);
        free(bad_t1);
        free(bad_t2);
        free(duplicate_membership);
    }

    fts_sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    expect_sql(db, "fanout-search-fts-negative", fts_sql, -1, 2, fts_sql, 0);

    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    expect_sql(db, "fanout-latest-negative", latest_sql, -1, 2, latest_sql, 0);
    require_absent("fanout-latest-negative/indexed-by", latest_sql,
                   "INDEXED BY idx_dshadow_emby_latest_gk_dc");

    free(resume_simple_count_sql);
    free(resume_simple_count_expected);
    free(resume_simple_bare_sql);
    free(resume_simple_bare_expected);
    free(resume_unplayed_sql);
    free(resume_bad_l1_sql);
    free(resume_bad_limit_sql);
    free(resume_simple_string_sql);
    free(resume_simple_string_expected);
    free(resume_simple_string_only_sql);
    free(resume_simple_duplicate_sql);
    free(resume_complex_sql);
    free(resume_complex_expected);
    free(resume_complex_no_not_null_sql);
    free(resume_complex_comment_sql);
    free(resume_complex_comment_expected);
    free(resume_0065_sql);
    free(resume_0065_expected);
    free(resume_bound_user_sql);
    free(resume_named_user_sql);
    free(resume_derived_param_sql);
    free(resume_conflict_user_sql);
    free(resume_duplicate_sql);
    free(similar_sql);
    free(similar_expected);
    free(similar_comment_sql);
    free(similar_comment_expected);
    free(similar_comment_only_sql);
    free(people_sql);
    free(people_repl);
    free(people_expected);
    free(links_sql);
    free(links_repl);
    free(links_expected);
    free(fts_sql);
    free(latest_sql);
    require_int("emby-slow-search-matrix/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [emby-slow-search-matrix]\n");
    return 0;
}

static int child_dashboard_off_value(const char *value, const char *label) {
    sqlite3 *db;
    char *latest_sql;
    char *movies_sql;
    char *mixed_sql;

    configure_env("1", "1", value[0] ? value : NULL);
    if (value[0] == 0 &&
        setenv("SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE", "", 1) != 0) {
        failf("setenv empty dashboard failed");
    }
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_movies_latest_rows(db);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    mixed_sql = make_mixed_latest_sql("42", "3");
    expect_sql(db, label, latest_sql, -1, 2, latest_sql, 0);
    expect_sql(db, label, movies_sql, -1, 2, movies_sql, 0);
    expect_sql(db, label, mixed_sql, -1, 2, mixed_sql, 0);
    free(latest_sql);
    free(movies_sql);
    free(mixed_sql);
    require_int("dashboard-off-value/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_dashboard_default_off(void) {
    sqlite3 *db;
    char *latest_sql;
    char *movies_sql;
    char *mixed_sql;

    configure_env("1", "1", NULL);
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_movies_latest_rows(db);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    mixed_sql = make_mixed_latest_sql("42", "3");
    expect_sql(db, "dashboard-default-off", latest_sql, -1, 2, latest_sql, 0);
    expect_sql(db, "dashboard-default-off-movies", movies_sql, -1, 2, movies_sql, 0);
    expect_sql(db, "dashboard-default-off-mixed", mixed_sql, -1, 2, mixed_sql, 0);
    free(latest_sql);
    free(movies_sql);
    free(mixed_sql);
    require_int("dashboard-default-off/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [dashboard-default-off]\n");
    return 0;
}

static int child_dashboard_matrix(void) {
    sqlite3 *db;
    sqlite3 *original_db;
    char *latest_sql;
    char *latest_expected;
    char *latest16_sql;
    char *latest16_expected;
    char *distinct_sql;
    char *aggregate_sql;
    char *over_sql;
    char *movies_sql[3];
    char *movies_expected[3];
    char *mixed_sql;
    char *mixed_expected;
    typed_rows vendor_rows;
    typed_rows candidate_rows;
    int i;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_movies_latest_rows(db);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    latest_expected = make_latest_expected("A.Id,A.SeriesName,A.SortName ", "12");
    require_absent("dashboard-latest-original/indexed-by", latest_sql,
                   "INDEXED BY idx_dshadow_emby_latest_gk_dc");
    require_int("dashboard-latest-expected/outer-index-count",
                count_occurrences(latest_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk"),
                1);
    require_int("dashboard-latest-expected/inner-index-count",
                count_occurrences(latest_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_gk_dc"),
                1);
    require_contains("dashboard-latest-expected/ranked", latest_expected,
                     "WITH ranked(id, dc, gk) AS MATERIALIZED (");
    require_absent("dashboard-latest-expected/keys", latest_expected, "WITH keys(gk)");
    require_absent("dashboard-latest-expected/picked", latest_expected, "picked AS MATERIALIZED");
    require_absent("dashboard-latest-expected/exact-groups", latest_expected,
                   "exact_groups AS MATERIALIZED");
    expect_sql(db, "dashboard-latest", latest_sql, -1, 2, latest_expected, 0);
    expect_sql(db, "dashboard-latest-nbyte-with-nul", latest_sql,
               (int)strlen(latest_sql) + 1, 2, latest_expected, 0);
    latest16_sql = make_latest_sql("A.Id ", "16");
    latest16_expected = make_latest_expected("A.Id ", "16");
    require_int("dashboard-latest-limit16/outer-index-count",
                count_occurrences(latest16_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk"),
                1);
    require_int("dashboard-latest-limit16/inner-index-count",
                count_occurrences(latest16_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_gk_dc"),
                1);
    require_contains("dashboard-latest-limit16/ranked", latest16_expected,
                     "WITH ranked(id, dc, gk) AS MATERIALIZED (");
    require_absent("dashboard-latest-limit16/keys", latest16_expected, "WITH keys(gk)");
    require_absent("dashboard-latest-limit16/picked", latest16_expected,
                   "picked AS MATERIALIZED");
    require_absent("dashboard-latest-limit16/exact-groups", latest16_expected,
                   "exact_groups AS MATERIALIZED");
    expect_sql(db, "dashboard-latest-limit16", latest16_sql, -1, 2, latest16_expected, 0);
    mixed_sql = make_mixed_latest_sql("42", "3");
    mixed_expected = make_mixed_latest_expected("42", "3");
    expect_mixed_latest_sql(
        db, "dashboard-mixed-same-control", mixed_sql, mixed_expected, 0, 0
    );

    distinct_sql = make_latest_sql("DISTINCT A.Id ", "12");
    expect_sql(db, "dashboard-distinct-negative", distinct_sql, -1, 2, distinct_sql, 0);
    aggregate_sql = make_latest_sql("MAX(A.DateCreated) ", "12");
    expect_sql(db, "dashboard-aggregate-negative", aggregate_sql, -1, 2, aggregate_sql, 0);
    over_sql = make_latest_sql("count(*) OVER() AS TotalRecordCount,A.Id ", "12");
    expect_sql(db, "dashboard-over-negative", over_sql, -1, 2, over_sql, 0);

    movies_sql[0] = make_movies_latest_sql(1, "100", "42", "12");
    movies_sql[1] = make_movies_latest_sql(1, "100", "42", "16");
    movies_sql[2] = make_movies_latest_sql(1, "100", "42", "20");
    movies_expected[0] = make_movies_latest_expected("100", "42", "12");
    movies_expected[1] = make_movies_latest_expected("100", "42", "16");
    movies_expected[2] = make_movies_latest_expected("100", "42", "20");
    for (i = 0; i < 3; i++) {
        expect_sql(db, "dashboard-movies-limits", movies_sql[i], -1, 2,
                   movies_expected[i], 0);
    }

    original_db = open_seeded_temp_with_dashboard_indexes("not-target.db", 0, 0, 0, 0);
    seed_movies_latest_rows(original_db);
    vendor_rows = collect_typed_rows(original_db, "movies-row-original",
                                     movies_sql[2], movies_sql[2]);
    candidate_rows = collect_typed_rows(db, "movies-row-rewritten",
                                        movies_sql[2], movies_expected[2]);
    require_ordered_full_row_identity("movies-row-identity", &vendor_rows, &candidate_rows);
    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);

    require_int("dashboard/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("dashboard/index-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 0, 1, 1);
    seed_movies_latest_rows(db);
    expect_sql(db, "dashboard-episodes-date-index-missing", latest_sql, -1, 2,
               latest_sql, 0);
    require_int("dashboard/episodes-date-index-missing-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 0, 1, 1, 1);
    seed_movies_latest_rows(db);
    expect_sql(db, "dashboard-episodes-group-index-missing", latest_sql, -1, 2,
               latest_sql, 0);
    require_int("dashboard/episodes-group-index-missing-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 0, 0, 1, 1);
    seed_movies_latest_rows(db);
    expect_sql(db, "dashboard-episodes-both-indexes-missing", latest_sql, -1, 2,
               latest_sql, 0);
    require_int("dashboard/episodes-both-indexes-missing-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 0, 1);
    seed_movies_latest_rows(db);
    expect_sql(db, "dashboard-movies-outer-missing", movies_sql[0], -1, 2,
               movies_sql[0], 0);
    require_int("dashboard/movies-outer-missing-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 0);
    seed_movies_latest_rows(db);
    expect_sql(db, "dashboard-movies-inner-missing", movies_sql[0], -1, 2,
               movies_sql[0], 0);
    require_int("dashboard/movies-inner-missing-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
    seed_movies_latest_rows(db);
    require_int("dashboard/probe-authorizer-set",
                sqlite3_set_authorizer(db, deny_sqlite_master_read, NULL), SQLITE_OK);
    expect_sql(db, "dashboard-episodes-probe-error", latest_sql, -1, 2, latest_sql, 0);
    expect_sql(db, "dashboard-movies-probe-error", movies_sql[0], -1, 2,
               movies_sql[0], 0);
    require_int("dashboard/probe-authorizer-clear",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    require_int("dashboard/probe-error-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    free(latest_sql);
    free(latest_expected);
    free(latest16_sql);
    free(latest16_expected);
    free(distinct_sql);
    free(aggregate_sql);
    free(over_sql);
    free(mixed_sql);
    free(mixed_expected);
    for (i = 0; i < 3; i++) {
        free(movies_sql[i]);
        free(movies_expected[i]);
    }
    printf("PASS [dashboard-matrix]\n");
    return 0;
}

static int child_dashboard_index_definition_gate(void) {
    sqlite3 *db;
    FILE *capture;
    char *log_output;
    char *episodes_sql;
    char *episodes_expected;
    char *movies_sql;
    char *movies_expected;
    int saved_stderr_fd;

    configure_env_observable("1", "1", "0");
    if (setenv("SQLITE3_DISABLE_STMT_TRACE", "1", 1) != 0 ||
        unsetenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL") != 0) {
        failf("FATAL: exec-child observable env failed: %s", strerror(errno));
    }
    make_temp_dir();
    episodes_sql = make_latest_sql("A.Id ", "12");
    episodes_expected = make_latest_expected("A.Id ", "12");
    movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    movies_expected = make_movies_latest_expected("100", "42", "12");
    capture = begin_stderr_capture(&saved_stderr_fd);

    db = open_seeded_temp_with_latest_indexes("library.db", 0, 1);
    exec_sql(db, "malformed-episodes-group-index",
        "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
        "ON MediaItems (Id) WHERE Type = 8;"
    );
    expect_sql(db, "malformed-episodes-group-index-fail-open", episodes_sql,
               -1, 2, episodes_sql, 0);
    require_int("malformed-episodes-group-index/close", sqlite3_close(db), SQLITE_OK);

    db = open_seeded_temp_with_latest_indexes("library.db", 1, 0);
    exec_sql(db, "malformed-episodes-date-index",
        "CREATE INDEX idx_dshadow_emby_latest_episodes_dcn_gk "
        "ON MediaItems (Id) WHERE Type = 8;"
    );
    expect_sql(db, "malformed-episodes-date-index-fail-open", episodes_sql,
               -1, 2, episodes_sql, 0);
    require_int("malformed-episodes-date-index/close", sqlite3_close(db), SQLITE_OK);

    db = open_seeded_temp_with_latest_indexes("library.db", 1, 1);
    expect_sql(db, "canonical-episodes-indexes-rewrite", episodes_sql, -1, 2,
               episodes_expected, 0);
    require_int("canonical-episodes-indexes/close", sqlite3_close(db), SQLITE_OK);

    db = open_seeded_temp_with_dashboard_indexes("library.db", 0, 0, 0, 1);
    exec_sql(db, "malformed-movies-outer-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_dcn_puk "
        "ON MediaItems (Id) WHERE Type = 5;"
    );
    expect_sql(db, "malformed-movies-outer-index-fail-open", movies_sql,
               -1, 2, movies_sql, 0);
    require_int("malformed-movies-outer-index/close", sqlite3_close(db), SQLITE_OK);

    db = open_seeded_temp_with_dashboard_indexes("library.db", 0, 0, 1, 0);
    exec_sql(db, "malformed-movies-inner-index",
        "CREATE INDEX idx_dshadow_emby_latest_movies_puk_dc_cover "
        "ON MediaItems (Id) WHERE Type = 5;"
    );
    expect_sql(db, "malformed-movies-inner-index-fail-open", movies_sql,
               -1, 2, movies_sql, 0);
    require_int("malformed-movies-inner-index/close", sqlite3_close(db), SQLITE_OK);

    db = open_seeded_temp_with_dashboard_indexes("library.db", 0, 0, 1, 1);
    expect_sql(db, "canonical-movies-indexes-rewrite", movies_sql, -1, 2,
               movies_expected, 0);
    require_int("canonical-movies-indexes/close", sqlite3_close(db), SQLITE_OK);

    log_output = end_stderr_capture(capture, saved_stderr_fd);
    require_contains(
        "malformed-episodes-index-definitions/index-missing-log", log_output,
        "reason=index_missing mode=dashboard+episodes_latest"
    );
    require_contains(
        "canonical-episodes-index-definitions/applied-log", log_output,
        "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
    );
    require_contains(
        "malformed-movies-index-definitions/index-missing-log", log_output,
        "reason=index_missing mode=dashboard+movies_latest"
    );
    require_contains(
        "canonical-movies-index-definitions/applied-log", log_output,
        "event=rewrite_applied target=emby mode=dashboard+movies_latest"
    );

    free(log_output);
    free(episodes_sql);
    free(episodes_expected);
    free(movies_sql);
    free(movies_expected);
    cleanup_temp_dir();
    printf("PASS [dashboard-index-definition-gate]\n");
    return 0;
}

#define TEST_MOVIES_LIST_PREFIX \
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
#define TEST_MOVIES_FROM \
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
#define TEST_MOVIES_TAIL \
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12"

static const char TEST_E1_FAVORITES[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
    "count(*) OVER() AS TotalRecordCount,A.Id,A.Name,UserDatas.IsFavorite "
    "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 and UserDatas.IsFavorite=1 "
    "where A.Type=8 AND UserDatas.IsFavorite=1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.SeriesName collate NATURALSORT ASC,A.SortName collate NATURALSORT ASC LIMIT 30";
static const char TEST_M1_FAVORITES[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select "
    "count(*) OVER() AS TotalRecordCount,A.Id,A.Name,UserDatas.IsFavorite "
    "from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 and UserDatas.IsFavorite=1 "
    "where A.Type=5 AND UserDatas.IsFavorite=1 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.SortName collate NATURALSORT ASC LIMIT 30";
static const char TEST_TYPE5_RESUME[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select A.Id,A.Name "
    "from mediaitems A left join MediaItems AS LastWatchedEpisodes ON LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
    "left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
    "where A.Type=5 AND UserDatas.PlaybackPositionTicks>0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 12";
static const char TEST_M2_PREMIERE_TAIL[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.EndDate,A.CommunityRating,A.Name,A.Path,A.PremiereDate,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM
    "where A.Type=5 AND A.PremiereDate>=1752355308 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.ProductionYear DESC,A.PremiereDate DESC,A.SortName collate NATURALSORT DESC LIMIT 30";
static const char TEST_EPISODES_DATE_WINDOW_OFFSET[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (15,16,17,18,19,20,3923210) )select "
    "A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=5 "
    "where A.Type=8 AND A.PremiereDate>=1782906237 AND A.PremiereDate <1784121997 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.ProductionYear DESC,A.PremiereDate DESC,COALESCE(A.SortParentIndexNumber,A.ParentIndexNumber) ASC,COALESCE(A.SortIndexNumber,A.IndexNumber) ASC LIMIT 12 OFFSET 12";
static const char TEST_MOVIES_CORRELATED_RANDOM_IMAGE[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=838031 AND ItemId=A.Id )select "
    "A.Id from mediaitems A where A.Type=5 AND A.Images like '%Primary%' AND A.Id in WithAncestors ORDER BY RANDOM() ASC LIMIT 12";
static const char TEST_MOVIES_AGGREGATE_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "MAX(A.DateCreated) AS DateCreated,A.Id " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_WINDOW_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "count(*) OVER() AS TotalRecordCount,A.Id " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_BARE_STAR_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "* " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_PAREN_PROJECTION[] =
    TEST_MOVIES_LIST_PREFIX "(A.Id) AS Id,A.Name " TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_BAD_LIMIT[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM
    "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 11";
static const char TEST_MOVIES_SCALAR_BIND[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=? )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_SCALAR_NONINTEGER[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=+100 )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_SCALAR_OPERATOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=100 OR AncestorId=200 )select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_MIXED_ANCESTOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "ScalarCarrier AS (WITH WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=200 )select itemid FROM WithAncestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_DUPLICATE_LIST_ANCESTOR[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "Other AS (WITH WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (200) )select itemid FROM WithAncestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_AMBIGUOUS_SELECT[] =
    "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) ),"
    "Other1 AS (WITH withancestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (200) )select itemid FROM withancestors),"
    "Other2 AS (WITH withancestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (300) )select itemid FROM withancestors) select "
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;
static const char TEST_MOVIES_MISSING_INDEX[] =
    TEST_MOVIES_LIST_PREFIX
    "A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex "
    TEST_MOVIES_FROM TEST_MOVIES_TAIL;

#undef TEST_MOVIES_LIST_PREFIX
#undef TEST_MOVIES_FROM
#undef TEST_MOVIES_TAIL

static void expect_movies_latest_semantics(sqlite3 *db) {
    static const char *keys[] = {
        "contract-decorrelated", "contract-mixed-null", "contract-all-null"
    };
    static const sqlite3_int64 dates[] = {300, 250, 0};
    char *sql = make_movies_latest_sql_form(
        1, "302", 0, "A.PresentationUniqueKey,A.DateCreated ", "42", "12"
    );
    sqlite3_stmt *stmt = prepare_entry(db, "movies-latest-semantics", sql, -1, 2, NULL);
    const char *saved_sql = sqlite3_sql(stmt);
    int row;

    if (!saved_sql || strcmp(saved_sql, sql) == 0) {
        failf("FAIL [movies-latest-semantics/rewrite-fired]: candidate SQL did not change");
    }
    require_int("movies-latest-semantics/columns", sqlite3_column_count(stmt), 2);
    require_int("movies-latest-semantics/binds", sqlite3_bind_parameter_count(stmt), 0);
    for (row = 0; row < 3; row++) {
        const unsigned char *key;
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [movies-latest-semantics/row-%d]: rc=%d want=%d err=%s",
                  row, rc, SQLITE_ROW, sqlite3_errmsg(db));
        }
        key = sqlite3_column_text(stmt, 0);
        if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT || !key ||
            strcmp((const char *)key, keys[row]) != 0) {
            failf("FAIL [movies-latest-semantics/key-%d]: type=%d got=%s want=%s",
                  row, sqlite3_column_type(stmt, 0),
                  key ? (const char *)key : "(null)", keys[row]);
        }
        if (row == 2) {
            if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                failf("FAIL [movies-latest-semantics/date-%d]: type=%d want=%d",
                      row, sqlite3_column_type(stmt, 1), SQLITE_NULL);
            }
        } else if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
                   sqlite3_column_int64(stmt, 1) != dates[row]) {
            failf("FAIL [movies-latest-semantics/date-%d]: type=%d got=%lld "
                  "want_type=%d want=%lld", row, sqlite3_column_type(stmt, 1),
                  (long long)sqlite3_column_int64(stmt, 1), SQLITE_INTEGER,
                  (long long)dates[row]);
        }
    }
    require_int("movies-latest-semantics/done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("movies-latest-semantics/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    free(sql);
}

static int child_dashboard_fix_b_c(void) {
    static const char *wide_ancestors[] = {"100", "100,200"};
    static const char *compact_ancestors[] = {
        "201,202,203,204", "9,10,11,12,13,14"
    };
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;
    sqlite3 *missing_index_db;
    size_t i;

    configure_env("1", "1", "0");
    make_temp_dir();
    vendor_db = open_seeded_temp_with_dashboard_indexes("not-target.db", 0, 0, 0, 0);
    candidate_db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
    seed_movies_latest_rows(vendor_db);
    seed_movies_latest_rows(candidate_db);
    exec_sql(vendor_db, "dashboard-scalar-minus-one-vendor-seed",
             "INSERT INTO AncestorIds2 VALUES(7,-1,0),(3,-1,0);");
    exec_sql(candidate_db, "dashboard-scalar-minus-one-candidate-seed",
             "INSERT INTO AncestorIds2 VALUES(7,-1,0),(3,-1,0);");
    expect_movies_latest_semantics(candidate_db);

    {
        static const sqlite3_int64 candidate_ids[] = {9303, 9312, 9321};
        movies_latest_contract contract = {
            vendor_db,
            candidate_db,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION
        };
        char *raw = make_movies_latest_sql(1, "302", "42", "12");
        char *expected = make_movies_latest_expected("302", "42", "12");

        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-representative-contract",
            raw, raw, expected, NULL, NULL,
            accept_movies_latest_legal_difference, &contract
        );
        require_movies_latest_vendor_rows(
            vendor_db, "emby-dashboard-movies-vendor-contract", raw,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION
        );
        require_movies_latest_candidate_rows(
            candidate_db, "emby-dashboard-movies-candidate-contract", raw,
            expected, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, candidate_ids,
            (int)(sizeof(candidate_ids) / sizeof(candidate_ids[0]))
        );
        free(raw);
        free(expected);
    }

    for (i = 0; i < sizeof(wide_ancestors) / sizeof(wide_ancestors[0]); i++) {
        char label[64];
        char *raw = make_movies_latest_sql_form(
            1, wide_ancestors[i], 0, EMBY_MOVIES_LATEST_WIDE_PROJECTION, "42", "20"
        );
        char *expected = make_movies_latest_expected_projection(
            wide_ancestors[i], EMBY_MOVIES_LATEST_WIDE_PROJECTION, "42", "20"
        );
        char *outer_select = xasprintf(
            " ) SELECT %sFROM ranked AS R", EMBY_MOVIES_LATEST_WIDE_PROJECTION
        );
        snprintf(label, sizeof(label), "dashboard-movies-wide-m%lu", (unsigned long)i + 3);
        require_contains(label, expected, outer_select);
        expect_sql(candidate_db, label, raw, -1, 2, expected, 0);
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2, label,
            raw, raw, expected, NULL, NULL, NULL, NULL
        );
        free(raw);
        free(expected);
        free(outer_select);
    }

    for (i = 0; i < sizeof(compact_ancestors) / sizeof(compact_ancestors[0]); i++) {
        char *raw = make_movies_latest_sql(1, compact_ancestors[i], "42", "12");
        char *expected = make_movies_latest_expected(compact_ancestors[i], "42", "12");
        expect_sql(candidate_db, i == 0 ? "dashboard-movies-m5-byte-identical"
                                       : "dashboard-movies-m6-byte-identical",
                   raw, -1, 2, expected, 0);
        free(raw);
        free(expected);
    }

    {
        char *oracle = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 0, "12"
        );
        char *scalar = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", 1, "12"
        );
        char *expected = make_latest_expected_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "100", "12"
        );
        expect_sql(candidate_db, "dashboard-episodes-e2-scalar", scalar, -1, 2, expected, 0);
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-episodes-contract", oracle, scalar, expected,
            NULL, NULL, NULL, NULL
        );
        free(oracle);
        free(scalar);
        free(expected);
    }

    {
        char ids[32];
        char *scalar = make_latest_sql_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "-1", 1, "12"
        );
        char *expected = make_latest_expected_form(
            EMBY_EPISODES_LATEST_WIDE_PROJECTION, "-1", "12"
        );

        expect_sql(candidate_db, "dashboard-episodes-scalar-minus-one",
                   scalar, -1, 2, expected, 0);
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-episodes-scalar-minus-one-contract",
            scalar, scalar, expected, NULL, NULL, NULL, NULL
        );
        collect_int_column(
            candidate_db, "dashboard-episodes-scalar-minus-one-ids",
            scalar, expected, -1, 0, ids, sizeof(ids)
        );
        require_str_eq("dashboard-episodes-scalar-minus-one-exact-id", ids, "7,");
        free(scalar);
        free(expected);
    }

    {
        char *oracle = make_movies_latest_sql(1, "100", "42", "12");
        char *scalar = make_movies_latest_sql_form(
            1, "100", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        char *expected = make_movies_latest_expected("100", "42", "12");
        expect_sql(candidate_db, "dashboard-movies-m7-scalar", scalar, -1, 2, expected, 0);
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-singleton-contract", oracle, scalar, expected,
            NULL, NULL, NULL, NULL
        );
        free(oracle);
        free(scalar);
        free(expected);
    }

    {
        char ids[32];
        char *scalar = make_movies_latest_sql_form(
            1, "-1", 1, EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
        );
        char *expected = make_movies_latest_expected("-1", "42", "12");

        expect_sql(candidate_db, "dashboard-movies-scalar-minus-one",
                   scalar, -1, 2, expected, 0);
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2,
            "emby-dashboard-movies-scalar-minus-one-contract",
            scalar, scalar, expected, NULL, NULL, NULL, NULL
        );
        collect_int_column(
            candidate_db, "dashboard-movies-scalar-minus-one-ids",
            scalar, expected, -1, 0, ids, sizeof(ids)
        );
        require_str_eq("dashboard-movies-scalar-minus-one-exact-id", ids, "3,");
        free(scalar);
        free(expected);
    }

    {
        FILE *capture;
        char *log_output;
        int saved_stderr_fd;
        capture = begin_stderr_capture(&saved_stderr_fd);
        expect_dashboard_negative(candidate_db, "dashboard-e1-favorites-silent",
                                  TEST_E1_FAVORITES, "where A.Type=8 ", 1);
        expect_dashboard_negative(candidate_db, "dashboard-m1-favorites-silent",
                                  TEST_M1_FAVORITES, "where A.Type=5 ", 1);
        expect_dashboard_negative(candidate_db, "dashboard-type5-resume-silent",
                                  TEST_TYPE5_RESUME, "LastWatchedEpisodes", 2);
        expect_dashboard_negative(candidate_db, "dashboard-m2-tail-miss",
                                  TEST_M2_PREMIERE_TAIL, "A.PremiereDate>=1752355308", 1);
        log_output = end_stderr_capture(capture, saved_stderr_fd);
        require_int("dashboard-fix-b-c/episodes-silent",
                    count_occurrences(log_output,
                        "reason=capture_miss mode=dashboard+episodes_latest"), 0);
        require_int("dashboard-fix-b-c/movies-only-m2-capture",
                    count_occurrences(log_output,
                        "reason=capture_miss mode=dashboard+movies_latest"), 1);
        free(log_output);
    }

    expect_dashboard_negative(candidate_db, "dashboard-movies-aggregate-projection",
                              TEST_MOVIES_AGGREGATE_PROJECTION, "MAX(A.DateCreated)", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-window-projection",
                              TEST_MOVIES_WINDOW_PROJECTION, "count(*) OVER()", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-bare-star-projection",
                              TEST_MOVIES_BARE_STAR_PROJECTION, "select * from", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-parenthesized-projection",
                              TEST_MOVIES_PAREN_PROJECTION, "select (A.Id)", 1);

    {
        static const struct {
            const char *label;
            const char *ancestor_slot;
            const char *boundary_needle;
        } scalar_negative_cases[] = {
            {"dashboard-movies-scalar-minus-two", "-2", "AncestorId=-2 )select"},
            {"dashboard-movies-scalar-leading-space", " -1", "AncestorId= -1 )select"},
            {"dashboard-movies-scalar-split-sign", "- 1", "AncestorId=- 1 )select"},
            {"dashboard-movies-scalar-expression", "-1+0", "AncestorId=-1+0 )select"},
            {"dashboard-movies-scalar-minus-one-or", "-1 OR AncestorId=200",
             "AncestorId=-1 OR AncestorId=200 )select"},
            {"dashboard-movies-scalar-minus-one-correlated", "-1 AND ItemId=A.Id",
             "AncestorId=-1 AND ItemId=A.Id )select"},
        };

        for (i = 0; i < sizeof(scalar_negative_cases) / sizeof(scalar_negative_cases[0]); i++) {
            char *sql = make_movies_latest_sql_form(
                1, scalar_negative_cases[i].ancestor_slot, 1,
                EMBY_MOVIES_LATEST_COMPACT_PROJECTION, "42", "12"
            );
            expect_dashboard_negative(
                candidate_db, scalar_negative_cases[i].label, sql,
                scalar_negative_cases[i].boundary_needle, 1
            );
            free(sql);
        }
    }

    expect_dashboard_negative(candidate_db, "dashboard-movies-scalar-bind",
                              TEST_MOVIES_SCALAR_BIND, "AncestorId=?", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-bad-limit",
                              TEST_MOVIES_BAD_LIMIT, "LIMIT 11", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-scalar-noninteger",
                              TEST_MOVIES_SCALAR_NONINTEGER, "AncestorId=+100", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-scalar-operator",
                              TEST_MOVIES_SCALAR_OPERATOR, "AncestorId=100 OR AncestorId=200", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-mixed-ancestor",
                              TEST_MOVIES_MIXED_ANCESTOR,
                              "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId=", 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-duplicate-list-ancestor",
                              TEST_MOVIES_DUPLICATE_LIST_ANCESTOR,
                              "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (", 2);
    expect_dashboard_negative(candidate_db, "dashboard-movies-ambiguous-select",
                              TEST_MOVIES_AMBIGUOUS_SELECT, ") )select ", 2);
    expect_dashboard_negative(candidate_db, "dashboard-episodes-date-window-offset",
                              TEST_EPISODES_DATE_WINDOW_OFFSET, "OFFSET 12", 1);
    require_int("dashboard-movies-correlated-random-image/images-boundary",
                count_occurrences(TEST_MOVIES_CORRELATED_RANDOM_IMAGE,
                                  "A.Images like '%Primary%'"), 1);
    require_int("dashboard-movies-correlated-random-image/order-boundary",
                count_occurrences(TEST_MOVIES_CORRELATED_RANDOM_IMAGE,
                                  "ORDER BY RANDOM()"), 1);
    expect_dashboard_negative(candidate_db, "dashboard-movies-correlated-random-image",
                              TEST_MOVIES_CORRELATED_RANDOM_IMAGE,
                              "AncestorId=838031 AND ItemId=A.Id", 1);

    require_int("dashboard-fix-b-c/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
    require_int("dashboard-fix-b-c/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    missing_index_db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 0, 1);
    seed_movies_latest_rows(missing_index_db);
    expect_dashboard_negative(missing_index_db, "dashboard-movies-missing-index",
                              TEST_MOVIES_MISSING_INDEX,
                              "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0", 1);
    require_int("dashboard-fix-b-c/missing-index-close",
                sqlite3_close(missing_index_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [dashboard-fix-b-c]\n");
    return 0;
}

static void seed_movies_latest_rows(sqlite3 *db) {
    exec_sql(db, "movies-matrix-seed",
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<30) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 5000+i,5,'matrix-hot-'||i,'/matrix/hot/'||i,1900+i,1000000+5000+i,9000+i,'img-hot-'||i,300000-i,printf('mx-hot-%02d',i),7000+i FROM n;"
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<24) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 6000+i,5,'matrix-typical-'||i,'/matrix/typical/'||i,1900+i,1000000+6000+i,9000+i,'img-typical-'||i,200000-i,printf('mx-typical-%02d',i),8000+i FROM n;"
        "INSERT INTO AncestorIds2 SELECT Id,100,0 FROM MediaItems WHERE Id BETWEEN 5001 AND 5030;"
        "INSERT INTO AncestorIds2 SELECT Id,200,0 FROM MediaItems WHERE Id BETWEEN 6001 AND 6024;"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,(Id%1000)%2,((Id%1000)%5=0),Id*10,(Id%1000)%3,(Id%1000)%4 "
        "FROM MediaItems WHERE Id BETWEEN 5001 AND 5030 OR Id BETWEEN 6001 AND 6024;"
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9301,5,'contract-middle-id','/contract/9301',2031,9301,1,'i9301',200,'contract-decorrelated',19301),"
        "(9302,5,'contract-oldest','/contract/9302',2032,9302,1,'i9302',100,'contract-decorrelated',19302),"
        "(9303,5,'contract-newest','/contract/9303',2033,9303,1,'i9303',300,'contract-decorrelated',19303),"
        "(9311,5,'contract-null','/contract/9311',2034,9311,1,'i9311',NULL,'contract-mixed-null',19311),"
        "(9312,5,'contract-mixed-newest','/contract/9312',2035,9312,1,'i9312',250,'contract-mixed-null',19312),"
        "(9321,5,'contract-all-null-a','/contract/9321',2036,9321,1,'i9321',NULL,'contract-all-null',19321),"
        "(9322,5,'contract-all-null-b','/contract/9322',2037,9322,1,'i9322',NULL,'contract-all-null',19322);"
        "INSERT INTO AncestorIds2 SELECT Id,302,0 FROM MediaItems WHERE Id BETWEEN 9301 AND 9322;"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,0,0,0,0,0 FROM MediaItems WHERE Id BETWEEN 9301 AND 9322;"
    );
}

static void seed_movies_latest_expanded_rows(sqlite3 *db) {
    exec_sql(db, "movies-expanded-seed",
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<10) "
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) "
        "SELECT 9000+i,5,'boundary-'||i,'/expanded/'||i,2000+i,900000+i,9900+i,'boundary-img-'||i,10000-i,'boundary-'||i,10000+i FROM n;"
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9101,5,'singleton','/e/9101',2001,9101,'',NULL,900,'p-single',11101),"
        "(9102,5,'dup-middle-id','/e/9102',2002,9102,1,'i9102',800,'p-dup',11102),"
        "(9103,5,'dup-oldest','/e/9103',2003,9103,1,'i9103',600,'p-dup',11103),"
        "(9104,5,'equal-min-id','/e/9104',2004,9104,1,'i9104',600,'p-equal',11104),"
        "(9105,5,'equal-other','/e/9105',2005,9105,1,'i9105',600,'p-equal',11105),"
        "(9106,5,'mixed-null','',NULL,9106,1,NULL,NULL,'p-mixed',11106),"
        "(9107,5,'mixed-value','/e/9107',2007,9107,1,'i9107',500,'p-mixed',11107),"
        "(9108,5,'all-null-min','/e/9108',2008,9108,1,'i9108',NULL,'p-all-null',11108),"
        "(9109,5,'all-null-other','/e/9109',2009,9109,1,'i9109',NULL,'p-all-null',11109),"
        "(9110,5,'null-puk-min','/e/9110',2010,9110,1,'i9110',400,NULL,11110),"
        "(9111,5,'null-puk-other','/e/9111',2011,9111,1,'i9111',450,NULL,11111),"
        "(9112,5,'missing-userdata','/e/9112',2012,9112,1,'i9112',390,'p-missing',11112),"
        "(9113,5,'played-zero','/e/9113',2013,9113,1,'i9113',380,'p-played-zero',11113),"
        "(9114,5,'played-one','/e/9114',2014,9114,1,'i9114',370,'p-played-one',11114),"
        "(9115,5,'xb-invisible-min','/e/9115',2015,9115,1,'i9115',100,'p-xb',11115),"
        "(9116,5,'xb-visible-next','/e/9116',2016,9116,1,'i9116',200,'p-xb',11116),"
        "(9117,5,'ub-played-min','/e/9117',2017,9117,1,'i9117',110,'p-ub',11117),"
        "(9118,5,'ub-unplayed-next','/e/9118',2018,9118,1,'i9118',210,'p-ub',11118),"
        "(9119,5,'dup-newest','/e/9119',2019,9119,1,'i9119',900,'p-dup',11119);"
        "INSERT INTO AncestorIds2 SELECT Id,300,0 FROM MediaItems WHERE Type=5 AND Id>=9001 AND Id<>9115;"
        "INSERT INTO AncestorIds2 VALUES(9115,999,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,Id%2,(Id=9114 OR Id=9117),Id*10,Id%3,Id%4 FROM MediaItems WHERE Type=5 AND Id>=9001 AND Id<>9112;"
    );
}

static void seed_movies_latest_rewrite_order_rows(sqlite3 *db) {
    exec_sql(db, "movies-rewrite-order-seed",
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9201,5,'tie-b','/e/9201',2021,9201,1,'i9201',100,'p-tie-b',11201),"
        "(9202,5,'tie-a','/e/9202',2022,9202,1,'i9202',100,'p-tie-a',11202),"
        "(9203,5,'null-b','/e/9203',2023,9203,1,'i9203',NULL,'p-null-b',11203),"
        "(9204,5,'null-a','/e/9204',2024,9204,1,'i9204',NULL,'p-null-a',11204);"
        "INSERT INTO AncestorIds2 VALUES(9201,301,0),(9202,301,0),(9203,301,0),(9204,301,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,0,0,0,0,0 FROM MediaItems WHERE Id BETWEEN 9201 AND 9204;"
    );
}

static void seed_episodes_latest_semantic_rows(sqlite3 *db) {
    exec_sql(db, "episodes-semantic-seed",
        "INSERT INTO MediaItems(Id,Type,Name,Path,ProductionYear,RunTimeTicks,ParentId,Images,DateCreated,SeriesPresentationUniqueKey,PresentationUniqueKey,UserDataKeyId) VALUES"
        "(9401,8,'ep-singleton','/ep/9401',2001,9401,1,'ep9401',900,'ep-single',NULL,19401),"
        "(9402,8,'ep-dup-old','/ep/9402',2002,9402,1,'ep9402',700,'ep-dup',NULL,19402),"
        "(9403,8,'ep-dup-new','/ep/9403',2003,9403,1,'ep9403',800,'ep-dup',NULL,19403),"
        "(9404,8,'ep-equal-min-id','/ep/9404',2004,9404,1,'ep9404',600,'ep-equal',NULL,19404),"
        "(9405,8,'ep-equal-other','/ep/9405',2005,9405,1,'ep9405',600,'ep-equal',NULL,19405),"
        "(9406,8,'ep-mixed-null','/ep/9406',2006,9406,1,'ep9406',NULL,'ep-mixed',NULL,19406),"
        "(9407,8,'ep-mixed-value','/ep/9407',2007,9407,1,'ep9407',500,'ep-mixed',NULL,19407),"
        "(9408,8,'ep-all-null-min','/ep/9408',2008,9408,1,'ep9408',NULL,'ep-all-null',NULL,19408),"
        "(9409,8,'ep-all-null-other','/ep/9409',2009,9409,1,'ep9409',NULL,'ep-all-null',NULL,19409),"
        "(9410,8,'ep-null-gk-old','/ep/9410',2010,9410,1,'ep9410',400,NULL,NULL,19410),"
        "(9411,8,'ep-null-gk-new','/ep/9411',2011,9411,1,'ep9411',450,NULL,NULL,19411),"
        "(9412,8,'ep-missing-userdata','/ep/9412',2012,9412,1,'ep9412',390,'ep-missing',NULL,19412),"
        "(9413,8,'ep-played-zero','/ep/9413',2013,9413,1,'ep9413',390,'ep-played-zero',NULL,19413),"
        "(9414,8,'ep-played-one','/ep/9414',2014,9414,1,'ep9414',370,'ep-played-one',NULL,19414),"
        "(9415,8,'ep-xb-invisible-new','/ep/9415',2015,9415,1,'ep9415',300,'ep-xb',NULL,19415),"
        "(9416,8,'ep-xb-visible','/ep/9416',2016,9416,1,'ep9416',200,'ep-xb',NULL,19416),"
        "(9417,8,'ep-ub-played-new','/ep/9417',2017,9417,1,'ep9417',310,'ep-ub',NULL,19417),"
        "(9418,8,'ep-ub-unplayed','/ep/9418',2018,9418,1,'ep9418',210,'ep-ub',NULL,19418);"
        "INSERT INTO AncestorIds2 SELECT Id,303,0 FROM MediaItems WHERE Id BETWEEN 9401 AND 9418 AND Id<>9415;"
        "INSERT INTO AncestorIds2 VALUES(9415,999,0);"
        "INSERT INTO UserDatas(UserDataKeyId,UserId,IsFavorite,Played,PlaybackPositionTicks,AudioStreamIndex,SubtitleStreamIndex) "
        "SELECT UserDataKeyId,42,Id%2,(Id=9414 OR Id=9417),Id*10,Id%3,Id%4 "
        "FROM MediaItems WHERE Id BETWEEN 9401 AND 9418 AND Id<>9412;"
    );
}

static unsigned int episodes_latest_fixture_group_bit(sqlite3_int64 id) {
    if (id == 9401) return 1u;
    if (id >= 9402 && id <= 9403) return 2u;
    if (id >= 9404 && id <= 9405) return 4u;
    if (id >= 9406 && id <= 9407) return 8u;
    if (id >= 9408 && id <= 9409) return 16u;
    if (id >= 9410 && id <= 9411) return 32u;
    if (id == 9412) return 64u;
    if (id == 9413) return 128u;
    if (id >= 9415 && id <= 9416) return 256u;
    if (id >= 9417 && id <= 9418) return 512u;
    return 0;
}

static int episodes_latest_row_is_eligible_max(sqlite3 *db, sqlite3_int64 id) {
    static const char sql[] =
        "SELECT EXISTS(SELECT 1 FROM MediaItems AS A WHERE A.Id=?1 AND A.Type=8 "
        "AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId=303) "
        "AND NOT EXISTS (SELECT 1 FROM UserDatas AS U WHERE U.UserDataKeyId=A.UserDataKeyId AND U.UserId=42 AND U.played<>0) "
        "AND NOT EXISTS (SELECT 1 FROM MediaItems AS B WHERE B.Type=8 "
        "AND coalesce(B.SeriesPresentationUniqueKey,B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey) "
        "AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated) "
        "AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId=303) "
        "AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=42 AND UB.played<>0)))";
    sqlite3_stmt *stmt = NULL;
    int value;

    require_int("episodes-semantic/oracle-prepare",
                sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    require_int("episodes-semantic/oracle-bind",
                sqlite3_bind_int64(stmt, 1, id), SQLITE_OK);
    require_int("episodes-semantic/oracle-row", sqlite3_step(stmt), SQLITE_ROW);
    value = sqlite3_column_int(stmt, 0);
    require_int("episodes-semantic/oracle-done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("episodes-semantic/oracle-finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return value;
}

static void require_episodes_latest_semantics(sqlite3 *vendor_db, sqlite3 *candidate_db) {
    static const sqlite3_int64 expected_candidate_ids[] = {
        9401, 9403, 9404, 9407, 9411, 9412, 9413, 9418, 9416, 9408
    };
    char *raw = make_latest_sql_form(
        EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, "303", 0, "20"
    );
    char *expected = make_latest_expected_form(
        EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, "303", "20"
    );
    const char *tail = NULL;
    sqlite3_stmt *stmt;
    unsigned int group_mask = 0;
    int row;
    int rc;

    stmt = prepare_entry(candidate_db, "episodes-semantic/candidate", raw, -1, 2, &tail);
    require_str_eq("episodes-semantic/candidate-sql", sqlite3_sql(stmt), expected);
    if (tail != raw + strlen(raw)) {
        failf("FAIL [episodes-semantic/candidate-tail]: got=%ld want=%ld",
              (long)(tail - raw), (long)strlen(raw));
    }
    for (row = 0; row < (int)(sizeof(expected_candidate_ids) / sizeof(expected_candidate_ids[0])); row++) {
        sqlite3_int64 id;
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            failf("FAIL [episodes-semantic/candidate-row-%d]: rc=%d want=%d err=%s",
                  row, rc, SQLITE_ROW, sqlite3_errmsg(candidate_db));
        }
        id = sqlite3_column_int64(stmt, 0);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER ||
            id != expected_candidate_ids[row]) {
            failf("FAIL [episodes-semantic/candidate-id-%d]: type=%d got=%lld want=%lld",
                  row, sqlite3_column_type(stmt, 0), (long long)id,
                  (long long)expected_candidate_ids[row]);
        }
        require_dashboard_latest_fixture_row(
            "episodes-semantic", "candidate", candidate_db, stmt,
            EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, id
        );
    }
    require_int("episodes-semantic/candidate-done", sqlite3_step(stmt), SQLITE_DONE);
    require_int("episodes-semantic/candidate-finalize", sqlite3_finalize(stmt), SQLITE_OK);

    tail = NULL;
    stmt = prepare_entry(vendor_db, "episodes-semantic/vendor", raw, -1, 2, &tail);
    require_str_eq("episodes-semantic/vendor-sql", sqlite3_sql(stmt), raw);
    if (tail != raw + strlen(raw)) {
        failf("FAIL [episodes-semantic/vendor-tail]: got=%ld want=%ld",
              (long)(tail - raw), (long)strlen(raw));
    }
    row = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        unsigned int bit = episodes_latest_fixture_group_bit(id);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || bit == 0) {
            failf("FAIL [episodes-semantic/vendor-id-%d]: type=%d got=%lld",
                  row, sqlite3_column_type(stmt, 0), (long long)id);
        }
        if ((group_mask & bit) != 0) {
            failf("FAIL [episodes-semantic/vendor-duplicate-group-%d]: id=%lld mask=0x%x bit=0x%x",
                  row, (long long)id, group_mask, bit);
        }
        require_int("episodes-semantic/vendor-eligible-max",
                    episodes_latest_row_is_eligible_max(vendor_db, id), 1);
        require_dashboard_latest_fixture_row(
            "episodes-semantic", "vendor", vendor_db, stmt,
            EMBY_EPISODES_LATEST_SEMANTIC_PROJECTION, id
        );
        group_mask |= bit;
        row++;
    }
    require_int("episodes-semantic/vendor-step", rc, SQLITE_DONE);
    require_int("episodes-semantic/vendor-row-count", row, 10);
    require_int("episodes-semantic/vendor-group-mask", (int)group_mask, 1023);
    require_int("episodes-semantic/vendor-finalize", sqlite3_finalize(stmt), SQLITE_OK);
    free(raw);
    free(expected);
}

static int query_int(sqlite3 *db, const char *label, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value;
    require_int(label, sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    require_int(label, sqlite3_step(stmt), SQLITE_ROW);
    value = sqlite3_column_int(stmt, 0);
    require_int(label, sqlite3_step(stmt), SQLITE_DONE);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
    return value;
}

static int child_links_two_level_row_parity(void) {
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;
    char *expected;
    typed_rows vendor_rows;
    typed_rows candidate_rows;

    configure_env("1", "0", NULL);
    make_temp_dir();
    vendor_db = open_seeded_temp("not-target.db");
    candidate_db = open_seeded_temp("library.db");
    seed_link_type_count_shape_05_rows(vendor_db);
    seed_link_type_count_shape_05_rows(candidate_db);
    expected = make_link_type_count_shape_05_expected();

    require_int(
        "links-two-level/l1-only-fixture",
        query_int(vendor_db, "links-two-level/l1-only-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 AS L JOIN AncestorIds2 AS X "
                  "ON X.itemid=L.ItemId WHERE X.AncestorId=15 AND L.LinkedId=1001 "
                  "AND L.Type IN (6,4,3,2)"),
        1
    );
    require_int(
        "links-two-level/l2-only-fixture",
        query_int(vendor_db, "links-two-level/l2-only-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 AS L2 WHERE L2.LinkedId=1002 "
                  "AND EXISTS (SELECT 1 FROM ItemLinks2 AS L1 JOIN AncestorIds2 AS X "
                  "ON X.itemid=L1.ItemId WHERE X.AncestorId=15 "
                  "AND L1.LinkedId=L2.ItemId AND L1.Type IN (7,0,1,5,6,2))"),
        1
    );
    require_int(
        "links-two-level/both-direct-duplicates",
        query_int(vendor_db, "links-two-level/both-direct-duplicates-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2003 "
                  "AND LinkedId=1003 AND Type=6"),
        2
    );
    require_int(
        "links-two-level/both-indirect-duplicates",
        query_int(vendor_db, "links-two-level/both-indirect-duplicates-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2103 "
                  "AND LinkedId=1003"),
        2
    );
    require_int(
        "links-two-level/null-linked-id-fixture",
        query_int(vendor_db, "links-two-level/null-linked-id-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE ItemId=2004 "
                  "AND LinkedId IS NULL AND Type=6"),
        1
    );
    require_int(
        "links-two-level/no-hit-fixture",
        query_int(vendor_db, "links-two-level/no-hit-fixture-sql",
                  "SELECT count(*) FROM ItemLinks2 WHERE LinkedId=1004"),
        0
    );

    expect_sql(candidate_db, "links-two-level/rewrite-fired",
               EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL, -1, 2, expected, 0);
    contract_parity_require(
        vendor_db, candidate_db, contract_prepare_v2,
        "links-two-level/ordered-contract",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        expected, NULL, NULL, NULL, NULL
    );
    vendor_rows = collect_typed_rows(
        vendor_db, "links-two-level/vendor-typed-rows",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL
    );
    candidate_rows = collect_typed_rows(
        candidate_db, "links-two-level/candidate-typed-rows",
        EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL,
        expected
    );
    require_ordered_full_row_identity(
        "links-two-level/ordered-type-tagged-identity",
        &vendor_rows, &candidate_rows
    );
    require_str_eq(
        "links-two-level/vendor-exact-output",
        (const char *)vendor_rows.bytes,
        "C1;N3;RI:9;RI:29;RI:34;"
    );
    require_str_eq(
        "links-two-level/candidate-exact-output",
        (const char *)candidate_rows.bytes,
        "C1;N3;RI:9;RI:29;RI:34;"
    );

    free_typed_rows(&vendor_rows);
    free_typed_rows(&candidate_rows);
    free(expected);
    require_int("links-two-level/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
    require_int("links-two-level/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [links-two-level-row-parity]\n");
    return 0;
}

static int child_dashboard_release_matrix(void) {
    static const char *ancestors[] = {"100", "200", "999"};
    static const char *users[] = {"42", "777"};
    static const char *limits[] = {"12", "16", "20"};
    int guarded = 0;
    int passthrough = 0;
    int ai, ui, li, stat;

    configure_env("1", "1", "0");
    require_str_eq("matrix/hot-literal", ancestors[0], "100");
    require_str_eq("matrix/typical-literal", ancestors[1], "200");
    require_str_eq("matrix/empty-literal", ancestors[2], "999");
    require_str_eq("matrix/heavy-user-literal", users[0], "42");
    require_str_eq("matrix/zero-user-literal", users[1], "777");
    if (!strcmp(ancestors[0], ancestors[1]) || !strcmp(ancestors[0], ancestors[2]) ||
        !strcmp(ancestors[1], ancestors[2]) || !strcmp(users[0], users[1])) {
        failf("FAIL [matrix/literal-distinctness]");
    }

    make_temp_dir();
    {
        sqlite3 *pre = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
        typed_rows streams[3][2];
        int pai;
        int pui;
        seed_movies_latest_rows(pre);
        require_int("matrix/hot-count", query_int(pre, "matrix/hot-count-sql", "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=100 AND itemid BETWEEN 5001 AND 5030"), 30);
        require_int("matrix/typical-count", query_int(pre, "matrix/typical-count-sql", "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=200 AND itemid BETWEEN 6001 AND 6024"), 24);
        require_int("matrix/empty-count", query_int(pre, "matrix/empty-count-sql", "SELECT count(*) FROM AncestorIds2 WHERE AncestorId=999 AND itemid BETWEEN 5001 AND 6024"), 0);
        require_int("matrix/user42-count", query_int(pre, "matrix/user42-count-sql", "SELECT count(*) FROM UserDatas WHERE UserId=42 AND UserDataKeyId BETWEEN 7001 AND 8024"), 54);
        require_int("matrix/user42-played", query_int(pre, "matrix/user42-played-sql", "SELECT count(*) FROM UserDatas WHERE UserId=42 AND Played=1 AND UserDataKeyId BETWEEN 7001 AND 8024"), 10);
        require_int("matrix/user777-count", query_int(pre, "matrix/user777-count-sql", "SELECT count(*) FROM UserDatas WHERE UserId=777"), 0);
        for (pai = 0; pai < 3; pai++) {
            for (pui = 0; pui < 2; pui++) {
                char *raw = make_movies_latest_sql(1, ancestors[pai], users[pui], "20");
                char *expected = make_movies_latest_expected(ancestors[pai], users[pui], "20");
                streams[pai][pui] = collect_typed_rows(pre, "matrix/distinct-preflight", raw, expected);
                free(raw);
                free(expected);
            }
        }
        require_typed_rows_differ("matrix/hot-vs-typical-user42", &streams[0][0], &streams[1][0]);
        require_typed_rows_differ("matrix/hot-vs-typical-user777", &streams[0][1], &streams[1][1]);
        require_typed_rows_differ("matrix/hot-users", &streams[0][0], &streams[0][1]);
        require_typed_rows_differ("matrix/typical-users", &streams[1][0], &streams[1][1]);
        require_int("matrix/empty-user42-rows", streams[2][0].rows, 0);
        require_int("matrix/empty-user777-rows", streams[2][1].rows, 0);
        for (pai = 0; pai < 3; pai++) {
            for (pui = 0; pui < 2; pui++) free_typed_rows(&streams[pai][pui]);
        }
        require_int("matrix/pre-close", sqlite3_close(pre), SQLITE_OK);
    }
    cleanup_temp_dir();

    for (stat = 0; stat < 2; stat++) {
        for (ai = 0; ai < 3; ai++) {
            for (ui = 0; ui < 2; ui++) {
                for (li = 0; li < 3; li++) {
                    sqlite3 *vendor_db;
                    sqlite3 *candidate_db;
                    char *raw;
                    char *expected;
                    typed_rows vendor;
                    typed_rows candidate;
                    int want_rows = ai == 2 ? 0 : atoi(limits[li]);

                    make_temp_dir();
                    vendor_db = open_seeded_temp_with_dashboard_indexes("not-target.db", 0, 0, 0, 0);
                    candidate_db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
                    seed_movies_latest_rows(vendor_db);
                    seed_movies_latest_rows(candidate_db);
                    if (stat) {
                        exec_sql(vendor_db, "matrix-vendor-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
                        exec_sql(candidate_db, "matrix-candidate-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
                    }
                    raw = make_movies_latest_sql(1, ancestors[ai], users[ui], limits[li]);
                    expected = make_movies_latest_expected(ancestors[ai], users[ui], limits[li]);
                    vendor = collect_typed_rows(vendor_db, "matrix/vendor", raw, raw);
                    candidate = collect_typed_rows(candidate_db, "matrix/candidate", raw, expected);
                    require_int("matrix/vendor-row-count", vendor.rows, want_rows);
                    require_int("matrix/candidate-row-count", candidate.rows, want_rows);
                    require_ordered_full_row_identity("matrix/guarded-identity", &vendor, &candidate);
                    if (ai < 2 && li > 0) {
                        char current_ids[1024];
                        char prior_ids[1024];
                        const char *prior_limit = li == 1 ? "12" : "16";
                        char *prior_raw = make_movies_latest_sql(1, ancestors[ai], users[ui], prior_limit);
                        collect_int_column(vendor_db, "matrix/current-ids", raw, raw, -1, 0,
                                           current_ids, sizeof(current_ids));
                        collect_int_column(vendor_db, "matrix/prior-ids", prior_raw, prior_raw, -1, 0,
                                           prior_ids, sizeof(prior_ids));
                        if (strncmp(current_ids, prior_ids, strlen(prior_ids)) != 0) {
                            failf("FAIL [matrix/row-slice]: current=%s prior=%s",
                                  current_ids, prior_ids);
                        }
                        free(prior_raw);
                    }
                    guarded++;
                    free_typed_rows(&vendor);
                    free_typed_rows(&candidate);

                    free(raw);
                    raw = make_movies_latest_sql(0, ancestors[ai], users[ui], limits[li]);
                    vendor = collect_typed_rows(vendor_db, "matrix/no-guard-vendor", raw, raw);
                    candidate = collect_typed_rows(candidate_db, "matrix/no-guard-candidate", raw, raw);
                    require_ordered_full_row_identity("matrix/no-guard-identity", &vendor, &candidate);
                    passthrough++;
                    free_typed_rows(&vendor);
                    free_typed_rows(&candidate);
                    free(raw);
                    free(expected);
                    require_int("matrix/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
                    require_int("matrix/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
                    cleanup_temp_dir();
                }
            }
        }
    }
    require_int("matrix/guarded-cells", guarded, 36);
    require_int("matrix/no-guard-cells", passthrough, 36);

    for (stat = 0; stat < 2; stat++) {
        sqlite3 *vendor_db;
        sqlite3 *candidate_db;

        make_temp_dir();
        vendor_db = open_seeded_temp_with_dashboard_indexes(
            "not-target.db", 0, 0, 0, 0
        );
        candidate_db = open_seeded_temp_with_dashboard_indexes(
            "library.db", 1, 1, 1, 1
        );
        seed_episodes_latest_semantic_rows(vendor_db);
        seed_episodes_latest_semantic_rows(candidate_db);
        if (stat) {
            exec_sql(vendor_db, "episodes-semantic-vendor-analyze",
                     "PRAGMA analysis_limit=0; ANALYZE;");
            exec_sql(candidate_db, "episodes-semantic-candidate-analyze",
                     "PRAGMA analysis_limit=0; ANALYZE;");
        }
        require_episodes_latest_semantics(vendor_db, candidate_db);
        require_int("episodes-semantic/vendor-close",
                    sqlite3_close(vendor_db), SQLITE_OK);
        require_int("episodes-semantic/candidate-close",
                    sqlite3_close(candidate_db), SQLITE_OK);
        cleanup_temp_dir();
    }

    for (stat = 0; stat < 2; stat++) {
        /* The movies-Latest date-ordered anti-join selects max DateCreated, then lower Id, and owns this exact order. */
        static const sqlite3_int64 expected_candidate_ids[] = {
            9001, 9002, 9003, 9004, 9005, 9006, 9007, 9008, 9009, 9010,
            9119, 9101, 9104, 9107, 9111, 9112, 9113, 9118, 9116, 9108
        };
        sqlite3 *vendor_db;
        sqlite3 *candidate_db;
        char *raw;
        char *expected;
        typed_rows vendor;
        typed_rows candidate;
        make_temp_dir();
        vendor_db = open_seeded_temp_with_dashboard_indexes("not-target.db", 0, 0, 0, 0);
        candidate_db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
        seed_movies_latest_expanded_rows(vendor_db);
        seed_movies_latest_expanded_rows(candidate_db);
        if (stat) {
            exec_sql(vendor_db, "expanded-vendor-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
            exec_sql(candidate_db, "expanded-candidate-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
        }
        raw = make_movies_latest_sql(1, "300", "42", "20");
        expected = make_movies_latest_expected("300", "42", "20");
        vendor = collect_typed_rows(vendor_db, "expanded/vendor", raw, raw);
        candidate = collect_typed_rows(candidate_db, "expanded/candidate", raw, expected);
        require_int("expanded/vendor-row-count", vendor.rows, 20);
        require_int("expanded/candidate-row-count", candidate.rows, 20);
        require_movies_latest_candidate_rows(
            candidate_db, "expanded/candidate-contract", raw, expected,
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION, expected_candidate_ids,
            (int)(sizeof(expected_candidate_ids) / sizeof(expected_candidate_ids[0]))
        );
        free_typed_rows(&vendor);
        free_typed_rows(&candidate);
        free(raw);
        free(expected);
        {
            char order_ids[128];
            char *order_raw;
            char *order_expected;
            seed_movies_latest_rewrite_order_rows(candidate_db);
            order_raw = make_movies_latest_sql(1, "301", "42", "12");
            order_expected = make_movies_latest_expected("301", "42", "12");
            collect_int_column(candidate_db, "expanded/rewrite-order", order_raw,
                               order_expected, -1, 0, order_ids, sizeof(order_ids));
            require_str_eq("expanded/rewrite-order-ids", order_ids,
                           "9202,9201,9204,9203,");
            free(order_raw);
            free(order_expected);
        }
        require_int("expanded/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
        require_int("expanded/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
        cleanup_temp_dir();
    }
    printf("PASS [dashboard-release-matrix]\n");
    return 0;
}

static int child_latest_limit20(void) {
    sqlite3 *db;
    char *latest_sql;
    char *latest_expected;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 1, 1);
    latest_sql = make_latest_sql("A.Id ", "20");
    latest_expected = make_latest_expected("A.Id ", "20");
    require_int("latest-limit20/outer-index-count",
                count_occurrences(latest_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk"),
                1);
    require_int("latest-limit20/inner-index-count",
                count_occurrences(latest_expected,
                                  "INDEXED BY idx_dshadow_emby_latest_gk_dc"),
                1);
    require_contains("latest-limit20/ranked", latest_expected,
                     "WITH ranked(id, dc, gk) AS MATERIALIZED (");
    require_absent("latest-limit20/keys", latest_expected, "WITH keys(gk)");
    require_absent("latest-limit20/picked", latest_expected, "picked AS MATERIALIZED");
    require_absent("latest-limit20/exact-groups", latest_expected,
                   "exact_groups AS MATERIALIZED");
    expect_sql(db, "latest-limit20", latest_sql, -1, 2, latest_expected, 0);
    free(latest_sql);
    free(latest_expected);
    require_int("latest-limit20/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [latest-limit20]\n");
    return 0;
}

static int child_latest_fixture_passthrough(const char *name) {
    sqlite3 *db;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 1, 1);
    expect_fixture(db, name);
    require_int("latest-fixture-passthrough/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", name);
    return 0;
}

static int child_latest_resume_no_misfire(void) {
    sqlite3 *db;
    char *resume_sql;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 1, 1);
    resume_sql = make_resume_sql();
    expect_sql(db, "latest-resume-no-misfire", resume_sql, -1, 2, resume_sql, 0);
    free(resume_sql);
    require_int("latest-resume-no-misfire/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [latest-resume-no-misfire]\n");
    return 0;
}

static int child_dashboard_mixed_latest(void) {
    sqlite3 *db;
    sqlite_master_auth_probe probe = {0};
    char *raw;
    char *expected;
    char *mutated;
    const char *users[] = {"42", "?", "42", "?"};
    const char *limits[] = {"3", "3", "?", "?"};
    int user_bound[] = {0, 1, 0, 1};
    int limit_bound[] = {0, 0, 1, 1};
    size_t i;

    configure_env("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    for (i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
        char label[96];
        raw = make_mixed_latest_sql(users[i], limits[i]);
        expected = make_mixed_latest_expected(users[i], limits[i]);
        snprintf(label, sizeof(label), "dashboard-mixed-latest-bind-%lu",
                 (unsigned long)i);
        expect_mixed_latest_sql(
            db, label, raw, expected, user_bound[i], limit_bound[i]
        );
        free(raw);
        free(expected);
    }

    raw = make_mixed_latest_sql("42", "3");
    require_int("dashboard-mixed/authorizer-set",
                sqlite3_set_authorizer(db, count_sqlite_master_read, &probe),
                SQLITE_OK);
    {
        static const char *const old_text[] = {
            "where A.Type in (8,5) ",
            "where A.Type in (8,5) ",
            "where A.Type in (8,5) ",
            "A.type,A.Id",
            "from mediaitems A left join",
            "Coalesce(UserDatas.played, 0)=0",
            "Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey)",
            "ORDER BY MAX(A.DateCreated) DESC",
            "LIMIT 3"
        };
        static const char *const new_text[] = {
            "where A.Type=8 ",
            "where A.Type in (5,8) ",
            "where A.Type in (8,5,6) ",
            "A.Type,A.Id",
            "FROM mediaitems AS A left join",
            "Coalesce(UserDatas.played,0)=0",
            "Group By coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey)",
            "ORDER BY MAX(A.DateCreated) desc",
            "LIMIT 4"
        };
        for (i = 0; i < sizeof(old_text) / sizeof(old_text[0]); i++) {
            char label[96];
            int before = probe.reads;
            mutated = replace_once(raw, old_text[i], new_text[i]);
            snprintf(label, sizeof(label), "dashboard-mixed-negative-%lu",
                     (unsigned long)i);
            expect_mixed_latest_negative(db, label, mutated);
            if (i < 3 && probe.reads != before) {
                failf("FAIL [%s/probe]: expected=%d actual=%d",
                      label, before, probe.reads);
            }
            free(mutated);
        }
    }
    {
        static const char *const invalid_user[] = {"?1", ":name", "@name", "$name"};
        for (i = 0; i < sizeof(invalid_user) / sizeof(invalid_user[0]); i++) {
            char label[96];
            mutated = make_mixed_latest_sql(invalid_user[i], "3");
            snprintf(label, sizeof(label), "dashboard-mixed-bind-reject-%lu",
                     (unsigned long)i);
            expect_mixed_latest_negative(db, label, mutated);
            free(mutated);
        }
    }
    mutated = replace_once(raw, "A.type,A.Id", "? AS extra,A.type,A.Id");
    expect_mixed_latest_negative(db, "dashboard-mixed-bind-outside-slots", mutated);
    free(mutated);
    {
        char *two_binds = make_mixed_latest_sql("?", "?");
        mutated = replace_once(two_binds, "A.type,A.Id", "? AS third,A.type,A.Id");
        expect_mixed_latest_negative(db, "dashboard-mixed-third-bind", mutated);
        free(two_binds);
        free(mutated);
    }
    require_int("dashboard-mixed/authorizer-clear",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    free(raw);
    require_int("dashboard-mixed/close", sqlite3_close(db), SQLITE_OK);

    raw = make_mixed_latest_sql("42", "3");
    db = open_seeded_temp_with_mixed_indexes("library.db", 0, 1);
    expect_mixed_latest_negative(db, "dashboard-mixed-outer-missing", raw);
    require_int("dashboard-mixed/outer-missing-close", sqlite3_close(db), SQLITE_OK);
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 0);
    expect_mixed_latest_negative(db, "dashboard-mixed-inner-missing", raw);
    require_int("dashboard-mixed/inner-missing-close", sqlite3_close(db), SQLITE_OK);
    db = open_seeded_temp_with_mixed_indexes("library.db", 0, 0);
    expect_mixed_latest_negative(db, "dashboard-mixed-both-missing", raw);
    require_int("dashboard-mixed/both-missing-close", sqlite3_close(db), SQLITE_OK);
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    exec_sql(db, "dashboard-mixed-outer-mismatch",
             "DROP INDEX idx_dshadow_emby_latest_mixed_dcn_gk;"
             "CREATE INDEX idx_dshadow_emby_latest_mixed_dcn_gk ON MediaItems (DateCreated) WHERE Type IN (8,5);");
    expect_mixed_latest_negative(db, "dashboard-mixed-outer-mismatch", raw);
    require_int("dashboard-mixed/outer-mismatch-close", sqlite3_close(db), SQLITE_OK);
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    exec_sql(db, "dashboard-mixed-inner-mismatch",
             "DROP INDEX idx_dshadow_emby_latest_mixed_gk_dc;"
             "CREATE INDEX idx_dshadow_emby_latest_mixed_gk_dc ON MediaItems (DateCreated) WHERE Type IN (8,5);");
    expect_mixed_latest_negative(db, "dashboard-mixed-inner-mismatch", raw);
    require_int("dashboard-mixed/inner-mismatch-close", sqlite3_close(db), SQLITE_OK);
    free(raw);

    cleanup_temp_dir();
    printf("PASS [dashboard-mixed-latest-real-path]\n");
    return 0;
}

static int child_dashboard_mixed_disabled(void) {
    sqlite3 *db;
    sqlite_master_auth_probe probe = {0};
    char *raw;

    configure_env("1", "1", NULL);
    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    raw = make_mixed_latest_sql("42", "3");
    require_int("dashboard-mixed-disabled/authorizer-set",
                sqlite3_set_authorizer(db, count_sqlite_master_read, &probe),
                SQLITE_OK);
    expect_mixed_latest_negative(db, "dashboard-mixed-disabled", raw);
    require_int("dashboard-mixed-disabled/probes", probe.reads, 0);
    require_int("dashboard-mixed-disabled/authorizer-clear",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    free(raw);
    require_int("dashboard-mixed-disabled/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [dashboard-mixed-disabled]\n");
    return 0;
}

static int child_dashboard_mixed_identity(void) {
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;
    char *raw;
    char *expected;
    typed_rows vendor_rows;
    typed_rows candidate_rows;
    char ids[128];
    int analyzed;

    configure_env("1", "1", "0");
    make_temp_dir();
    vendor_db = open_seeded_temp_with_mixed_indexes("not-target.db", 1, 1);
    candidate_db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_mixed_latest_identity_rows(vendor_db);
    seed_mixed_latest_identity_rows(candidate_db);
    raw = make_mixed_latest_sql("42", "3");
    expected = make_mixed_latest_expected("42", "3");
    for (analyzed = 0; analyzed < 2; analyzed++) {
        if (analyzed) {
            exec_sql(vendor_db, "mixed-vendor-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
            exec_sql(candidate_db, "mixed-candidate-analyze", "PRAGMA analysis_limit=0; ANALYZE;");
        }
        vendor_rows = collect_typed_rows(
            vendor_db, analyzed ? "mixed-vendor-after-analyze" : "mixed-vendor-before-analyze",
            raw, raw
        );
        candidate_rows = collect_typed_rows(
            candidate_db, analyzed ? "mixed-candidate-after-analyze" : "mixed-candidate-before-analyze",
            raw, expected
        );
        require_ordered_full_row_identity(
            analyzed ? "mixed-byte-identity-after-analyze" : "mixed-byte-identity-before-analyze",
            &vendor_rows, &candidate_rows
        );
        free_typed_rows(&vendor_rows);
        free_typed_rows(&candidate_rows);
    }
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql("?", "?");
    expected = make_mixed_latest_expected("?", "?");
    {
        mixed_limit_bind_case limit_cases[] = {
            MIXED_LIMIT_ZERO,
            MIXED_LIMIT_NEGATIVE,
            MIXED_LIMIT_NULL,
            MIXED_LIMIT_TEXT_THREE,
            MIXED_LIMIT_TEXT_GARBAGE
        };
        size_t i;
        for (i = 0; i < sizeof(limit_cases) / sizeof(limit_cases[0]); i++) {
            char vendor_label[96];
            char candidate_label[96];
            int vendor_rc;
            int candidate_rc;
            snprintf(vendor_label, sizeof(vendor_label),
                     "mixed-bound-edge-vendor-%lu", (unsigned long)i);
            snprintf(candidate_label, sizeof(candidate_label),
                     "mixed-bound-edge-candidate-%lu", (unsigned long)i);
            vendor_rows = collect_bound_mixed_rows(
                vendor_db, vendor_label, raw, raw, limit_cases[i], &vendor_rc
            );
            candidate_rows = collect_bound_mixed_rows(
                candidate_db, candidate_label, raw, expected,
                limit_cases[i], &candidate_rc
            );
            require_int("mixed-bound-edge/rc-parity", candidate_rc, vendor_rc);
            require_ordered_full_row_identity(
                "mixed-bound-edge/typed-row-parity", &vendor_rows, &candidate_rows
            );
            free_typed_rows(&vendor_rows);
            free_typed_rows(&candidate_rows);
        }
    }
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_form("101", "42", "3");
    expected = make_mixed_latest_expected_form("101", "42", "3");
    collect_int_column(candidate_db, "mixed-all-null-date", raw, expected, -1, 1,
                       ids, sizeof(ids));
    require_str_eq("mixed-all-null-date/lower-id", ids, "2010,");
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_form("102", "42", "3");
    expected = make_mixed_latest_expected_form("102", "42", "3");
    collect_int_column(candidate_db, "mixed-played-state", raw, expected, -1, 1,
                       ids, sizeof(ids));
    require_str_eq("mixed-played-state/exclusion", ids, "2021,");
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_form("103", "42", "3");
    expected = make_mixed_latest_expected_form("103", "42", "3");
    collect_int_column(candidate_db, "mixed-ancestor-invisible", raw, expected, -1, 1,
                       ids, sizeof(ids));
    require_str_eq("mixed-ancestor-invisible/exclusion", ids, "2031,");
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_form("105", "42", "3");
    expected = make_mixed_latest_expected_form("105", "42", "3");
    collect_int_column(candidate_db, "mixed-same-date-lower-id", raw, expected, -1, 1,
                       ids, sizeof(ids));
    require_str_eq("mixed-same-date-lower-id/selection", ids, "2039,");
    free(raw);
    free(expected);

    raw = make_mixed_latest_sql_form("104", "42", "3");
    expected = make_mixed_latest_expected_form("104", "42", "3");
    collect_int_column(candidate_db, "mixed-limit-boundary-gk", raw, expected, -1, 1,
                       ids, sizeof(ids));
    require_str_eq("mixed-limit-boundary-gk/order", ids, "2051,2053,2052,");
    free(raw);
    free(expected);

    require_int("mixed-identity/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
    require_int("mixed-identity/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [dashboard-mixed-identity]\n");
    return 0;
}

static int child_dashboard_matcher_negatives(void) {
    static const char *binds[] = {
        "?", "?1", ":name", "@name", "$name", ":1", "@1", "$1", ":\xC3\xA9"
    };
    sqlite_master_auth_probe readiness_probe = {0};
    sqlite3 *db;
    FILE *capture;
    char *log_output;
    char *episodes;
    char *movies;
    int saved_stderr_fd;
    size_t i;

    configure_env_observable("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 1);
    seed_movies_latest_rows(db);
    episodes = make_latest_sql("A.Id ", "12");
    movies = make_movies_latest_sql(1, "100", "42", "12");

    capture = begin_stderr_capture(&saved_stderr_fd);
    require_int("dashboard-bind/authorizer-set",
                sqlite3_set_authorizer(db, count_sqlite_master_read, &readiness_probe),
                SQLITE_OK);

    for (i = 0; i < sizeof(binds) / sizeof(binds[0]); i++) {
        char label[128];
        char *cases[8];
        cases[0] = replace_once(episodes, "select A.Id from mediaitems A",
                                xasprintf("select %s AS bound,A.Id from mediaitems A", binds[i]));
        cases[1] = replace_once(episodes, "in (100)", xasprintf("in (%s)", binds[i]));
        cases[2] = replace_once(episodes, "UserDatas.UserId=42", xasprintf("UserDatas.UserId=%s", binds[i]));
        cases[3] = replace_once(episodes, "LIMIT 12", xasprintf("LIMIT %s", binds[i]));
        cases[4] = replace_once(movies, "A.Id,A.Name", xasprintf("%s AS bound,A.Id,A.Name", binds[i]));
        cases[5] = replace_once(movies, "in (100)", xasprintf("in (%s)", binds[i]));
        cases[6] = replace_once(movies, "UserDatas.UserId=42", xasprintf("UserDatas.UserId=%s", binds[i]));
        cases[7] = replace_once(movies, "LIMIT 12", xasprintf("LIMIT %s", binds[i]));
        for (int c = 0; c < 8; c++) {
            snprintf(label, sizeof(label), "dashboard-bind-%lu-%d", (unsigned long)i, c);
            expect_sql(db, label, cases[c], -1, 2, cases[c], 0);
            free(cases[c]);
        }
    }
    require_int("dashboard-bind/authorizer-clear",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    log_output = end_stderr_capture(capture, saved_stderr_fd);
    require_int("dashboard-bind/readiness-probes", readiness_probe.reads, 0);
    require_absent("dashboard-bind/applied-log", log_output,
                   "event=rewrite_applied target=emby mode=dashboard+");
    require_absent("dashboard-bind/index-missing-log", log_output,
                   "reason=index_missing mode=dashboard+");
    require_absent("dashboard-bind/index-probe-error-log", log_output,
                   "reason=index_probe_error mode=dashboard+");
    free(log_output);

    {
        const char *episode_needles[] = {
            "select A.Id from mediaitems A", " Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey)", " ORDER BY MAX(A.DateCreated) DESC", " LIMIT 12"
        };
        const char *episode_repls[] = {
            "select * from mediaitems A", " Group by A.PresentationUniqueKey", " ORDER BY A.DateCreated DESC", " LIMIT 11"
        };
        const char *movies_needles[] = {
            "A.Id,A.Name", " Group by A.PresentationUniqueKey", " ORDER BY A.DateCreated DESC", " LIMIT 12",
            "AND A.Id in WithAncestors", "A.Id,A.Name"
        };
        const char *movies_repls[] = {
            "A.Name,A.Id", " Group by A.Id", " ORDER BY A.Name DESC", " LIMIT 21",
            "AND A.Id in WithAncestors AND A.IsPublic=1", "MAX(A.Id),A.Name"
        };
        size_t c;
        for (c = 0; c < sizeof(episode_needles) / sizeof(episode_needles[0]); c++) {
            char label[128];
            char *mutated = replace_once(episodes, episode_needles[c], episode_repls[c]);
            snprintf(label, sizeof(label), "episodes-structural-negative[%lu]",
                     (unsigned long)c);
            expect_sql(db, label, mutated, -1, 2, mutated, 0);
            free(mutated);
        }
        for (c = 0; c < sizeof(movies_needles) / sizeof(movies_needles[0]); c++) {
            char label[128];
            char *mutated = replace_once(movies, movies_needles[c], movies_repls[c]);
            if (c == 0) {
                char *projection = replace_once(
                    EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
                    movies_needles[c], movies_repls[c]
                );
                char *expected = make_movies_latest_expected_projection(
                    "100", projection, "42", "12"
                );
                snprintf(label, sizeof(label), "movies-structural-positive[%lu]",
                         (unsigned long)c);
                expect_sql(db, label, mutated, -1, 2, expected, 0);
                free(projection);
                free(expected);
            } else {
                snprintf(label, sizeof(label), "movies-structural-negative[%lu]",
                         (unsigned long)c);
                expect_sql(db, label, mutated, -1, 2, mutated, 0);
            }
            free(mutated);
        }
    }

    {
        char *movies_no_guard = make_movies_latest_sql(0, "100", "42", "12");
        char *movies_empty = make_movies_latest_sql(1, "", "42", "12");
        char *movies_comment = replace_once(
            movies,
            "where A.Type=5 AND Coalesce(UserDatas.played, 0)=0",
            "where A.Type=5 /* guarded tail text: where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 */ AND Coalesce(UserDatas.played, 0)=0"
        );
        char *movies_string_projection = replace_once(
            EMBY_MOVIES_LATEST_COMPACT_PROJECTION,
            "A.Id,A.Name",
            "'where A.Type=5 ' AS marker,A.Id,A.Name"
        );
        char *movies_string = make_movies_latest_sql_form(
            1, "100", 0, movies_string_projection, "42", "12"
        );
        char *movies_string_expected = make_movies_latest_expected_projection(
            "100", movies_string_projection, "42", "12"
        );
        char *episodes_explain = xasprintf("EXPLAIN %s", episodes);
        expect_sql(db, "movies-no-guard-negative", movies_no_guard, -1, 2, movies_no_guard, 0);
        expect_sql(db, "movies-empty-ancestor-negative", movies_empty, -1, 2, movies_empty, 0);
        expect_sql(db, "movies-comment-tail-negative", movies_comment, -1, 2, movies_comment, 0);
        require_int("movies-string-type-gate-immunity/type-gate-count",
                    count_occurrences(movies_string, "where A.Type=5 "), 2);
        expect_sql(db, "movies-string-type-gate-immunity", movies_string, -1, 2,
                   movies_string_expected, 0);
        expect_sql(db, "episodes-explain-negative", episodes_explain, -1, 2, episodes_explain, 0);
        free(movies_no_guard);
        free(movies_empty);
        free(movies_comment);
        free(movies_string_projection);
        free(movies_string);
        free(movies_string_expected);
        free(episodes_explain);
    }

    free(episodes);
    free(movies);
    require_int("dashboard-negatives/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [dashboard-matcher-negatives]\n");
    return 0;
}

static int child_observability_logs(void) {
    sqlite3 *db;
    FILE *capture;
    char *log_output;
    char *links_sql;
    char *links_repl;
    char *links_expected;
    char *links_two_level_expected;
    char *links_miss_sql;
    char *resume_simple_sql;
    char *resume_simple_expected;
    char *resume_early_sql;
    char *similar_sql;
    char *similar_expected;
    char *latest_sql;
    char *latest_expected;
    char *latest_new_corr_sql;
    char *latest_new_corr_expected;
    char *latest_miss_sql;
    char *latest_unsupported_sql;
    char *latest_bind_sql;
    char *latest_series_sql;
    char *movies_sql;
    char *movies_expected;
    char *movies_miss_sql;
    char *movies_unsupported_sql;
    char *movies_bind_sql;
    char *movies_no_guard_sql;
    char *mixed_sql;
    char *mixed_expected;
    char *mixed_miss_sql;
    char *mixed_bind_sql;
    int saved_stderr_fd;

    configure_env_observable("1", "0", "0");
    if (setenv("SQLITE3_DISABLE_STMT_TRACE", "1", 1) != 0 ||
        unsetenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL") != 0) {
        failf("FATAL: exec-child observable env failed: %s", strerror(errno));
    }
    make_temp_dir();
    capture = begin_stderr_capture(&saved_stderr_fd);

    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_movies_latest_rows(db);
    links_sql = make_links_search_sql("2,3");
    links_repl = make_exists_links_one("100", "2,3");
    links_expected = replace_once(links_sql, "A.Id in WithItemLinkItemIds", links_repl);
    expect_sql(db, "obs-fanout-links-applied", links_sql, -1, 2, links_expected, 0);
    links_two_level_expected = make_link_type_count_shape_05_expected();
    expect_sql(db, "obs-fanout-links-two-level-applied",
               EMBY_LINK_TYPE_COUNT_SHAPE_05_SQL, -1, 2,
               links_two_level_expected, 0);

    links_miss_sql = make_links_search_sql("'bad'");
    expect_sql(db, "obs-fanout-links-capture-miss", links_miss_sql, -1, 2, links_miss_sql, 0);

    resume_simple_sql = make_resume_simple_sql(1, "12");
    resume_simple_expected = make_resume_simple_expected(resume_simple_sql);
    expect_sql(db, "obs-fanout-resume-simple-applied", resume_simple_sql,
               -1, 2, resume_simple_expected, 0);

    {
        char *resume_sql = make_resume_sql();
        resume_early_sql = replace_once(
            resume_sql,
            "AND LastWatchedEpisodes.LastPlayedDateInt not null",
            "AND 1=1"
        );
        free(resume_sql);
    }
    expect_sql(db, "obs-fanout-resume-early-miss", resume_early_sql,
               -1, 2, resume_early_sql, 0);

    similar_sql = make_similar_sql();
    similar_expected = make_similar_expected(similar_sql);
    expect_sql(db, "obs-fanout-similar-applied", similar_sql, -1, 2, similar_expected, 0);

    latest_sql = make_latest_sql("A.Id ", "12");
    latest_expected = make_latest_expected("A.Id ", "12");
    expect_sql(db, "obs-dashboard-latest-applied", latest_sql, -1, 2, latest_expected, 0);
    latest_new_corr_sql = replace_once(latest_sql, "in (100)", "in (101)");
    latest_new_corr_expected = make_latest_expected_form("A.Id ", "101", "12");
    expect_sql(db, "obs-dashboard-latest-new-corr", latest_new_corr_sql,
               -1, 2, latest_new_corr_expected, 0);
    expect_sql(db, "obs-dashboard-latest-new-corr-repeat", latest_new_corr_sql,
               -1, 2, latest_new_corr_expected, 0);

    latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");
    expect_sql(db, "obs-dashboard-capture-miss", latest_miss_sql, -1, 2, latest_miss_sql, 0);
    latest_unsupported_sql = make_latest_sql("A.Id ", "11");
    expect_sql(db, "obs-dashboard-limit-unsupported", latest_unsupported_sql,
               -1, 2, latest_unsupported_sql, 0);
    latest_bind_sql = replace_once(
        latest_sql,
        "select A.Id from mediaitems A",
        "select ? AS bound,A.Id from mediaitems A"
    );
    expect_sql(db, "obs-dashboard-bind-out-of-scope", latest_bind_sql,
               -1, 2, latest_bind_sql, 0);
    latest_series_sql = read_text_file(
        "tests/fixtures/emby-fts-rewrite/latest-series-browse-negative.sql"
    );
    expect_sql(db, "obs-dashboard-series-browse-clean-miss", latest_series_sql,
               -1, 2, latest_series_sql, 0);

    movies_sql = make_movies_latest_sql(1, "100", "42", "12");
    movies_expected = make_movies_latest_expected("100", "42", "12");
    expect_sql(db, "obs-dashboard-movies-applied", movies_sql, -1, 2,
               movies_expected, 0);
    movies_miss_sql = replace_once(movies_sql, " Group by A.PresentationUniqueKey",
                                   " Group by A.Id");
    expect_sql(db, "obs-dashboard-movies-capture-miss", movies_miss_sql, -1, 2,
               movies_miss_sql, 0);
    movies_unsupported_sql = make_movies_latest_sql(1, "100", "42", "21");
    expect_sql(db, "obs-dashboard-movies-limit-unsupported",
               movies_unsupported_sql, -1, 2, movies_unsupported_sql, 0);
    movies_bind_sql = replace_once(
        movies_sql, "A.Id,A.Name", "? AS bound,A.Id,A.Name"
    );
    expect_sql(db, "obs-dashboard-movies-bind-out-of-scope", movies_bind_sql,
               -1, 2, movies_bind_sql, 0);
    movies_no_guard_sql = make_movies_latest_sql(0, "100", "42", "12");
    expect_sql(db, "obs-dashboard-movies-no-guard", movies_no_guard_sql, -1, 2,
               movies_no_guard_sql, 0);
    mixed_sql = make_mixed_latest_sql("42", "3");
    mixed_expected = make_mixed_latest_expected("42", "3");
    expect_mixed_latest_sql(
        db, "obs-dashboard-mixed-applied", mixed_sql, mixed_expected, 0, 0
    );
    mixed_miss_sql = replace_once(
        mixed_sql, "A.type,A.Id", "A.Type,A.Id"
    );
    expect_sql(db, "obs-dashboard-mixed-capture-miss", mixed_miss_sql, -1, 2,
               mixed_miss_sql, 0);
    mixed_bind_sql = make_mixed_latest_sql("?1", "3");
    expect_sql(db, "obs-dashboard-mixed-bind-out-of-scope", mixed_bind_sql,
               -1, 2, mixed_bind_sql, 0);
    require_int("observability/index-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 0, 1);
    seed_movies_latest_rows(db);
    expect_sql(db, "obs-dashboard-movies-outer-missing", movies_sql, -1, 2,
               movies_sql, 0);
    expect_sql(db, "obs-dashboard-movies-outer-missing-repeat", movies_sql, -1, 2,
               movies_sql, 0);
    require_int("observability/movies-outer-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_dashboard_indexes("library.db", 1, 1, 1, 0);
    seed_movies_latest_rows(db);
    expect_sql(db, "obs-dashboard-movies-inner-missing", movies_sql, -1, 2,
               movies_sql, 0);
    expect_sql(db, "obs-dashboard-movies-inner-missing-repeat", movies_sql, -1, 2,
               movies_sql, 0);
    require_int("observability/movies-inner-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 0, 1);
    expect_sql(db, "obs-dashboard-mixed-index-missing", mixed_sql, -1, 2,
               mixed_sql, 0);
    expect_sql(db, "obs-dashboard-mixed-index-missing-repeat", mixed_sql, -1, 2,
               mixed_sql, 0);
    require_int("observability/mixed-missing-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 1, 0);
    expect_sql(db, "obs-dashboard-date-index-missing", latest_sql, -1, 2,
               latest_sql, 0);
    expect_sql(db, "obs-dashboard-date-index-missing-repeat", latest_sql, -1, 2,
               latest_sql, 0);
    require_int("observability/date-index-missing-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 0, 1);
    expect_sql(db, "obs-dashboard-group-index-missing-new-connection", latest_sql,
               -1, 2, latest_sql, 0);
    require_int("observability/group-index-missing-new-connection-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_latest_indexes("library.db", 0, 0);
    expect_sql(db, "obs-dashboard-both-indexes-missing-new-connection", latest_sql,
               -1, 2, latest_sql, 0);
    require_int("observability/both-indexes-missing-new-connection-close",
                sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_mixed_indexes("library.db", 1, 1);
    seed_movies_latest_rows(db);
    require_int("observability/probe-authorizer/set",
                sqlite3_set_authorizer(db, deny_sqlite_master_read, NULL), SQLITE_OK);
    expect_sql(db, "obs-dashboard-index-probe-error", latest_sql, -1, 2, latest_sql, 0);
    expect_sql(db, "obs-dashboard-index-probe-error-repeat", latest_sql,
               -1, 2, latest_sql, 0);
    expect_sql(db, "obs-dashboard-movies-index-probe-error", movies_sql, -1, 2,
               movies_sql, 0);
    expect_sql(db, "obs-dashboard-movies-index-probe-error-repeat", movies_sql,
               -1, 2, movies_sql, 0);
    expect_sql(db, "obs-dashboard-mixed-index-probe-error", mixed_sql, -1, 2,
               mixed_sql, 0);
    expect_sql(db, "obs-dashboard-mixed-index-probe-error-repeat", mixed_sql,
               -1, 2, mixed_sql, 0);
    require_int("observability/probe-authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL),
                SQLITE_OK);
    require_int("observability/probe-error-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    log_output = end_stderr_capture(capture, saved_stderr_fd);
    require_contains(
        "observability/fanout-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+links_search"
    );
    require_int(
        "observability/fanout-links-applied-count",
        count_occurrences(
            log_output,
            "event=rewrite_applied target=emby mode=fanout+links_search"
        ),
        2
    );
    require_contains(
        "observability/fanout-resume-simple-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+resume_simple"
    );
    require_contains(
        "observability/fanout-similar-applied",
        log_output,
        "event=rewrite_applied target=emby mode=fanout+similar"
    );
    require_contains(
        "observability/dashboard-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-applied-count",
        count_occurrences(
            log_output,
            "event=rewrite_applied target=emby mode=dashboard+episodes_latest"
        ),
        2
    );
    require_contains(
        "observability/dashboard-applied-new",
        log_output,
        "sample=new count=2"
    );
    require_contains(
        "observability/dashboard-applied-new-source",
        log_output,
        latest_new_corr_sql
    );
    require_contains(
        "observability/dashboard-movies-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+movies_latest"
    );
    require_contains(
        "observability/dashboard-mixed-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+mixed_latest"
    );
    require_contains(
        "observability/fanout-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=fanout+links_search sub_reason=type_slot"
    );
    require_contains(
        "observability/dashboard-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+episodes_latest sub_reason=projection"
    );
    {
        char metadata[160];
        int rc = snprintf(
            metadata, sizeof(metadata),
            "sql_len=%lu corr=%016llx",
            (unsigned long)strlen(latest_miss_sql),
            (unsigned long long)sql_corr_key(latest_miss_sql, strlen(latest_miss_sql))
        );
        if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
            failf("FATAL: observability capture metadata overflow");
        }
        require_contains("observability/dashboard-capture-correlation", log_output, metadata);
        require_contains("observability/dashboard-capture-source", log_output, latest_miss_sql);
    }
    require_absent("observability/series-early-miss-source", log_output, latest_series_sql);
    require_absent("observability/resume-early-miss-source", log_output, resume_early_sql);
    require_int(
        "observability/dashboard-capture-miss-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+episodes_latest"
        ),
        1
    );
    require_int(
        "observability/dashboard-movies-capture-miss-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+movies_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-mixed-capture-miss",
        log_output,
        "reason=capture_miss mode=dashboard+mixed_latest sub_reason=projection",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-out-of-scope-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=out_of_scope mode=dashboard+episodes_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-limit-unsupported",
        log_output,
        "reason=out_of_scope mode=dashboard+episodes_latest sub_reason=limit_unsupported",
        "sample=first count=1"
    );
    require_same_line(
        "observability/dashboard-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+episodes_latest sub_reason=bind",
        "sample=new count=2"
    );
    require_int(
        "observability/dashboard-movies-out-of-scope-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=out_of_scope mode=dashboard+movies_latest"
        ),
        2
    );
    require_same_line(
        "observability/dashboard-movies-limit-unsupported",
        log_output,
        "reason=out_of_scope mode=dashboard+movies_latest sub_reason=limit_unsupported",
        "sample=first count=1"
    );
    require_same_line(
        "observability/dashboard-movies-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+movies_latest sub_reason=bind",
        "sample=new count=2"
    );
    require_same_line(
        "observability/dashboard-mixed-bind-out-of-scope",
        log_output,
        "reason=out_of_scope mode=dashboard+mixed_latest sub_reason=bind",
        "sample=first count=1"
    );
    require_contains(
        "observability/dashboard-index-missing",
        log_output,
        "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+episodes_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+episodes_latest",
        "sample=first count=1"
    );
    require_contains(
        "observability/dashboard-index-probe-error",
        log_output,
        "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+episodes_latest"
    );
    require_int(
        "observability/dashboard-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+episodes_latest"
        ),
        2
    );
    require_int(
        "observability/dashboard-movies-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+movies_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-movies-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+movies_latest",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-movies-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+movies_latest"
        ),
        2
    );
    require_int(
        "observability/dashboard-mixed-index-missing-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+mixed_latest"
        ),
        1
    );
    require_same_line(
        "observability/dashboard-mixed-index-missing-sample",
        log_output,
        "reason=index_missing mode=dashboard+mixed_latest",
        "sample=first count=1"
    );
    require_int(
        "observability/dashboard-mixed-index-probe-error-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+mixed_latest"
        ),
        2
    );
    require_absent(
        "observability/dashboard-movies-no-rewritten-prepare-failure",
        log_output,
        "event=rewrite_skipped target=emby reason=rewritten_prepare_failed mode=dashboard+movies_latest"
    );
    require_absent("observability/old-dashboard-mode", log_output, "dashboard+" "latest");

    free(log_output);
    free(links_sql);
    free(links_repl);
    free(links_expected);
    free(links_two_level_expected);
    free(links_miss_sql);
    free(resume_simple_sql);
    free(resume_simple_expected);
    free(resume_early_sql);
    free(similar_sql);
    free(similar_expected);
    free(latest_sql);
    free(latest_expected);
    free(latest_new_corr_sql);
    free(latest_new_corr_expected);
    free(latest_miss_sql);
    free(latest_unsupported_sql);
    free(latest_bind_sql);
    free(latest_series_sql);
    free(movies_sql);
    free(movies_expected);
    free(movies_miss_sql);
    free(movies_unsupported_sql);
    free(movies_bind_sql);
    free(movies_no_guard_sql);
    free(mixed_sql);
    free(mixed_expected);
    free(mixed_miss_sql);
    free(mixed_bind_sql);
    printf("PASS [observability-logs]\n");
    return 0;
}

static int child_observability_master_disabled(void) {
    sqlite3 *db;
    FILE *capture;
    char *log_output;
    char *latest_sql;
    char *latest_expected;
    char *latest_miss_sql;
    int saved_stderr_fd;

    configure_env("1", "1", "0");
    if (setenv("SQLITE3_DISABLE_STMT_TRACE", "0", 1) != 0) {
        failf("setenv STMT trace failed");
    }
    make_temp_dir();
    capture = begin_stderr_capture(&saved_stderr_fd);
    db = open_seeded_temp_with_latest_indexes("library.db", 1, 1);
    latest_sql = make_latest_sql("A.Id ", "12");
    latest_expected = make_latest_expected("A.Id ", "12");
    latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");
    expect_sql(db, "master-disabled-applied", latest_sql, -1, 2, latest_expected, 0);
    expect_sql(db, "master-disabled-capture", latest_miss_sql, -1, 2,
               latest_miss_sql, 0);
    exec_sql(db, "master-disabled-stmt", latest_sql);
    require_int("master-disabled/close", sqlite3_close(db), SQLITE_OK);
    log_output = end_stderr_capture(capture, saved_stderr_fd);
    require_absent("master-disabled/capture", log_output, "reason=capture_miss");
    require_absent("master-disabled/applied", log_output, "event=rewrite_applied");
    require_absent("master-disabled/stmt", log_output, "event=SQLITE_TRACE_STMT");
    free(log_output);
    free(latest_sql);
    free(latest_expected);
    free(latest_miss_sql);
    cleanup_temp_dir();
    printf("PASS [observability-master-disabled]\n");
    return 0;
}

static int child_collision(void) {
    sqlite3 *db;
    char *sql;

    configure_env("0", "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    g_collision_scalar_calls = 0;
    require_int("collision/register",
                sqlite3_create_function_v2(db, "dshadow_emby_fts_rewrite", 1, SQLITE_UTF8,
                                           NULL, scalar_collision_tripwire,
                                           NULL, NULL, NULL),
                SQLITE_OK);
    sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    expect_sql(db, "collision", sql, -1, 2, sql, 0);
    require_int("collision/scalar-calls", g_collision_scalar_calls, 0);
    free(sql);
    require_int("collision/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [collision]\n");
    return 0;
}

static int child_authorizer_and_ownership(void) {
    sqlite3 *db;
    char *sql;
    char *expected;

    configure_env("0", "1", "1");
    make_temp_dir();
    db = open_seeded_temp("library.db");
    sql = make_emby_sql(0, "100", "100", "6", "7", 1);
    expected = make_expected_sql(0, "100", "100", "6", "7", 1);

    require_int("authorizer/set", sqlite3_set_authorizer(db, deny_pragma_function_list, NULL), SQLITE_OK);
    expect_sql(db, "authorizer-deny-probe", sql, -1, 2, sql, 0);
    require_int("authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);

    expect_sql(db, "ownership-ready", sql, -1, 2, expected, 1);
    require_int("ownership/replace",
                sqlite3_create_function_v2(db, "dshadow_emby_fts_rewrite", 1, SQLITE_UTF8,
                                           NULL, scalar_owner_spoof,
                                           NULL, NULL, NULL),
                SQLITE_OK);
    expect_sql(db, "ownership-replaced", sql, -1, 2, sql, 0);

    free(sql);
    free(expected);
    require_int("authorizer-ownership/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [authorizer-ownership]\n");
    return 0;
}

#define DEFINE_LEGACY_THUNK(name, call) \
    static int legacy_##name(void) { return (call); }

#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
DEFINE_LEGACY_THUNK(direct_test_api, child_direct_test_api())
DEFINE_LEGACY_THUNK(direct_fail_open_episodes, child_direct_fail_open_episodes())
DEFINE_LEGACY_THUNK(direct_fail_open_movies, child_direct_fail_open_movies())
DEFINE_LEGACY_THUNK(direct_fail_open_mixed, child_direct_fail_open_mixed())
#endif
DEFINE_LEGACY_THUNK(positive, child_positive())
DEFINE_LEGACY_THUNK(fts_default_on, child_fts_env(NULL, "fts-default-on", 1))
DEFINE_LEGACY_THUNK(fts_zero_enables, child_fts_env("0", "fts-zero-enables", 1))
DEFINE_LEGACY_THUNK(fts_one_disables, child_fts_env("1", "fts-one-disables", 0))
DEFINE_LEGACY_THUNK(fts_garbage_enables, child_fts_env("xyz", "fts-garbage-enables", 1))
DEFINE_LEGACY_THUNK(path_negative, child_path_negative())
DEFINE_LEGACY_THUNK(nonmatch, child_nonmatch())
DEFINE_LEGACY_THUNK(collision, child_collision())
DEFINE_LEGACY_THUNK(authorizer_ownership, child_authorizer_and_ownership())
DEFINE_LEGACY_THUNK(fixture_canary, child_fixture_canary())
DEFINE_LEGACY_THUNK(row_parity, child_row_parity())
DEFINE_LEGACY_THUNK(
    complex_resume_watched_progress_row_parity,
    child_complex_resume_watched_progress_row_parity()
)
DEFINE_LEGACY_THUNK(
    fanout_default_on, child_fanout_env(NULL, "fanout-default-on", 1)
)
DEFINE_LEGACY_THUNK(
    fanout_zero_enables, child_fanout_env("0", "fanout-zero-enables", 1)
)
DEFINE_LEGACY_THUNK(
    fanout_one_disables, child_fanout_env("1", "fanout-one-disables", 0)
)
DEFINE_LEGACY_THUNK(
    fanout_garbage_enables,
    child_fanout_env("false", "fanout-garbage-enables", 1)
)
DEFINE_LEGACY_THUNK(fanout_matrix, child_fanout_matrix())
DEFINE_LEGACY_THUNK(links_two_level_row_parity, child_links_two_level_row_parity())
DEFINE_LEGACY_THUNK(emby_slow_search_matrix, child_emby_slow_search_matrix())
DEFINE_LEGACY_THUNK(dashboard_default_off, child_dashboard_default_off())
DEFINE_LEGACY_THUNK(
    dashboard_one_off, child_dashboard_off_value("1", "dashboard-one-off")
)
DEFINE_LEGACY_THUNK(
    dashboard_empty_off, child_dashboard_off_value("", "dashboard-empty-off")
)
DEFINE_LEGACY_THUNK(
    dashboard_garbage_off,
    child_dashboard_off_value("false", "dashboard-garbage-off")
)
DEFINE_LEGACY_THUNK(dashboard_matrix, child_dashboard_matrix())
DEFINE_LEGACY_THUNK(dashboard_fix_b_c, child_dashboard_fix_b_c())
DEFINE_LEGACY_THUNK(dashboard_mixed_latest, child_dashboard_mixed_latest())
DEFINE_LEGACY_THUNK(dashboard_mixed_disabled, child_dashboard_mixed_disabled())
DEFINE_LEGACY_THUNK(dashboard_mixed_identity, child_dashboard_mixed_identity())
DEFINE_LEGACY_THUNK(dashboard_matcher_negatives, child_dashboard_matcher_negatives())
DEFINE_LEGACY_THUNK(dashboard_release_matrix, child_dashboard_release_matrix())
DEFINE_LEGACY_THUNK(latest_limit20, child_latest_limit20())
DEFINE_LEGACY_THUNK(
    latest_capture_miss_negative,
    child_latest_fixture_passthrough("latest-capture-miss-negative")
)
DEFINE_LEGACY_THUNK(
    latest_series_browse_negative,
    child_latest_fixture_passthrough("latest-series-browse-negative")
)
DEFINE_LEGACY_THUNK(latest_resume_no_misfire, child_latest_resume_no_misfire())
DEFINE_LEGACY_THUNK(
    dashboard_index_definition_gate, child_dashboard_index_definition_gate()
)
DEFINE_LEGACY_THUNK(observability_logs, child_observability_logs())
DEFINE_LEGACY_THUNK(
    observability_master_disabled, child_observability_master_disabled()
)

#undef DEFINE_LEGACY_THUNK

static const char *const emby_controlled_env_gates[] = {
    "SQLITE3_DISABLE_AUTOPRAGMA",
    "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
    "SQLITE3_DISABLE_OBSERVABILITY",
    "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
    "SQLITE3_DISABLE_EMBY_FTS_REWRITE",
    "SQLITE3_DISABLE_EMBY_FANOUT_REWRITE",
    "SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE",
    "SQLITE3_DISABLE_STMT_TRACE",
    "SQLITE3_DISABLE_REWRITE_APPLIED_SQL"
};

static const rsh_env_assignment emby_exec_observable_env[] = {
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_env_assignment emby_exec_master_disabled_env[] = {
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    }
};

static const rsh_env_assignment emby_negative_control_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_FTS_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_FANOUT_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_db_spec emby_negative_control_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const char emby_vendor_prepare_control_sql[] =
    "SELECT 'emby-negative-control-vendor-prepare' FROM";

static const rsh_case_spec emby_vendor_prepare_control_cases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_STATIC,
            .sql = emby_vendor_prepare_control_sql,
            .discriminating_needle = "emby-negative-control-vendor-prepare",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_case_spec emby_matcher_miss_control_cases[] = {
    {
        .label = "negative-control-matcher-miss",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_FIXTURE,
            .fixture_path = "tests/fixtures/emby-fts-rewrite/slow-type.sql",
            .strip_fixture_final_lf = 1,
            .discriminating_needle = "fts_search9 match @SearchTerm",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_phase_spec emby_vendor_prepare_control_phases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .dbs = emby_negative_control_dbs,
        .db_count = sizeof(emby_negative_control_dbs) /
                    sizeof(emby_negative_control_dbs[0]),
        .cases = emby_vendor_prepare_control_cases,
        .case_count = sizeof(emby_vendor_prepare_control_cases) /
                      sizeof(emby_vendor_prepare_control_cases[0])
    }
};

static const rsh_phase_spec emby_matcher_miss_control_phases[] = {
    {
        .label = "negative-control-matcher-miss",
        .dbs = emby_negative_control_dbs,
        .db_count = sizeof(emby_negative_control_dbs) /
                    sizeof(emby_negative_control_dbs[0]),
        .cases = emby_matcher_miss_control_cases,
        .case_count = sizeof(emby_matcher_miss_control_cases) /
                      sizeof(emby_matcher_miss_control_cases[0])
    }
};

static const char *const emby_vendor_prepare_earlier_labels[] = {
    "FAIL [negative-control-vendor-prepare/needle-unique]"
};

static const rsh_abort_expectation emby_vendor_prepare_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-vendor-prepare/vendor-prepare]",
    .earlier_stage_labels = emby_vendor_prepare_earlier_labels,
    .earlier_stage_label_count = sizeof(emby_vendor_prepare_earlier_labels) /
                                 sizeof(emby_vendor_prepare_earlier_labels[0])
};

static const char *const emby_matcher_miss_earlier_labels[] = {
    "FAIL [negative-control-matcher-miss/needle-unique]",
    "FAIL [negative-control-matcher-miss/vendor-prepare]",
    "FAIL [negative-control-matcher-miss/vendor-sql]",
    "FAIL [negative-control-matcher-miss/vendor-tail]",
    "FAIL [negative-control-matcher-miss/vendor-finalize]",
    "FAIL [negative-control-matcher-miss/matcher-prepare]"
};

static const rsh_abort_expectation emby_matcher_miss_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-matcher-miss/matcher-miss]",
    .earlier_stage_labels = emby_matcher_miss_earlier_labels,
    .earlier_stage_label_count = sizeof(emby_matcher_miss_earlier_labels) /
                                 sizeof(emby_matcher_miss_earlier_labels[0])
};

static const rsh_suite_spec emby_suite_spec = {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    .suite_name = "emby fts rewrite direct test passed",
    .prepare = rsh_direct_prepare,
#else
    .suite_name = "emby fts rewrite smoke passed",
    .prepare = rsh_public_prepare,
#endif
    .temp_prefix = "/tmp/emby-fts-rewrite-smoke",
    .vendor_basename = "not-target.db",
    .target_basename = "library.db",
    .controlled_env_gates = emby_controlled_env_gates,
    .controlled_env_gate_count =
        sizeof(emby_controlled_env_gates) / sizeof(emby_controlled_env_gates[0]),
    .open = rsh_open,
    .base_seed = rsh_base_seed,
    .resolve_setup_profile = rsh_resolve_setup_profile,
    .failure = failf
};

#define LEGACY_RUN(dispatch, label, process, mask, body) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (label), \
        .process_kind = (process), \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = (mask), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .legacy_body = (body) \
    }

#define LEGACY_EXEC_RUN(dispatch, label, env, mask, body) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (label), \
        .process_kind = RSH_PROCESS_EXEC, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = (mask), \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .legacy_body = (body) \
    }

static const rsh_run_spec emby_runs[] = {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    LEGACY_RUN("direct-test-api", "direct-test-api", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_DIRECT, legacy_direct_test_api),
    LEGACY_RUN("direct-fail-open-episodes", "direct-fail-open-episodes",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_DIRECT,
               legacy_direct_fail_open_episodes),
    LEGACY_RUN("direct-fail-open-movies", "direct-fail-open-movies",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_DIRECT,
               legacy_direct_fail_open_movies),
    LEGACY_RUN("direct-fail-open-mixed", "direct-fail-open-mixed",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_DIRECT,
               legacy_direct_fail_open_mixed),
#endif
    LEGACY_RUN("positive", "positive", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_positive),
    LEGACY_RUN("fts-default-on", "fts-default-on", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fts_default_on),
    LEGACY_RUN("fts-zero-enables", "fts-zero-enables", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fts_zero_enables),
    LEGACY_RUN("fts-one-disables", "fts-one-disables", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fts_one_disables),
    LEGACY_RUN("fts-garbage-enables", "fts-garbage-enables", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fts_garbage_enables),
    LEGACY_RUN("path-negative", "path-negative", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_path_negative),
    LEGACY_RUN("nonmatch", "nonmatch", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_nonmatch),
    LEGACY_RUN("collision", "collision", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_collision),
    LEGACY_RUN("authorizer-ownership", "authorizer-ownership", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_authorizer_ownership),
    LEGACY_RUN("fixture-canary", "fixture-canary", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fixture_canary),
    LEGACY_RUN("row-parity", "row-parity", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_row_parity),
    LEGACY_RUN("complex-resume-watched-progress-row-parity",
               "complex-resume-watched-progress-row-parity", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED,
               legacy_complex_resume_watched_progress_row_parity),
    LEGACY_RUN("fanout-default-on", "fanout-default-on", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fanout_default_on),
    LEGACY_RUN("fanout-zero-enables", "fanout-zero-enables", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fanout_zero_enables),
    LEGACY_RUN("fanout-one-disables", "fanout-one-disables", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fanout_one_disables),
    LEGACY_RUN("fanout-garbage-enables", "fanout-garbage-enables",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_fanout_garbage_enables),
    LEGACY_RUN("fanout-matrix", "fanout-matrix", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_fanout_matrix),
    LEGACY_RUN("links-two-level-row-parity", "links-two-level-row-parity",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_links_two_level_row_parity),
    LEGACY_RUN("emby-slow-search-matrix", "emby-slow-search-matrix",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_emby_slow_search_matrix),
    LEGACY_RUN("dashboard-default-off", "dashboard-default-off", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_default_off),
    LEGACY_RUN("dashboard-one-off", "dashboard-one-off", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_one_off),
    LEGACY_RUN("dashboard-empty-off", "dashboard-empty-off", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_empty_off),
    LEGACY_RUN("dashboard-garbage-off", "dashboard-garbage-off", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_garbage_off),
    LEGACY_RUN("dashboard-matrix", "dashboard-matrix", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_matrix),
    LEGACY_RUN("dashboard-fix-b-c", "dashboard-fix-b-c", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_dashboard_fix_b_c),
    LEGACY_RUN("dashboard-mixed-latest", "dashboard-mixed-latest-real-path",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_dashboard_mixed_latest),
    LEGACY_RUN("dashboard-mixed-disabled", "dashboard-mixed-disabled",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_dashboard_mixed_disabled),
    LEGACY_RUN("dashboard-mixed-identity", "dashboard-mixed-identity",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_dashboard_mixed_identity),
    LEGACY_RUN("dashboard-matcher-negatives", "dashboard-matcher-negatives",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_dashboard_matcher_negatives),
    LEGACY_RUN("dashboard-release-matrix", "dashboard-release-matrix",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_dashboard_release_matrix),
    LEGACY_RUN("latest-limit20", "latest-limit20", RSH_PROCESS_FORK,
               RSH_BUILD_EMBY_LINKED, legacy_latest_limit20),
    LEGACY_RUN("latest-capture-miss-negative", "latest-capture-miss-negative",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_latest_capture_miss_negative),
    LEGACY_RUN("latest-series-browse-negative", "latest-series-browse-negative",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_latest_series_browse_negative),
    LEGACY_RUN("latest-resume-no-misfire", "latest-resume-no-misfire",
               RSH_PROCESS_FORK, RSH_BUILD_EMBY_LINKED,
               legacy_latest_resume_no_misfire),
    LEGACY_EXEC_RUN(
        "dashboard-index-definition-gate", "dashboard-index-definition-gate",
        emby_exec_observable_env, RSH_BUILD_EMBY_LINKED,
        legacy_dashboard_index_definition_gate
    ),
    LEGACY_EXEC_RUN(
        "observability-logs", "observability-logs", emby_exec_observable_env,
        RSH_BUILD_EMBY_LINKED, legacy_observability_logs
    ),
    LEGACY_EXEC_RUN(
        "observability-master-disabled", "observability-master-disabled",
        emby_exec_master_disabled_env, RSH_BUILD_EMBY_LINKED,
        legacy_observability_master_disabled
    ),
    {
        .dispatch_name = "negative-control-vendor-prepare",
        .pass_label = "negative-control-vendor-prepare",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_negative_control_env,
        .preload_env_count = sizeof(emby_negative_control_env) /
                             sizeof(emby_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = emby_vendor_prepare_control_phases,
        .phase_count = sizeof(emby_vendor_prepare_control_phases) /
                       sizeof(emby_vendor_prepare_control_phases[0]),
        .abort_expectation = &emby_vendor_prepare_abort_expectation
    },
    {
        .dispatch_name = "negative-control-matcher-miss",
        .pass_label = "negative-control-matcher-miss",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_EMBY_LINKED,
        .preload_env = emby_negative_control_env,
        .preload_env_count = sizeof(emby_negative_control_env) /
                             sizeof(emby_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = emby_matcher_miss_control_phases,
        .phase_count = sizeof(emby_matcher_miss_control_phases) /
                       sizeof(emby_matcher_miss_control_phases[0]),
        .abort_expectation = &emby_matcher_miss_abort_expectation
    }
};

#undef LEGACY_EXEC_RUN
#undef LEGACY_RUN

int main(int argc, char **argv) {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    const uint32_t active_build = RSH_BUILD_EMBY_DIRECT;
#else
    const uint32_t active_build = RSH_BUILD_EMBY_LINKED;
#endif

    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return rsh_run_child(
            &emby_suite_spec, emby_runs,
            sizeof(emby_runs) / sizeof(emby_runs[0]), active_build, argv[2]
        );
    }
    return rsh_run_all(
        &emby_suite_spec, emby_runs,
        sizeof(emby_runs) / sizeof(emby_runs[0]), argv[0], active_build
    );
}
