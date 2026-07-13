#include "fts_lex.h"

#include <stdint.h>
#include <string.h>

static int fts_lex_is_sqlite_id_char(unsigned char c) {
    return fts_lex_is_ident_char(c) || c == '$' || c >= 0x80;
}

__attribute__((visibility("hidden"))) void fts_lex_init(
    fts_lex *lx,
    const char *sql,
    size_t len,
    fts_lex_param_policy param_policy
) {
    lx->sql = sql;
    lx->len = len;
    lx->pos = 0;
    lx->param_policy = param_policy;
}

__attribute__((visibility("hidden"))) int fts_lex_token_text_eq(
    const char *sql,
    const fts_lex_token *tok,
    const char *want
) {
    size_t n = tok->end - tok->start;
    size_t i;
    if (strlen(want) != n) return 0;
    for (i = 0; i < n; i++) {
        if (fts_lex_ascii_lower((unsigned char)sql[tok->start + i]) !=
            fts_lex_ascii_lower((unsigned char)want[i])) {
            return 0;
        }
    }
    return 1;
}

__attribute__((visibility("hidden"))) fts_lex_token fts_lex_next_token(fts_lex *lx) {
    fts_lex_token tok;
    const char *sql = lx->sql;
    size_t len = lx->len;
    size_t p = lx->pos;

    for (;;) {
        while (p < len && fts_lex_is_space((unsigned char)sql[p])) p++;
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
                tok.type = FTS_LEX_TOK_ERROR;
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
        tok.type = FTS_LEX_TOK_EOF;
        return tok;
    }
    if (sql[p] == 0) {
        lx->pos = p;
        tok.type = FTS_LEX_TOK_ERROR;
        return tok;
    }

    if (fts_lex_is_ident_start((unsigned char)sql[p])) {
        p++;
        while (p < len && fts_lex_is_ident_char((unsigned char)sql[p])) p++;
        tok.type = FTS_LEX_TOK_IDENT;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (sql[p] >= '0' && sql[p] <= '9') {
        p++;
        while (p < len && sql[p] >= '0' && sql[p] <= '9') p++;
        tok.type = FTS_LEX_TOK_NUMBER;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (sql[p] == '\'' || sql[p] == '"') {
        char quote = sql[p++];
        while (p < len) {
            if (sql[p] == 0) {
                lx->pos = p;
                tok.type = FTS_LEX_TOK_ERROR;
                tok.end = p;
                return tok;
            }
            if (sql[p] == quote) {
                p++;
                if (p < len && sql[p] == quote) {
                    p++;
                    continue;
                }
                tok.type = FTS_LEX_TOK_STRING;
                tok.end = p;
                lx->pos = p;
                return tok;
            }
            p++;
        }
        lx->pos = p;
        tok.type = FTS_LEX_TOK_ERROR;
        tok.end = p;
        return tok;
    }

    if (sql[p] == '?') {
        p++;
        if (lx->param_policy == FTS_LEX_PARAM_NUMBERED_OR_NAMED ||
            lx->param_policy == FTS_LEX_PARAM_SQLITE_VARIABLE) {
            while (p < len && sql[p] >= '0' && sql[p] <= '9') p++;
        }
        tok.type = FTS_LEX_TOK_PARAM;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (lx->param_policy == FTS_LEX_PARAM_SQLITE_VARIABLE &&
        (sql[p] == '@' || sql[p] == ':' || sql[p] == '$')) {
        size_t name_chars = 0;
        int valid = 1;

        p++;
        while (p < len && sql[p] != 0) {
            unsigned char c = (unsigned char)sql[p];
            if (fts_lex_is_sqlite_id_char(c)) {
                name_chars++;
                p++;
            } else if (c == '(' && name_chars > 0) {
                p++;
                while (p < len && sql[p] != 0 &&
                       !fts_lex_is_space((unsigned char)sql[p]) && sql[p] != ')') {
                    p++;
                }
                if (p < len && sql[p] == ')') {
                    p++;
                } else {
                    valid = 0;
                }
                break;
            } else if (c == ':' && p + 1 < len && sql[p + 1] == ':') {
                p += 2;
            } else {
                break;
            }
        }
        if (name_chars == 0) valid = 0;
        tok.type = valid ? FTS_LEX_TOK_PARAM : FTS_LEX_TOK_ERROR;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    if (lx->param_policy == FTS_LEX_PARAM_NUMBERED_OR_NAMED &&
        (sql[p] == '@' || sql[p] == ':' || sql[p] == '$') &&
        p + 1 < len &&
        fts_lex_is_ident_start((unsigned char)sql[p + 1])) {
        p += 2;
        while (p < len && fts_lex_is_ident_char((unsigned char)sql[p])) p++;
        tok.type = FTS_LEX_TOK_PARAM;
        tok.end = p;
        lx->pos = p;
        return tok;
    }

    tok.type = FTS_LEX_TOK_SYMBOL;
    tok.symbol = sql[p];
    tok.end = p + 1;
    lx->pos = p + 1;
    return tok;
}

static uint64_t fts_lex_shape_mix_byte(uint64_t hash, unsigned char value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t fts_lex_shape_mix_token(
    uint64_t hash,
    const char *sql,
    const fts_lex_token *tok
) {
    size_t i;

    switch (tok->type) {
        case FTS_LEX_TOK_IDENT:
            for (i = tok->start; i < tok->end; i++) {
                hash = fts_lex_shape_mix_byte(
                    hash,
                    (unsigned char)fts_lex_ascii_lower((unsigned char)sql[i])
                );
            }
            break;
        case FTS_LEX_TOK_NUMBER:
            hash = fts_lex_shape_mix_byte(hash, 0x01u);
            break;
        case FTS_LEX_TOK_STRING:
            hash = fts_lex_shape_mix_byte(hash, 0x02u);
            break;
        case FTS_LEX_TOK_PARAM:
            for (i = tok->start; i < tok->end; i++) {
                hash = fts_lex_shape_mix_byte(hash, (unsigned char)sql[i]);
            }
            break;
        case FTS_LEX_TOK_SYMBOL:
            hash = fts_lex_shape_mix_byte(hash, (unsigned char)tok->symbol);
            break;
        default:
            break;
    }
    return fts_lex_shape_mix_byte(hash, 0x00u);
}

__attribute__((visibility("hidden"))) uint64_t fts_lex_shape_key(
    const char *sql,
    size_t len
) {
    fts_lex lx;
    fts_lex_token tok;
    fts_lex_token_type list_literal = FTS_LEX_TOK_EOF;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t shape_len;
    int pending_comma = 0;
    int list_collapsed = 0;

    if (!sql) return 0;
    shape_len = len < FTS_LEX_SHAPE_CAP ? len : FTS_LEX_SHAPE_CAP;
    fts_lex_init(&lx, sql, shape_len, FTS_LEX_PARAM_SQLITE_VARIABLE);
    for (;;) {
        tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) {
            if (pending_comma) {
                fts_lex_token comma = {
                    FTS_LEX_TOK_SYMBOL, 0u, 0u, ','
                };
                hash = fts_lex_shape_mix_token(hash, sql, &comma);
            }
            return hash;
        }

        if (pending_comma) {
            if (tok.type == list_literal) {
                if (!list_collapsed) {
                    fts_lex_token collapse = {
                        FTS_LEX_TOK_SYMBOL, 0u, 0u, 0x04
                    };
                    hash = fts_lex_shape_mix_token(hash, sql, &collapse);
                    list_collapsed = 1;
                }
                pending_comma = 0;
                continue;
            }
            {
                fts_lex_token comma = {
                    FTS_LEX_TOK_SYMBOL, 0u, 0u, ','
                };
                hash = fts_lex_shape_mix_token(hash, sql, &comma);
            }
            pending_comma = 0;
            list_literal = FTS_LEX_TOK_EOF;
            list_collapsed = 0;
        }

        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ',' &&
            (list_literal == FTS_LEX_TOK_NUMBER ||
             list_literal == FTS_LEX_TOK_STRING)) {
            pending_comma = 1;
            continue;
        }

        hash = fts_lex_shape_mix_token(hash, sql, &tok);
        if (tok.type == FTS_LEX_TOK_NUMBER || tok.type == FTS_LEX_TOK_STRING) {
            list_literal = tok.type;
            list_collapsed = 0;
        } else {
            list_literal = FTS_LEX_TOK_EOF;
            list_collapsed = 0;
        }
    }
}

static int fts_lex_match_rhs_boundary_keyword(const char *sql, const fts_lex_token *tok) {
    return fts_lex_token_text_eq(sql, tok, "and") ||
           fts_lex_token_text_eq(sql, tok, "or") ||
           fts_lex_token_text_eq(sql, tok, "where") ||
           fts_lex_token_text_eq(sql, tok, "on") ||
           fts_lex_token_text_eq(sql, tok, "join") ||
           fts_lex_token_text_eq(sql, tok, "left") ||
           fts_lex_token_text_eq(sql, tok, "right") ||
           fts_lex_token_text_eq(sql, tok, "inner") ||
           fts_lex_token_text_eq(sql, tok, "outer") ||
           fts_lex_token_text_eq(sql, tok, "cross") ||
           fts_lex_token_text_eq(sql, tok, "full") ||
           fts_lex_token_text_eq(sql, tok, "group") ||
           fts_lex_token_text_eq(sql, tok, "having") ||
           fts_lex_token_text_eq(sql, tok, "order") ||
           fts_lex_token_text_eq(sql, tok, "limit") ||
           fts_lex_token_text_eq(sql, tok, "union") ||
           fts_lex_token_text_eq(sql, tok, "except") ||
           fts_lex_token_text_eq(sql, tok, "intersect") ||
           fts_lex_token_text_eq(sql, tok, "window");
}

static int fts_lex_match_rhs_boundary(const char *sql, const fts_lex_token *tok) {
    if (tok->type == FTS_LEX_TOK_EOF) return 1;
    if (tok->type == FTS_LEX_TOK_SYMBOL && tok->symbol == ')') return 1;
    if (tok->type == FTS_LEX_TOK_IDENT) return fts_lex_match_rhs_boundary_keyword(sql, tok);
    return 0;
}

__attribute__((visibility("hidden"))) int fts_lex_match_rhs_is_complete(
    const fts_lex *after_value,
    const fts_lex_token *value
) {
    fts_lex look;
    fts_lex_token boundary;

    if (!after_value || !value) return 0;
    if (value->type == FTS_LEX_TOK_PARAM &&
        after_value->param_policy == FTS_LEX_PARAM_NUMBERED_OR_NAMED &&
        value->end < after_value->len &&
        (after_value->sql[value->end] == ':' || after_value->sql[value->end] == '(')) {
        return 0;
    }

    look = *after_value;
    boundary = fts_lex_next_token(&look);
    if (boundary.type == FTS_LEX_TOK_ERROR) return 0;
    return fts_lex_match_rhs_boundary(after_value->sql, &boundary);
}

__attribute__((visibility("hidden"))) int fts_rewrite_db_basename_matches(
    const char *raw_fn,
    const char *basename
) {
    size_t raw_len;
    char fnbuf[2048];
    char *q;
    const char *base;

    if (!raw_fn || raw_fn[0] == 0 || !basename || basename[0] == 0) return 0;
    raw_len = strlen(raw_fn);
    if (raw_len >= sizeof(fnbuf)) return 0;
    memcpy(fnbuf, raw_fn, raw_len + 1);
    q = strchr(fnbuf, '?');
    if (q) *q = 0;
    if (strcmp(fnbuf, ":memory:") == 0 || fnbuf[0] == 0) return 0;
    base = strrchr(fnbuf, '/');
    base = base ? base + 1 : fnbuf;
    for (q = fnbuf; *q; q++) {
        if (((unsigned char)*q) >= 0x80) return 0;
    }
    return strcmp(base, basename) == 0;
}
