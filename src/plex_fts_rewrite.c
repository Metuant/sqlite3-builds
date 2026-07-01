#include "plex_fts_rewrite.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REWRITE_OPEN "unlikely("
#define REWRITE_OPEN_LEN 9
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

typedef enum token_type {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PARAM,
    TOK_SYMBOL,
    TOK_ERROR
} token_type;

typedef struct token {
    token_type type;
    size_t start;
    size_t end;
    char symbol;
} token;

typedef struct lexer {
    const char *sql;
    size_t len;
    size_t pos;
} lexer;

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

static pthread_once_t g_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_rewrite_enabled;

static void plex_fts_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE");
    atomic_store_explicit(
        &g_rewrite_enabled,
        (value && strcmp(value, "0") == 0) ? 1 : 0,
        memory_order_release
    );
}

static int rewrite_enabled(void) {
    pthread_once(&g_rewrite_once, plex_fts_rewrite_init_once);
    return atomic_load_explicit(&g_rewrite_enabled, memory_order_acquire);
}

static int call_real_prepare(
    plex_fts_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    switch (kind) {
        case PLEX_FTS_PREPARE_LEGACY:
            return sqlite3_prepare_real(db, zSql, nByte, ppStmt, pzTail);
        case PLEX_FTS_PREPARE_V2:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
        case PLEX_FTS_PREPARE_V3:
            return sqlite3_prepare_v3_real(db, zSql, nByte, prepFlags, ppStmt, pzTail);
        default:
            return sqlite3_prepare_v2_real(db, zSql, nByte, ppStmt, pzTail);
    }
}

static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int is_ident_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_ident_char(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int token_text_eq(const char *sql, const token *tok, const char *want) {
    size_t n = tok->end - tok->start;
    size_t i;
    if (strlen(want) != n) return 0;
    for (i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)sql[tok->start + i]) !=
            ascii_lower((unsigned char)want[i])) {
            return 0;
        }
    }
    return 1;
}

static token next_token(lexer *lx) {
    token tok;
    const char *sql = lx->sql;
    size_t len = lx->len;
    size_t p = lx->pos;

    for (;;) {
        while (p < len && is_space((unsigned char)sql[p])) p++;
        if (p + 1 < len && sql[p] == '-' && sql[p + 1] == '-') {
            p += 2;
            while (p < len && sql[p] != '\n') p++;
            continue;
        }
        if (p + 1 < len && sql[p] == '/' && sql[p + 1] == '*') {
            p += 2;
            while (p + 1 < len && !(sql[p] == '*' && sql[p + 1] == '/')) p++;
            if (p + 1 >= len) {
                lx->pos = p;
                tok.type = TOK_ERROR;
                tok.start = p;
                tok.end = p;
                tok.symbol = 0;
                return tok;
            }
            p += 2;
            continue;
        }
        break;
    }

    tok.start = p;
    tok.end = p;
    tok.symbol = 0;
    if (p >= len) {
        lx->pos = p;
        tok.type = TOK_EOF;
        return tok;
    }
    if (sql[p] == 0) {
        lx->pos = p;
        tok.type = TOK_ERROR;
        return tok;
    }

    if (is_ident_start((unsigned char)sql[p])) {
        p++;
        while (p < len && is_ident_char((unsigned char)sql[p])) p++;
        tok.type = TOK_IDENT;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (sql[p] >= '0' && sql[p] <= '9') {
        p++;
        while (p < len && sql[p] >= '0' && sql[p] <= '9') p++;
        tok.type = TOK_NUMBER;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (sql[p] == '\'' || sql[p] == '"') {
        char quote = sql[p++];
        while (p < len) {
            if (sql[p] == 0) {
                lx->pos = p;
                tok.type = TOK_ERROR;
                tok.end = p;
                return tok;
            }
            if (sql[p] == quote) {
                p++;
                if (p < len && sql[p] == quote) {
                    p++;
                    continue;
                }
                tok.type = TOK_STRING;
                tok.end = p;
                lx->pos = p;
                return tok;
            }
            p++;
        }
        lx->pos = p;
        tok.type = TOK_ERROR;
        tok.end = p;
        return tok;
    }

    if (sql[p] == '?') {
        p++;
        tok.type = TOK_PARAM;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    tok.type = TOK_SYMBOL;
    tok.symbol = sql[p];
    tok.end = p + 1;
    lx->pos = p + 1;
    return tok;
}

static int parse_any_value(lexer *lx, token *value) {
    *value = next_token(lx);
    return value->type == TOK_NUMBER || value->type == TOK_STRING || value->type == TOK_PARAM;
}

static int is_predicate_boundary_keyword(const char *sql, const token *tok) {
    return token_text_eq(sql, tok, "and") ||
           token_text_eq(sql, tok, "or") ||
           token_text_eq(sql, tok, "group") ||
           token_text_eq(sql, tok, "having") ||
           token_text_eq(sql, tok, "order") ||
           token_text_eq(sql, tok, "limit") ||
           token_text_eq(sql, tok, "union") ||
           token_text_eq(sql, tok, "except") ||
           token_text_eq(sql, tok, "intersect") ||
           token_text_eq(sql, tok, "window");
}

static int is_predicate_value_boundary(const char *sql, const token *tok) {
    if (tok->type == TOK_EOF) return 1;
    if (tok->type == TOK_SYMBOL && tok->symbol == ')') return 1;
    if (tok->type == TOK_IDENT) return is_predicate_boundary_keyword(sql, tok);
    return 0;
}

static int parse_predicate_value(lexer *lx, token *value) {
    lexer look;
    token boundary;

    *value = next_token(lx);
    if (!(value->type == TOK_NUMBER || value->type == TOK_STRING ||
          (value->type == TOK_PARAM && value->end == value->start + 1))) {
        return 0;
    }
    if (value->type == TOK_PARAM) {
        if (value->end < lx->len &&
            lx->sql[value->end] >= '0' &&
            lx->sql[value->end] <= '9') {
            return 0;
        }
    }
    look = *lx;
    boundary = next_token(&look);
    if (boundary.type == TOK_ERROR) return 0;
    return is_predicate_value_boundary(lx->sql, &boundary);
}

static int token_starts_fts_match(const char *sql, const lexer *after_target) {
    lexer look = *after_target;
    token next = next_token(&look);
    token value;

    if (next.type == TOK_IDENT && token_text_eq(sql, &next, "match")) {
        return parse_any_value(&look, &value);
    }
    if (next.type == TOK_SYMBOL && next.symbol == '.') {
        token column = next_token(&look);
        token match = next_token(&look);
        if (column.type == TOK_IDENT &&
            match.type == TOK_IDENT &&
            token_text_eq(sql, &match, "match") &&
            parse_any_value(&look, &value)) {
            return 1;
        }
    }
    return 0;
}

static int token_is_keyword(
    const char *sql,
    const token *tok,
    const char *keyword
) {
    return tok->type == TOK_IDENT && token_text_eq(sql, tok, keyword);
}

static void update_query_clause(query_block_match *block, const char *sql, const token *tok) {
    if (token_text_eq(sql, tok, "select")) {
        block->clause = QUERY_CLAUSE_SELECT;
    } else if (token_text_eq(sql, tok, "from") || token_text_eq(sql, tok, "join")) {
        block->clause = QUERY_CLAUSE_FROM;
    } else if (token_text_eq(sql, tok, "where") || token_text_eq(sql, tok, "on")) {
        block->clause = QUERY_CLAUSE_PREDICATE;
    } else if (token_text_eq(sql, tok, "group") || token_text_eq(sql, tok, "having") ||
               token_text_eq(sql, tok, "order") || token_text_eq(sql, tok, "limit") ||
               token_text_eq(sql, tok, "union") || token_text_eq(sql, tok, "except") ||
               token_text_eq(sql, tok, "intersect") || token_text_eq(sql, tok, "window")) {
        block->clause = QUERY_CLAUSE_OTHER;
    }
}

static int match_target_prefix_query(
    const rewrite_target *target,
    const char *sql,
    size_t len,
    rewrite_match *out
) {
    lexer lx;
    token prev;
    query_block_match blocks[REWRITE_MAX_QUERY_BLOCKS];
    int block_at_depth[REWRITE_MAX_QUERY_DEPTH];
    int block_count = 0;
    int total_predicate_count = 0;
    int depth = 0;
    int i;

    lx.sql = sql;
    lx.len = len;
    lx.pos = 0;
    memset(blocks, 0, sizeof(blocks));
    for (i = 0; i < REWRITE_MAX_QUERY_DEPTH; i++) block_at_depth[i] = -1;
    memset(&prev, 0, sizeof(prev));
    prev.type = TOK_EOF;

    for (;;) {
        lexer look;
        token tok;
        int current_block;

        tok = next_token(&lx);
        if (tok.type == TOK_ERROR) return 0;
        if (tok.type == TOK_EOF) {
            if (depth != 0) return 0;
            break;
        }
        if (tok.type == TOK_SYMBOL && tok.symbol == ';') return 0;

        current_block = block_at_depth[depth];
        if (tok.type == TOK_SYMBOL && tok.symbol == '(') {
            if (depth + 1 >= REWRITE_MAX_QUERY_DEPTH) return 0;
            block_at_depth[depth + 1] = current_block;
            depth++;
            prev = tok;
            continue;
        }
        if (tok.type == TOK_SYMBOL && tok.symbol == ')') {
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
        if (current_block >= 0 && tok.type == TOK_IDENT) {
            update_query_clause(&blocks[current_block], sql, &tok);
            if (token_text_eq(sql, &tok, target->fts_table) &&
                token_starts_fts_match(sql, &lx)) {
                blocks[current_block].fts_match = 1;
            }
        }
        if (tok.type == TOK_IDENT && token_text_eq(sql, &tok, target->predicate_column)) {
            token eq;
            token value;

            look = lx;
            eq = next_token(&look);
            if (eq.type == TOK_ERROR) return 0;
            if (eq.type == TOK_SYMBOL && eq.symbol == '=') {
                if (prev.type == TOK_SYMBOL && prev.symbol == '.') return 0;
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

static int target_db_path_matches(const rewrite_target *target, const char *raw_fn) {
    size_t raw_len;
    char fnbuf[2048];
    char *q;
    const char *base;

    if (!auto_extension_path_is_target(raw_fn)) return 0;
    if (!raw_fn || raw_fn[0] == 0) return 0;
    raw_len = strlen(raw_fn);
    if (raw_len >= sizeof(fnbuf)) return 0;
    memcpy(fnbuf, raw_fn, raw_len + 1);
    q = strchr(fnbuf, '?');
    if (q) *q = 0;
    base = strrchr(fnbuf, '/');
    base = base ? base + 1 : fnbuf;
    return strcmp(base, target->db_basename) == 0;
}

static int prepare_input_lengths(
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

static char *build_rewritten_sql(
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

static int map_end_tail(
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

static void log_rewrite_applied(sqlite3 *db, const char *rewritten) {
    char sqlbuf[REWRITE_LOG_SQLBUF];

    obs_escape_sql(rewritten, sqlbuf, sizeof(sqlbuf));
    obs_logf("plex_fts_rewrite", "event=rewrite_applied target=plex db=%p sql=\"%s\"",
             (void*)db, sqlbuf);
}

static int retry_original_after_rewrite_failure(
    plex_fts_prepare_kind kind,
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
    plex_fts_prepare_kind kind
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

    if (!rewrite_enabled() || !db || !zSql || !ppStmt || nByte == 0) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    raw_fn = sqlite3_db_filename(db, "main");
    if (!target_db_path_matches(&PLEX_FTS_TARGET, raw_fn)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!prepare_input_lengths(zSql, nByte, &bounded_len, &scan_len, &rewrite_nbyte)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!match_target_prefix_query(&PLEX_FTS_TARGET, zSql, scan_len, &match)) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    rewritten = build_rewritten_sql(zSql, bounded_len, &match);
    if (!rewritten) {
        return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    if (ppStmt) *ppStmt = NULL;
    rc = call_real_prepare(
        kind, db, rewritten, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
    );
    if (rc != SQLITE_OK ||
        !map_end_tail(zSql, rewritten, rewritten_tail, scan_len, &mapped_tail)) {
        free(rewritten);
        return retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }

    log_rewrite_applied(db, rewritten);
    if (pzTail) *pzTail = mapped_tail;
    free(rewritten);
    return rc;
#else
    return call_real_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
#endif
}
