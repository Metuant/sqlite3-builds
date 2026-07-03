#ifndef PLEX_FTS_REWRITE_H
#define PLEX_FTS_REWRITE_H

#include "sqlite3.h"

typedef enum fts_rewrite_prepare_kind {
    FTS_REWRITE_PREPARE_LEGACY = 0,
    FTS_REWRITE_PREPARE_V2 = 1,
    FTS_REWRITE_PREPARE_V3 = 2
} fts_rewrite_prepare_kind;

__attribute__((visibility("hidden"))) int plex_fts_rewrite_prepare(
    sqlite3 *db,
    const char *zSql,
    int nByte,
    unsigned int prepFlags,
    sqlite3_stmt **ppStmt,
    const char **pzTail,
    fts_rewrite_prepare_kind kind
);

#endif
