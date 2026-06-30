/* Single home for sibling-module internal dep-stubs; link into every
 * standalone compile of slow_query_tracker.c / observability.c that omits
 * runtime_optimize.c. */
#include "sqlite3.h"

__attribute__((visibility("hidden"))) int runtime_optimize_in_progress(void) {
    return 0;
}

__attribute__((visibility("hidden"))) void runtime_optimize_note_stmt_elapsed(
    sqlite3 *db, sqlite3_stmt *stmt, sqlite3_int64 elapsed_ns
) {
    (void)db;
    (void)stmt;
    (void)elapsed_ns;
}
