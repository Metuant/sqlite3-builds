#ifndef AUTO_EXTENSION_INTERNAL_H
#define AUTO_EXTENSION_INTERNAL_H

#include "sqlite3.h"

__attribute__((visibility("hidden"))) void runtime_optimize_seed_path(sqlite3 *db, const char *raw_fn);
int auto_extension_path_is_target(const char *raw_fn);
extern _Thread_local int g_autopragma_depth;

#endif
