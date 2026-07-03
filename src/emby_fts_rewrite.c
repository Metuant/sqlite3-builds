#include "emby_fts_rewrite.h"
#include "fts_lex.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EMBY_DB_BASENAME "library.db"
#define EMBY_SCALAR_NAME "dshadow_emby_fts_rewrite"
#define EMBY_SCALAR_OPEN "dshadow_emby_fts_rewrite("
#define EMBY_SCALAR_OPEN_LEN (sizeof(EMBY_SCALAR_OPEN) - 1)
#define EMBY_SCALAR_CLOSE_LEN 1
#define EMBY_LOG_SQLBUF 4352
#define EMBY_SLOT_CAP (64u * 1024u)
#define EMBY_SCALAR_INPUT_CAP (64u * 1024u)
#define EMBY_CLIENTDATA_KEY "sqlite3-builds-emby-fts-rewrite"
#define EMBY_OWNER_PROBE "__dshadow_emby_owner_probe__"

typedef struct emby_span {
    size_t start;
    size_t end;
} emby_span;

typedef struct emby_slots {
    emby_span l1;
    emby_span t1;
    emby_span t2;
    emby_span l2;
    emby_span membership;
} emby_slots;

__attribute__((visibility("hidden"))) SQLITE_API int sqlite3_prepare_v2_real(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    sqlite3_stmt **ppStmt,
    const char **pzTail
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);
__attribute__((visibility("hidden"))) SQLITE_API void obs_escape_sql(
    const char *src,
    char *dst,
    size_t dst_n
);

static pthread_once_t g_emby_rewrite_once = PTHREAD_ONCE_INIT;
static atomic_int g_emby_rewrite_enabled;
static char g_emby_scalar_owner_sentinel;
static char g_emby_scalar_ready_sentinel;
static char g_emby_scalar_bypass_sentinel;
static _Thread_local int g_emby_owner_canary_seen;

#ifdef EMBY_FTS_REWRITE_TEST_API
static atomic_int g_emby_scalar_calls;

__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_scalar_calls(void) {
    return atomic_load_explicit(&g_emby_scalar_calls, memory_order_acquire);
}

__attribute__((visibility("hidden"))) void emby_fts_rewrite_test_reset_scalar_calls(void) {
    atomic_store_explicit(&g_emby_scalar_calls, 0, memory_order_release);
}
#endif

static void emby_fts_rewrite_init_once(void) {
    const char *value = getenv("SQLITE3_DISABLE_EMBY_FTS_REWRITE");
    atomic_store_explicit(
        &g_emby_rewrite_enabled,
        (value && strcmp(value, "0") == 0) ? 1 : 0,
        memory_order_release
    );
}

static int emby_rewrite_enabled(void) {
    pthread_once(&g_emby_rewrite_once, emby_fts_rewrite_init_once);
    return atomic_load_explicit(&g_emby_rewrite_enabled, memory_order_acquire);
}

static int call_downstream_prepare(
    fts_rewrite_prepare_kind kind,
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail
) {
    return plex_fts_rewrite_prepare(db, zSql, nByte, prepFlags, ppStmt, pzTail, kind);
}

static int emby_prepare_input_lengths(
    const char *zSql,
    int nByte,
    size_t *bounded_len,
    size_t *scan_len
) {
    if (nByte < 0) {
        size_t n = strlen(zSql);
        if (n > SIZE_MAX - EMBY_SCALAR_OPEN_LEN - EMBY_SCALAR_CLOSE_LEN - 1) return 0;
        *bounded_len = n;
        *scan_len = n;
        return 1;
    }

    if (nByte == 0) return 0;
    *bounded_len = (size_t)nByte;
    *scan_len = *bounded_len;
    if (*scan_len > 0 && zSql[*scan_len - 1] == 0) {
        (*scan_len)--;
    }
    if (memchr(zSql, 0, *scan_len) != NULL) return 0;
    return 1;
}

static int validate_single_statement_and_match(
    const char *zSql,
    size_t len,
    emby_span *rhs_span
) {
    fts_lex lx;
    int depth = 0;
    int matches = 0;

    fts_lex_init(&lx, zSql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);
    rhs_span->start = 0;
    rhs_span->end = 0;

    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ';') return 0;
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == '(') {
            depth++;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_SYMBOL && tok.symbol == ')') {
            if (depth == 0) return 0;
            depth--;
            continue;
        }
        if (tok.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(zSql, &tok, "fts_search9")) {
            fts_lex look = lx;
            fts_lex_token match = fts_lex_next_token(&look);
            fts_lex_token rhs;
            if (match.type == FTS_LEX_TOK_IDENT && fts_lex_token_text_eq(zSql, &match, "match")) {
                rhs = fts_lex_next_token(&look);
                if (rhs.type == FTS_LEX_TOK_PARAM) {
                    if (!fts_lex_match_rhs_is_complete(&look, &rhs)) return 0;
                    matches++;
                    rhs_span->start = rhs.start;
                    rhs_span->end = rhs.end;
#ifdef EMBY_FTS_REWRITE_TEST_LITERAL_MATCH
                } else if (rhs.type == FTS_LEX_TOK_STRING) {
                    if (!fts_lex_match_rhs_is_complete(&look, &rhs)) return 0;
                    matches++;
                    rhs_span->start = rhs.start;
                    rhs_span->end = rhs.end;
#endif
                } else {
                    return 0;
                }
            }
        }
    }
    if (depth != 0) return 0;
    return matches == 1;
}

static int find_unique_token_after(
    const char *sql,
    size_t len,
    size_t start,
    const char *needle,
    emby_span *out
) {
    size_t needle_len = strlen(needle);
    fts_lex lx;
    int found = 0;

    if (needle_len == 0 || start > len || needle_len > len) return 0;
    fts_lex_init(&lx, sql, len, FTS_LEX_PARAM_NUMBERED_OR_NAMED);

    for (;;) {
        fts_lex_token tok = fts_lex_next_token(&lx);
        if (tok.type == FTS_LEX_TOK_ERROR) return 0;
        if (tok.type == FTS_LEX_TOK_EOF) break;
        if (tok.start < start || tok.start > len - needle_len) continue;
        if (memcmp(sql + tok.start, needle, needle_len) != 0) continue;
        if (found) return 0;
        out->start = tok.start;
        out->end = tok.start + needle_len;
        found = 1;
    }
    return found;
}

#ifdef EMBY_FTS_REWRITE_TEST_API
__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_duplicate_anchor_guard(void) {
    static const char sql[] = "select 1 select 2";
    emby_span out;
    return find_unique_token_after(sql, sizeof(sql) - 1, 0, "select", &out);
}

__attribute__((visibility("hidden"))) int emby_fts_rewrite_test_string_anchor_immunity(void) {
    static const char sql[] = "select 'select'";
    emby_span out;
    if (!find_unique_token_after(sql, sizeof(sql) - 1, 0, "select", &out)) return 0;
    return out.start == 0 && out.end == 6;
}
#endif

static int validate_numeric_slot(const char *sql, const emby_span *slot) {
    size_t i;
    int saw_digit = 0;
    if (slot->end <= slot->start) return 0;
    if (slot->end - slot->start > EMBY_SLOT_CAP) return 0;
    for (i = slot->start; i < slot->end; i++) {
        unsigned char c = (unsigned char)sql[i];
        if (c >= '0' && c <= '9') {
            saw_digit = 1;
            continue;
        }
        if (c == ',' || fts_lex_is_space(c)) continue;
        return 0;
    }
    return saw_digit;
}

static int capture_membership_slots(const char *sql, size_t len, emby_slots *slots) {
    static const char pre_l1[] =
        "WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (";
    static const char after_l1[] =
        ") ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t1[] =
        ") union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (";
    static const char after_t2[] =
        ")))select";
    static const char pre_l2[] =
        "(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (";
    static const char after_l2[] =
        ")) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)";
    emby_span a;
    emby_span b;
    emby_span c;
    emby_span d;
    emby_span e;
    emby_span f;

    memset(slots, 0, sizeof(*slots));
    if (!find_unique_token_after(sql, len, 0, pre_l1, &a)) return 0;
    if (!find_unique_token_after(sql, len, a.end, after_l1, &b)) return 0;
    if (!find_unique_token_after(sql, len, b.end, after_t1, &c)) return 0;
    if (!find_unique_token_after(sql, len, c.end, after_t2, &d)) return 0;
    if (!find_unique_token_after(sql, len, d.end, pre_l2, &e)) return 0;
    if (!find_unique_token_after(sql, len, e.end, after_l2, &f)) return 0;

    slots->l1.start = a.end;
    slots->l1.end = b.start;
    slots->t1.start = b.end;
    slots->t1.end = c.start;
    slots->t2.start = c.end;
    slots->t2.end = d.start;
    slots->l2.start = e.end;
    slots->l2.end = f.start;
    slots->membership.start = e.start;
    slots->membership.end = f.end;

    if (!validate_numeric_slot(sql, &slots->l1)) return 0;
    if (!validate_numeric_slot(sql, &slots->t1)) return 0;
    if (!validate_numeric_slot(sql, &slots->t2)) return 0;
    if (!validate_numeric_slot(sql, &slots->l2)) return 0;
    return 1;
}

static int add_size(size_t *acc, size_t v) {
    if (*acc > SIZE_MAX - v) return 0;
    *acc += v;
    return 1;
}

typedef enum emby_slot_ref {
    EMBY_SLOT_L1 = 0,
    EMBY_SLOT_L2,
    EMBY_SLOT_T1,
    EMBY_SLOT_T2
} emby_slot_ref;

static const char *const EMBY_EXISTS_PARTS[] = {
    "(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (",
    ")) OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (",
    ") where ListItems.ListId=A.Id) OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (",
    ")) OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (",
    ")  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (",
    ")) OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (",
    ")  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (",
    ")) and ItemLinks2TwoLevel.LinkedId=A.Id))"
};

static const emby_slot_ref EMBY_EXISTS_SLOT_ORDER[] = {
    EMBY_SLOT_L1,
    EMBY_SLOT_L2,
    EMBY_SLOT_L1,
    EMBY_SLOT_L1,
    EMBY_SLOT_T1,
    EMBY_SLOT_L1,
    EMBY_SLOT_T2
};

static const emby_span *slot_by_ref(const emby_slots *slots, emby_slot_ref ref) {
    switch (ref) {
        case EMBY_SLOT_L1:
            return &slots->l1;
        case EMBY_SLOT_L2:
            return &slots->l2;
        case EMBY_SLOT_T1:
            return &slots->t1;
        case EMBY_SLOT_T2:
            return &slots->t2;
        default:
            return &slots->l1;
    }
}

static void append_bytes(char **p, const char *z, size_t n) {
    if (n) {
        memcpy(*p, z, n);
        *p += n;
    }
}

static void append_slot(char **p, const char *sql, const emby_span *slot) {
    append_bytes(p, sql + slot->start, slot->end - slot->start);
}

static size_t exists_arms_static_len(void) {
    size_t i;
    size_t n = 0;
    for (i = 0; i < sizeof(EMBY_EXISTS_PARTS) / sizeof(EMBY_EXISTS_PARTS[0]); i++) {
        n += strlen(EMBY_EXISTS_PARTS[i]);
    }
    return n;
}

static char *build_exists_arms(const char *sql, const emby_slots *slots, size_t *out_len) {
    size_t n = exists_arms_static_len();
    char *buf;
    char *p;
    size_t i;

    for (i = 0; i < sizeof(EMBY_EXISTS_SLOT_ORDER) / sizeof(EMBY_EXISTS_SLOT_ORDER[0]); i++) {
        const emby_span *slot = slot_by_ref(slots, EMBY_EXISTS_SLOT_ORDER[i]);
        if (!add_size(&n, slot->end - slot->start)) return NULL;
    }
    buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    p = buf;
    for (i = 0; i < sizeof(EMBY_EXISTS_SLOT_ORDER) / sizeof(EMBY_EXISTS_SLOT_ORDER[0]); i++) {
        append_bytes(&p, EMBY_EXISTS_PARTS[i], strlen(EMBY_EXISTS_PARTS[i]));
        append_slot(&p, sql, slot_by_ref(slots, EMBY_EXISTS_SLOT_ORDER[i]));
    }
    append_bytes(&p, EMBY_EXISTS_PARTS[i], strlen(EMBY_EXISTS_PARTS[i]));
    *p = 0;
    *out_len = (size_t)(p - buf);
    return buf;
}

static char *emby_build_rewritten_sql(
    sqlite3 *db,
    const char *zSql,
    size_t bounded_len,
    size_t scan_len,
    const emby_span *rhs_span,
    const emby_slots *slots,
    size_t *scan_out_len,
    size_t *rewrite_len
) {
    size_t arms_len = 0;
    char *arms = build_exists_arms(zSql, slots, &arms_len);
    size_t membership_old_len = slots->membership.end - slots->membership.start;
    size_t rhs_len = rhs_span->end - rhs_span->start;
    size_t out_len = bounded_len;
    size_t limit;
    char *rewritten;
    char *p;

    if (!arms) return NULL;
    if (rhs_span->start > rhs_span->end ||
        slots->membership.start > slots->membership.end ||
        rhs_span->end > scan_len ||
        slots->membership.end > scan_len) {
        free(arms);
        return NULL;
    }
    if (rhs_span->end > slots->membership.start) {
        free(arms);
        return NULL;
    }

    if (out_len < membership_old_len) {
        free(arms);
        return NULL;
    }
    out_len -= membership_old_len;
    if (!add_size(&out_len, arms_len)) {
        free(arms);
        return NULL;
    }
    if (!add_size(&out_len, EMBY_SCALAR_OPEN_LEN + EMBY_SCALAR_CLOSE_LEN)) {
        free(arms);
        return NULL;
    }

    limit = (size_t)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1);
    if (out_len > limit || out_len > (size_t)INT_MAX) {
        free(arms);
        return NULL;
    }

    rewritten = (char *)malloc(out_len + 1);
    if (!rewritten) {
        free(arms);
        return NULL;
    }
    p = rewritten;

    append_bytes(&p, zSql, rhs_span->start);
    append_bytes(&p, EMBY_SCALAR_OPEN, EMBY_SCALAR_OPEN_LEN);
    append_bytes(&p, zSql + rhs_span->start, rhs_len);
    *p++ = ')';
    append_bytes(&p, zSql + rhs_span->end, slots->membership.start - rhs_span->end);
    append_bytes(&p, arms, arms_len);
    append_bytes(&p, zSql + slots->membership.end, bounded_len - slots->membership.end);

    rewritten[out_len] = 0;
    *scan_out_len = scan_len - membership_old_len + arms_len +
                    EMBY_SCALAR_OPEN_LEN + EMBY_SCALAR_CLOSE_LEN;
    *rewrite_len = out_len;
    free(arms);
    return rewritten;
}

static int emby_map_end_tail(
    const char *zSql,
    const char *rewritten,
    const char *rewritten_tail,
    size_t original_sql_len,
    size_t rewritten_sql_len,
    const char **mapped_tail
) {
    if (!rewritten_tail) {
        *mapped_tail = NULL;
        return 1;
    }
    if (rewritten_tail != rewritten + rewritten_sql_len) return 0;
    *mapped_tail = zSql + original_sql_len;
    return 1;
}

static void emby_log_rewrite_applied(sqlite3 *db, const char *rewritten) {
    char sqlbuf[EMBY_LOG_SQLBUF];

    obs_escape_sql(rewritten, sqlbuf, sizeof(sqlbuf));
    obs_logf("emby_fts_rewrite",
             "event=rewrite_applied target=emby mode=fts+membership db=%p sql=\"%s\"",
             (void*)db, sqlbuf);
}

static void log_rewrite_skipped(sqlite3 *db, const char *reason) {
    obs_logf("emby_fts_rewrite",
             "event=rewrite_skipped target=emby reason=%s db=%p",
             reason, (void*)db);
}

static void maybe_log_l1_l2_mismatch(sqlite3 *db, const char *sql, const emby_slots *slots) {
    size_t l1_len = slots->l1.end - slots->l1.start;
    size_t l2_len = slots->l2.end - slots->l2.start;
    if (l1_len == l2_len &&
        memcmp(sql + slots->l1.start, sql + slots->l2.start, l1_len) == 0) {
        return;
    }
    obs_logf("emby_fts_rewrite",
             "event=slot_mismatch target=emby slot=L1_L2 db=%p l1_len=%zu l2_len=%zu",
             (void*)db, l1_len, l2_len);
}

static int emby_retry_original_after_rewrite_failure(
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
    return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
}

static int probe_existing_function(sqlite3 *db, int *exists) {
    static const char probe_sql[] =
        "SELECT 1 FROM pragma_function_list WHERE name='dshadow_emby_fts_rewrite' COLLATE NOCASE LIMIT 1";
    sqlite3_stmt *probe_stmt = NULL;
    const char *probe_tail = NULL;
    int rc;
    int step_rc;
    int final_rc;

    *exists = 0;
    rc = sqlite3_prepare_v2_real(db, probe_sql, -1, &probe_stmt, &probe_tail);
    if (rc != SQLITE_OK) {
        if (probe_stmt) sqlite3_finalize(probe_stmt);
        return 0;
    }
    if (!probe_tail || probe_tail != probe_sql + strlen(probe_sql)) {
        sqlite3_finalize(probe_stmt);
        return 0;
    }
    step_rc = sqlite3_step(probe_stmt);
    if (step_rc == SQLITE_ROW) {
        *exists = 1;
    } else if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(probe_stmt);
        return 0;
    }
    final_rc = sqlite3_finalize(probe_stmt);
    if (final_rc != SQLITE_OK) return 0;
    return 1;
}

static void *scalar_connection_state(sqlite3 *db) {
    return sqlite3_get_clientdata(db, EMBY_CLIENTDATA_KEY);
}

static int set_scalar_connection_state(sqlite3 *db, void *state) {
    return sqlite3_set_clientdata(db, EMBY_CLIENTDATA_KEY, state, NULL);
}

static int parse_scalar_atom(const unsigned char *z, int n, int start, int *end) {
    int p = start;
    if (p + 5 > n) return 0;
    if (z[p++] != '(') return 0;
    if (z[p++] != '"') return 0;
    if (p >= n || z[p] == '"') return 0;
    /* ASCII-only OR-to-AND is intentional; any atom byte >= 0x80 fails open to original OR text. */
    while (p < n && z[p] != '"') {
        if (z[p] >= 0x80 || fts_lex_is_space(z[p]) || z[p] == '(' || z[p] == ')' || z[p] == '*') {
            return 0;
        }
        p++;
    }
    if (p >= n || z[p++] != '"') return 0;
    if (p >= n || z[p++] != '*') return 0;
    if (p >= n || z[p++] != ')') return 0;
    *end = p;
    return 1;
}

static char *rewrite_scalar_or_to_and(const unsigned char *z, int n, int *out_n) {
    int p = 0;
    int atom_end = 0;
    int count = 0;
    char *out;
    char *w;

    if (n <= 0 || n > (int)EMBY_SCALAR_INPUT_CAP) return NULL;
    if (!parse_scalar_atom(z, n, p, &atom_end)) return NULL;
    count = 1;
    p = atom_end;
    while (p < n) {
        if (p + 4 > n || memcmp(z + p, " OR ", 4) != 0) return NULL;
        p += 4;
        if (!parse_scalar_atom(z, n, p, &atom_end)) return NULL;
        count++;
        p = atom_end;
    }
    if (count < 2) return NULL;

    *out_n = n + (count - 1);
    out = (char *)malloc((size_t)*out_n + 1);
    if (!out) return NULL;
    w = out;
    p = 0;
    for (;;) {
        parse_scalar_atom(z, n, p, &atom_end);
        memcpy(w, z + p, (size_t)(atom_end - p));
        w += atom_end - p;
        p = atom_end;
        if (p >= n) break;
        memcpy(w, " AND ", 5);
        w += 5;
        p += 4;
    }
    out[*out_n] = 0;
    return out;
}

static void emby_fts_scalar(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    void *owner = sqlite3_user_data(ctx);
    int value_type;
    const unsigned char *text;
    int n;
    int out_n = 0;
    char *rewritten;

    if (argc != 1) {
        sqlite3_result_null(ctx);
        return;
    }
#ifdef EMBY_FTS_REWRITE_TEST_API
    atomic_fetch_add_explicit(&g_emby_scalar_calls, 1, memory_order_acq_rel);
#endif
    value_type = sqlite3_value_type(argv[0]);
    if (value_type == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    if (value_type != SQLITE_TEXT) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    text = sqlite3_value_text(argv[0]);
    n = sqlite3_value_bytes(argv[0]);
    if (!text) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    if (n == (int)strlen(EMBY_OWNER_PROBE) &&
        memcmp(text, EMBY_OWNER_PROBE, (size_t)n) == 0 &&
        owner == &g_emby_scalar_owner_sentinel) {
        g_emby_owner_canary_seen = 1;
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    rewritten = rewrite_scalar_or_to_and(text, n, &out_n);
    if (!rewritten) {
        sqlite3_result_value(ctx, argv[0]);
        return;
    }
    sqlite3_result_text(ctx, rewritten, out_n, free);
}

static int ownership_canary_ok(sqlite3 *db) {
    static const char canary_sql[] =
        "SELECT dshadow_emby_fts_rewrite('__dshadow_emby_owner_probe__')";
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;
    int step_rc;
    int ok = 0;

    g_emby_owner_canary_seen = 0;
    rc = sqlite3_prepare_v2_real(db, canary_sql, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        g_emby_owner_canary_seen = 0;
        return 0;
    }
    if (!tail || tail != canary_sql + strlen(canary_sql)) {
        sqlite3_finalize(stmt);
        g_emby_owner_canary_seen = 0;
        return 0;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        ok = g_emby_owner_canary_seen;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) ok = 0;
    g_emby_owner_canary_seen = 0;
    return ok;
}

static int ensure_scalar_ready(sqlite3 *db) {
    void *state = scalar_connection_state(db);
    int exists = 0;
    int rc;

    if (state == &g_emby_scalar_bypass_sentinel) return 0;
    /* Per-prepare ready-sentinel re-verification trades cost for collision safety; do not remove it as dead work. */
    if (state == &g_emby_scalar_ready_sentinel) return ownership_canary_ok(db);
    if (state) return 0;

    /* The get/probe-create/set path is idempotent for one-thread-per-connection use; benign double-register races fail open. */
    if (!probe_existing_function(db, &exists)) return 0;
    if (exists) {
        (void)set_scalar_connection_state(db, &g_emby_scalar_bypass_sentinel);
        return 0;
    }

    rc = sqlite3_create_function_v2(
        db,
        EMBY_SCALAR_NAME,
        1,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY,
        &g_emby_scalar_owner_sentinel,
        emby_fts_scalar,
        NULL,
        NULL,
        NULL
    );
    if (rc != SQLITE_OK) return 0;
    /* OOM-only safe fail-open: next candidate prepare converges this connection to permanent bypass. */
    if (set_scalar_connection_state(db, &g_emby_scalar_ready_sentinel) != SQLITE_OK) return 0;
    return ownership_canary_ok(db);
}

__attribute__((visibility("hidden"))) int emby_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
) {
    size_t bounded_len = 0;
    size_t scan_len = 0;
    size_t scan_out_len = 0;
    size_t rewrite_len = 0;
    int rewrite_nbyte = -1;
    emby_span rhs_span;
    emby_slots slots;
    const char *raw_fn;
    const char *rewritten_tail = NULL;
    const char *mapped_tail = NULL;
    char *rewritten = NULL;
    int rc;

    if (!emby_rewrite_enabled() || !db || !zSql || !ppStmt || nByte == 0) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    raw_fn = sqlite3_db_filename(db, "main");
    if (!fts_rewrite_db_basename_matches(raw_fn, EMBY_DB_BASENAME)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!emby_prepare_input_lengths(zSql, nByte, &bounded_len, &scan_len)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!validate_single_statement_and_match(zSql, scan_len, &rhs_span)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!capture_membership_slots(zSql, scan_len, &slots)) {
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }
    if (!ensure_scalar_ready(db)) {
        log_rewrite_skipped(db, "scalar_unavailable");
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    rewritten = emby_build_rewritten_sql(
        db, zSql, bounded_len, scan_len, &rhs_span, &slots, &scan_out_len, &rewrite_len
    );
    if (!rewritten) {
        log_rewrite_skipped(db, "build_failed");
        return call_downstream_prepare(kind, db, zSql, nByte, prepFlags, ppStmt, pzTail);
    }

    rewrite_nbyte = nByte < 0 ? -1 : (int)rewrite_len;
    if (ppStmt) *ppStmt = NULL;
    rc = call_downstream_prepare(
        kind, db, rewritten, rewrite_nbyte, prepFlags, ppStmt, &rewritten_tail
    );
    if (rc != SQLITE_OK) {
        log_rewrite_skipped(db, "rewritten_prepare_failed");
        free(rewritten);
        return emby_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }
    if (!emby_map_end_tail(zSql, rewritten, rewritten_tail, scan_len, scan_out_len, &mapped_tail)) {
        log_rewrite_skipped(db, "tail_mismatch");
        free(rewritten);
        return emby_retry_original_after_rewrite_failure(
            kind, db, zSql, nByte, prepFlags, ppStmt, pzTail
        );
    }

    maybe_log_l1_l2_mismatch(db, zSql, &slots);
    emby_log_rewrite_applied(db, rewritten);
    if (pzTail) *pzTail = mapped_tail;
    free(rewritten);
    return rc;
}
