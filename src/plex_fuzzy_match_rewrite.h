#ifndef PLEX_FUZZY_MATCH_REWRITE_H
#define PLEX_FUZZY_MATCH_REWRITE_H

#include "fts_lex.h"
#include <stdlib.h>
#include <string.h>

/* Spec 2026-09-12, sections 2-4. Offsets always refer to the original bytes.
 * No tokenizer-dependent absorption is attempted outside ASCII alphanumerics. */
typedef struct plex_fuzzy_term {
    size_t start, len, word_len;
    int ascii, prefix, removed;
} plex_fuzzy_term;

static int plex_fuzzy_bytes(const char *s, size_t n, const char *want) {
    return n == strlen(want) && memcmp(s, want, n) == 0;
}

/* Returns an allocated statement, or NULL with a spec capture sub-reason.
 * NULL reason denotes allocation failure. The caller owns preparation/OBS. */
static char *plex_build_fuzzy_match_sql(const char *sql, size_t bounded,
    size_t scan, size_t *out_scan, size_t *out_len, const char **reason) {
    fts_lex lx;
    fts_lex_token t, prev[3] = {{0}}, lit = {0};
    size_t p, w, begin, end, count, i, j, words, tokens;
    int matches = 0, target = 0, operand = 0, is_or, last_or, saw_or;
    char *out = NULL;
    plex_fuzzy_term *terms = NULL;
    *reason = "grammar";
    if (!sql || !scan || scan > 65536 || bounded < scan || bounded-scan > 1 ||
        (bounded > scan && sql[scan]) || memchr(sql, 0, scan)) return NULL;
    for (p = 0; p+1 < scan; ++p) {
        if ((sql[p]=='-' && sql[p+1]=='-') || (sql[p]=='/' && sql[p+1]=='*')) {
            *reason = "comment"; return NULL;
        }
    }
    fts_lex_init(&lx, sql, scan, FTS_LEX_PARAM_SQLITE_VARIABLE);
    for (;;) {
        t = fts_lex_next_token(&lx);
        if (t.start < scan && sql[t.start]=='"') { *reason="dqs"; return NULL; }
        if (t.type == FTS_LEX_TOK_ERROR) { *reason="quote"; return NULL; }
        if (t.type == FTS_LEX_TOK_EOF) break;
        /* Section 2 is membership-only: phrase-count/position consumers do
         * not obey duplicate elimination even when their row sets do. */
        if (t.type == FTS_LEX_TOK_IDENT &&
            (fts_lex_token_text_eq(sql,&t,"matchinfo") ||
             fts_lex_token_text_eq(sql,&t,"offsets") ||
             fts_lex_token_text_eq(sql,&t,"snippet") ||
             fts_lex_token_text_eq(sql,&t,"rank"))) {
            fts_lex look=lx;
            fts_lex_token next=fts_lex_next_token(&look);
            if (next.type==FTS_LEX_TOK_SYMBOL && next.symbol=='(') return NULL;
        }
        /* The shared lexer does not consume quoted identifiers. Skip them
         * atomically here, so their contents cannot impersonate MATCH sites. */
        if (t.type == FTS_LEX_TOK_SYMBOL && (t.symbol=='`' || t.symbol=='[')) {
            char close=t.symbol=='[' ? ']' : '`';
            p=t.end;
            for (;;) {
                if (p>=scan) { *reason="quote"; return NULL; }
                if (sql[p++]==close) {
                    if (close=='`' && p<scan && sql[p]=='`') { ++p; continue; }
                    break;
                }
            }
            t.type=FTS_LEX_TOK_STRING; t.end=p; lx.pos=p;
        }
        if (t.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql,&t,"match")) {
            fts_lex look = lx;
            fts_lex_token rhs = fts_lex_next_token(&look);
            ++matches;
            if (matches > 1) { *reason="multi_match"; return NULL; }
            operand = rhs.type == FTS_LEX_TOK_PARAM ? 1 : 0;
            if (rhs.type == FTS_LEX_TOK_ERROR) { *reason="quote"; return NULL; }
            if (rhs.type == FTS_LEX_TOK_STRING && sql[rhs.start]=='\'') {
                size_t n = rhs.start-prev[0].start;
                const char *s = sql+prev[0].start;
                target = prev[0].type == FTS_LEX_TOK_IDENT &&
                    prev[1].symbol == '.' && prev[2].type == FTS_LEX_TOK_IDENT &&
                    (plex_fuzzy_bytes(s,n,"fts4_tag_titles_icu.tag match ") ||
                     plex_fuzzy_bytes(s,n,"fts4_metadata_titles_icu.title match ") ||
                     plex_fuzzy_bytes(s,n,"fts4_metadata_titles_icu.title_sort match ") ||
                     plex_fuzzy_bytes(s,n,"fts4_metadata_titles_icu.original_title match "));
                if (!fts_lex_match_rhs_is_complete(&look,&rhs)) {
                    fts_lex_token boundary=fts_lex_next_token(&look);
                    if (boundary.type!=FTS_LEX_TOK_SYMBOL || boundary.symbol!=';') {
                        *reason="quote"; return NULL;
                    }
                }
                lit = rhs;
            }
        }
        prev[0]=prev[1]; prev[1]=prev[2]; prev[2]=t;
    }
    if (!matches) { *reason="no_match"; return NULL; }
    if (operand) { *reason="bind_operand"; return NULL; }
    if (!target) { *reason="no_match"; return NULL; }
    begin = lit.start+1; end = lit.end-1;
    if (begin == end) return NULL;
    /* Each term occupies at least one byte; bounded allocation, no recursion. */
    terms = malloc((end-begin) * sizeof(*terms));
    out = malloc(bounded+1);
    if (!terms || !out) { *reason=NULL; goto fail; }
    memcpy(out, sql, begin); w=begin; p=begin;
    while (p < end) {
        if (sql[p++]!='(') goto fail;
        if (p < end && sql[p]==')') { *reason="empty_group"; goto fail; }
        count=0; tokens=0; saw_or=0; last_or=0;
        while (p < end && sql[p]!=')') {
            plex_fuzzy_term term;
            term.start=p; term.ascii=1; term.removed=0;
            while (p < end && sql[p]!=' ' && sql[p]!=')' && sql[p]!='*') {
                unsigned char c=(unsigned char)sql[p];
                if (c <= 32 || c=='(' || c=='"' || c==':' || c=='^' || c==',') goto fail;
                if (c=='\'') {
                    if (p+1>=end || sql[p+1]!='\'') { *reason="quote"; goto fail; }
                    ++p;
                }
                if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9')))
                    term.ascii=0;
                ++p;
            }
            term.word_len=p-term.start;
            if (!term.word_len) goto fail;
            term.prefix=p<end && sql[p]=='*';
            if (term.prefix) ++p;
            term.len=p-term.start;
            is_or=plex_fuzzy_bytes(sql+term.start,term.len,"OR");
            if (plex_fuzzy_bytes(sql+term.start,term.len,"AND") ||
                plex_fuzzy_bytes(sql+term.start,term.len,"NOT") ||
                plex_fuzzy_bytes(sql+term.start,term.len,"NEAR") ||
                (term.len>=5 && memcmp(sql+term.start,"NEAR/",5)==0)) {
                *reason="keyword"; goto fail;
            }
            if (is_or) {
                if (!tokens || last_or) { *reason="or_layout"; goto fail; }
                saw_or=1;
            } else terms[count++]=term;
            last_or=is_or; ++tokens;
            if (p < end && sql[p]==')') break;
            if (p >= end || sql[p++]!=' ' || p>=end || sql[p]==' ' || sql[p]==')') goto fail;
        }
        if (p>=end || sql[p++]!=')') goto fail;
        if (last_or || (saw_or && tokens != 2*count-1)) { *reason="or_layout"; goto fail; }
        words=count;
        for (i=0; i<count; ++i) {
            for (j=0; j<i; ++j) {
                if (!terms[j].removed && terms[i].len==terms[j].len &&
                    memcmp(sql+terms[i].start,sql+terms[j].start,terms[i].len)==0) {
                    terms[i].removed=1; --words; break;
                }
            }
        }
        for (i=0; i<count && words>1; ++i) {
            plex_fuzzy_term *a=&terms[i];
            if (a->removed || !a->ascii || (!saw_or && !a->prefix)) continue;
            for (j=0; j<count; ++j) {
                plex_fuzzy_term *b=&terms[j];
                if (i==j || b->removed || !b->ascii) continue;
                if ((saw_or && b->prefix && b->word_len<=a->word_len &&
                     (!a->prefix || b->word_len<a->word_len) &&
                     memcmp(sql+a->start,sql+b->start,b->word_len)==0) ||
                    (!saw_or && b->word_len>a->word_len &&
                     memcmp(sql+a->start,sql+b->start,a->word_len)==0)) {
                    a->removed=1; --words; break;
                }
            }
        }
        out[w++]='('; words=0;
        for (i=0; i<count; ++i) if (!terms[i].removed) {
            if (words++) {
                const char *sep=saw_or ? " OR " : " ";
                size_t n=saw_or ? 4 : 1;
                memcpy(out+w,sep,n); w+=n;
            }
            memcpy(out+w,sql+terms[i].start,terms[i].len); w+=terms[i].len;
        }
        out[w++]=')';
        if (p<end && sql[p]=='*') out[w++]=sql[p++];
        if (p==end) break;
        if (sql[p++]!=' ' || p==end) goto fail;
        out[w++]=' ';
    }
    if (w==end) { *reason="noop"; goto fail; }
    memcpy(out+w,sql+end,bounded-end);
    *out_len=w+bounded-end; *out_scan=w+scan-end;
    out[*out_len]=0;
    free(terms); *reason=NULL; return out;
fail:
    free(terms); free(out); return NULL;
}
#endif
