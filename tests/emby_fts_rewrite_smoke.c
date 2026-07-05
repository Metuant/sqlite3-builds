#include "sqlite3.h"

#include <errno.h>
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
    if (kind == FTS_REWRITE_PREPARE_V3) {
        return sqlite3_prepare_v3(db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (kind == FTS_REWRITE_PREPARE_V2) {
        return sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
    }
    (void)prepFlags;
    return sqlite3_prepare(db, zSql, nByte, ppStmt, pzTail);
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...) {
    (void)fn;
    (void)fmt;
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

static int scalar_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
);

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

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

static sqlite3 *open_seeded_temp(const char *basename) {
    char path[512];
    sqlite3 *db;

    temp_path(path, sizeof(path), basename);
    unlink(path);
    db = open_db_path(path);
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
        "IsPublic INTEGER, ExtraType INTEGER);"
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
    return db;
}

static sqlite3 *open_seeded_temp_with_latest_index(const char *basename, int with_latest_index) {
    sqlite3 *db = open_seeded_temp(basename);
    if (with_latest_index) {
        exec_sql(db, "latest-index",
            "CREATE INDEX idx_dshadow_emby_latest_gk_dc "
            "ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) "
            "WHERE Type = 8;"
        );
    }
    return db;
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

static char *make_exists_ancestor(const char *l1) {
    return xasprintf(
        "EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))",
        l1
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
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select count(*) OVER() AS TotalRecordCount,A.Id,A.SeriesPresentationUniqueKey "
        "from mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 "
        "left join (select N.SeriesPresentationUniqueKey,max(userdatas.lastplayeddateint) as lastplayeddateint from mediaitems N left join UserDatas on N.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=1 where N.Type=8 Group by N.SeriesPresentationUniqueKey) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey "
        "where A.Type=8 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) DESC,Min(EpisodeAbsoluteIndexNumber) ASC LIMIT 12"
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

static char *make_latest_sql(const char *projection, const char *limit) {
    return xasprintf(
        "with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (100) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=42 "
        "where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT %s",
        projection, limit
    );
}

static char *make_latest_expected(const char *projection, const char *limit) {
    return xasprintf(
        "WITH keys(gk) AS MATERIALIZED (SELECT DISTINCT coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey) FROM MediaItems WHERE Type = 8), picked AS MATERIALIZED (SELECT K.gk, (SELECT A2.Id FROM MediaItems AS A2 WHERE A2.Type = 8 AND coalesce(A2.SeriesPresentationUniqueKey, A2.PresentationUniqueKey) IS K.gk AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A2.Id AND X.AncestorId IN (100)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS U2 WHERE U2.UserDataKeyId = A2.UserDataKeyId AND U2.UserId = 42 AND U2.played <> 0) ORDER BY A2.DateCreated DESC LIMIT 1) AS id FROM keys AS K), exact_groups AS MATERIALIZED (SELECT P.gk, P.id, Amax.DateCreated AS maxdc FROM picked AS P JOIN MediaItems AS Amax ON Amax.Id = P.id WHERE P.id IS NOT NULL), ranked AS MATERIALIZED (SELECT gk, id, maxdc FROM exact_groups ORDER BY maxdc DESC LIMIT %s) SELECT %s FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = 42 ORDER BY R.maxdc DESC LIMIT %s",
        limit, projection, limit
    );
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

static void collect_ids(sqlite3 *db, const char *label, const char *sql, int nbyte,
                        int want_rewrite, const char *search_term, char *out, size_t out_n) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, 2, &tail);
    int index = sqlite3_bind_parameter_index(stmt, "@SearchTerm");
    int rc;
    int rows = 0;
    size_t used = 0;

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
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n;
        int id = sqlite3_column_int(stmt, 2);
        n = snprintf(out + used, out_n - used, "%d,", id);
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: row-id buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("row-parity/finalize", sqlite3_finalize(stmt), SQLITE_OK);
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

static int child_positive(void) {
    sqlite3 *db;
    char *type_sql;
    char *numbered_sql;
    char *presentation_sql;
    char *type_expected;
    char *numbered_expected;
    char *presentation_expected;

    configure_env(NULL, NULL, NULL);
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

    configure_env("0", NULL, NULL);
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
#endif

static int child_fts_env(const char *value, const char *label, int want_rewrite) {
    sqlite3 *db;
    char *sql;
    char *expected;

    configure_env(value, NULL, NULL);
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

    configure_env("0", NULL, NULL);
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

    configure_env("0", NULL, NULL);
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
        "fanout-resume",
        "fanout-people",
        "fanout-links-search",
        "fanout-browse",
        "fanout-favorites",
        "latest-limit12",
        "latest-limit16",
        "latest-limit20",
        "latest-seriesname-projection",
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
    db = open_seeded_temp_with_latest_index("library.db", 1);
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
    char original_ids[128];
    char rewritten_ids[128];

    configure_env("0", NULL, NULL);
    make_temp_dir();
    original_db = open_seeded_temp("not-target.db");
    rewritten_db = open_seeded_temp("library.db");
    sql = make_emby_sql(1, "100", "100", "6", "7", 1);

    collect_ids(original_db, "row-parity-original", sql, -1, 0, "(\"alpha\"*)",
                original_ids, sizeof(original_ids));
    collect_ids(rewritten_db, "row-parity-rewritten", sql, (int)strlen(sql) + 1, 1,
                "(\"alpha\"*)", rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("row-parity/ids", rewritten_ids, original_ids);
    require_str_eq("row-parity/expected-arms", rewritten_ids, "1,2,3,4,5,");
    collect_ids(original_db, "row-parity-or-original", sql, -1, 0,
                "(\"alpha\"*) OR (\"alpha\"*)", original_ids, sizeof(original_ids));
    collect_ids(rewritten_db, "row-parity-or-rewritten", sql, (int)strlen(sql) + 1, 1,
                "(\"alpha\"*) OR (\"alpha\"*)", rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("row-parity-or/ids", rewritten_ids, original_ids);
    require_str_eq("row-parity-or/expected-arms", rewritten_ids, "1,2,3,4,5,");

    free(sql);
    require_int("row-parity/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("row-parity/rewrite-close", sqlite3_close(rewritten_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [row-parity]\n");
    return 0;
}

static int child_fanout_default_off(void) {
    sqlite3 *db;
    char *browse_sql;

    configure_env("1", NULL, NULL);
    make_temp_dir();
    db = open_seeded_temp("library.db");
    browse_sql = make_browse_sql();
    expect_sql(db, "fanout-default-off", browse_sql, -1, 2, browse_sql, 0);
    free(browse_sql);
    require_int("fanout-default-off/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [fanout-default-off]\n");
    return 0;
}

static int child_fanout_off_value(const char *value, const char *label) {
    sqlite3 *db;
    char *browse_sql;

    configure_env("1", value, NULL);
    make_temp_dir();
    db = open_seeded_temp("library.db");
    browse_sql = make_browse_sql();
    expect_sql(db, label, browse_sql, -1, 2, browse_sql, 0);
    free(browse_sql);
    require_int("fanout-off-value/close", sqlite3_close(db), SQLITE_OK);
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
    char *resume_ancestor;
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
    favorites_ancestor = make_exists_ancestor("100");
    favorites_one = make_exists_links_one("100", "6");
    favorites_repl = xasprintf("(%s OR %s)", favorites_ancestor, favorites_one);
    favorites_expected = replace_once(favorites_sql, "(A.Id in WithAncestors OR A.Id in WithItemLinkItemIds)", favorites_repl);
    expect_sql(db, "fanout-favorites", favorites_sql, -1, 2, favorites_expected, 0);

    resume_sql = make_resume_sql();
    resume_ancestor = make_exists_ancestor("100");
    resume_expected = replace_once(resume_sql, "A.Id in WithAncestors", resume_ancestor);
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
    free(resume_ancestor);
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

static int child_dashboard_off_value(const char *value, const char *label) {
    sqlite3 *db;
    char *latest_sql;

    configure_env("1", NULL, value);
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    expect_sql(db, label, latest_sql, -1, 2, latest_sql, 0);
    free(latest_sql);
    require_int("dashboard-off-value/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_dashboard_default_off(void) {
    sqlite3 *db;
    char *latest_sql;

    configure_env("1", NULL, NULL);
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    expect_sql(db, "dashboard-default-off", latest_sql, -1, 2, latest_sql, 0);
    free(latest_sql);
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
    char *aggregate_sql;
    char *over_sql;
    char original_ids[128];
    char rewritten_ids[128];

    configure_env("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    latest_sql = make_latest_sql("A.Id,A.SeriesName,A.SortName ", "12");
    latest_expected = make_latest_expected("A.Id,A.SeriesName,A.SortName ", "12");
    expect_sql(db, "dashboard-latest", latest_sql, -1, 2, latest_expected, 0);
    expect_sql(db, "dashboard-latest-nbyte-with-nul", latest_sql,
               (int)strlen(latest_sql) + 1, 2, latest_expected, 0);
    latest16_sql = make_latest_sql("A.Id ", "16");
    latest16_expected = make_latest_expected("A.Id ", "16");
    expect_sql(db, "dashboard-latest-limit16", latest16_sql, -1, 2, latest16_expected, 0);

    aggregate_sql = make_latest_sql("MAX(A.DateCreated) ", "12");
    expect_sql(db, "dashboard-aggregate-negative", aggregate_sql, -1, 2, aggregate_sql, 0);
    over_sql = make_latest_sql("count(*) OVER() AS TotalRecordCount,A.Id ", "12");
    expect_sql(db, "dashboard-over-negative", over_sql, -1, 2, over_sql, 0);

    original_db = open_seeded_temp_with_latest_index("not-target.db", 1);
    collect_first_ints(original_db, "latest-row-original", latest_sql, latest_sql,
                       original_ids, sizeof(original_ids));
    collect_first_ints(db, "latest-row-rewritten", latest_sql, latest_expected,
                       rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("latest-row-identity", rewritten_ids, original_ids);

    require_int("dashboard/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("dashboard/index-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    configure_env("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 0);
    expect_sql(db, "dashboard-index-absent", latest_sql, -1, 2, latest_sql, 0);
    require_int("dashboard/index-absent-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    free(latest_sql);
    free(latest_expected);
    free(latest16_sql);
    free(latest16_expected);
    free(aggregate_sql);
    free(over_sql);
    printf("PASS [dashboard-matrix]\n");
    return 0;
}

static int child_latest_limit20(void) {
    sqlite3 *db;

    configure_env("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    expect_fixture(db, "latest-limit20");
    require_int("latest-limit20/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [latest-limit20]\n");
    return 0;
}

static int child_latest_fixture_passthrough(const char *name) {
    sqlite3 *db;

    configure_env("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    expect_fixture(db, name);
    require_int("latest-fixture-passthrough/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", name);
    return 0;
}

static int child_latest_resume_no_misfire(void) {
    sqlite3 *db;
    char *resume_sql;

    configure_env("1", NULL, "0");
    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    resume_sql = make_resume_sql();
    expect_sql(db, "latest-resume-no-misfire", resume_sql, -1, 2, resume_sql, 0);
    free(resume_sql);
    require_int("latest-resume-no-misfire/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [latest-resume-no-misfire]\n");
    return 0;
}

static int child_observability_logs(void) {
    sqlite3 *db;
    FILE *capture;
    char *log_output;
    char *links_sql;
    char *links_repl;
    char *links_expected;
    char *links_miss_sql;
    char *latest_sql;
    char *latest_expected;
    char *latest_miss_sql;
    char *latest_series_sql;
    int saved_stderr_fd;

    configure_env_observable("1", "0", "0");
    make_temp_dir();
    capture = begin_stderr_capture(&saved_stderr_fd);

    db = open_seeded_temp_with_latest_index("library.db", 1);
    links_sql = make_links_search_sql("2,3");
    links_repl = make_exists_links_one("100", "2,3");
    links_expected = replace_once(links_sql, "A.Id in WithItemLinkItemIds", links_repl);
    expect_sql(db, "obs-fanout-links-applied", links_sql, -1, 2, links_expected, 0);

    links_miss_sql = make_links_search_sql("'bad'");
    expect_sql(db, "obs-fanout-links-capture-miss", links_miss_sql, -1, 2, links_miss_sql, 0);

    latest_sql = make_latest_sql("A.Id ", "12");
    latest_expected = make_latest_expected("A.Id ", "12");
    expect_sql(db, "obs-dashboard-latest-applied", latest_sql, -1, 2, latest_expected, 0);

    latest_miss_sql = make_latest_sql("(A.DateCreated) ", "12");
    expect_sql(db, "obs-dashboard-capture-miss", latest_miss_sql, -1, 2, latest_miss_sql, 0);
    latest_series_sql = read_text_file(
        "tests/fixtures/emby-fts-rewrite/latest-series-browse-negative.sql"
    );
    expect_sql(db, "obs-dashboard-series-browse-clean-miss", latest_series_sql,
               -1, 2, latest_series_sql, 0);
    require_int("observability/index-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 0);
    expect_sql(db, "obs-dashboard-index-missing", latest_sql, -1, 2, latest_sql, 0);
    require_int("observability/no-index-close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp_with_latest_index("library.db", 1);
    require_int("observability/probe-authorizer/set",
                sqlite3_set_authorizer(db, deny_sqlite_master_read, NULL), SQLITE_OK);
    expect_sql(db, "obs-dashboard-index-probe-error", latest_sql, -1, 2, latest_sql, 0);
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
    require_contains(
        "observability/dashboard-applied",
        log_output,
        "event=rewrite_applied target=emby mode=dashboard+latest"
    );
    require_contains(
        "observability/fanout-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=fanout+links_search"
    );
    require_contains(
        "observability/dashboard-capture-miss",
        log_output,
        "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+latest"
    );
    require_int(
        "observability/dashboard-capture-miss-count",
        count_occurrences(
            log_output,
            "event=rewrite_skipped target=emby reason=capture_miss mode=dashboard+latest"
        ),
        1
    );
    require_contains(
        "observability/dashboard-index-missing",
        log_output,
        "event=rewrite_skipped target=emby reason=index_missing mode=dashboard+latest"
    );
    require_contains(
        "observability/dashboard-index-probe-error",
        log_output,
        "event=rewrite_skipped target=emby reason=index_probe_error mode=dashboard+latest"
    );

    free(log_output);
    free(links_sql);
    free(links_repl);
    free(links_expected);
    free(links_miss_sql);
    free(latest_sql);
    free(latest_expected);
    free(latest_miss_sql);
    free(latest_series_sql);
    printf("PASS [observability-logs]\n");
    return 0;
}

static int child_collision(void) {
    sqlite3 *db;
    char *sql;

    configure_env("0", NULL, NULL);
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

    configure_env("0", NULL, NULL);
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

static void run_exec_child(const char *program, const char *name) {
    pid_t pid = fork();
    int status;

    if (pid < 0) failf("FATAL: fork-exec(%s) failed: %s", name, strerror(errno));
    if (pid == 0) {
        char *const args[] = {(char *)program, "--child", (char *)name, NULL};
        if (unsetenv("SQLITE3_DISABLE_OBSERVABILITY") != 0) {
            failf("FATAL: exec-child unsetenv OBS failed: %s", strerror(errno));
        }
        if (strchr(program, '/')) {
            execv(program, args);
        } else {
            execvp(program, args);
        }
        failf("FATAL: exec child %s failed: %s", name, strerror(errno));
    }
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid(%s) failed: %s", name, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d", name, status);
    }
}

static void run_child(const char *name) {
    pid_t pid = fork();
    int status;
    if (pid < 0) failf("FATAL: fork(%s) failed: %s", name, strerror(errno));
    if (pid == 0) {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
        if (strcmp(name, "direct-test-api") == 0) _exit(child_direct_test_api());
#endif
        if (strcmp(name, "positive") == 0) _exit(child_positive());
        if (strcmp(name, "fts-default-on") == 0) _exit(child_fts_env(NULL, "fts-default-on", 1));
        if (strcmp(name, "fts-zero-enables") == 0) _exit(child_fts_env("0", "fts-zero-enables", 1));
        if (strcmp(name, "fts-one-disables") == 0) _exit(child_fts_env("1", "fts-one-disables", 0));
        if (strcmp(name, "fts-garbage-enables") == 0) _exit(child_fts_env("xyz", "fts-garbage-enables", 1));
        if (strcmp(name, "path-negative") == 0) _exit(child_path_negative());
        if (strcmp(name, "nonmatch") == 0) _exit(child_nonmatch());
        if (strcmp(name, "collision") == 0) _exit(child_collision());
        if (strcmp(name, "authorizer-ownership") == 0) _exit(child_authorizer_and_ownership());
        if (strcmp(name, "fixture-canary") == 0) _exit(child_fixture_canary());
        if (strcmp(name, "row-parity") == 0) _exit(child_row_parity());
        if (strcmp(name, "fanout-default-off") == 0) _exit(child_fanout_default_off());
        if (strcmp(name, "fanout-one-off") == 0) _exit(child_fanout_off_value("1", "fanout-one-off"));
        if (strcmp(name, "fanout-garbage-off") == 0) _exit(child_fanout_off_value("false", "fanout-garbage-off"));
        if (strcmp(name, "fanout-matrix") == 0) _exit(child_fanout_matrix());
        if (strcmp(name, "dashboard-default-off") == 0) _exit(child_dashboard_default_off());
        if (strcmp(name, "dashboard-one-off") == 0) _exit(child_dashboard_off_value("1", "dashboard-one-off"));
        if (strcmp(name, "dashboard-garbage-off") == 0) _exit(child_dashboard_off_value("false", "dashboard-garbage-off"));
        if (strcmp(name, "dashboard-matrix") == 0) _exit(child_dashboard_matrix());
        if (strcmp(name, "latest-limit20") == 0) _exit(child_latest_limit20());
        if (strcmp(name, "latest-capture-miss-negative") == 0) {
            _exit(child_latest_fixture_passthrough("latest-capture-miss-negative"));
        }
        if (strcmp(name, "latest-series-browse-negative") == 0) {
            _exit(child_latest_fixture_passthrough("latest-series-browse-negative"));
        }
        if (strcmp(name, "latest-resume-no-misfire") == 0) _exit(child_latest_resume_no_misfire());
        failf("FATAL: unknown child %s", name);
    }
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid(%s) failed: %s", name, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d", name, status);
    }
}

int main(int argc, char **argv) {
#ifdef EMBY_FTS_REWRITE_DIRECT_TEST
    (void)argc;
    (void)argv;
    run_child("direct-test-api");
    printf("emby fts rewrite direct test passed\n");
    return 0;
#else
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        if (strcmp(argv[2], "observability-logs") == 0) return child_observability_logs();
        failf("FATAL: unknown exec child %s", argv[2]);
    }
    run_child("positive");
    run_child("fts-default-on");
    run_child("fts-zero-enables");
    run_child("fts-one-disables");
    run_child("fts-garbage-enables");
    run_child("path-negative");
    run_child("nonmatch");
    run_child("collision");
    run_child("authorizer-ownership");
    run_child("fixture-canary");
    run_child("row-parity");
    run_child("fanout-default-off");
    run_child("fanout-one-off");
    run_child("fanout-garbage-off");
    run_child("fanout-matrix");
    run_child("dashboard-default-off");
    run_child("dashboard-one-off");
    run_child("dashboard-garbage-off");
    run_child("dashboard-matrix");
    run_child("latest-limit20");
    run_child("latest-capture-miss-negative");
    run_child("latest-series-browse-negative");
    run_child("latest-resume-no-misfire");
    run_exec_child(argv[0], "observability-logs");
    printf("emby fts rewrite smoke passed\n");
    return 0;
#endif
}
