#ifndef FTS_LEX_H
#define FTS_LEX_H

#include <stddef.h>
#include <stdint.h>

#define FTS_LEX_SHAPE_CAP 8192u

typedef enum fts_lex_token_type {
    FTS_LEX_TOK_EOF = 0,
    FTS_LEX_TOK_IDENT,
    FTS_LEX_TOK_NUMBER,
    FTS_LEX_TOK_STRING,
    FTS_LEX_TOK_PARAM,
    FTS_LEX_TOK_SYMBOL,
    FTS_LEX_TOK_ERROR
} fts_lex_token_type;

typedef enum fts_lex_param_policy {
    FTS_LEX_PARAM_BARE_QMARK = 0,
    FTS_LEX_PARAM_NUMBERED_OR_NAMED = 1,
    FTS_LEX_PARAM_SQLITE_VARIABLE = 2
} fts_lex_param_policy;

typedef struct fts_lex_token {
    fts_lex_token_type type;
    size_t start;
    size_t end;
    char symbol;
} fts_lex_token;

typedef struct fts_lex {
    const char *sql;
    size_t len;
    size_t pos;
    fts_lex_param_policy param_policy;
} fts_lex;

static inline int fts_lex_ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static inline int fts_lex_is_ident_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static inline int fts_lex_is_ident_char(unsigned char c) {
    return fts_lex_is_ident_start(c) || (c >= '0' && c <= '9');
}

static inline int fts_lex_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

__attribute__((visibility("hidden"))) void fts_lex_init(
    fts_lex *lx,
    const char *sql,
    size_t len,
    fts_lex_param_policy param_policy
);
__attribute__((visibility("hidden"))) int fts_lex_token_text_eq(
    const char *sql,
    const fts_lex_token *tok,
    const char *want
);
__attribute__((visibility("hidden"))) fts_lex_token fts_lex_next_token(fts_lex *lx);
/* Returns 0 when the token stream cannot produce a structural shape. */
__attribute__((visibility("hidden"))) uint64_t fts_lex_shape_key(
    const char *sql,
    size_t len
);
__attribute__((visibility("hidden"))) int fts_lex_match_rhs_is_complete(
    const fts_lex *after_value,
    const fts_lex_token *value
);
__attribute__((visibility("hidden"))) int fts_rewrite_db_basename_matches(
    const char *raw_fn,
    const char *basename
);

#endif
