/* Test adapter: compile the real production prepare wrapper against stock
 * SQLite. Only its external logging/path hooks and *_real entrypoints are
 * supplied here; no replacement SQL planner or fake query results. */
#include "../src/plex_fts_rewrite.h"
#include "../src/observability.h"
#include "../src/plex_fts_join_rewrite.h"
#include "../src/plex_title_scope_rewrite.h"
#include "../src/plex_collection_tag_rewrite.h"
#include "../src/plex_recent_episode_rewrite.h"
char *test_recent_build(const char*s,size_t n,size_t scan,size_t*o,size_t*l){return plex_build_recent_episode_sql(s,n,scan,o,l);}
char *test_collection_build(const char*s,size_t n,size_t scan,size_t*o,size_t*l){int cols;return plex_build_collection_tag_sql(s,n,scan,o,l,&cols);}
#include <string.h>
#include "../src/plex_fuzzy_match_rewrite.h"
static const char *fuzzy_reason, *fuzzy_miss;
static int fuzzy_applied;
char *test_fuzzy_build(const char*s,size_t n,size_t k,size_t*a,size_t*b){return plex_build_fuzzy_match_sql(s,n,k,a,b,&fuzzy_reason);}
const char *test_fuzzy_reason(void){return fuzzy_reason ? fuzzy_reason : "";}
const char *test_fuzzy_miss(void){return fuzzy_miss ? fuzzy_miss : "";}
int test_fuzzy_applied(void){return fuzzy_applied;}

static int fault_mode, path_allowed = 1, calls, last_kind;
static unsigned last_flags;
static const char *last_skip;
static int is_test_candidate(const char *sql) {
    return strstr(sql, "match '(fault*)'") || strstr(sql, "CROSS JOIN") || strstr(sql, "WITH recent_parameters") ||
        (strstr(sql, "  and metadata_items.metadata_type=") && !strstr(sql, " or "));
}
static int real_prepare(int kind, sqlite3 *db, const char *sql, int n,
                        unsigned flags, sqlite3_stmt **stmt, const char **tail) {
    int rc;
    calls++; last_kind = kind; last_flags = flags;
    if (fault_mode && is_test_candidate(sql)) {
        if (fault_mode == 1) { *stmt = NULL; return SQLITE_ERROR; }
        if (fault_mode == 3 || fault_mode == 4) {
            rc = sqlite3_prepare_v2(db, fault_mode == 3 ? "SELECT 1,2" : "SELECT ?", -1, stmt, tail);
            if (rc == SQLITE_OK) *tail = sql + strlen(sql);
            return rc;
        }
    }
    if (kind == 0) rc = sqlite3_prepare(db, sql, n, stmt, tail);
    else if (kind == 1) rc = sqlite3_prepare_v2(db, sql, n, stmt, tail);
    else rc = sqlite3_prepare_v3(db, sql, n, flags, stmt, tail);
    if (rc == SQLITE_OK && fault_mode == 2 && is_test_candidate(sql)) *tail = sql;
    return rc;
}
int sqlite3_prepare_real(sqlite3 *d,const char *s,int n,sqlite3_stmt **p,const char **t) {
    return real_prepare(0,d,s,n,0,p,t);
}
int sqlite3_prepare_v2_real(sqlite3 *d,const char *s,int n,sqlite3_stmt **p,const char **t) {
    return real_prepare(1,d,s,n,0,p,t);
}
int sqlite3_prepare_v3_real(sqlite3 *d,const char *s,int n,unsigned f,sqlite3_stmt **p,const char **t) {
    return real_prepare(2,d,s,n,f,p,t);
}
int auto_extension_path_is_target(const char *s) { return s && path_allowed; }
int obs_is_disabled(void) { return 1; }
void obs_log_rewrite_miss(obs_rewrite_mode m,obs_miss_reason r,const char *s,sqlite3 *d,const char *q,size_t n,uint64_t h) {if(m==OBS_MODE_PLEX_FUZZY_MATCH)fuzzy_miss=s;(void)r;(void)d;(void)q;(void)n;(void)h;}
void obs_log_index_missing(sqlite3 *d,obs_rewrite_mode m) {(void)d;(void)m;}
void obs_log_rewrite_applied(obs_rewrite_mode m,sqlite3 *d,const char *s,size_t n,const char *r,size_t k) {if(m==OBS_MODE_PLEX_FUZZY_MATCH)++fuzzy_applied;(void)d;(void)s;(void)n;(void)r;(void)k;}
void obs_log_rewrite_skipped(sqlite3 *d,const char *s,obs_rewrite_mode m) {(void)d;(void)m;last_skip=s;}
const char *test_last_skip(void) { return last_skip ? last_skip : ""; }
int test_prepare(sqlite3 *d,const char *s,int n,unsigned f,sqlite3_stmt **p,const char **t,int k) {
    calls = 0; fuzzy_miss=NULL; fuzzy_applied=0;
    return plex_fts_rewrite_prepare(d,s,n,f,p,t,(fts_rewrite_prepare_kind)k);
}
void test_fault(int mode) { fault_mode = mode; }
void test_path_allowed(int yes) { path_allowed = yes; }
int test_calls(void) { return calls; }
int test_kind(void) { return last_kind; }
unsigned test_flags(void) { return last_flags; }
char *test_build(const char *sql, size_t bounded, size_t scan, size_t *out_scan, size_t *out_len) {
    return plex_build_fts_join_sql(sql, bounded, scan, out_scan, out_len);
}
void test_free(void *p) { free(p); }

char *test_title_build(const char *s, size_t n, size_t k, size_t *a, size_t *b) { return plex_build_title_scope_sql(s,n,k,a,b); }
