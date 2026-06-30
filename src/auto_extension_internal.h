#ifndef AUTO_EXTENSION_INTERNAL_H
#define AUTO_EXTENSION_INTERNAL_H

#include "sqlite3.h"

#include <stddef.h>
#include <stdint.h>

/* Fixed-layout hidden ABI mirrored inline in build/sqlite-amalgamation.patch. */
typedef struct auto_extension_busy_handler_snapshot {
    int (*xBusyHandler)(void*, int);
    void *pBusyArg;
    int nBusy;
    int busyTimeout;
    int setlkTimeout;
} auto_extension_busy_handler_snapshot;

typedef struct auto_extension_change_counter_snapshot {
    sqlite3_int64 nChange;
    sqlite3_int64 nTotalChange;
} auto_extension_change_counter_snapshot;

_Static_assert(sizeof(((auto_extension_busy_handler_snapshot *)0)->xBusyHandler) == sizeof(void *),
               "auto_extension_busy_handler_snapshot pointer size drift");
_Static_assert(offsetof(auto_extension_busy_handler_snapshot, xBusyHandler) == 0,
               "auto_extension_busy_handler_snapshot xBusyHandler offset drift");
_Static_assert(offsetof(auto_extension_busy_handler_snapshot, pBusyArg) ==
                   sizeof(((auto_extension_busy_handler_snapshot *)0)->xBusyHandler),
               "auto_extension_busy_handler_snapshot pBusyArg offset drift");
_Static_assert(offsetof(auto_extension_busy_handler_snapshot, nBusy) ==
                   sizeof(((auto_extension_busy_handler_snapshot *)0)->xBusyHandler) + sizeof(void *),
               "auto_extension_busy_handler_snapshot nBusy offset drift");
_Static_assert(offsetof(auto_extension_busy_handler_snapshot, busyTimeout) ==
                   sizeof(((auto_extension_busy_handler_snapshot *)0)->xBusyHandler) +
                       sizeof(void *) + sizeof(int),
               "auto_extension_busy_handler_snapshot busyTimeout offset drift");
_Static_assert(offsetof(auto_extension_busy_handler_snapshot, setlkTimeout) ==
                   sizeof(((auto_extension_busy_handler_snapshot *)0)->xBusyHandler) +
                       sizeof(void *) + (2 * sizeof(int)),
               "auto_extension_busy_handler_snapshot setlkTimeout offset drift");
_Static_assert(offsetof(auto_extension_change_counter_snapshot, nChange) == 0,
               "auto_extension_change_counter_snapshot nChange offset drift");
_Static_assert(offsetof(auto_extension_change_counter_snapshot, nTotalChange) == sizeof(sqlite3_int64),
               "auto_extension_change_counter_snapshot nTotalChange offset drift");
_Static_assert(sizeof(auto_extension_change_counter_snapshot) == 2 * sizeof(sqlite3_int64),
               "auto_extension_change_counter_snapshot size drift");

__attribute__((visibility("hidden"))) void runtime_optimize_seed_path(sqlite3 *db, const char *raw_fn);
__attribute__((visibility("hidden"))) int runtime_optimize_in_progress(void);
__attribute__((visibility("hidden"))) void runtime_optimize_note_stmt_elapsed(sqlite3 *db, sqlite3_stmt *stmt, sqlite3_int64 elapsed_ns);
__attribute__((visibility("hidden"))) uint64_t slow_query_threshold_ns(void);
int auto_extension_path_is_target(const char *raw_fn);
extern _Thread_local int g_autopragma_depth;

#endif
