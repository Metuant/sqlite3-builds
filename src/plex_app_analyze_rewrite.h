#ifndef PLEX_APP_ANALYZE_REWRITE_H
#define PLEX_APP_ANALYZE_REWRITE_H

#include "fts_lex.h"
#include <string.h>

/* Spec 2026-09-13, section 2. Match only the caller's bounded bytes.
 * A NULL miss reason is unrelated traffic, which stays silent. */
static int plex_match_app_analyze(const char *sql, size_t bounded,
    size_t scan, const char **reason) {
    size_t p = 0, start, i;
    *reason = NULL;
    if (!sql || bounded < scan || bounded-scan > 1) return 0;
    while (p < scan && fts_lex_is_space((unsigned char)sql[p])) ++p;
    /* Leading comments are ineligible, but still capture ANALYZE misses. */
    if (p+1 < scan && ((sql[p]=='-' && sql[p+1]=='-') ||
                      (sql[p]=='/' && sql[p+1]=='*'))) {
        fts_lex lx;
        fts_lex_token t;
        fts_lex_init(&lx, sql, scan, FTS_LEX_PARAM_SQLITE_VARIABLE);
        t = fts_lex_next_token(&lx);
        if (t.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql,&t,"analyze"))
            *reason = "comment";
        return 0;
    }
    if (p == scan || (sql[p]!='A' && sql[p]!='a') || scan-p < 7) return 0;
    for (i = 0; i < 7; ++i)
        if (fts_lex_ascii_lower((unsigned char)sql[p+i]) != "analyze"[i]) return 0;
    *reason = "trailing";
    if ((bounded > scan && sql[scan]) || memchr(sql,0,scan)) return 0;
    for (i = p+7; i < scan; ++i) {
        if (i+1 < scan && ((sql[i]=='-' && sql[i+1]=='-') ||
                          (sql[i]=='/' && sql[i+1]=='*'))) {
            *reason = "comment"; return 0;
        }
        if (sql[i]=='"') { *reason = "dqs"; return 0; }
    }
    p += 7;
    if (p < scan && sql[p]!=';' && !fts_lex_is_space((unsigned char)sql[p])) return 0;
    while (p < scan && fts_lex_is_space((unsigned char)sql[p])) ++p;
    if (p < scan && sql[p]!=';') {
        start = p;
        while (p < scan && sql[p]!=';' && !fts_lex_is_space((unsigned char)sql[p])) ++p;
        if (p-start != 4 || fts_lex_ascii_lower((unsigned char)sql[start])!='m' ||
            fts_lex_ascii_lower((unsigned char)sql[start+1])!='a' ||
            fts_lex_ascii_lower((unsigned char)sql[start+2])!='i' ||
            fts_lex_ascii_lower((unsigned char)sql[start+3])!='n') {
            *reason = "targeted"; return 0;
        }
        while (p < scan && fts_lex_is_space((unsigned char)sql[p])) ++p;
    }
    if (p < scan && sql[p]==';') ++p;
    while (p < scan && fts_lex_is_space((unsigned char)sql[p])) ++p;
    if (p != scan) return 0;
    *reason = NULL;
    return 1;
}
#endif
