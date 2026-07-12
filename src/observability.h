#ifndef SQLITE3_BUILDS_OBSERVABILITY_H
#define SQLITE3_BUILDS_OBSERVABILITY_H

#include "sqlite3.h"

#include <stddef.h>

__attribute__((visibility("hidden"))) SQLITE_API int obs_is_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_stmt_trace_disabled(void);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_cb(
    unsigned trace,
    void *ctx,
    void *p,
    void *x
);
__attribute__((visibility("hidden"))) SQLITE_API int obs_trace_stmt_cb(
    unsigned trace,
    void *ctx,
    void *p,
    void *x
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_logf(
    const char *fn,
    const char *fmt,
    ...
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_capture_miss(
    const char *target,
    const char *mode,
    const char *sub_reason,
    sqlite3 *db,
    const char *sql,
    size_t len
);
__attribute__((visibility("hidden"))) SQLITE_API int obs_should_log_index_missing(
    sqlite3 *db,
    const char *target,
    const char *mode
);
__attribute__((visibility("hidden"))) SQLITE_API void obs_log_rewrite_applied(
    const char *target,
    const char *mode,
    sqlite3 *db,
    const char *source,
    size_t source_len,
    const char *rewritten,
    size_t rewritten_len
);

#endif
