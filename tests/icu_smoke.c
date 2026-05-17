#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#define ICU_COLLATION_SQL "SELECT icu_load_collation('root@colNumeric=yes', 'icu_root');"
#define CREATE_TABLE_SQL "CREATE TABLE t(x TEXT);"
/* WHY: The mixed-case seed distinguishes ICU root ordering from bytewise ASCII. */
#define INSERT_ROWS_SQL "INSERT INTO t(x) VALUES ('a'), ('B'), ('c');"
#define ORDER_QUERY_SQL "SELECT x FROM t ORDER BY x COLLATE icu_root;"

static int fail_sql(sqlite3 *db, const char *context, int rc) {
    if (db != NULL) {
        fprintf(stderr, "FAIL: %s: %s (rc=%d)\n", context, sqlite3_errmsg(db), rc);
    } else {
        fprintf(stderr, "FAIL: %s: sqlite rc=%d\n", context, rc);
    }
    return 1;
}

static int run_exec(sqlite3 *db, const char *sql, const char *context) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        return fail_sql(db, context, rc);
    }
    return 0;
}

int main(void) {
    const char *expected[] = {"a", "B", "c"};
    const int expected_count = (int)(sizeof(expected) / sizeof(expected[0]));
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    const unsigned char *value;
    int rc;
    int row = 0;
    int failed = 0;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        failed = fail_sql(db, "open :memory:", rc);
        goto cleanup;
    }

    if (run_exec(db, ICU_COLLATION_SQL, "register ICU collation") != 0) {
        failed = 1;
        goto cleanup;
    }
    if (run_exec(db, CREATE_TABLE_SQL, "create smoke table") != 0) {
        failed = 1;
        goto cleanup;
    }
    if (run_exec(db, INSERT_ROWS_SQL, "insert smoke rows") != 0) {
        failed = 1;
        goto cleanup;
    }

    /* WHY: Preparing and stepping COLLATE icu_root proves SQLite calls through
     * the registered ICU comparator, not just that the load function exists. */
    rc = sqlite3_prepare_v2(db, ORDER_QUERY_SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        failed = fail_sql(db, "prepare ICU ORDER BY query", rc);
        goto cleanup;
    }

    for (;;) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            failed = fail_sql(db, "step ICU ORDER BY query", rc);
            goto cleanup;
        }
        if (row >= expected_count) {
            fprintf(stderr, "FAIL: extra row %d returned\n", row + 1);
            failed = 1;
            goto cleanup;
        }
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            fprintf(stderr, "FAIL: row %d is NULL (expected %s)\n",
                    row + 1, expected[row]);
            failed = 1;
            goto cleanup;
        }
        value = sqlite3_column_text(stmt, 0);
        if (value == NULL) {
            fprintf(stderr, "FAIL: row %d value is unreadable (expected %s)\n",
                    row + 1, expected[row]);
            failed = 1;
            goto cleanup;
        }
        if (strcmp((const char *)value, expected[row]) != 0) {
            fprintf(stderr, "FAIL: row %d is %s (expected %s)\n",
                    row + 1, (const char *)value, expected[row]);
            failed = 1;
            goto cleanup;
        }
        row++;
    }

    if (row < expected_count) {
        fprintf(stderr, "FAIL: only %d rows returned (expected %d)\n",
                row, expected_count);
        failed = 1;
        goto cleanup;
    }

cleanup:
    if (stmt != NULL) {
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            failed = fail_sql(db, "finalize ICU ORDER BY query", rc);
        }
    }

    if (db != NULL) {
        rc = sqlite3_close(db);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "FAIL: close database: sqlite rc=%d\n", rc);
            failed = 1;
        }
    }

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
