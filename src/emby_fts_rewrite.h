#ifndef EMBY_FTS_REWRITE_H
#define EMBY_FTS_REWRITE_H

#include "plex_fts_rewrite.h"

__attribute__((visibility("hidden"))) int emby_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
);

#endif
