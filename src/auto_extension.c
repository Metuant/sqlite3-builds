#include "auto_extension_internal.h"

#include <stdlib.h>
#include <string.h>

/* WHY: SQLITE_OMIT_SHARED_CACHE strips sqlite3_enable_shared_cache from the
 * library, but PMS's libpython27.so links against this symbol at load time.
 * The stub satisfies the link without touching
 * sqlite3GlobalConfig.sharedCacheEnabled, so OMIT's compile-time removal of
 * the SQLITE_OPEN_SHAREDCACHE consumer keeps all connections private
 * regardless of caller request (process-level call, open flag, or URI param). */
SQLITE_API int sqlite3_enable_shared_cache(int enable) {
    (void)enable;
    return SQLITE_OK;
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_stmt_trace_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_cb(unsigned trace, void *ctx, void *p, void *x);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);
__attribute__((visibility("hidden"))) SQLITE_API int slow_query_disabled(void);

/* WHY: Constructor stores cfg_rc here so tests can assert that the runtime
 * SORTERREF_SIZE config call landed before sqlite3_initialize(). The variable
 * is written exactly once during single-threaded library load and read later;
 * int writes are atomic on all supported targets, so no locking is needed. */
static int g_sorterref_cfg_rc = SQLITE_OK;
static int g_pmasz_cfg_rc = SQLITE_OK;
_Thread_local int g_autopragma_depth;

int auto_extension_sorterref_cfg_rc(void) {
    return g_sorterref_cfg_rc;
}

int auto_extension_pmasz_cfg_rc(void) {
    return g_pmasz_cfg_rc;
}

static int sqlite_log_should_route(int err_code, const char *msg) {
    int primary = err_code & 0xff;
    if (primary == SQLITE_NOTICE || primary == SQLITE_WARNING) return 1;
    if (!msg) return 0;
    return strstr(msg, "PANIC") != NULL || strstr(msg, "panic") != NULL;
}

static void sqlite_log_to_observability(void *ctx, int err_code, const char *msg) {
    (void)ctx;
    if (!sqlite_log_should_route(err_code, msg)) return;
    obs_logf("sqlite3_log", "rc=%d msg=\"%s\"", err_code, msg ? msg : "");
}

static int ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t ls = strlen(str), lf = strlen(suffix);
    if (lf > ls) return 0;
    return strcmp(str + ls - lf, suffix) == 0;
}

/* Returns 1 when raw_fn matches a known media-server database suffix after
 * defensively stripping any URI query suffix; returns 0 for NULL, empty,
 * overlong, or non-target paths.
 *
 * WHY: Extracted from autopragma_init so the 2 KiB truncation guard is
 * directly testable. That guard is not reachable through sqlite3_open()
 * because os_unix.c rejects longer paths at MAX_PATHNAME=512 before this
 * callback fires. It stays as defense-in-depth for custom VFSes or future
 * SQLite versions that may return longer paths from sqlite3_db_filename().
 *
 * sqlite3_db_filename is documented to strip URI components on the standard
 * VFSes; the query strip remains defensive for non-default VFSes. */
int auto_extension_path_is_target(const char *raw_fn) {
    if (!raw_fn || raw_fn[0] == 0) return 0;

    size_t raw_len = strlen(raw_fn);
    char fnbuf[2048];
    if (raw_len >= sizeof(fnbuf)) {
        /* WHY: Truncated paths can accidentally end with a target suffix; fail
         * closed as a filter miss instead. */
        return 0;
    }
    memcpy(fnbuf, raw_fn, raw_len + 1);
    char *q = strchr(fnbuf, '?');
    if (q) *q = 0;
    const char *fn = fnbuf;

    /* Filters are supported target filenames plus the legacy, unsupported
     * JF-style main database filename.
     *
     * WHY: Suffixes match deployed absolute paths without baking container
     * root directories into the library; new filename matches require explicit
     * entries, and supported JF deployment requires a binding design plus
     * validation plan.
     */
    int is_target = 0;
    if (ends_with(fn, "com.plexapp.plugins.library.db")) is_target = 1;
    else if (ends_with(fn, "library.db")) is_target = 1;
    else if (ends_with(fn, "jellyfin.db")) is_target = 1;
    return is_target;
}

static int autopragma_init(
    sqlite3 *db, char **pzErr, const sqlite3_api_routines *pApi
) {
    /* Compiled into the library (-DSQLITE_CORE); pApi is NULL when invoked by
     * sqlite3_auto_extension on a statically-compiled-in extension and is
     * unused. pzErr is the SQL error message output pointer; we log via
     * sqlite3_log instead. */
    (void)pApi;
    (void)pzErr;

    if (!obs_is_disabled()) {
        unsigned trace_mask = 0;
        if (!obs_stmt_trace_disabled()) trace_mask |= SQLITE_TRACE_STMT;
        if (!slow_query_disabled()) trace_mask |= SQLITE_TRACE_PROFILE;
        if (trace_mask != 0) {
            int trace_rc = sqlite3_trace_v2(db, trace_mask, obs_trace_cb, NULL);
            if (trace_rc != SQLITE_OK) {
                obs_logf("sqlite3_trace_v2",
                         "event=registration_failed db=%p rc=%d",
                         (void*)db, trace_rc);
            }
        }
    }

    /* WHY: Only literal "1" disables tuning so empty or diagnostic values do
     * not silently turn it off. */
    const char *disable = getenv("SQLITE3_DISABLE_AUTOPRAGMA");
    if (disable && strcmp(disable, "1") == 0) return SQLITE_OK;

    /* Filter: apply only to known media-server DB names.
     *
     * sqlite3_db_filename returns an absolute path; the standard SQLite VFSes
     * (`unix`, `unix-excl`, `win32`) strip URI scheme + query strings before
     * returning. The `?`-strip below is defensive for non-default VFSes that
     * might leave a query suffix on the returned path.
     *
     * Empty, in-memory, temporary, and attached-only handles are skipped. */
    const char *raw_fn = sqlite3_db_filename(db, "main");
    if (!auto_extension_path_is_target(raw_fn)) return SQLITE_OK;

    /* WHY: Read-only opens cannot accept mutating PRAGMAs and should stay quiet. */
    if (sqlite3_db_readonly(db, "main") == 1) return SQLITE_OK;

    runtime_optimize_seed_path(db, raw_fn);

    /* Per-connection performance tuning. PRAGMAs that duplicate compile-time
     * defaults provide defense-in-depth (compile defaults apply unless
     * something interferes -- different library loaded, env tampering, etc.).
     * busy_timeout is the only PRAGMA without a compile-time analog; it adds
     * net new behavior.
     *
     * PRAGMA temp_store=2 is retained per user policy on compile-default-
     * mirroring PRAGMAs, but the current compile SQLITE_TEMP_STORE=3 profile
     * is strictly stronger: compile=3 means always memory and ignores the
     * PRAGMA, while PRAGMA=2 means prefer memory but allow file. Under the
     * current profile this is a no-op that documents intent only; it does not
     * restore the always-memory guarantee if the compile profile relaxes.
     *
     * PMS issues its own per-connect PRAGMAs after 2-arg sqlite3_open returns
     * with implicit SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, so these settings
     * are best-effort for Plex and sticky for Emby connections that do not run
     * an app-side PRAGMA pass. The legacy JF-style filename match gets the same
     * library behavior when reached, but JF deployment is unsupported until a
     * binding design and validation plan land.
     *
     * PRAGMA synchronous: deliberately NOT set. Compile-time
     * DEFAULT_WAL_SYNCHRONOUS=1 already provides synchronous=NORMAL for WAL
     * databases. Setting it here would impose NORMAL on rollback-journal DBs
     * too, a durability change callers did not opt into. PMS itself sets it
     * on every connect anyway.
     *
     * PRAGMA optimize: deliberately NOT set. For the Plex variant, PMS
     * registers the icu_root collation and the `collating` FTS tokenizer
     * *after* sqlite3_open returns, so this hook runs too early to analyze
     * indexes/FTS tables that depend on them -- a guaranteed open-time
     * failure path. Runtime optimize is off unless
     * SQLITE3_DISABLE_RUNTIME_OPTIMIZE is explicitly set to "0"; when enabled,
     * it runs only on later safe-idle hooks. Planned downtime maintenance
     * remains the explicit full ANALYZE lifecycle. */
    char *err = NULL;
    int rc;
    g_autopragma_depth++;
    rc = sqlite3_exec(db,
        "PRAGMA cache_size=-1048576;"
        "PRAGMA mmap_size=34359738368;"
        "PRAGMA temp_store=2;"
        "PRAGMA threads=8;"
        "PRAGMA wal_autocheckpoint=16000;"
        "PRAGMA journal_size_limit=67108864;"
        "PRAGMA busy_timeout=10000;"
        "PRAGMA analysis_limit=1024;",
        NULL, NULL, &err);
    g_autopragma_depth--;
    if (rc != SQLITE_OK) {
        sqlite3_log(rc, "auto_extension: pragma block failed on %s: %s",
                    raw_fn, err ? err : "(no message)");
        sqlite3_free(err);
    }

    /* WHY: sqlite3_auto_extension propagates non-OK returns into sqlite3_open*;
     * best-effort tuning must never block application database opens. */
    return SQLITE_OK;
}

__attribute__((visibility("hidden"))) SQLITE_API void auto_extension_register_for_open(void) {
    int rc = sqlite3_auto_extension((void(*)(void))autopragma_init);
    if (rc != SQLITE_OK) {
        sqlite3_log(rc, "auto_extension: registration failed: rc=%d", rc);
        obs_logf("auto_extension_register_for_open", "event=registration_failed rc=%d", rc);
    }
}

/* Library constructor: configures pre-init SQLite process-wide hooks.
 *
 * Registration is lazy from the public sqlite3_open, sqlite3_open_v2, and
 * sqlite3_open16 wrappers. sqlite3_auto_extension() is not called at dlopen
 * because it performs effective SQLite initialization internally. Embedders may
 * call startup-only sqlite3_config() verbs between dlopen and first public open.
 *
 * CONFIG_LOG routes selected process-wide SQLite diagnostics into the existing
 * observability sink. Stock SQLite defaults SORTERREF_SIZE to INT_MAX, leaving
 * sorter references effectively disabled. build/Build.sh sets
 * -DSQLITE_DEFAULT_SORTERREF_SIZE=512, and this priority-102 constructor
 * re-asserts sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 512) for
 * visibility and drift protection, matching the PMASZ compile-default +
 * runtime-config pair below. CONFIG_LOG and SORTERREF_SIZE stay in this
 * priority-102 constructor so the config lands before any helper-triggered
 * sqlite3_initialize(). */
__attribute__((constructor(102)))
static void register_autopragma(void) {
    int log_rc = sqlite3_config(SQLITE_CONFIG_LOG, sqlite_log_to_observability, NULL);
    if (log_rc != SQLITE_OK) {
        obs_logf("sqlite3_config", "op=SQLITE_CONFIG_LOG rc=%d", log_rc);
    }

    /* WHY: 512 B stores the sort key plus rowid references for sorter records
     * whose estimated size exceeds 512 B, deferring the remaining columns to a
     * post-sort rowid fetch. At this project tuning value, reference-mode engages
     * for most metadata sorter records. */
    int cfg_rc = sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 512);
    g_sorterref_cfg_rc = cfg_rc;
    if (cfg_rc != SQLITE_OK) {
        /* WHY: Config failure leaves sorter-references inert; lazy
         * auto-extension registration still applies the rest of its tuning. */
        sqlite3_log(cfg_rc,
            "auto_extension: SORTERREF_SIZE config failed: rc=%d", cfg_rc);
    }

    /* WHY: 8192 is the Minimum PMA Size in PAGES (SQLITE_CONFIG_PMASZ /
     * SQLITE_SORTER_PMASZ). Per the SQLite docs the multithreaded sorter
     * starts writing+merging PMAs once the content to sort exceeds
     * page_size * min(PRAGMA cache_size, PMASZ). With the 16 KiB page size
     * and the 1 GiB cache_size set above (>> 8192 pages), that threshold is
     * 8192 * 16 KiB = 128 MiB, so media-server library / search sorts up to
     * ~128 MiB stay in a single in-memory PMA before partitioning to k-way
     * merge. Multi-threaded sorter workers each hold their own PMA -- up to
     * PRAGMA threads=8 concurrent per connection. Compile-time default is
     * also -DSQLITE_SORTER_PMASZ=8192 in build/Build.sh; this runtime config
     * re-asserts the value at priority-102 for visibility and as protection
     * against accidental compile-time drift. */
    int pmasz_rc = sqlite3_config(SQLITE_CONFIG_PMASZ, 8192);
    g_pmasz_cfg_rc = pmasz_rc;
    if (pmasz_rc != SQLITE_OK) {
        /* WHY: Config failure leaves szPma at whatever the compile-time
         * default is (currently 8192, matching the intended runtime value).
         * Logging surfaces the divergence if the compile-time pin ever
         * drifts from the runtime intent. */
        sqlite3_log(pmasz_rc,
            "auto_extension: PMASZ config failed: rc=%d", pmasz_rc);
    }
}
