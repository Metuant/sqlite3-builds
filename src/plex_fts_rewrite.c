#include "plex_fts_rewrite.h"
#include "fts_lex.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REWRITE_OPEN "unlikely("
#define REWRITE_OPEN_LEN (sizeof(REWRITE_OPEN) - 1)
#define REWRITE_CLOSE_LEN 1
#define REWRITE_LOG_SQLBUF 4352
#define REWRITE_MAX_QUERY_BLOCKS 64
#define REWRITE_MAX_QUERY_DEPTH 64

typedef struct rewrite_target {
    const char *fts_table;
    const char *predicate_column;
    const char *db_basename;
} rewrite_target;

/* Plex-only target identifiers. A second target (e.g. Emby) is not a trivial config
   add: it needs a different DB path, MATCH syntax, and rewrite shape -- so treat this
   as identifier isolation, not an extension point. */
static const rewrite_target PLEX_FTS_TARGET = {
    "fts4_tag_titles_icu",
    "tag_type",
    "com.plexapp.plugins.library.db"
};

typedef struct rewrite_match {
    size_t open_off;
    size_t close_off;
} rewrite_match;

typedef enum query_clause {
    QUERY_CLAUSE_NONE = 0,
    QUERY_CLAUSE_SELECT,
    QUERY_CLAUSE_FROM,
    QUERY_CLAUSE_PREDICATE,
    QUERY_CLAUSE_OTHER
} query_clause;

typedef struct query_block_match {
    query_clause clause;
    int fts_match;
    int predicate_count;
    rewrite_match predicate;
} query_block_match;

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v3_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
extern int auto_extension_path_is_target(const char *raw_fn);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);
__attribute__((visibility("hidden"))) SQLITE_API void obs_escape_sql(
    const char *src,
    char *dst,
    size_t dst_n
);

static pthread_once_t g_plex_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_plex_rewrite_enabled;

static void plex_fts_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE");
    atomic_store_explicit(
        &g_plex_rewrite_enabled,
        (value && strcmp(value, "1") == 0) ? 0 : 1,
        memory_order_release
    );
}

static int plex_rewrite_enabled(void) {
    pthread_once(&g_plex_rewrite_once, plex_fts_rewrite_init_once);
    return atomic_load_explicit(&g_plex_rewrite_enabled, memory_order_acquire);
}

static int call_real_prepare(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    switch (kind) {
        case FTS_REWRITE_PREPARE_LEGACY:
            return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
        case FTS_REWRITE_PREPARE_V2:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
        case FTS_REWRITE_PREPARE_V3:
            return sqlite3_prepare_v3_real(db, zSql, nByte, prepFlags, ppStmt, pzTail);
        default:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
    }
}

static int parse_match_value(fts_lex *lx, fts_lex_token *value) {
    *value = fts_lex_next_token(lx);
    if (!(value->type == FTS_LEX_TOK_NUMBER ||
          value->type == FTS_LEX_TOK_STRING ||
          value->type == FTS_LEX_TOK_PARAM)) {
        return 0;
    }
    return fts_lex_match_rhs_is_complete(lx, value);
}

static int is_predicate_boundary_keyword(const char *sql, const fts_lex_token *tok) {
    return fts_lex_token_text_eq(sql, tok, "and") ||
           fts_lex_token_text_eq(sql, tok, "or") ||
           fts_lex_token_text_eq(sql, tok, "group") ||
           fts_lex_token_text_eq(sql, tok, "having") ||
           fts_lex_token_text_eq(sql, tok, "order") ||
           fts_lex_token_text_eq(sql, tok, "limit") ||
           fts_lex_token_text_eq(sql, tok, "union") ||
           fts_lex_token_text_eq(sql, tok, "except") ||
           fts_lex_token_text_eq(sql, tok, "intersect") ||
           fts_lex_token_text_eq(sql, tok, "window");
}

static int is_predicate_value_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == ')') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) return is_predicate_boundary_keyword(sql, tok);
    return 0;
}

static int is_predicate_left_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == '(') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) {
        return fts_lex_token_text_eq(sql, tok, "and") ||
               fts_lex_token_text_eq(sql, tok, "or") ||
               fts_lex_token_text_eq(sql, tok, "where") ||
               fts_lex_token_text_eq(sql, tok, "on");
    }
    return 0;
}

static int parse_predicate_value(fts_lex *lx, fts_lex_token *value) {
    fts_lex look;
    fts_lex_token boundary;

    *value = fts_lex_next_token(lx);
    if (!(value->type == FTS_LEX_TOK_NUMBER || value->type == FTS_LEX_TOK_STRING ||
          (value->type == FTS_LEX_TOK_PARAM && value->end == value->start + 1))) {
        return 0;
    }
    if (value->type == FTS_LEX_TOK_PARAM) {
        if (value->end < lx->len &&
            lx->sql[value->end] >= '0' &&
            lx->sql[value->end] <= '9') {
            return 0;
        }
    }
    look = *lx;
    boundary = fts_lex_next_token(&look);
    if (boundary.type == FTS_LEX_TOK_ERROR) return 0;
    return is_predicate_value_boundary(lx->sql, &boundary);
}

static int token_starts_fts_match(const char *sql, const fts_lex *after_target) {
    fts_lex look = *after_target;
    fts_lex_token next = fts_lex_next_token(&look);
    fts_lex_token value;

    if (next.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &next, "match")) {
        return parse_match_value(&look, &value);
    }
    if (next.type == FTS_LEX_TOK_SYMBOL && next.symbol == '.') {
        fts_lex_token column = fts_lex_next_token(&look);
        fts_lex_token match = fts_lex_next_token(&look);
        if (column.type == FTS_LEX_TOK_IDENT &&
            match.type == FTS_LEX_TOK_IDENT &&
            fts_lex_token_text_eq(sql, &match, "match") &&
            parse_match_value(&look, &value)) {
            return 1;
        }
    }
    return 0;
}

static int token_is_keyword(
    const char *sql,
    const fts_lex_token *tok,
    const char *keyword
) {
    return tok->type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, tok, keyword);
}

static void update_query_clause(query_block_match *block, const char *sql, const fts_lex_token *tok) {
    if (fts_lex_token_text_eq(sql, tok, "select")) {
        block->clause = QUERY_CLAUSE_SELECT;
    } else if (fts_lex_token_text_eq(sql, tok, "from") || fts_lex_token_text_eq(sql, tok, "join")) {
        block->clause = QUERY_CLAUSE_FROM;
    } else if (fts_lex_token_text_eq(sql, tok, "where") || fts_lex_token_text_eq(sql, tok, "on")) {
        block->clause = QUERY_CLAUSE_PREDICATE;
    } else if (fts_lex_token_text_eq(sql, tok, "group") || fts_lex_token_text_eq(sql, tok, "having") ||
               fts_lex_token_text_eq(sql, tok, "order") || fts_lex_token_text_eq(sql, tok, "limit") ||
               fts_lex_token_text_eq(sql, tok, "union") || fts_lex_token_text_eq(sql, tok, "except") ||
               fts_lex_token_text_eq(sql, tok, "intersect") || fts_lex_token_text_eq(sql, tok, "window")) {
        block->clause = QUERY_CLAUSE_OTHER;
    }
}

static int match_target_prefix_query(
    const rewrite_target *target,
    const char *sql,
    size_t len,
    rewrite_match *out
) {
    fts_lex lx;
    fts_lex_token prev;
    query_block_match blocks[REWRITE_MAX_QUERY_BLOCKS];
    int block_at_depth[REWRITE_MAX_QUERY_DEPTH];
    int block_count = 0;
    int total_predicate_count = 0;
    int depth = 0;
    int i;

    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_BARE_QMARK);
    memset(blocks, 0, sizeof(blocks));
    for (i = 0; i < REWRITE_MAX_QUERY_DEPTH; i++) block_at_depth[i] = -1;
    memset(&prev, 0, sizeof(prev));
    prev.type = FTS_LEX_TOK_EOF;

    for (;;) {
        fts_lex look;
        fts_lex_token tok;
        int current_block;

        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) {
            if (depth != 0) return 0;
            break;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return 0;

        current_block = block_at_depth[depth];
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            if (depth + 1 >= REWRITE_MAX_QUERY_DEPTH) return 0;
            block_at_depth[depth + 1] = current_block;
            depth++;
            prev = tok;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return 0;
            block_at_depth[depth] = -1;
            depth--;
            prev = tok;
            continue;
        }
        if (token_is_keyword(sql, &tok, "select")) {
            if (block_count >= REWRITE_MAX_QUERY_BLOCKS) return 0;
            current_block = block_count++;
            memset(&blocks[current_block], 0, sizeof(blocks[current_block]));
            blocks[current_block].clause = QUERY_CLAUSE_SELECT;
            block_at_depth[depth] = current_block;
            prev = tok;
            continue;
        }

        current_block = block_at_depth[depth];
        if (current_block >= 0 && tok.type == FTS_LEX_TOK_IDENT) {
            update_query_clause(&blocks[current_block], sql, &tok);
            if (fts_lex_token_text_eq(sql, &tok, target->fts_table) &&
                token_starts_fts_match(sql, &lx)) {
                blocks[current_block].fts_match = 1;
            }
        }
        if (tok.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(sql, &tok, target->predicate_column)) {
            fts_lex_token eq;
            fts_lex_token value;

            look = lx;
            eq = fts_lex_next_token(&look);
            if (eq.type == FTS_LEX_TOK_ERROR) return 0;
            if (eq.type == FTS_LEX_TOK_SYMBOL && eq.symbol == '=') {
                if (!is_predicate_left_boundary(sql, &prev)) return 0;
                if (!parse_predicate_value(&look, &value)) return 0;
                total_predicate_count++;
                if (total_predicate_count > 1) return 0;
                if (current_block >= 0 &&
                    blocks[current_block].clause == QUERY_CLAUSE_PREDICATE) {
                    blocks[current_block].predicate_count++;
                    blocks[current_block].predicate.open_off = tok.start;
                    blocks[current_block].predicate.close_off = value.end;
                }
            }
        }
        prev = tok;
    }

    if (total_predicate_count != 1) return 0;
    for (i = 0; i < block_count; i++) {
        if (blocks[i].fts_match && blocks[i].predicate_count == 1) {
            *out = blocks[i].predicate;
            return 1;
        }
    }
    return 0;
}

static int plex_prepare_input_lengths(
    const char *zSql,
    int nByte,
    size_t *bounded_len,
    size_t *scan_len,
    int *rewrite_nbyte
) {
    if (nByte < 0) {
        size_t n = strlen(zSql);
        if (n > SIZE_MAX - REWRITE_OPEN_LEN - REWRITE_CLOSE_LEN - 1) return 0;
        *bounded_len = n;
        *scan_len = n;
        *rewrite_nbyte = -1;
        return 1;
    }

    if (nByte == 0) return 0;
    if (nByte > INT_MAX - (REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN)) return 0;
    *bounded_len = (size_t)nByte;
    *scan_len = *bounded_len;
    if (*scan_len > 0 && zSql[*scan_len - 1] == 0) {
        (*scan_len)--;
    }
    *rewrite_nbyte = nByte + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN;
    return 1;
}

static char *plex_build_rewritten_sql(
    const char *zSql,
    size_t bounded_len,
    const rewrite_match *match
) {
    char *rewritten;
    size_t out_len = bounded_len + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN;

    if (match->open_off > match->close_off || match->close_off > bounded_len) return NULL;
    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) return NULL;

    memcpy(rewritten, zSql, match->open_off);
    memcpy(rewritten + match->open_off, REWRITE_OPEN, REWRITE_OPEN_LEN);
    memcpy(rewritten + match->open_off + REWRITE_OPEN_LEN,
           zSql + match->open_off,
           match->close_off - match->open_off);
    rewritten[match->close_off + REWRITE_OPEN_LEN] = ')';
    memcpy(rewritten + match->close_off + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN,
           zSql + match->close_off,
           bounded_len - match->close_off);
    rewritten[out_len] = 0;
    return rewritten;
}

static int plex_map_end_tail(
    const char *zSql,
    const char *rewritten,
    const char *rewritten_tail,
    size_t original_sql_len,
    const char **mapped_tail
) {
    if (!rewritten_tail) {
        *mapped_tail = NULL;
        return 1;
    }
    if (rewritten_tail != rewritten + original_sql_len + REWRITE_OPEN_LEN + REWRITE_CLOSE_LEN) {
        return 0;
    }
    *mapped_tail = zSql + original_sql_len;
    return 1;
}

static void plex_log_rewrite_applied(sqlite3 *db, const char *rewritten) {
    char sqlbuf[REWRITE_LOG_SQLBUF];

    obs_escape_sql(rewritten, sqlbuf, sizeof(sqlbuf));
    obs_logf("plex_fts_rewrite", "event=rewrite_applied target=plex db=%p sql=\"%s\"",
             (void*)db, sqlbuf);
}

static void plex_log_rewrite_skipped(sqlite3 *db, const char *reason) {
    obs_logf("plex_fts_rewrite",
             "event=rewrite_skipped target=plex reason=%s db=%p",
             reason, (void*)db);
}

static int plex_retry_original_after_rewrite_failure(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    if (ppStmt && *ppStmt) {
        sqlite3_finalize(*ppStmt);
        *ppStmt = NULL;
    }
    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
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
#ifdef SQLITE_ENABLE_ICU
    size_t bounded_len = 0;
    size_t scan_len = 0;
    int rewrite_nbyte = -1;
    rewrite_match match;
    const char *raw_fn;
    const char *rewritten_tail = NULL;
    const char *mapped_tail = NULL;
    char *rewritten = NULL;
    int rc;

    if (!plex_rewrite_enabled() || !db || !zSql || !ppStmt || nByte == 0) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    raw_fn = sqlite3_db_filename(db, "main");
    if (!auto_extension_path_is_target(raw_fn) ||
        !fts_rewrite_db_basename_matches(raw_fn, PLEX_FTS_TARGET.db_basename)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!plex_prepare_input_lengths(zSql, nByte, &bounded_len, &scan_len, &rewrite_nbyte)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!match_target_prefix_query(&PLEX_FTS_TARGET, zSql, scan_len, &match)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    rewritten = plex_build_rewritten_sql(zSql, bounded_len, &match);
    if (!rewritten) {
        plex_log_rewrite_skipped(db, "build_failed");
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    if (ppStmt) *ppStmt = NULL;
    rc = call_real_prepare(
        kind, db, rewritten, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
    );
    if (rc != SQLITE_OK) {
        plex_log_rewrite_skipped(db, "rewritten_prepare_failed");
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (!plex_map_end_tail(zSql, rewritten, rewritten_tail, scan_len, &mapped_tail)) {
        plex_log_rewrite_skipped(db, "tail_mismatch");
        free(rewritten);
        return plex_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }

    plex_log_rewrite_applied(db, rewritten);
    if (pzTail) *pzTail = mapped_tail;
    free(rewritten);
    return rc;
#else
    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
#endif
}
