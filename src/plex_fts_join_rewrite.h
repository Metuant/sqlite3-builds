#ifndef PLEX_FTS_JOIN_REWRITE_H
#define PLEX_FTS_JOIN_REWRITE_H

/* Private, conservative recognizer for the observed PMS aggregate tag search.
 * No MATCH-language parsing: every scalar/bind and the complete WHERE suffix
 * are copied verbatim. FROM has no binds, so numbering/order cannot change.
 */
#include "fts_lex.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int plex_join_expect(fts_lex *input, const char *pattern) {
    fts_lex expected;
    fts_lex_init(&expected, pattern, strlen(pattern), FTS_LEX_PARAM_SQLITE_VARIABLE);
    for (;;) {
        fts_lex_token e = fts_lex_next_token(&expected), a;
        char word[80]; size_t n;
        if (e.type == FTS_LEX_TOK_EOF) return 1;
        a = fts_lex_next_token(input);
        if (a.type != e.type) return 0;
        if (e.type == FTS_LEX_TOK_SYMBOL) {
            if (a.symbol != e.symbol) return 0;
        } else if (e.type == FTS_LEX_TOK_IDENT) {
            n = e.end - e.start;
            if (n >= sizeof(word)) return 0;
            memcpy(word, pattern + e.start, n); word[n] = 0;
            if (!fts_lex_token_text_eq(input->sql, &a, word)) return 0;
        } else return 0;
    }
}

static int plex_join_scalar(fts_lex *input, int match_value) {
    fts_lex_token t = fts_lex_next_token(input);
    size_t i;
    if (t.type == FTS_LEX_TOK_PARAM || t.type == FTS_LEX_TOK_STRING) return 1;
    if (match_value || t.type != FTS_LEX_TOK_NUMBER || t.start == t.end) return 0;
    for (i = t.start; i < t.end; ++i)
        if (input->sql[i] < '0' || input->sql[i] > '9') return 0;
    return 1;
}

/* NULL means unsupported input or allocation failure: caller keeps old path. */
static char *plex_build_fts_join_sql(const char *sql, size_t bounded_len,
                                    size_t scan_len, size_t *out_scan,
                                    size_t *out_len) {
    static const char from[] =
        " FROM fts4_tag_titles_icu"
        " CROSS JOIN tags ON fts4_tag_titles_icu.rowid=tags.id"
        " CROSS JOIN taggings INDEXED BY idx_dshadow_taggings_tag_id_metadata_item_id"
        " ON tags.id=taggings.tag_id"
        " CROSS JOIN metadata_items ON taggings.metadata_item_id=metadata_items.id"
        " WHERE";
    fts_lex lx, look;
    fts_lex_token t;
    size_t a, b, n, i;
    int wrapped = 0;
    char *result;
    if (!sql || !out_scan || !out_len || bounded_len > 64u * 1024u ||
        scan_len > bounded_len ||
        bounded_len - scan_len > 1 ||
        (bounded_len > scan_len && sql[scan_len] != 0) ||
        memchr(sql, 0, scan_len)) return NULL;
    fts_lex_init(&lx, sql, scan_len, FTS_LEX_PARAM_SQLITE_VARIABLE);
    if (!plex_join_expect(&lx, "SELECT DISTINCT(tags.id)")) return NULL;
    a = lx.pos;
    if (!plex_join_expect(&lx,
        "FROM metadata_items JOIN taggings ON taggings.metadata_item_id=metadata_items.id"
        " JOIN tags ON tags.id=taggings.tag_id JOIN fts4_tag_titles_icu"
        " ON fts4_tag_titles_icu.rowid=tags.id WHERE")) return NULL;
    b = lx.pos;
    if (!plex_join_expect(&lx, "fts4_tag_titles_icu.tag MATCH") ||
        !plex_join_scalar(&lx, 1) || !plex_join_expect(&lx, "AND")) return NULL;
    look = lx; t = fts_lex_next_token(&look);
    if (t.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &t, "unlikely")) {
        if (!plex_join_expect(&lx, "unlikely(")) return NULL;
        wrapped = 1;
    }
    if (!plex_join_expect(&lx, "tag_type=") || !plex_join_scalar(&lx, 0) ||
        (wrapped && !plex_join_expect(&lx, ")")) ||
        !plex_join_expect(&lx, "AND metadata_items.library_section_id IN (")) return NULL;
    for (i = 0; ; ++i) {
        if (i >= 64 || !plex_join_scalar(&lx, 0)) return NULL;
        look = lx; t = fts_lex_next_token(&look);
        if (t.type != FTS_LEX_TOK_SYMBOL || t.symbol != ',') break;
        lx = look;
    }
    if (!plex_join_expect(&lx, ") AND")) return NULL;
    /* PMS album searches exclude remote metadata. Recognize only the exact
     * observed parenthesized predicate; copy it unchanged with the suffix. */
    look = lx; t = fts_lex_next_token(&look);
    wrapped = t.type == FTS_LEX_TOK_SYMBOL && t.symbol == '(';
    if (wrapped) lx = look;
    if (!plex_join_expect(&lx, "metadata_items.metadata_type=") ||
        !plex_join_scalar(&lx, 0)) return NULL;
    if (wrapped && !plex_join_expect(&lx,
        "AND (metadata_items.remote IS NULL))")) return NULL;
    if (!plex_join_expect(&lx, "GROUP BY tags.id ORDER BY count(*) DESC LIMIT") ||
        !plex_join_scalar(&lx, 0)) return NULL;
    t = fts_lex_next_token(&lx);
    if (t.type != FTS_LEX_TOK_EOF) return NULL;
    /* Preserve the optional positive-nByte terminating NUL. The normal tail
     * mapper verifies the rewritten parse ends at the expected boundary. */
    n = a + sizeof(from) - 1 + bounded_len - b;
    if (n > 64u * 1024u) return NULL;
    result = malloc(n + 1);
    if (!result) return NULL;
    memcpy(result, sql, a);
    memcpy(result + a, from, sizeof(from) - 1);
    memcpy(result + a + sizeof(from) - 1, sql + b, bounded_len - b);
    result[n] = 0;
    *out_len = n;
    *out_scan = a + sizeof(from) - 1 + scan_len - b;
    return result;
}
#endif
