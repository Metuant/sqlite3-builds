#include "auto_extension_internal.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);

typedef struct auto_extension_progress_snapshot {
    unsigned nProgressOps;
    int (*xProgress)(void*);
    void *pProgressArg;
} auto_extension_progress_snapshot;

typedef struct auto_extension_error_snapshot {
    int errCode;
    int errByteOffset;
    sqlite3_value *pErr;
} auto_extension_error_snapshot;

__attribute__((visibility("hidden"))) void
auto_extension_progress_handler_push(sqlite3 *db, int nOps,
                                     int (*xProgress)(void*), void *pArg,
                                     auto_extension_progress_snapshot *snapshot);

__attribute__((visibility("hidden"))) void
auto_extension_progress_handler_pop(sqlite3 *db,
                                    const auto_extension_progress_snapshot *snapshot);

__attribute__((visibility("hidden"))) int
auto_extension_error_state_push(sqlite3 *db,
                                auto_extension_error_snapshot *snapshot);

__attribute__((visibility("hidden"))) void
auto_extension_error_state_pop(sqlite3 *db,
                               auto_extension_error_snapshot *snapshot);

#define RUNTIME_OPTIMIZE_PATH_MAX 2048
#define RUNTIME_OPTIMIZE_REGISTRY_CAP 16
#define RUNTIME_OPTIMIZE_NS_PER_SECOND 1000000000LL
#define RUNTIME_OPTIMIZE_DEFAULT_LIMITED_SECONDS 1800LL
#define RUNTIME_OPTIMIZE_DEFAULT_FULL_SECONDS 86400LL
#define RUNTIME_OPTIMIZE_BACKOFF_MAX_SECONDS 60LL
#define RUNTIME_OPTIMIZE_PROGRESS_OPS 1000
#define RUNTIME_OPTIMIZE_LIMITED_DEADLINE_NS 23000000LL
#define RUNTIME_OPTIMIZE_FULL_DEADLINE_NS 4000000000LL
#define RUNTIME_OPTIMIZE_CLIENTDATA_KEY "sqlite3-builds-runtime-optimize"
/* WHY: Bound repeated O(statement-cache) blocked scans and skip logs. */
#define RUNTIME_OPTIMIZE_BLOCKED_REARM_NS (30LL * RUNTIME_OPTIMIZE_NS_PER_SECOND)
/* WHY: Keep runtime optimize off unless SQLITE3_DISABLE_RUNTIME_OPTIMIZE is
 * explicitly set to "0"; unset and every other value leave hooks disabled. */
#define RUNTIME_OPTIMIZE_UNSET_DEFAULT_ENABLED 0
#define RUNTIME_OPTIMIZE_SINCE_LAST_NEVER_NS (-1LL)

typedef enum {
    RUNTIME_OPTIMIZE_TIER_NONE = 0,
    RUNTIME_OPTIMIZE_TIER_LIMITED = 1,
    RUNTIME_OPTIMIZE_TIER_FULL = 2
} runtime_optimize_tier;

typedef struct {
    int used;
    char path[RUNTIME_OPTIMIZE_PATH_MAX];
    sqlite3_int64 last_limited_success_ns;
    sqlite3_int64 last_full_success_ns;
    sqlite3_int64 backoff_until_ns;
    runtime_optimize_tier inflight_tier;
} runtime_optimize_slot;

typedef enum {
    RUNTIME_OPTIMIZE_TRIGGER_CLOSE = 1,
    RUNTIME_OPTIMIZE_TRIGGER_INLINE = 2
} runtime_optimize_trigger;

typedef struct {
    sqlite3_int64 deadline_ns;
} runtime_optimize_progress_ctx;

typedef struct {
    int has_path;
    char path[RUNTIME_OPTIMIZE_PATH_MAX];
    atomic_llong next_due_ns;
} runtime_optimize_connection_state;

static pthread_mutex_t g_runtime_optimize_mu = PTHREAD_MUTEX_INITIALIZER;
static runtime_optimize_slot g_runtime_optimize_slots[RUNTIME_OPTIMIZE_REGISTRY_CAP];
static _Thread_local int g_runtime_optimize_depth;

static int env_is_literal_1(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "1") == 0;
}

static int runtime_optimize_is_enabled(void) {
    const char *value = getenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE");
    if (!value) return RUNTIME_OPTIMIZE_UNSET_DEFAULT_ENABLED;
    return strcmp(value, "0") == 0;
}

static int runtime_optimize_copy_path_key(
    const char *raw_fn,
    char *out,
    size_t out_n
) {
    size_t raw_len;
    char *q;

    if (!raw_fn || raw_fn[0] == 0 || out_n == 0) return 0;
    raw_len = strlen(raw_fn);
    if (raw_len >= out_n) return 0;
    memcpy(out, raw_fn, raw_len + 1);
    q = strchr(out, '?');
    if (q) *q = 0;
    return out[0] != 0;
}

static sqlite3_int64 runtime_optimize_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (sqlite3_int64)ts.tv_sec * RUNTIME_OPTIMIZE_NS_PER_SECOND +
               (sqlite3_int64)ts.tv_nsec;
    }
    return (sqlite3_int64)time(NULL) * RUNTIME_OPTIMIZE_NS_PER_SECOND;
}

static sqlite3_int64 runtime_optimize_parse_cadence_ns(
    const char *name,
    sqlite3_int64 default_seconds
) {
    const char *value = getenv(name);
    sqlite3_int64 seconds = default_seconds;
    unsigned long long acc = 0;
    int ok = 1;
    const char *p;

    if (value && value[0] >= '0' && value[0] <= '9') {
        for (p = value; *p; p++) {
            if (*p < '0' || *p > '9') {
                ok = 0;
                break;
            }
            if (acc > ((unsigned long long)LLONG_MAX / 10ULL)) {
                ok = 0;
                break;
            }
            acc *= 10ULL;
            if (acc > (unsigned long long)LLONG_MAX - (unsigned long long)(*p - '0')) {
                ok = 0;
                break;
            }
            acc += (unsigned long long)(*p - '0');
        }
        if (ok && acc > 0ULL && acc <= (unsigned long long)(LLONG_MAX / RUNTIME_OPTIMIZE_NS_PER_SECOND)) {
            seconds = (sqlite3_int64)acc;
        }
    }

    if (seconds > LLONG_MAX / RUNTIME_OPTIMIZE_NS_PER_SECOND) {
        seconds = default_seconds;
    }
    return seconds * RUNTIME_OPTIMIZE_NS_PER_SECOND;
}

static sqlite3_int64 runtime_optimize_limited_cadence_ns(void) {
    return runtime_optimize_parse_cadence_ns(
        "SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS",
        RUNTIME_OPTIMIZE_DEFAULT_LIMITED_SECONDS
    );
}

static sqlite3_int64 runtime_optimize_full_cadence_ns(void) {
    return runtime_optimize_parse_cadence_ns(
        "SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS",
        RUNTIME_OPTIMIZE_DEFAULT_FULL_SECONDS
    );
}

static sqlite3_int64 runtime_optimize_failure_backoff_ns(sqlite3_int64 limited_cadence_ns) {
    sqlite3_int64 max_backoff = RUNTIME_OPTIMIZE_BACKOFF_MAX_SECONDS * RUNTIME_OPTIMIZE_NS_PER_SECOND;
    sqlite3_int64 backoff = limited_cadence_ns < max_backoff ? limited_cadence_ns : max_backoff;
    if (backoff < RUNTIME_OPTIMIZE_NS_PER_SECOND) backoff = RUNTIME_OPTIMIZE_NS_PER_SECOND;
    return backoff;
}

static sqlite3_int64 runtime_optimize_safe_add_ns(sqlite3_int64 base, sqlite3_int64 delta) {
    if (base > LLONG_MAX - delta) return LLONG_MAX;
    return base + delta;
}

static double runtime_optimize_ns_to_ms(sqlite3_int64 elapsed_ns) {
    if (elapsed_ns < 0) elapsed_ns = 0;
    return (double)elapsed_ns / 1000000.0;
}

static long long runtime_optimize_since_last_ms(sqlite3_int64 since_last_ns) {
    if (since_last_ns < 0) return -1LL;
    return (long long)(since_last_ns / 1000000LL);
}

static runtime_optimize_slot *runtime_optimize_find_slot_locked(const char *path) {
    int i;

    for (i = 0; i < RUNTIME_OPTIMIZE_REGISTRY_CAP; i++) {
        runtime_optimize_slot *slot = &g_runtime_optimize_slots[i];
        if (slot->used && strcmp(slot->path, path) == 0) return slot;
    }
    return NULL;
}

static runtime_optimize_slot *runtime_optimize_insert_slot_locked(const char *path) {
    int empty = -1;
    int evict = -1;
    sqlite3_int64 oldest_seen = 0;
    int i;

    for (i = 0; i < RUNTIME_OPTIMIZE_REGISTRY_CAP; i++) {
        runtime_optimize_slot *slot = &g_runtime_optimize_slots[i];
        sqlite3_int64 slot_age;
        if (!slot->used) {
            empty = i;
            break;
        }
        if (slot->inflight_tier != RUNTIME_OPTIMIZE_TIER_NONE) continue;
        slot_age = slot->last_full_success_ns ?
                   slot->last_full_success_ns :
                   slot->last_limited_success_ns;
        if (evict < 0 || slot_age < oldest_seen) {
            evict = i;
            oldest_seen = slot_age;
        }
    }

    if (empty >= 0) evict = empty;
    if (evict < 0) return NULL;

    memset(&g_runtime_optimize_slots[evict], 0, sizeof(g_runtime_optimize_slots[evict]));
    g_runtime_optimize_slots[evict].used = 1;
    snprintf(g_runtime_optimize_slots[evict].path,
             sizeof(g_runtime_optimize_slots[evict].path),
             "%s", path);
    return &g_runtime_optimize_slots[evict];
}

static sqlite3_int64 runtime_optimize_slot_due_ns(
    const runtime_optimize_slot *slot,
    sqlite3_int64 limited_cadence_ns,
    sqlite3_int64 full_cadence_ns
) {
    sqlite3_int64 limited_due;
    sqlite3_int64 full_due;
    sqlite3_int64 due;

    if (!slot->used) return LLONG_MAX;
    if (slot->inflight_tier != RUNTIME_OPTIMIZE_TIER_NONE) return LLONG_MAX;

    limited_due = slot->last_limited_success_ns == 0 ?
                  0 :
                  runtime_optimize_safe_add_ns(slot->last_limited_success_ns, limited_cadence_ns);
    full_due = slot->last_full_success_ns == 0 ?
               0 :
               runtime_optimize_safe_add_ns(slot->last_full_success_ns, full_cadence_ns);
    due = limited_due < full_due ? limited_due : full_due;
    if (slot->backoff_until_ns > due) due = slot->backoff_until_ns;
    return due;
}

static sqlite3_int64 runtime_optimize_connection_due_value(
    const runtime_optimize_slot *slot,
    sqlite3_int64 now_ns,
    sqlite3_int64 limited_cadence_ns,
    sqlite3_int64 full_cadence_ns
) {
    sqlite3_int64 due = runtime_optimize_slot_due_ns(
        slot,
        limited_cadence_ns,
        full_cadence_ns
    );

    if (due == LLONG_MAX || due == 0 || due <= now_ns) return 0;
    return due;
}

static int runtime_optimize_tier_due(
    sqlite3_int64 now_ns,
    sqlite3_int64 last_success_ns,
    sqlite3_int64 cadence_ns
) {
    if (last_success_ns == 0) return 1;
    if (now_ns < last_success_ns) return 0;
    return now_ns - last_success_ns >= cadence_ns;
}

static void runtime_optimize_connection_state_free(void *arg) {
    sqlite3_free(arg);
}

static runtime_optimize_connection_state *runtime_optimize_connection_state_get(
    sqlite3 *db,
    int create
) {
    runtime_optimize_connection_state *state;
    int rc;

    state = (runtime_optimize_connection_state*)sqlite3_get_clientdata(
        db,
        RUNTIME_OPTIMIZE_CLIENTDATA_KEY
    );
    if (state || !create) return state;

    state = (runtime_optimize_connection_state*)sqlite3_malloc64(sizeof(*state));
    if (!state) return NULL;
    memset(state, 0, sizeof(*state));
    atomic_init(&state->next_due_ns, 0LL);

    rc = sqlite3_set_clientdata(
        db,
        RUNTIME_OPTIMIZE_CLIENTDATA_KEY,
        state,
        runtime_optimize_connection_state_free
    );
    if (rc != SQLITE_OK) {
        sqlite3_free(state);
        return NULL;
    }
    return state;
}

static void runtime_optimize_connection_state_set(
    runtime_optimize_connection_state *state,
    const char *path,
    sqlite3_int64 next_due_ns
) {
    if (!state) return;
    snprintf(state->path, sizeof(state->path), "%s", path);
    state->has_path = 1;
    atomic_store_explicit(
        &state->next_due_ns,
        (long long)next_due_ns,
        memory_order_relaxed
    );
}

static int runtime_optimize_connection_hot_skip(
    sqlite3 *db,
    runtime_optimize_connection_state **state_out,
    sqlite3_int64 *now_out
) {
    runtime_optimize_connection_state *state;
    sqlite3_int64 now_ns;
    sqlite3_int64 next_due_ns;

    *state_out = NULL;
    *now_out = 0;
    state = runtime_optimize_connection_state_get(db, 0);
    if (!state || !state->has_path) return 0;

    now_ns = runtime_optimize_now_ns();
    next_due_ns = (sqlite3_int64)atomic_load_explicit(
        &state->next_due_ns,
        memory_order_relaxed
    );
    if (next_due_ns != 0 && now_ns < next_due_ns) return 1;

    *state_out = state;
    *now_out = now_ns;
    return 0;
}

__attribute__((visibility("hidden"))) void runtime_optimize_seed_path(sqlite3 *db, const char *raw_fn) {
    char path[RUNTIME_OPTIMIZE_PATH_MAX];
    runtime_optimize_connection_state *state;
    runtime_optimize_slot *slot;
    sqlite3_int64 now_ns;
    sqlite3_int64 next_due_ns = 0;

    if (!runtime_optimize_is_enabled()) return;
    if (!runtime_optimize_copy_path_key(raw_fn, path, sizeof(path))) return;
    state = runtime_optimize_connection_state_get(db, 1);
    now_ns = runtime_optimize_now_ns();
    pthread_mutex_lock(&g_runtime_optimize_mu);
    slot = runtime_optimize_find_slot_locked(path);
    if (!slot) slot = runtime_optimize_insert_slot_locked(path);
    if (slot) {
        next_due_ns = runtime_optimize_connection_due_value(
            slot,
            now_ns,
            runtime_optimize_limited_cadence_ns(),
            runtime_optimize_full_cadence_ns()
        );
    }
    pthread_mutex_unlock(&g_runtime_optimize_mu);
    runtime_optimize_connection_state_set(state, path, next_due_ns);
}

static void runtime_optimize_update_connection_due_value(
    runtime_optimize_connection_state *state,
    const char *path,
    sqlite3_int64 next_due_ns
) {
    runtime_optimize_connection_state_set(state, path, next_due_ns);
}

static void runtime_optimize_rearm_due_blocked(
    sqlite3 *db,
    runtime_optimize_connection_state *state,
    const char *path,
    sqlite3_int64 now_ns,
    const char *reason
) {
    sqlite3_int64 next_due_ns;

    if (!state || !state->has_path) return;
    if (now_ns == 0) now_ns = runtime_optimize_now_ns();
    next_due_ns = (sqlite3_int64)atomic_load_explicit(
        &state->next_due_ns,
        memory_order_relaxed
    );
    if (next_due_ns != 0 && now_ns < next_due_ns) return;

    /* WHY: A blocked due hook should re-enable the cheap hot skip without
     * changing shared per-path success cadence or failure backoff state. */
    atomic_store_explicit(
        &state->next_due_ns,
        (long long)runtime_optimize_safe_add_ns(now_ns, RUNTIME_OPTIMIZE_BLOCKED_REARM_NS),
        memory_order_relaxed
    );
    obs_logf("runtime_optimize",
             "event=optimize_skipped db=%p path=\"%s\" reason=%s",
             (void*)db, path, reason);
}

static runtime_optimize_tier runtime_optimize_reserve(
    const char *path,
    sqlite3_int64 now_ns,
    sqlite3_int64 limited_cadence_ns,
    sqlite3_int64 full_cadence_ns,
    sqlite3_int64 *path_next_due_ns,
    sqlite3_int64 *tier_since_last_ns
) {
    runtime_optimize_slot *slot;
    runtime_optimize_tier tier = RUNTIME_OPTIMIZE_TIER_NONE;
    sqlite3_int64 last_success_ns = 0;

    *path_next_due_ns = 0;
    *tier_since_last_ns = RUNTIME_OPTIMIZE_SINCE_LAST_NEVER_NS;
    pthread_mutex_lock(&g_runtime_optimize_mu);
    slot = runtime_optimize_find_slot_locked(path);
    if (!slot) slot = runtime_optimize_insert_slot_locked(path);
    if (!slot) goto done;

    if (slot->backoff_until_ns > now_ns) goto done;
    if (slot->inflight_tier != RUNTIME_OPTIMIZE_TIER_NONE) goto done;

    if (runtime_optimize_tier_due(now_ns, slot->last_full_success_ns, full_cadence_ns)) {
        tier = RUNTIME_OPTIMIZE_TIER_FULL;
        last_success_ns = slot->last_full_success_ns;
    } else if (runtime_optimize_tier_due(now_ns, slot->last_limited_success_ns, limited_cadence_ns)) {
        tier = RUNTIME_OPTIMIZE_TIER_LIMITED;
        last_success_ns = slot->last_limited_success_ns;
    }
    if (last_success_ns > 0) {
        *tier_since_last_ns = now_ns >= last_success_ns ? now_ns - last_success_ns : 0;
    }
    if (tier != RUNTIME_OPTIMIZE_TIER_NONE) slot->inflight_tier = tier;

done:
    if (slot) {
        *path_next_due_ns = runtime_optimize_connection_due_value(
            slot,
            now_ns,
            limited_cadence_ns,
            full_cadence_ns
        );
    }
    pthread_mutex_unlock(&g_runtime_optimize_mu);
    return tier;
}

static void runtime_optimize_finish(
    const char *path,
    runtime_optimize_tier tier,
    int success,
    sqlite3_int64 finish_ns,
    sqlite3_int64 failure_backoff_ns,
    sqlite3_int64 *path_next_due_ns
) {
    runtime_optimize_slot *slot;

    *path_next_due_ns = 0;
    pthread_mutex_lock(&g_runtime_optimize_mu);
    slot = runtime_optimize_find_slot_locked(path);
    if (slot) {
        if (slot->inflight_tier == tier) {
            slot->inflight_tier = RUNTIME_OPTIMIZE_TIER_NONE;
        }
        if (success) {
            if (tier == RUNTIME_OPTIMIZE_TIER_FULL) {
                slot->last_full_success_ns = finish_ns;
                slot->last_limited_success_ns = finish_ns;
            } else if (tier == RUNTIME_OPTIMIZE_TIER_LIMITED) {
                slot->last_limited_success_ns = finish_ns;
            }
            slot->backoff_until_ns = 0;
        } else {
            slot->backoff_until_ns = runtime_optimize_safe_add_ns(finish_ns, failure_backoff_ns);
        }
        *path_next_due_ns = runtime_optimize_connection_due_value(
            slot,
            finish_ns,
            runtime_optimize_limited_cadence_ns(),
            runtime_optimize_full_cadence_ns()
        );
    }
    pthread_mutex_unlock(&g_runtime_optimize_mu);
}

static int runtime_optimize_no_busy_peer(sqlite3 *db, sqlite3_stmt *self_stmt) {
    sqlite3_stmt *stmt = NULL;
    while ((stmt = sqlite3_next_stmt(db, stmt)) != NULL) {
        if (stmt == self_stmt) continue;
        if (sqlite3_stmt_busy(stmt)) return 0;
    }
    return 1;
}

static int runtime_optimize_read_analysis_limit(sqlite3 *db, int *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA main.analysis_limit;", -1, &stmt, NULL);
    int final_rc;
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(stmt, 0);
        rc = SQLITE_OK;
    }
    final_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && final_rc != SQLITE_OK) rc = final_rc;
    return rc;
}

static int runtime_optimize_exec_runtime_sql(sqlite3 *db, const char *sql, char **err) {
    sqlite3_free(*err);
    *err = NULL;
    return sqlite3_exec(db, sql, NULL, NULL, err);
}

static int runtime_optimize_restore_analysis_limit(sqlite3 *db, int value, char **err) {
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA main.analysis_limit=%d;", value);
    return runtime_optimize_exec_runtime_sql(db, sql, err);
}

static int runtime_optimize_progress_cb(void *arg) {
    runtime_optimize_progress_ctx *ctx = (runtime_optimize_progress_ctx*)arg;
    return runtime_optimize_now_ns() >= ctx->deadline_ns;
}

static sqlite3_int64 runtime_optimize_tier_deadline_ns(runtime_optimize_tier tier) {
    return tier == RUNTIME_OPTIMIZE_TIER_FULL ?
           RUNTIME_OPTIMIZE_FULL_DEADLINE_NS :
           RUNTIME_OPTIMIZE_LIMITED_DEADLINE_NS;
}

static const char *runtime_optimize_tier_sql(runtime_optimize_tier tier) {
    return tier == RUNTIME_OPTIMIZE_TIER_FULL ?
           "PRAGMA main.optimize=0x10002;" :
           "PRAGMA main.optimize;";
}

static const char *runtime_optimize_tier_name(runtime_optimize_tier tier) {
    return tier == RUNTIME_OPTIMIZE_TIER_FULL ? "full" : "limited";
}

static int runtime_optimize_run(
    sqlite3 *db,
    const char *path,
    runtime_optimize_tier tier,
    runtime_optimize_trigger trigger,
    sqlite3_int64 since_last_ns
) {
    int success = 0;
    int rc = SQLITE_OK;
    int saved_limit = 0;
    int analysis_limit_changed = 0;
    int error_pushed = 0;
    int progress_pushed = 0;
    char *err = NULL;
    sqlite3_int64 changes_before = 0;
    sqlite3_int64 changes_after = 0;
    sqlite3_int64 stat_rows = 0;
    sqlite3_int64 optimize_start_ns = 0;
    sqlite3_int64 optimize_end_ns = 0;
    long long since_last_ms = runtime_optimize_since_last_ms(since_last_ns);
    auto_extension_error_snapshot error_snapshot;
    auto_extension_progress_snapshot progress_snapshot;
    runtime_optimize_progress_ctx progress_ctx;

    (void)trigger;
    memset(&error_snapshot, 0, sizeof(error_snapshot));
    memset(&progress_snapshot, 0, sizeof(progress_snapshot));
    g_runtime_optimize_depth++;
    rc = auto_extension_error_state_push(db, &error_snapshot);
    if (rc != SQLITE_OK) {
        sqlite3_log(rc,
                    "auto_extension: runtime optimize error-state snapshot failed on %s: rc=%d",
                    path, rc);
        obs_logf("runtime_optimize",
                 "event=error_state_snapshot_failed db=%p path=\"%s\" tier=%s rc=%d",
                 (void*)db, path, runtime_optimize_tier_name(tier), rc);
        goto cleanup;
    }
    error_pushed = 1;

    progress_ctx.deadline_ns = runtime_optimize_safe_add_ns(
        runtime_optimize_now_ns(),
        runtime_optimize_tier_deadline_ns(tier)
    );
    auto_extension_progress_handler_push(
        db,
        RUNTIME_OPTIMIZE_PROGRESS_OPS,
        runtime_optimize_progress_cb,
        &progress_ctx,
        &progress_snapshot
    );
    progress_pushed = 1;

    rc = runtime_optimize_read_analysis_limit(db, &saved_limit);
    if (rc != SQLITE_OK) {
        sqlite3_log(rc,
                    "auto_extension: runtime optimize read analysis_limit failed on %s: rc=%d",
                    path, rc);
        obs_logf("runtime_optimize",
                 "event=analysis_limit_read_failed db=%p path=\"%s\" tier=%s rc=%d",
                 (void*)db, path, runtime_optimize_tier_name(tier), rc);
        goto cleanup;
    }

    rc = runtime_optimize_exec_runtime_sql(
        db,
        tier == RUNTIME_OPTIMIZE_TIER_FULL ?
            "PRAGMA main.analysis_limit=0;" :
            "PRAGMA main.analysis_limit=1024;",
        &err
    );
    if (rc != SQLITE_OK) {
        sqlite3_log(rc,
                    "auto_extension: runtime optimize set analysis_limit failed on %s: %s",
                    path, err ? err : "(no message)");
        obs_logf("runtime_optimize",
                 "event=analysis_limit_set_failed db=%p path=\"%s\" tier=%s rc=%d err=\"%s\"",
                 (void*)db, path, runtime_optimize_tier_name(tier), rc, err ? err : "");
        goto cleanup;
    }
    analysis_limit_changed = 1;

    sqlite3_free(err);
    err = NULL;
    changes_before = sqlite3_total_changes64(db);
    optimize_start_ns = runtime_optimize_now_ns();
    obs_logf("runtime_optimize",
             "event=optimize_start db=%p path=\"%s\" tier=%s elapsed_ms=%.3f stat_rows=%lld since_last_ms=%lld",
             (void*)db, path, runtime_optimize_tier_name(tier), 0.0, 0LL, since_last_ms);
    rc = sqlite3_exec(db, runtime_optimize_tier_sql(tier), NULL, NULL, &err);
    optimize_end_ns = runtime_optimize_now_ns();
    changes_after = sqlite3_total_changes64(db);
    if (changes_after >= changes_before) stat_rows = changes_after - changes_before;
    if (rc != SQLITE_OK) {
        sqlite3_log(rc, "auto_extension: runtime optimize %s failed on %s: %s",
                    runtime_optimize_tier_name(tier), path, err ? err : "(no message)");
        obs_logf("runtime_optimize",
                 "event=optimize_failed db=%p path=\"%s\" tier=%s elapsed_ms=%.3f stat_rows=%lld since_last_ms=%lld rc=%d err=\"%s\"",
                 (void*)db, path, runtime_optimize_tier_name(tier),
                 runtime_optimize_ns_to_ms(optimize_end_ns - optimize_start_ns),
                 (long long)stat_rows, since_last_ms, rc, err ? err : "");
        goto cleanup;
    }
    obs_logf("runtime_optimize",
             "event=optimize_done db=%p path=\"%s\" tier=%s elapsed_ms=%.3f stat_rows=%lld since_last_ms=%lld",
             (void*)db, path, runtime_optimize_tier_name(tier),
             runtime_optimize_ns_to_ms(optimize_end_ns - optimize_start_ns),
             (long long)stat_rows, since_last_ms);
    success = 1;

cleanup:
    if (analysis_limit_changed) {
        int restore_rc;
        sqlite3_free(err);
        err = NULL;
        restore_rc = runtime_optimize_restore_analysis_limit(db, saved_limit, &err);
        if (restore_rc != SQLITE_OK) {
            int residual_limit = -1;
            int residual_rc;
            success = 0;
            residual_rc = runtime_optimize_read_analysis_limit(db, &residual_limit);
            sqlite3_log(restore_rc,
                        "auto_extension: runtime optimize restore analysis_limit failed on %s: %s; residual analysis_limit=%s%d read_rc=%d",
                        path, err ? err : "(no message)",
                        residual_rc == SQLITE_OK ? "" : "unknown:",
                        residual_rc == SQLITE_OK ? residual_limit : -1,
                        residual_rc);
            obs_logf("runtime_optimize",
                     "event=analysis_limit_restore_failed db=%p path=\"%s\" tier=%s rc=%d err=\"%s\" residual_limit=%d residual_rc=%d",
                     (void*)db, path, runtime_optimize_tier_name(tier), restore_rc, err ? err : "",
                     residual_rc == SQLITE_OK ? residual_limit : -1, residual_rc);
        }
    }
    if (progress_pushed) {
        auto_extension_progress_handler_pop(db, &progress_snapshot);
    }
    if (error_pushed) {
        auto_extension_error_state_pop(db, &error_snapshot);
    }
    sqlite3_free(err);
    g_runtime_optimize_depth--;
    return success;
}

static void runtime_optimize_maybe_run(
    sqlite3 *db,
    sqlite3_stmt *self_stmt,
    runtime_optimize_trigger trigger
) {
    const char *raw_fn;
    char path[RUNTIME_OPTIMIZE_PATH_MAX];
    runtime_optimize_connection_state *conn_state = NULL;
    sqlite3_int64 now_ns = 0;
    sqlite3_int64 next_due_ns;
    sqlite3_int64 path_next_due_ns = 0;
    sqlite3_int64 limited_cadence_ns;
    sqlite3_int64 full_cadence_ns;
    sqlite3_int64 failure_backoff_ns;
    sqlite3_int64 tier_since_last_ns = RUNTIME_OPTIMIZE_SINCE_LAST_NEVER_NS;
    runtime_optimize_tier tier;
    int success;
    int rc;

    if (!db) return;
    if (g_autopragma_depth != 0) return;
    if (trigger == RUNTIME_OPTIMIZE_TRIGGER_INLINE &&
        runtime_optimize_connection_hot_skip(db, &conn_state, &now_ns)) {
        return;
    }
    if (env_is_literal_1("SQLITE3_DISABLE_AUTOPRAGMA")) return;
    if (!runtime_optimize_is_enabled()) return;
    if (g_runtime_optimize_depth != 0) return;

    raw_fn = sqlite3_db_filename(db, "main");
    if (!auto_extension_path_is_target(raw_fn)) return;
    if (!runtime_optimize_copy_path_key(raw_fn, path, sizeof(path))) return;
    if (sqlite3_db_readonly(db, "main") == 1) {
        runtime_optimize_rearm_due_blocked(db, conn_state, path, now_ns, "readonly");
        return;
    }
    if (sqlite3_get_autocommit(db) != 1) {
        runtime_optimize_rearm_due_blocked(db, conn_state, path, now_ns, "open_txn");
        return;
    }
    if (trigger == RUNTIME_OPTIMIZE_TRIGGER_CLOSE) {
        if (sqlite3_next_stmt(db, NULL) != NULL) return;
    } else if (!runtime_optimize_no_busy_peer(db, self_stmt)) {
        runtime_optimize_rearm_due_blocked(db, conn_state, path, now_ns, "busy_peer");
        return;
    }
    if (!conn_state) conn_state = runtime_optimize_connection_state_get(db, 1);

    if (now_ns == 0) now_ns = runtime_optimize_now_ns();
    next_due_ns = conn_state ? (sqlite3_int64)atomic_load_explicit(
        &conn_state->next_due_ns,
        memory_order_relaxed
    ) : 0;
    if (next_due_ns != 0 && now_ns < next_due_ns) return;

    limited_cadence_ns = runtime_optimize_limited_cadence_ns();
    full_cadence_ns = runtime_optimize_full_cadence_ns();
    failure_backoff_ns = runtime_optimize_failure_backoff_ns(limited_cadence_ns);
    tier = runtime_optimize_reserve(
        path,
        now_ns,
        limited_cadence_ns,
        full_cadence_ns,
        &path_next_due_ns,
        &tier_since_last_ns
    );
    runtime_optimize_update_connection_due_value(conn_state, path, path_next_due_ns);
    if (tier == RUNTIME_OPTIMIZE_TIER_NONE) return;

    if (trigger == RUNTIME_OPTIMIZE_TRIGGER_CLOSE) {
        rc = sqlite3_busy_timeout(db, 1);
        if (rc != SQLITE_OK) {
            sqlite3_log(rc,
                        "auto_extension: runtime optimize busy_timeout failed on %s: rc=%d",
                        path, rc);
            obs_logf("runtime_optimize",
                     "event=busy_timeout_failed db=%p path=\"%s\" tier=%s rc=%d",
                     (void*)db, path, runtime_optimize_tier_name(tier), rc);
            runtime_optimize_finish(
                path,
                tier,
                0,
                runtime_optimize_now_ns(),
                failure_backoff_ns,
                &path_next_due_ns
            );
            runtime_optimize_update_connection_due_value(conn_state, path, path_next_due_ns);
            return;
        }
    }

    success = runtime_optimize_run(db, path, tier, trigger, tier_since_last_ns);
    runtime_optimize_finish(
        path,
        tier,
        success,
        runtime_optimize_now_ns(),
        failure_backoff_ns,
        &path_next_due_ns
    );
    runtime_optimize_update_connection_due_value(conn_state, path, path_next_due_ns);
}

__attribute__((visibility("hidden"))) SQLITE_API void auto_extension_optimize_before_close(sqlite3 *db) {
    runtime_optimize_maybe_run(db, NULL, RUNTIME_OPTIMIZE_TRIGGER_CLOSE);
}

__attribute__((visibility("hidden"))) SQLITE_API void
auto_extension_optimize_after_stmt(sqlite3 *db, sqlite3_stmt *self_stmt) {
    runtime_optimize_maybe_run(db, self_stmt, RUNTIME_OPTIMIZE_TRIGGER_INLINE);
}
