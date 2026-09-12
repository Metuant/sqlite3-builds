#ifndef PLEX_TITLE_SCOPE_REWRITE_H
#define PLEX_TITLE_SCOPE_REWRITE_H
#include "plex_fts_join_rewrite.h"

/* Only the observed title + user-tag projection, with two exact sort shapes.
 * tags.id=<scalar> rejects NULL-extended rows, making these two LEFT JOINs
 * inner joins. Duplicate links are retained; no DISTINCT/LIMIT is introduced.
 * No FROM-clause binds exist. The entire predicate/sort/page suffix is copied.
 */
/* Double-quoted tokens may resolve to columns under SQLite DQS; they are
 * not safe constants when changing loop order. Only single quotes or binds. */
static int plex_title_scope_scalar(fts_lex *lx, int match_value) {
    fts_lex look=*lx;
    fts_lex_token t=fts_lex_next_token(&look);
    if (t.type==FTS_LEX_TOK_STRING && lx->sql[t.start]!='\'') return 0;
    return plex_join_scalar(lx,match_value);
}
static char *plex_build_title_scope_sql(const char *sql, size_t bounded_len,
                                      size_t scan_len, size_t *out_scan,
                                      size_t *out_len) {
    static const char ancestors[] =
        " JOIN metadata_items AS parents ON parents.id=metadata_items.parent_id"
        " JOIN metadata_items AS grandparents ON grandparents.id=parents.parent_id";
    static const char from[] =
        " FROM tags CROSS JOIN fts4_metadata_titles_icu"
        " CROSS JOIN taggings INDEXED BY idx_dshadow_taggings_tag_id_metadata_item_id"
        " ON tags.id=taggings.tag_id"
        " AND taggings.metadata_item_id=fts4_metadata_titles_icu.rowid"
        " CROSS JOIN metadata_items ON metadata_items.id=fts4_metadata_titles_icu.rowid"
        " AND taggings.metadata_item_id=metadata_items.id";
    static const char movie_order[] =
        "metadata_items.title_sort COLLATE icu_root ASC,"
        "metadata_items.originally_available_at ASC,metadata_items.id ASC";
    static const char episode_order[] =
        "grandparents.title_sort COLLATE icu_root ASC,"
        "parents.`index` IS NULL,parents.`index` ASC,"
        "metadata_items.`index` IS NULL,metadata_items.`index` ASC,"
        "metadata_items.originally_available_at IS NULL,"
        "metadata_items.originally_available_at ASC,"
        "metadata_items.title_sort COLLATE icu_root ASC,metadata_items.id ASC";
    fts_lex lx, look;
    fts_lex_token t;
    size_t a,b,n,insert_len;
    int hierarchy=0;
    char *r;
    if (!sql || !out_scan || !out_len || bounded_len>64u*1024u ||
        scan_len>bounded_len || bounded_len-scan_len>1 ||
        (bounded_len>scan_len && sql[scan_len]!=0) || memchr(sql,0,scan_len)) return NULL;
    fts_lex_init(&lx,sql,scan_len,FTS_LEX_PARAM_SQLITE_VARIABLE);
    if (!plex_join_expect(&lx,"SELECT metadata_items.id")) return NULL;
    a=lx.pos;
    if (!plex_join_expect(&lx,"FROM metadata_items")) return NULL;
    look=lx;
    if (plex_join_expect(&look,ancestors)) { lx=look; hierarchy=1; }
    if (!plex_join_expect(&lx,
        "JOIN fts4_metadata_titles_icu ON metadata_items.id=fts4_metadata_titles_icu.rowid"
        " LEFT JOIN taggings ON taggings.metadata_item_id=metadata_items.id"
        " LEFT JOIN tags ON taggings.tag_id=tags.id WHERE")) return NULL;
    b=lx.pos;
    if (!plex_join_expect(&lx,"fts4_metadata_titles_icu.")) return NULL;
    t=fts_lex_next_token(&lx);
    if (t.type!=FTS_LEX_TOK_IDENT ||
        (!fts_lex_token_text_eq(sql,&t,"title") &&
         !fts_lex_token_text_eq(sql,&t,"title_sort"))) return NULL;
    if (!plex_join_expect(&lx,"MATCH") || !plex_title_scope_scalar(&lx,1) ||
        !plex_join_expect(&lx,"AND metadata_items.metadata_type=") ||
        !plex_title_scope_scalar(&lx,0) || !plex_join_expect(&lx,"AND tags.id=") ||
        !plex_title_scope_scalar(&lx,0) || !plex_join_expect(&lx,"ORDER BY") ||
        !plex_join_expect(&lx,hierarchy ? episode_order : movie_order) ||
        !plex_join_expect(&lx,"LIMIT") || !plex_title_scope_scalar(&lx,0) ||
        !plex_join_expect(&lx,"OFFSET") || !plex_title_scope_scalar(&lx,0)) return NULL;
    t=fts_lex_next_token(&lx);
    if (t.type!=FTS_LEX_TOK_EOF) return NULL;
    insert_len=sizeof(from)-1+(hierarchy ? sizeof(ancestors)-1 : 0)+6;
    n=a+insert_len+bounded_len-b;
    if (n>64u*1024u) return NULL;
    r=malloc(n+1);
    if (!r) return NULL;
    memcpy(r,sql,a);memcpy(r+a,from,sizeof(from)-1);
    if (hierarchy) memcpy(r+a+sizeof(from)-1,ancestors,sizeof(ancestors)-1);
    memcpy(r+a+insert_len-6," WHERE",6);
    memcpy(r+a+insert_len,sql+b,bounded_len-b);r[n]=0;
    *out_len=n;*out_scan=a+insert_len+scan_len-b;
    return r;
}
#endif
