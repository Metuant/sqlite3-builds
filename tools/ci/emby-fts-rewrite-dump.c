#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void failf(sqlite3 *db, const char *msg) {
    fprintf(stderr, "FATAL: %s", msg);
    if (db) fprintf(stderr, ": %s", sqlite3_errmsg(db));
    fputc('\n', stderr);
    exit(1);
}

static char *read_all(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp) {
        fprintf(stderr, "FATAL: open input %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(fp, 0, SEEK_END) != 0) failf(NULL, "seek input end failed");
    size = ftell(fp);
    if (size < 0) failf(NULL, "tell input failed");
    if (fseek(fp, 0, SEEK_SET) != 0) failf(NULL, "seek input start failed");
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) failf(NULL, "malloc input failed");
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) failf(NULL, "read input failed");
    if (fclose(fp) != 0) failf(NULL, "close input failed");
    while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) size--;
    buf[size] = 0;
    return buf;
}

static void write_all(const char *path, const char *sql) {
    FILE *fp = fopen(path, "wb");
    size_t n = strlen(sql);

    if (!fp) {
        fprintf(stderr, "FATAL: open output %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fwrite(sql, 1, n, fp) != n) failf(NULL, "write output failed");
    if (fputc('\n', fp) == EOF) failf(NULL, "write output newline failed");
    if (fclose(fp) != 0) failf(NULL, "close output failed");
}

static int has_library_basename(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "library.db") == 0;
}

int main(int argc, char **argv) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char *sql;
    const char *saved;
    int rc;

    if (argc != 4) {
        fprintf(stderr, "usage: %s /path/to/library.db input-template.sql output-rewritten.sql\n", argv[0]);
        return 2;
    }
    if (!has_library_basename(argv[1])) {
        fprintf(stderr, "FATAL: database path basename must be exactly library.db: %s\n", argv[1]);
        return 2;
    }
    if (setenv("SQLITE3_DISABLE_EMBY_FTS_REWRITE", "0", 1) != 0) {
        fprintf(stderr, "FATAL: setenv SQLITE3_DISABLE_EMBY_FTS_REWRITE failed: %s\n", strerror(errno));
        return 1;
    }

    sql = read_all(argv[2]);
    rc = sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) failf(db, "sqlite3_open_v2 failed");
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf(db, "sqlite3_prepare_v2 failed");
    saved = sqlite3_sql(stmt);
    if (!saved) failf(db, "sqlite3_sql returned NULL");
    write_all(argv[3], saved);
    if (sqlite3_finalize(stmt) != SQLITE_OK) failf(db, "sqlite3_finalize failed");
    if (sqlite3_close(db) != SQLITE_OK) failf(db, "sqlite3_close failed");
    free(sql);
    return 0;
}
