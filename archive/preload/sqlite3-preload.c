/* ======================================================================== */
/* sqlite3-preload.c - Run SQLite commands on open via LD_PRELOAD           */
/* ======================================================================== */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* ======================== Original Library Calls ======================== */
/* ======================================================================== */

#define SQLITE_CANTOPEN 14
struct sqlite3;
typedef struct sqlite3 sqlite3;

static void *
lib;

static
int (*orig_sqlite3_open) (const char *filename, sqlite3 **ppDb);

static
int (*orig_sqlite3_open16) (const char *filename, sqlite3 **ppDb);

static
int (*orig_sqlite3_open_v2) (const char *filename, sqlite3 **ppDb, int flags,
    const char *zVfs);

static
int (*orig_sqlite3_close) (sqlite3 *pDb);

static
void (*orig_sqlite3_free) (void *data);

static
int (*orig_sqlite3_exec) (sqlite3 *pDb, const char *sql,
    int (*callback)(void*,int,char**,char**), void *user, char **errmsg);

/* ======================================================================== */
/* =========================== Helper Functions =========================== */
/* ======================================================================== */

static void
show_err (
    const char *filename,
    const char *op
)
{
    char ebuf[256];
    strerror_r(errno, ebuf, sizeof(ebuf));
    fprintf(stderr, "SQLITE3_INIT_SQL(%s) - failed - %s (%d) %s\n",
            filename, op, errno, ebuf);
}

/* ======================================================================== */

static int
ends_with (
    const char *str,
    const char *suffix
)
{
    if (!str || !suffix) {
        return 0;
    }

    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr) {
        return 0;
    }

    return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

/* ======================================================================== */

static int
read_file (
    char **obuf
)
{
    size_t sz_read, sz_buf, sz_buf_total, sz_need;
    char rdbuf[1024];
    char *buf, *nbuf;
    int ret = 0;

    /* see if we have a file to execute */
    const char *filename = getenv("SQLITE3_INIT_SQL");
    if (filename == 0) {
        *obuf = 0;
        return 0;
    }

    /* open the file */
    FILE *file = fopen(filename, "r");
    if (file == 0) {
        show_err(filename, "cannot fopen()");
        return -1;
    }

    /* allocate initial buffer */
    sz_buf = 0;
    sz_buf_total = 1024;
    buf = malloc(1024);
    if (buf == 0) {
        show_err(filename, "cannot malloc()");
        ret = -1;
        goto out;
    }

    /* read file */
    while (!feof(file)) {
        /* read a data chunk */
        sz_read = fread(rdbuf, 1, sizeof(rdbuf), file);
        if (ferror(file)) {
            show_err(filename, "cannot fread()");
            ret = -1;
            goto out;
        }

        /* grow buffer */
        sz_need = sz_buf + sz_read + 1;
        if (sz_need > sz_buf_total) {
            sz_need += 4096;
            nbuf = realloc(buf, sz_need);
            if (nbuf == 0) {
                show_err(filename, "cannot realloc()");
                ret = -1;
                goto out;
            }
            buf = nbuf;
            sz_buf_total = sz_need;
        }

        /* copy data into buffer */
        memcpy(buf + sz_buf, rdbuf, sz_read);
        sz_buf += sz_read;
    }

    /* NULL-terminate */
    buf[sz_buf] = 0;

    /* donate buffer to caller */
    *obuf = buf;
    buf = 0;

out:
    /* release buffer */
    free(buf);

    /* close file */
    fclose(file);

    /* done! */
    return ret;
}

/* ======================================================================== */

static void *
get_sym (
    const char *name
)
{
    const char *err;
    void *sym;

    (void)dlerror();
    sym = dlsym(lib, name);
    err = dlerror();
    if (err == 0 && sym == 0)
        err = "dlsym() returned NULL!";
    if (err != 0) {
        fprintf(stderr, "Can't find symbol %s: %s!\n", name, err);
        abort();
    }

    return sym;
}

/* ======================================================================== */

static void
__attribute__((constructor))
load_syms (void)
{
    const char *library;
    const char *err;

    /* do not double load */
    if (lib != 0)
        return;

    /* load library */
    library = getenv("SQLITE3_LIBRARY");
    if (library == 0)
        library = "libsqlite3.so.0";
    (void)dlerror();
    lib = dlopen(library, RTLD_NOW|RTLD_GLOBAL);
    err = dlerror();
    if (err == 0 && lib == 0)
        err = "dlopen() returned NULL!";
    if (err != 0) {
        fprintf(stderr, "Can't open library %s: %s!\n", library, err);
        abort();
    }

    /* load symbols */
    orig_sqlite3_open = get_sym("sqlite3_open");
    orig_sqlite3_open16 = get_sym("sqlite3_open16");
    orig_sqlite3_open_v2 = get_sym("sqlite3_open_v2");
    orig_sqlite3_exec = get_sym("sqlite3_exec");
    orig_sqlite3_free = get_sym("sqlite3_free");
    orig_sqlite3_close = get_sym("sqlite3_close");
}

/* ======================================================================== */
/* ============================ Hook Functions ============================ */
/* ======================================================================== */

int
sqlite3_open (
    const char *filename,
    sqlite3 **ppDb
)
{
    char *out_filename;
    FILE *output;
    char *cmdbuf, *err;
    int ret;

    /* read SQL commands from file */
    ret = read_file(&cmdbuf);
    if (ret == -1) {
        return SQLITE_CANTOPEN;
    }

    output = stderr;

    /* open output file for logging */
    out_filename = getenv("SQLITE3_LOGGING_OUTPUT");
    if (out_filename) {
      output = fopen(out_filename, "a+");

      if (output == NULL) {
        output = stderr;
        fprintf(output, "ERROR: Couldn't open file\n");
      }
    }

    /* open database */
    ret = orig_sqlite3_open(filename, ppDb);
    if (ret != 0) {
        free(cmdbuf);
        return ret;
    }

    fprintf(output, "TRACE: (OPEN) (%s)\n", filename);

    /* execute command */
    if (cmdbuf && ends_with(filename, "com.plexapp.plugins.library.db")) {
        ret = orig_sqlite3_exec(*ppDb, cmdbuf, 0, 0, &err);
        free(cmdbuf);
        if (ret != 0) {
            fprintf(output, "sqlite3_open error: %s\n", err);
            orig_sqlite3_free(err);
            orig_sqlite3_close(*ppDb);
            return SQLITE_CANTOPEN;
        }
        fprintf(output, "TRACE: (EXECUTED) (%s) %s\n", filename, getenv("SQLITE3_INIT_SQL"));
    }

    /* done! */
    return ret;
}

/* ======================================================================== */

int sqlite3_open16 (
    const void *filename,
    sqlite3 **ppDb
)
{
    char *cmdbuf, *err;
    int ret;

    /* read SQL commands from file */
    ret = read_file(&cmdbuf);
    if (ret == -1)
        return SQLITE_CANTOPEN;

    /* open database */
    ret = orig_sqlite3_open16(filename, ppDb);
    if (ret != 0) {
        free(cmdbuf);
        return ret;
    }

    /* execute command */
    if (cmdbuf) {
        ret = orig_sqlite3_exec(*ppDb, cmdbuf, 0, 0, &err);
        free(cmdbuf);
        if (ret != 0) {
            fprintf(stderr, "sqlite3_open error: %s\n", err);
            orig_sqlite3_free(err);
            orig_sqlite3_close(*ppDb);
            return SQLITE_CANTOPEN;
        }
    }

    /* done! */
    return ret;
}

/* ======================================================================== */

int sqlite3_open_v2 (
    const char *filename,
    sqlite3 **ppDb,
    int flags,
    const char *zVfs
)
{
    char *out_filename;
    FILE *output;
    char *cmdbuf, *err;
    int ret;
    int file_opened;

    /* read SQL commands from file */
    ret = read_file(&cmdbuf);
    if (ret == -1) {
        return SQLITE_CANTOPEN;
    }

    output = stderr;
    file_opened = 0;

    /* open output file for logging */
    out_filename = getenv("SQLITE3_LOGGING_OUTPUT");
    if (out_filename) {
      output = fopen(out_filename, "a+");

      if (output == NULL) {
        output = stderr;
        fprintf(output, "ERROR: Couldn't open file\n");
      } else {
        file_opened = 1;
      }
    }

    /* open database */
    ret = orig_sqlite3_open_v2(filename, ppDb, flags, zVfs);
    if (ret != 0) {
        free(cmdbuf);
        return ret;
    }

    fprintf(output, "TRACE: (OPEN) (%s) %d\n", filename, flags);

    /* execute command */
    // Plex - com.plexapp.plugins.library.db
    // Emby - library.db
    if (cmdbuf && ends_with(filename, "library.db")) {
        ret = orig_sqlite3_exec(*ppDb, cmdbuf, 0, 0, &err);
        free(cmdbuf);
        if (ret != 0) {
            fprintf(output, "ERROR: sqlite3_open error: %s\n", err);
            orig_sqlite3_free(err);
            orig_sqlite3_close(*ppDb);
            return SQLITE_CANTOPEN;
        }

        fprintf(output, "TRACE: (EXECUTED) (%s) %s\n", filename, getenv("SQLITE3_INIT_SQL"));
    }

    /* close output file */
    if (file_opened == 1) {
        fclose(output);
    }

    /* done! */
    return ret;
}

/* ======================================================================== */

