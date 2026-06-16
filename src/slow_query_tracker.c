#include "sqlite3.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOW_QUERY_MAX_ENTRIES 2048u
#define SLOW_QUERY_SQL_CAP 1024u
#define SLOW_QUERY_BUCKETS 4096u
#define SLOW_QUERY_NIL UINT16_MAX
#define SLOW_QUERY_DEFAULT_THRESHOLD_MS 500u
#define SLOW_QUERY_DUMP_INTERVAL_NS (300ull * 1000000000ull)
#define SLOW_QUERY_TRUNC_TAIL "...[TRUNC]"
#define SLOW_QUERY_FALLBACK_CAP (1u << 20)

#if defined(__SIZEOF_INT128__) && __SIZEOF_INT128__ == 16
typedef unsigned __int128 slow_accum_t;
#define SLOW_QUERY_HAVE_INT128 1
#else
typedef uint64_t slow_accum_t;
#define SLOW_QUERY_HAVE_INT128 0
#endif

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);
__attribute__((visibility("hidden"))) SQLITE_API int slow_query_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int slow_query_is_disabled_cached(void);

/* Decode tables stay in src/observability.c. */

/* Observability satellite -- depends on obs_logf/obs_is_disabled hidden ABI in
 * MVP; Phase 2 introduces a sink-abstraction layer. */
struct slow_entry {
    slow_accum_t sum_ns, sum_sq_ns;
    uint64_t hash, min_ns, max_ns;
    uint32_t full_sql_len;
    uint32_t count;
    uint16_t lru_prev, lru_next, sql_len;
    char sql[SLOW_QUERY_SQL_CAP + 1];
};

struct slow_table {
    pthread_mutex_t mu;
    struct slow_entry entries[SLOW_QUERY_MAX_ENTRIES];
    uint16_t bucket[SLOW_QUERY_BUCKETS];
    uint16_t lru_head, lru_tail;
    uint32_t entries_used;
    uint32_t tombstones;
    _Atomic uint64_t last_dump_mono_ns;
};

struct slow_entry_snapshot {
    slow_accum_t sum_ns, sum_sq_ns;
    uint64_t hash, min_ns, max_ns;
    uint32_t full_sql_len;
    uint32_t count;
    uint16_t sql_len;
    char sql[SLOW_QUERY_SQL_CAP + 1];
};

static struct slow_table g_slow = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .lru_head = SLOW_QUERY_NIL,
    .lru_tail = SLOW_QUERY_NIL,
};
static pthread_once_t g_slow_once = PTHREAD_ONCE_INIT;
static atomic_int g_slow_disabled;
static uint64_t g_threshold_ns = SLOW_QUERY_DEFAULT_THRESHOLD_MS * 1000000ull;
static _Atomic uint64_t g_negative_diag_mono_ns;
static _Atomic int g_in_atexit = 0;
static uint8_t g_fallback_cap_warned[SLOW_QUERY_MAX_ENTRIES];

static uint64_t slow_mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static int slow_parse_ms(const char *s, uint64_t *out_ms) {
    char *end = NULL;
    unsigned long long v;

    if (!s || s[0] == 0 || s[0] == '-' || s[0] == '+') return 0;
    if (!isdigit((unsigned char)s[0])) return 0;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno == ERANGE || end == s || *end != 0) return 0;
    if (v > UINT64_MAX / 1000000ull) return 0;
    *out_ms = (uint64_t)v;
    return 1;
}

static void slow_init_once(void) {
    const char *disable = getenv("SQLITE3_DISABLE_SLOW_QUERY");
    const char *threshold = getenv("SQLITE3_SLOW_QUERY_THRESHOLD_MS");
    uint64_t ms = SLOW_QUERY_DEFAULT_THRESHOLD_MS;

    atomic_store_explicit(
        &g_slow_disabled,
        (disable && strcmp(disable, "1") == 0) ? 1 : 0,
        memory_order_release
    );
    if (slow_parse_ms(threshold, &ms)) {
        g_threshold_ns = ms * 1000000ull;
    } else {
        g_threshold_ns = SLOW_QUERY_DEFAULT_THRESHOLD_MS * 1000000ull;
    }
}

__attribute__((visibility("hidden"))) SQLITE_API int slow_query_disabled(void) {
    pthread_once(&g_slow_once, slow_init_once);
    return atomic_load_explicit(&g_slow_disabled, memory_order_acquire);
}

/* Hot path (slow_query_record_sql) uses this cached variant: atomic-load only.
 * Non-cached slow_query_disabled (with pthread_once) is reserved for non-hot
 * test/registration paths. constructor(103) calls pthread_once at dlopen so
 * the cached load sees the resolved value. */
__attribute__((visibility("hidden"))) SQLITE_API int slow_query_is_disabled_cached(void) {
    return atomic_load_explicit(&g_slow_disabled, memory_order_acquire);
}

static uint64_t slow_fnv1a(const char *sql, uint32_t *full_len) {
    const unsigned char *p = (const unsigned char *)(sql ? sql : "");
    uint64_t h = 1469598103934665603ull;
    uint64_t n = 0;

    while (*p) {
        h ^= (uint64_t)*p++;
        h *= 1099511628211ull;
        if (n < UINT32_MAX) n++;
    }
    *full_len = (uint32_t)n;
    return h;
}

static int slow_identity_matches(
    const struct slow_entry *e, uint64_t hash, uint32_t full_len, const char *sql
) {
    if (e->hash != hash || e->full_sql_len != full_len) return 0;
    if (full_len <= SLOW_QUERY_SQL_CAP) return memcmp(e->sql, sql, full_len) == 0;
    return e->sql_len == SLOW_QUERY_SQL_CAP;
}

static void slow_lru_remove_locked(uint16_t idx) {
    struct slow_entry *e = &g_slow.entries[idx];
    if (e->lru_prev != SLOW_QUERY_NIL) g_slow.entries[e->lru_prev].lru_next = e->lru_next;
    else g_slow.lru_head = e->lru_next;
    if (e->lru_next != SLOW_QUERY_NIL) g_slow.entries[e->lru_next].lru_prev = e->lru_prev;
    else g_slow.lru_tail = e->lru_prev;
    e->lru_prev = SLOW_QUERY_NIL;
    e->lru_next = SLOW_QUERY_NIL;
}

static void slow_lru_push_head_locked(uint16_t idx) {
    struct slow_entry *e = &g_slow.entries[idx];
    e->lru_prev = SLOW_QUERY_NIL;
    e->lru_next = g_slow.lru_head;
    if (g_slow.lru_head != SLOW_QUERY_NIL) g_slow.entries[g_slow.lru_head].lru_prev = idx;
    else g_slow.lru_tail = idx;
    g_slow.lru_head = idx;
}

static void slow_lru_touch_locked(uint16_t idx) {
    if (g_slow.lru_head == idx) return;
    slow_lru_remove_locked(idx);
    slow_lru_push_head_locked(idx);
}

static void slow_bucket_insert_locked(uint16_t idx) {
    uint32_t pos = (uint32_t)(g_slow.entries[idx].hash & (SLOW_QUERY_BUCKETS - 1u));
    uint32_t probes;
    for (probes = 0; probes < SLOW_QUERY_BUCKETS; probes++) {
        uint16_t stored = g_slow.bucket[pos];
        if (stored == 0) {
            g_slow.bucket[pos] = (uint16_t)(idx + 1u);
            return;
        }
        if (stored == SLOW_QUERY_NIL) {
            pos = (pos + 1u) & (SLOW_QUERY_BUCKETS - 1u);
            continue;
        }
        pos = (pos + 1u) & (SLOW_QUERY_BUCKETS - 1u);
    }
}

static void slow_bucket_tombstone_locked(uint16_t idx) {
    uint16_t needle = (uint16_t)(idx + 1u);
    uint32_t pos = (uint32_t)(g_slow.entries[idx].hash & (SLOW_QUERY_BUCKETS - 1u));
    uint32_t probes;
    for (probes = 0; probes < SLOW_QUERY_BUCKETS; probes++) {
        if (g_slow.bucket[pos] == 0) return;
        if (g_slow.bucket[pos] == needle) {
            g_slow.bucket[pos] = SLOW_QUERY_NIL;
            g_slow.tombstones++;
            return;
        }
        pos = (pos + 1u) & (SLOW_QUERY_BUCKETS - 1u);
    }
}

static void slow_rebuild_buckets_locked(void) {
    uint32_t i;
    memset(g_slow.bucket, 0, sizeof(g_slow.bucket));
    g_slow.tombstones = 0;
    for (i = 0; i < g_slow.entries_used; i++) {
        if (i != g_slow.lru_head &&
            g_slow.entries[i].lru_prev == SLOW_QUERY_NIL &&
            g_slow.entries[i].lru_next == SLOW_QUERY_NIL) {
            continue;
        }
        slow_bucket_insert_locked((uint16_t)i);
    }
}

static uint16_t slow_find_locked(uint64_t hash, uint32_t full_len, const char *sql) {
    uint32_t pos = (uint32_t)(hash & (SLOW_QUERY_BUCKETS - 1u));
    uint32_t probes;
    for (probes = 0; probes < SLOW_QUERY_BUCKETS; probes++) {
        uint16_t stored = g_slow.bucket[pos];
        if (stored == 0) return SLOW_QUERY_NIL;
        if (stored == SLOW_QUERY_NIL) {
            pos = (pos + 1u) & (SLOW_QUERY_BUCKETS - 1u);
            continue;
        }
        if (slow_identity_matches(&g_slow.entries[stored - 1u], hash, full_len, sql)) {
            return (uint16_t)(stored - 1u);
        }
        pos = (pos + 1u) & (SLOW_QUERY_BUCKETS - 1u);
    }
    return SLOW_QUERY_NIL;
}

static uint16_t slow_insert_locked(uint64_t hash, uint32_t full_len, const char *sql) {
    uint16_t idx;
    struct slow_entry *e;
    size_t copy_len = full_len < SLOW_QUERY_SQL_CAP ? (size_t)full_len : SLOW_QUERY_SQL_CAP;

    if (g_slow.entries_used < SLOW_QUERY_MAX_ENTRIES) {
        idx = (uint16_t)g_slow.entries_used++;
    } else {
        idx = g_slow.lru_tail;
        slow_lru_remove_locked(idx);
        slow_bucket_tombstone_locked(idx);
        /* Amortized O(1): rebuild only when tombstones >= 25% of entries_used;
         * steady-state eviction is O(1) vs O(N) on every eviction. */
        if (g_slow.tombstones >= g_slow.entries_used / 4u) {
            slow_rebuild_buckets_locked();
        }
    }

    e = &g_slow.entries[idx];
    memset(e, 0, sizeof(*e));
    e->hash = hash;
    e->full_sql_len = full_len;
    e->sql_len = (uint16_t)copy_len;
    if (copy_len > 0) memcpy(e->sql, sql, copy_len);
    e->sql[copy_len] = 0;
    e->lru_prev = SLOW_QUERY_NIL;
    e->lru_next = SLOW_QUERY_NIL;
    g_fallback_cap_warned[idx] = 0;
    slow_lru_push_head_locked(idx);
    slow_bucket_insert_locked(idx);
    return idx;
}

static slow_accum_t slow_accum_square(uint64_t v) {
#if SLOW_QUERY_HAVE_INT128
    return (slow_accum_t)v * (slow_accum_t)v;
#else
    if (v != 0 && v > UINT64_MAX / v) return UINT64_MAX;
    return v * v;
#endif
}

static int slow_update_locked(uint16_t idx, uint64_t elapsed_ns) {
    struct slow_entry *e = &g_slow.entries[idx];

#if !SLOW_QUERY_HAVE_INT128
    if (e->count >= SLOW_QUERY_FALLBACK_CAP) {
        if (!g_fallback_cap_warned[idx]) {
            g_fallback_cap_warned[idx] = 1;
            return 1;
        }
        return 0;
    }
#endif

    if (e->count == 0) {
        e->min_ns = elapsed_ns;
        e->max_ns = elapsed_ns;
    } else {
        if (elapsed_ns < e->min_ns) e->min_ns = elapsed_ns;
        if (elapsed_ns > e->max_ns) e->max_ns = elapsed_ns;
    }
    e->count++;
    e->sum_ns += (slow_accum_t)elapsed_ns;
    e->sum_sq_ns += slow_accum_square(elapsed_ns);
    slow_lru_touch_locked(idx);
    return 0;
}

static int slow_accum_cmp(slow_accum_t a, slow_accum_t b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int slow_snapshot_cmp(const void *a, const void *b) {
    const struct slow_entry_snapshot *ea = (const struct slow_entry_snapshot *)a;
    const struct slow_entry_snapshot *eb = (const struct slow_entry_snapshot *)b;
    return -slow_accum_cmp(ea->sum_ns, eb->sum_ns);
}

static long double slow_accum_to_ld(slow_accum_t v) {
#if SLOW_QUERY_HAVE_INT128
    const slow_accum_t base = (slow_accum_t)1000000000ull;
    long double result = 0.0L;
    long double scale = 1.0L;
    while (v != 0) {
        result += (long double)(uint64_t)(v % base) * scale;
        v /= base;
        scale *= 1000000000.0L;
    }
    return result;
#else
    return (long double)v;
#endif
}

static void slow_escape_string(
    const char *src, char *dst, size_t dst_n, size_t cap, int *truncated
) {
    size_t i = 0;
    size_t o = 0;
    *truncated = 0;
    if (dst_n == 0) return;
    if (!src) {
        snprintf(dst, dst_n, "(null)");
        return;
    }
    while (src[i] != 0 && i < cap && o + 5 < dst_n) {
        unsigned char c = (unsigned char)src[i++];
        switch (c) {
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '"': dst[o++] = '\\'; dst[o++] = '"'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n'; break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r'; break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't'; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789ABCDEF";
                    dst[o++] = '\\';
                    dst[o++] = 'x';
                    dst[o++] = hex[c >> 4];
                    dst[o++] = hex[c & 0x0f];
                } else {
                    dst[o++] = (char)c;
                }
                break;
        }
    }
    if (src[i] != 0) *truncated = 1;
    dst[o] = 0;
}

static void slow_escape_sql(const char *src, char *dst, size_t dst_n) {
    int truncated = 0;
    size_t tail_len = strlen(SLOW_QUERY_TRUNC_TAIL);
    slow_escape_string(src, dst, dst_n, SLOW_QUERY_SQL_CAP, &truncated);
    if (truncated && dst_n > tail_len + 1) {
        size_t len = strlen(dst);
        if (len + tail_len >= dst_n) len = dst_n - tail_len - 1;
        memcpy(dst + len, SLOW_QUERY_TRUNC_TAIL, tail_len + 1);
    }
}

static void slow_escape_unlimited(const char *src, char *dst, size_t dst_n) {
    int truncated = 0;
    slow_escape_string(src, dst, dst_n, SIZE_MAX, &truncated);
}

static void slow_emit_stats_snapshot(const struct slow_entry_snapshot *s) {
    char sqlbuf[SLOW_QUERY_SQL_CAP + 256];
    long double count = (long double)s->count;
    long double mean_ns = slow_accum_to_ld(s->sum_ns) / count;
    long double mean_sq_ns = slow_accum_to_ld(s->sum_sq_ns) / count;
    long double variance_ns2 = mean_sq_ns - (mean_ns * mean_ns);
    long double stddev_ns;

    if (variance_ns2 < 0.0L) variance_ns2 = 0.0L;
    stddev_ns = sqrtl(variance_ns2);
    slow_escape_sql(s->sql, sqlbuf, sizeof(sqlbuf));
    obs_logf("slow_query_stats",
             "sql=\"%s\" count=%" PRIu32 " mean_ms=%.3f stddev_ms=%.3f min_ms=%.3f max_ms=%.3f",
             sqlbuf, s->count,
             (double)(mean_ns / 1000000.0L),
             (double)(stddev_ns / 1000000.0L),
             (double)((long double)s->min_ns / 1000000.0L),
             (double)((long double)s->max_ns / 1000000.0L));
}

/* Heap-snapshot WHY: snapshot is heap-allocated to keep qsort + emit work
 * OUTSIDE the mutex (held only for per-entry value copy). Stack allocation is
 * forbidden: 2048 entries x ~1.1 KiB per entry would blow the default stack. */
static void slow_query_dump(int try_lock, const char *reason) {
    struct slow_entry_snapshot *snap = NULL;
    uint32_t n = 0;
    uint32_t had_entries = 0;
    uint32_t i;
    int lock_rc;

    lock_rc = try_lock ? pthread_mutex_trylock(&g_slow.mu) : pthread_mutex_lock(&g_slow.mu);
    if (lock_rc != 0) {
        obs_logf("slow_query_stats", "event=dump_skipped reason=%s contention=1", reason);
        return;
    }

    had_entries = g_slow.entries_used;
    if (had_entries > 0) {
        snap = calloc(g_slow.entries_used, sizeof(*snap));
        if (snap) {
            for (i = 0; i < g_slow.entries_used; i++) {
                const struct slow_entry *e = &g_slow.entries[i];
                if (e->count == 0) continue;
                snap[n].sum_ns = e->sum_ns;
                snap[n].sum_sq_ns = e->sum_sq_ns;
                snap[n].hash = e->hash;
                snap[n].min_ns = e->min_ns;
                snap[n].max_ns = e->max_ns;
                snap[n].full_sql_len = e->full_sql_len;
                snap[n].count = e->count;
                snap[n].sql_len = e->sql_len;
                memcpy(snap[n].sql, e->sql, (size_t)e->sql_len + 1u);
                n++;
            }
        }
    }
    pthread_mutex_unlock(&g_slow.mu);

    if (!snap) {
        if (had_entries > 0) {
            obs_logf("slow_query_stats", "event=dump_skipped reason=%s oom=1", reason);
        }
        return;
    }
    qsort(snap, n, sizeof(*snap), slow_snapshot_cmp);
    for (i = 0; i < n; i++) slow_emit_stats_snapshot(&snap[i]);
    free(snap);
}

static void slow_maybe_periodic_dump(void) {
    uint64_t now = slow_mono_ns();
    uint64_t last = atomic_load_explicit(&g_slow.last_dump_mono_ns, memory_order_acquire);
    if (now == 0) return;
    if (last == 0) {
        atomic_compare_exchange_strong_explicit(
            &g_slow.last_dump_mono_ns, &last, now, memory_order_acq_rel, memory_order_acquire
        );
        return;
    }
    if (now - last < SLOW_QUERY_DUMP_INTERVAL_NS) return;
    if (atomic_compare_exchange_strong_explicit(
            &g_slow.last_dump_mono_ns, &last, now, memory_order_acq_rel, memory_order_acquire)) {
        slow_query_dump(0, "periodic");
    }
}

static void slow_negative_elapsed_diag(void) {
    uint64_t now = slow_mono_ns();
    uint64_t last = atomic_load_explicit(&g_negative_diag_mono_ns, memory_order_acquire);
    if (now == 0 || now - last < 60000000000ull) return;
    if (atomic_compare_exchange_strong_explicit(
            &g_negative_diag_mono_ns, &last, now, memory_order_acq_rel, memory_order_acquire)) {
        obs_logf("slow_query", "event=negative_elapsed_sample_dropped");
    }
}

static int slow_query_record_sql(
    sqlite3 *db, sqlite3_stmt *stmt, const char *sql, sqlite3_int64 elapsed_ns
) {
    uint64_t elapsed;
    uint64_t hash;
    uint32_t full_len;
    uint16_t idx;
    int fallback_warn = 0;
    uint64_t threshold_ns;

    if (atomic_load_explicit(&g_in_atexit, memory_order_acquire)) return 0;
    if (obs_is_disabled()) return 0;
    if (slow_query_is_disabled_cached()) return 0;
    if (elapsed_ns < 0) {
        slow_negative_elapsed_diag();
        return 0;
    }
    if (elapsed_ns > (sqlite3_int64)(INT64_MAX / 2)) elapsed_ns = (sqlite3_int64)(INT64_MAX / 2);
    elapsed = (uint64_t)elapsed_ns;
    threshold_ns = g_threshold_ns;
    if (!sql) sql = "(null)";

    /* FNV-1a is 3-5 cycles/byte; typical templates keep the single-thread
     * tracker path near 1000-1400 cycles / 300-470 ns at 3 GHz. Under N=4
     * contention, mutex acquisition can peak near 1-2 us. Threshold=0 is
     * debug-only and measures about 0.5-0.6% against 100 us statements. The
     * duplicate obs_is_disabled check above is intentional defense-in-depth. */
    hash = slow_fnv1a(sql, &full_len);

    pthread_mutex_lock(&g_slow.mu);
    idx = slow_find_locked(hash, full_len, sql);
    if (idx == SLOW_QUERY_NIL) idx = slow_insert_locked(hash, full_len, sql);
    fallback_warn = slow_update_locked(idx, elapsed);
    pthread_mutex_unlock(&g_slow.mu);

    if (fallback_warn) {
        char sqlbuf[SLOW_QUERY_SQL_CAP + 256];
        slow_escape_sql(sql, sqlbuf, sizeof(sqlbuf));
        obs_logf("slow_query",
                 "event=fallback_accumulator_cap sql=\"%s\" cap=%u",
                 sqlbuf, SLOW_QUERY_FALLBACK_CAP);
    }

    if (elapsed >= threshold_ns) {
        char file[4096];
        char sqlbuf[SLOW_QUERY_SQL_CAP + 256];
        const char *file_name = db ? sqlite3_db_filename(db, "main") : NULL;
        slow_escape_unlimited(file_name, file, sizeof(file));
        slow_escape_sql(sql, sqlbuf, sizeof(sqlbuf));
        obs_logf("slow_query", "db=\"%s\" elapsed_ms=%.3f sql=\"%s\"",
                 file, (double)((long double)elapsed / 1000000.0L), sqlbuf);
    }
    slow_maybe_periodic_dump();
    (void)stmt;
    return 0;
}

/* B-F5/OD2 invariant: g_in_atexit acquire-load MUST remain the FIRST
 * executable instruction here. Any sqlite3_* accessor or pointer deref before
 * this guard reintroduces use-after-teardown risk on PROFILE callbacks during
 * exit. */
__attribute__((visibility("hidden"))) SQLITE_API int slow_query_trace_profile(
    sqlite3_stmt *stmt, sqlite3_int64 elapsed_ns
) {
    if (atomic_load_explicit(&g_in_atexit, memory_order_acquire)) return 0;
    sqlite3 *db = stmt ? sqlite3_db_handle(stmt) : NULL;
    const char *sql = stmt ? sqlite3_sql(stmt) : NULL;
    return slow_query_record_sql(db, stmt, sql, elapsed_ns);
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_cb(
    unsigned trace, void *ctx, void *p, void *x
) {
    if (trace == SQLITE_TRACE_STMT) return obs_trace_stmt_cb(trace, ctx, p, x);
    if (trace == SQLITE_TRACE_PROFILE) {
        if (atomic_load_explicit(&g_in_atexit, memory_order_acquire)) return 0;
        sqlite3_int64 elapsed_ns = x ? *(sqlite3_int64 *)x : 0;
        return slow_query_trace_profile((sqlite3_stmt *)p, elapsed_ns);
    }
    return 0;
}

static void slow_query_atexit_dump(void) {
    if (atomic_exchange_explicit(&g_in_atexit, 1, memory_order_acq_rel) != 0) return;
    if (obs_is_disabled() || slow_query_disabled()) return;
    slow_query_dump(1, "atexit");
}

__attribute__((constructor(103)))
static void slow_query_constructor(void) {
    pthread_once(&g_slow_once, slow_init_once);
    if (!obs_is_disabled() && !slow_query_is_disabled_cached()) {
        atexit(slow_query_atexit_dump);
    }
}

#ifdef SLOW_QUERY_TRACKER_TEST_API
int slow_query_test_record_sql(const char *sql, sqlite3_int64 elapsed_ns) {
    return slow_query_record_sql(NULL, NULL, sql, elapsed_ns);
}

void slow_query_test_dump(void) {
    slow_query_dump(0, "test");
}

uint32_t slow_query_test_entries_used(void) {
    uint32_t entries;
    pthread_mutex_lock(&g_slow.mu);
    entries = g_slow.entries_used;
    pthread_mutex_unlock(&g_slow.mu);
    return entries;
}

uint32_t slow_query_test_max_entries(void) {
    return SLOW_QUERY_MAX_ENTRIES;
}

uint32_t slow_query_test_count_for_sql(const char *sql) {
    uint64_t hash;
    uint32_t full_len;
    uint16_t idx;
    uint32_t count = 0;

    hash = slow_fnv1a(sql, &full_len);
    pthread_mutex_lock(&g_slow.mu);
    idx = slow_find_locked(hash, full_len, sql ? sql : "");
    if (idx != SLOW_QUERY_NIL) count = g_slow.entries[idx].count;
    pthread_mutex_unlock(&g_slow.mu);
    return count;
}

uint32_t slow_query_test_tombstones(void) {
    uint32_t tombstones;
    pthread_mutex_lock(&g_slow.mu);
    tombstones = g_slow.tombstones;
    pthread_mutex_unlock(&g_slow.mu);
    return tombstones;
}

uint64_t slow_query_test_parse_threshold_ns(const char *value) {
    uint64_t ms = SLOW_QUERY_DEFAULT_THRESHOLD_MS;
    return slow_parse_ms(value, &ms) ? ms * 1000000ull : SLOW_QUERY_DEFAULT_THRESHOLD_MS * 1000000ull;
}

void slow_query_test_disable_atexit_dump(void) {
    atomic_store_explicit(&g_in_atexit, 1, memory_order_release);
}
#endif
