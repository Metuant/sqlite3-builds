#include "sqlite3.h"

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

#include <stdatomic.h>

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...);

/* WHY: Constructor stores cfg_rc here so tests can assert that the runtime
 * SORTERREF_SIZE config call landed before sqlite3_initialize(). The variable
 * is written exactly once during single-threaded library load and read later;
 * int writes are atomic on all supported targets, so no locking is needed. */
static int g_sorterref_cfg_rc = SQLITE_OK;

int auto_extension_sorterref_cfg_rc(void) {
    return g_sorterref_cfg_rc;
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

    /* Targets are the current media-server main database filenames.
     *
     * WHY: Suffixes match deployed absolute paths without baking container
     * root directories into the library; new targets require explicit entries.
     */
    int is_target = 0;
    if (ends_with(fn, "com.plexapp.plugins.library.db")) is_target = 1;
    else if (ends_with(fn, "/library.db")) is_target = 1;
    else if (ends_with(fn, "/jellyfin.db")) is_target = 1;
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
        int trace_rc = sqlite3_trace_v2(db, SQLITE_TRACE_STMT, obs_trace_stmt_cb, NULL);
        if (trace_rc != SQLITE_OK) {
            obs_logf("sqlite3_trace_v2",
                     "event=registration_failed db=%p rc=%d",
                     (void*)db, trace_rc);
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
     * PMS issues its own per-connect PRAGMAs after sqlite3_open_v2 returns,
     * so these settings are best-effort for Plex and sticky for Emby/JF
     * connections that do not run an app-side PRAGMA pass.
     *
     * PRAGMA synchronous: deliberately NOT set. Compile-time
     * DEFAULT_WAL_SYNCHRONOUS=1 already provides synchronous=NORMAL for WAL
     * DBs (which all three media-server apps use). Setting it here would
     * impose NORMAL on rollback-journal DBs too, a durability change the
     * apps did not opt into. PMS itself sets it on every connect anyway.
     *
     * PRAGMA optimize: deliberately NOT set. For the Plex variant, PMS
     * registers the icu_root collation and the `collating` FTS tokenizer
     * *after* sqlite3_open returns, so this hook runs too early to analyze
     * indexes/FTS tables that depend on them -- a guaranteed open-time
     * failure path. The maintenance script (scripts/optimize_media_servers.sh)
     * runs `PRAGMA analysis_limit=0; PRAGMA optimize;` periodically; that is
     * the correct lifecycle hook for ANALYZE work. */
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "PRAGMA cache_size=-1048576;"
        "PRAGMA mmap_size=8589934592;"
        "PRAGMA temp_store=2;"
        "PRAGMA threads=8;"
        "PRAGMA wal_autocheckpoint=16000;"
        "PRAGMA journal_size_limit=67108864;"
        "PRAGMA busy_timeout=10000;"
        "PRAGMA analysis_limit=1024;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_log(rc, "auto_extension: pragma block failed on %s: %s",
                    raw_fn, err ? err : "(no message)");
        sqlite3_free(err);
    }

    /* WHY: sqlite3_auto_extension propagates non-OK returns into sqlite3_open*;
     * best-effort tuning must never block application database opens. */
    return SQLITE_OK;
}

/* Library constructor: registers the auto-extension at dlopen time.
 *
 * Order matters. sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE) MUST run before
 * sqlite3_auto_extension(), which triggers sqlite3_initialize() internally.
 * After init, sqlite3_config() returns SQLITE_MISUSE for every verb except
 * SQLITE_CONFIG_LOG and SQLITE_CONFIG_PCACHE_HDRSZ.
 *
 * SORTERREF_SIZE is what actually activates the SQLITE_ENABLE_SORTER_REFERENCES
 * compile flag. Without this runtime config, the feature stays disabled
 * (threshold = INT_MAX, effectively never trips) even though it is compiled in.
 *
 * Embedding hosts that need other sqlite3_config verbs must call them before
 * this library is dlopen'd. The target media-server surfaces are not known
 * to make late sqlite3_config calls; this has not been verified by
 * disassembly. Compile-time defaults (SQLITE_THREADSAFE=2,
 * SQLITE_DEFAULT_LOOKASIDE, SQLITE_DEFAULT_MEMSTATUS) already lock in the
 * most common pre-init knobs; under THREADSAFE=2 (Multi-thread), connections
 * are not per-handle-mutexed by default -- callers must externally serialize
 * a single connection or pass SQLITE_OPEN_FULLMUTEX. Any unaudited late call
 * to sqlite3_config would no-op with SQLITE_MISUSE rather than degrade
 * observed behavior. */
__attribute__((constructor(102)))
static void register_autopragma(void) {
    /* WHY: 16384 (16 KB) replaces inline content with rowid references only
     * for rows above 16 KB. Plex/Emby metadata rows are well under that and
     * skip the feature; large FTS shadow rows get the memory benefit. */
    int cfg_rc = sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 16384);
    g_sorterref_cfg_rc = cfg_rc;
    if (cfg_rc != SQLITE_OK) {
        /* WHY: Config failure leaves sorter-references inert; library still
         * registers the auto-extension and applies the rest of its tuning. */
        sqlite3_log(cfg_rc,
            "auto_extension: SORTERREF_SIZE config failed: rc=%d", cfg_rc);
    }

    int rc = sqlite3_auto_extension((void(*)(void))autopragma_init);
    if (rc != SQLITE_OK) {
        /* WHY: Registration failure disables optional tuning only; application
         * startup should continue with SQLite's normal behavior. */
        sqlite3_log(rc, "auto_extension: registration failed: rc=%d", rc);
    }
}
