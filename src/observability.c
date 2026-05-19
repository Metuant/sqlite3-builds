#include "sqlite3.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define OBS_SQL_CAP 3072
#define OBS_TRUNC_TAIL "...[TRUNC]"

typedef void (*obs_log_func)(void*, int, const char*);
typedef void (*obs_sqllog_func)(void*, sqlite3*, const char*, int);

extern int sqlite3_initialize_real(void);
extern int sqlite3_config_real(int op, ...);
extern int sqlite3_db_config_real(sqlite3 *db, int op, ...);
extern int sqlite3_open_real(const char *filename, sqlite3 **ppDb);
extern int sqlite3_open_v2_real(
    const char *filename, sqlite3 **ppDb, int flags, const char *zVfs
);
extern int sqlite3_open16_real(const void *filename, sqlite3 **ppDb);

static pthread_once_t g_obs_once = PTHREAD_ONCE_INIT;
static atomic_int g_obs_disabled;
static __thread int s_init_depth = 0;

struct obs_name {
    int value;
    const char *name;
};

static const struct obs_name g_config_names[] = {
    { SQLITE_CONFIG_SINGLETHREAD, "SQLITE_CONFIG_SINGLETHREAD" },
    { SQLITE_CONFIG_MULTITHREAD, "SQLITE_CONFIG_MULTITHREAD" },
    { SQLITE_CONFIG_SERIALIZED, "SQLITE_CONFIG_SERIALIZED" },
    { SQLITE_CONFIG_MALLOC, "SQLITE_CONFIG_MALLOC" },
    { SQLITE_CONFIG_GETMALLOC, "SQLITE_CONFIG_GETMALLOC" },
    { SQLITE_CONFIG_SCRATCH, "SQLITE_CONFIG_SCRATCH" },
    { SQLITE_CONFIG_PAGECACHE, "SQLITE_CONFIG_PAGECACHE" },
    { SQLITE_CONFIG_HEAP, "SQLITE_CONFIG_HEAP" },
    { SQLITE_CONFIG_MEMSTATUS, "SQLITE_CONFIG_MEMSTATUS" },
    { SQLITE_CONFIG_MUTEX, "SQLITE_CONFIG_MUTEX" },
    { SQLITE_CONFIG_GETMUTEX, "SQLITE_CONFIG_GETMUTEX" },
    { SQLITE_CONFIG_LOOKASIDE, "SQLITE_CONFIG_LOOKASIDE" },
    { SQLITE_CONFIG_PCACHE, "SQLITE_CONFIG_PCACHE" },
    { SQLITE_CONFIG_GETPCACHE, "SQLITE_CONFIG_GETPCACHE" },
    { SQLITE_CONFIG_LOG, "SQLITE_CONFIG_LOG" },
    { SQLITE_CONFIG_URI, "SQLITE_CONFIG_URI" },
    { SQLITE_CONFIG_PCACHE2, "SQLITE_CONFIG_PCACHE2" },
    { SQLITE_CONFIG_GETPCACHE2, "SQLITE_CONFIG_GETPCACHE2" },
    { SQLITE_CONFIG_COVERING_INDEX_SCAN, "SQLITE_CONFIG_COVERING_INDEX_SCAN" },
    { SQLITE_CONFIG_SQLLOG, "SQLITE_CONFIG_SQLLOG" },
    { SQLITE_CONFIG_MMAP_SIZE, "SQLITE_CONFIG_MMAP_SIZE" },
    { SQLITE_CONFIG_WIN32_HEAPSIZE, "SQLITE_CONFIG_WIN32_HEAPSIZE" },
    { SQLITE_CONFIG_PCACHE_HDRSZ, "SQLITE_CONFIG_PCACHE_HDRSZ" },
    { SQLITE_CONFIG_PMASZ, "SQLITE_CONFIG_PMASZ" },
    { SQLITE_CONFIG_STMTJRNL_SPILL, "SQLITE_CONFIG_STMTJRNL_SPILL" },
    { SQLITE_CONFIG_SMALL_MALLOC, "SQLITE_CONFIG_SMALL_MALLOC" },
    { SQLITE_CONFIG_SORTERREF_SIZE, "SQLITE_CONFIG_SORTERREF_SIZE" },
    { SQLITE_CONFIG_MEMDB_MAXSIZE, "SQLITE_CONFIG_MEMDB_MAXSIZE" },
    { SQLITE_CONFIG_ROWID_IN_VIEW, "SQLITE_CONFIG_ROWID_IN_VIEW" },
};

static const struct obs_name g_dbconfig_names[] = {
    { SQLITE_DBCONFIG_MAINDBNAME, "SQLITE_DBCONFIG_MAINDBNAME" },
    { SQLITE_DBCONFIG_LOOKASIDE, "SQLITE_DBCONFIG_LOOKASIDE" },
    { SQLITE_DBCONFIG_ENABLE_FKEY, "SQLITE_DBCONFIG_ENABLE_FKEY" },
    { SQLITE_DBCONFIG_ENABLE_TRIGGER, "SQLITE_DBCONFIG_ENABLE_TRIGGER" },
    { SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER, "SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER" },
    { SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, "SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION" },
    { SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, "SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE" },
    { SQLITE_DBCONFIG_ENABLE_QPSG, "SQLITE_DBCONFIG_ENABLE_QPSG" },
    { SQLITE_DBCONFIG_TRIGGER_EQP, "SQLITE_DBCONFIG_TRIGGER_EQP" },
    { SQLITE_DBCONFIG_RESET_DATABASE, "SQLITE_DBCONFIG_RESET_DATABASE" },
    { SQLITE_DBCONFIG_DEFENSIVE, "SQLITE_DBCONFIG_DEFENSIVE" },
    { SQLITE_DBCONFIG_WRITABLE_SCHEMA, "SQLITE_DBCONFIG_WRITABLE_SCHEMA" },
    { SQLITE_DBCONFIG_LEGACY_ALTER_TABLE, "SQLITE_DBCONFIG_LEGACY_ALTER_TABLE" },
    { SQLITE_DBCONFIG_DQS_DML, "SQLITE_DBCONFIG_DQS_DML" },
    { SQLITE_DBCONFIG_DQS_DDL, "SQLITE_DBCONFIG_DQS_DDL" },
    { SQLITE_DBCONFIG_ENABLE_VIEW, "SQLITE_DBCONFIG_ENABLE_VIEW" },
    { SQLITE_DBCONFIG_LEGACY_FILE_FORMAT, "SQLITE_DBCONFIG_LEGACY_FILE_FORMAT" },
    { SQLITE_DBCONFIG_TRUSTED_SCHEMA, "SQLITE_DBCONFIG_TRUSTED_SCHEMA" },
    { SQLITE_DBCONFIG_STMT_SCANSTATUS, "SQLITE_DBCONFIG_STMT_SCANSTATUS" },
    { SQLITE_DBCONFIG_REVERSE_SCANORDER, "SQLITE_DBCONFIG_REVERSE_SCANORDER" },
    { SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE, "SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE" },
    { SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE, "SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE" },
    { SQLITE_DBCONFIG_ENABLE_COMMENTS, "SQLITE_DBCONFIG_ENABLE_COMMENTS" },
    { SQLITE_DBCONFIG_FP_DIGITS, "SQLITE_DBCONFIG_FP_DIGITS" },
};

static const struct obs_name g_open_flag_names[] = {
    { SQLITE_OPEN_READONLY, "SQLITE_OPEN_READONLY" },
    { SQLITE_OPEN_READWRITE, "SQLITE_OPEN_READWRITE" },
    { SQLITE_OPEN_CREATE, "SQLITE_OPEN_CREATE" },
    { SQLITE_OPEN_DELETEONCLOSE, "SQLITE_OPEN_DELETEONCLOSE" },
    { SQLITE_OPEN_EXCLUSIVE, "SQLITE_OPEN_EXCLUSIVE" },
    { SQLITE_OPEN_AUTOPROXY, "SQLITE_OPEN_AUTOPROXY" },
    { SQLITE_OPEN_URI, "SQLITE_OPEN_URI" },
    { SQLITE_OPEN_MEMORY, "SQLITE_OPEN_MEMORY" },
    { SQLITE_OPEN_MAIN_DB, "SQLITE_OPEN_MAIN_DB" },
    { SQLITE_OPEN_TEMP_DB, "SQLITE_OPEN_TEMP_DB" },
    { SQLITE_OPEN_TRANSIENT_DB, "SQLITE_OPEN_TRANSIENT_DB" },
    { SQLITE_OPEN_MAIN_JOURNAL, "SQLITE_OPEN_MAIN_JOURNAL" },
    { SQLITE_OPEN_TEMP_JOURNAL, "SQLITE_OPEN_TEMP_JOURNAL" },
    { SQLITE_OPEN_SUBJOURNAL, "SQLITE_OPEN_SUBJOURNAL" },
    { SQLITE_OPEN_SUPER_JOURNAL, "SQLITE_OPEN_SUPER_JOURNAL" },
    { SQLITE_OPEN_NOMUTEX, "SQLITE_OPEN_NOMUTEX" },
    { SQLITE_OPEN_FULLMUTEX, "SQLITE_OPEN_FULLMUTEX" },
    { SQLITE_OPEN_SHAREDCACHE, "SQLITE_OPEN_SHAREDCACHE" },
    { SQLITE_OPEN_PRIVATECACHE, "SQLITE_OPEN_PRIVATECACHE" },
    { SQLITE_OPEN_WAL, "SQLITE_OPEN_WAL" },
    { SQLITE_OPEN_NOFOLLOW, "SQLITE_OPEN_NOFOLLOW" },
    { SQLITE_OPEN_EXRESCODE, "SQLITE_OPEN_EXRESCODE" },
};

static void obs_init_once(void) {
    const char *v = getenv("SQLITE3_DISABLE_OBSERVABILITY");
    atomic_store_explicit(
        &g_obs_disabled,
        (v && strcmp(v, "1") == 0) ? 1 : 0,
        memory_order_release
    );
}

__attribute__((constructor(101)))
static void obs_init(void) {
    pthread_once(&g_obs_once, obs_init_once);
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void) {
    pthread_once(&g_obs_once, obs_init_once);
    return atomic_load_explicit(&g_obs_disabled, memory_order_acquire);
}

static long obs_tid(void) {
#if defined(__linux__) && defined(SYS_gettid)
    return (long)syscall(SYS_gettid);
#else
    return (long)getpid();
#endif
}

static void obs_timestamp(char *buf, size_t n) {
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
        memset(&tm, 0, sizeof(tm));
    }
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
}

__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(const char *fn, const char *fmt, ...) {
    char ts[32];
    va_list ap;

    if (obs_is_disabled()) return;

    obs_timestamp(ts, sizeof(ts));
    flockfile(stderr);
    fprintf(stderr, "[sqlite3-builds-obs] %s %ld %ld %s",
            ts, (long)getpid(), obs_tid(), fn);
    if (fmt && fmt[0]) {
        fputc(' ', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    funlockfile(stderr);
}

static const char *obs_lookup_name(
    const struct obs_name *names, size_t count, int value
) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (names[i].value == value) return names[i].name;
    }
    return NULL;
}

static const char *obs_config_name(int op, char *buf, size_t n) {
    const char *name = obs_lookup_name(
        g_config_names, sizeof(g_config_names) / sizeof(g_config_names[0]), op
    );
    if (name) return name;
    snprintf(buf, n, "UNKNOWN(%d)", op);
    return buf;
}

static const char *obs_dbconfig_name(int op, char *buf, size_t n) {
    const char *name = obs_lookup_name(
        g_dbconfig_names,
        sizeof(g_dbconfig_names) / sizeof(g_dbconfig_names[0]),
        op
    );
    if (name) return name;
    snprintf(buf, n, "UNKNOWN(%d)", op);
    return buf;
}

static void obs_escape_string(
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
            case '\\':
                dst[o++] = '\\';
                dst[o++] = '\\';
                break;
            case '"':
                dst[o++] = '\\';
                dst[o++] = '"';
                break;
            case '\n':
                dst[o++] = '\\';
                dst[o++] = 'n';
                break;
            case '\r':
                dst[o++] = '\\';
                dst[o++] = 'r';
                break;
            case '\t':
                dst[o++] = '\\';
                dst[o++] = 't';
                break;
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

static void obs_escape_unlimited(const char *src, char *dst, size_t dst_n) {
    int truncated = 0;
    obs_escape_string(src, dst, dst_n, SIZE_MAX, &truncated);
}

static void obs_escape_sql(const char *src, char *dst, size_t dst_n) {
    int truncated = 0;
    size_t tail_len = strlen(OBS_TRUNC_TAIL);
    obs_escape_string(src, dst, dst_n, OBS_SQL_CAP, &truncated);
    if (truncated) {
        size_t len = strlen(dst);
        if (dst_n > tail_len + 1) {
            if (len + tail_len >= dst_n) {
                len = dst_n - tail_len - 1;
            }
            memcpy(dst + len, OBS_TRUNC_TAIL, tail_len + 1);
        }
    }
}

static void obs_decode_open_flags(int flags, char *buf, size_t n) {
    unsigned int remaining = (unsigned int)flags;
    size_t i;
    size_t used = 0;
    int wrote = 0;

    if (n == 0) return;
    buf[0] = 0;
    for (i = 0; i < sizeof(g_open_flag_names) / sizeof(g_open_flag_names[0]); i++) {
        unsigned int bit = (unsigned int)g_open_flag_names[i].value;
        if ((remaining & bit) == bit) {
            int rc = snprintf(buf + used, n - used, "%s%s",
                              wrote ? "|" : "", g_open_flag_names[i].name);
            if (rc < 0 || (size_t)rc >= n - used) {
                buf[n - 1] = 0;
                return;
            }
            used += (size_t)rc;
            wrote = 1;
            remaining &= ~bit;
        }
    }
    if (remaining || !wrote) {
        int rc = snprintf(buf + used, n - used, "%s0x%x",
                          wrote ? "|" : "", remaining);
        if (rc < 0 || (size_t)rc >= n - used) {
            buf[n - 1] = 0;
        }
    }
}

static int obs_forward_config(int op, va_list *ap) {
    switch (op) {
        case SQLITE_CONFIG_SINGLETHREAD:
        case SQLITE_CONFIG_MULTITHREAD:
        case SQLITE_CONFIG_SERIALIZED:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_MALLOC:
        case SQLITE_CONFIG_GETMALLOC: {
            sqlite3_mem_methods *p = va_arg(*ap, sqlite3_mem_methods*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_SCRATCH:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_PAGECACHE:
        case SQLITE_CONFIG_HEAP: {
            void *p = va_arg(*ap, void*);
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_config_real(op, p, a, b);
        }
        case SQLITE_CONFIG_MEMSTATUS:
        case SQLITE_CONFIG_URI:
        case SQLITE_CONFIG_COVERING_INDEX_SCAN:
        case SQLITE_CONFIG_WIN32_HEAPSIZE:
        case SQLITE_CONFIG_STMTJRNL_SPILL:
        case SQLITE_CONFIG_SMALL_MALLOC:
        case SQLITE_CONFIG_SORTERREF_SIZE: {
            int v = va_arg(*ap, int);
            return sqlite3_config_real(op, v);
        }
        case SQLITE_CONFIG_MUTEX:
        case SQLITE_CONFIG_GETMUTEX: {
            sqlite3_mutex_methods *p = va_arg(*ap, sqlite3_mutex_methods*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_LOOKASIDE: {
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_config_real(op, a, b);
        }
        case SQLITE_CONFIG_PCACHE:
        case SQLITE_CONFIG_GETPCACHE:
            return sqlite3_config_real(op);
        case SQLITE_CONFIG_LOG: {
            obs_log_func x = va_arg(*ap, obs_log_func);
            void *p = va_arg(*ap, void*);
            return sqlite3_config_real(op, x, p);
        }
        case SQLITE_CONFIG_PCACHE2:
        case SQLITE_CONFIG_GETPCACHE2: {
            sqlite3_pcache_methods2 *p = va_arg(*ap, sqlite3_pcache_methods2*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_SQLLOG: {
            obs_sqllog_func x = va_arg(*ap, obs_sqllog_func);
            void *p = va_arg(*ap, void*);
            return sqlite3_config_real(op, x, p);
        }
        case SQLITE_CONFIG_MMAP_SIZE: {
            sqlite3_int64 a = va_arg(*ap, sqlite3_int64);
            sqlite3_int64 b = va_arg(*ap, sqlite3_int64);
            return sqlite3_config_real(op, a, b);
        }
        case SQLITE_CONFIG_PCACHE_HDRSZ:
        case SQLITE_CONFIG_ROWID_IN_VIEW: {
            int *p = va_arg(*ap, int*);
            return sqlite3_config_real(op, p);
        }
        case SQLITE_CONFIG_PMASZ: {
            unsigned int v = va_arg(*ap, unsigned int);
            return sqlite3_config_real(op, v);
        }
        case SQLITE_CONFIG_MEMDB_MAXSIZE: {
            sqlite3_int64 v = va_arg(*ap, sqlite3_int64);
            return sqlite3_config_real(op, v);
        }
        default:
            return sqlite3_config_real(op);
    }
}

static int obs_forward_db_config(sqlite3 *db, int op, va_list *ap) {
    switch (op) {
        case SQLITE_DBCONFIG_MAINDBNAME: {
            char *z = va_arg(*ap, char*);
            return sqlite3_db_config_real(db, op, z);
        }
        case SQLITE_DBCONFIG_LOOKASIDE: {
            void *p = va_arg(*ap, void*);
            int a = va_arg(*ap, int);
            int b = va_arg(*ap, int);
            return sqlite3_db_config_real(db, op, p, a, b);
        }
        case SQLITE_DBCONFIG_FP_DIGITS: {
            int v = va_arg(*ap, int);
            int *p = va_arg(*ap, int*);
            return sqlite3_db_config_real(db, op, v, p);
        }
        case SQLITE_DBCONFIG_ENABLE_FKEY:
        case SQLITE_DBCONFIG_ENABLE_TRIGGER:
        case SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER:
        case SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION:
        case SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE:
        case SQLITE_DBCONFIG_ENABLE_QPSG:
        case SQLITE_DBCONFIG_TRIGGER_EQP:
        case SQLITE_DBCONFIG_RESET_DATABASE:
        case SQLITE_DBCONFIG_DEFENSIVE:
        case SQLITE_DBCONFIG_WRITABLE_SCHEMA:
        case SQLITE_DBCONFIG_LEGACY_ALTER_TABLE:
        case SQLITE_DBCONFIG_DQS_DML:
        case SQLITE_DBCONFIG_DQS_DDL:
        case SQLITE_DBCONFIG_ENABLE_VIEW:
        case SQLITE_DBCONFIG_LEGACY_FILE_FORMAT:
        case SQLITE_DBCONFIG_TRUSTED_SCHEMA:
        case SQLITE_DBCONFIG_STMT_SCANSTATUS:
        case SQLITE_DBCONFIG_REVERSE_SCANORDER:
        case SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE:
        case SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE:
        case SQLITE_DBCONFIG_ENABLE_COMMENTS: {
            int onoff = va_arg(*ap, int);
            int *p = va_arg(*ap, int*);
            return sqlite3_db_config_real(db, op, onoff, p);
        }
        default:
            return sqlite3_db_config_real(db, op);
    }
}

SQLITE_API int sqlite3_initialize(void) {
    int outer;
    int rc;
    pthread_once(&g_obs_once, obs_init_once);
    outer = (s_init_depth++ == 0);
    rc = sqlite3_initialize_real();
    s_init_depth--;
    if (outer && rc != SQLITE_OK &&
        !atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_logf("sqlite3_initialize", "rc=%d", rc);
    }
    return rc;
}

SQLITE_API int sqlite3_config(int op, ...) {
    va_list ap;
    int rc;
    char unknown[32];
    const char *name;

    pthread_once(&g_obs_once, obs_init_once);
    va_start(ap, op);
    rc = obs_forward_config(op, &ap);
    va_end(ap);

    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        name = obs_config_name(op, unknown, sizeof(unknown));
        obs_logf("sqlite3_config", "op=%s rc=%d", name, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_db_config(sqlite3 *db, int op, ...) {
    va_list ap;
    int rc;
    char unknown[32];
    const char *name;

    pthread_once(&g_obs_once, obs_init_once);
    va_start(ap, op);
    rc = obs_forward_db_config(db, op, &ap);
    va_end(ap);

    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        name = obs_dbconfig_name(op, unknown, sizeof(unknown));
        obs_logf("sqlite3_db_config", "db=%p op=%s rc=%d", (void*)db, name, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_open(const char *filename, sqlite3 **ppDb) {
    int rc;
    char file[4096];

    pthread_once(&g_obs_once, obs_init_once);
    rc = sqlite3_open_real(filename, ppDb);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_escape_unlimited(filename, file, sizeof(file));
        obs_logf("sqlite3_open",
                 "file=\"%s\" flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE db=%p rc=%d",
                 file, ppDb ? (void*)*ppDb : NULL, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_open_v2(
    const char *filename, sqlite3 **ppDb, int flags, const char *zVfs
) {
    int rc;
    char file[4096];
    char vfs[512];
    char flagbuf[1024];

    pthread_once(&g_obs_once, obs_init_once);
    rc = sqlite3_open_v2_real(filename, ppDb, flags, zVfs);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        obs_escape_unlimited(filename, file, sizeof(file));
        obs_escape_unlimited(zVfs, vfs, sizeof(vfs));
        obs_decode_open_flags(flags, flagbuf, sizeof(flagbuf));
        obs_logf("sqlite3_open_v2", "file=\"%s\" flags=%s vfs=\"%s\" db=%p rc=%d",
                 file, flagbuf, vfs, ppDb ? (void*)*ppDb : NULL, rc);
    }
    return rc;
}

SQLITE_API int sqlite3_open16(const void *filename, sqlite3 **ppDb) {
    int rc;

    pthread_once(&g_obs_once, obs_init_once);
    rc = sqlite3_open16_real(filename, ppDb);
    if (!atomic_load_explicit(&g_obs_disabled, memory_order_acquire)) {
        if (rc == SQLITE_OK) {
            obs_logf("sqlite3_open16",
                     "filename16=%p flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE db=%p rc=%d",
                     filename, ppDb ? (void*)*ppDb : NULL, rc);
        } else {
            obs_logf("sqlite3_open16", "filename16=%p db=%p rc=%d",
                     filename, ppDb ? (void*)*ppDb : NULL, rc);
        }
    }
    return rc;
}

__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    sqlite3_stmt *stmt;
    sqlite3 *db;
    const char *sql;
    const char *file_name;
    char file[4096];
    char sqlbuf[OBS_SQL_CAP + 256];

    (void)ctx;
    if (trace != SQLITE_TRACE_STMT || obs_is_disabled()) return 0;

    stmt = (sqlite3_stmt*)p;
    db = stmt ? sqlite3_db_handle(stmt) : NULL;
    sql = x ? (const char*)x : (stmt ? sqlite3_sql(stmt) : NULL);
    file_name = db ? sqlite3_db_filename(db, "main") : NULL;
    obs_escape_unlimited(file_name, file, sizeof(file));
    obs_escape_sql(sql, sqlbuf, sizeof(sqlbuf));
    obs_logf("trace_stmt", "event=SQLITE_TRACE_STMT db=%p stmt=%p file=\"%s\" sql=\"%s\"",
             (void*)db, p, file, sqlbuf);
    return 0;
}
