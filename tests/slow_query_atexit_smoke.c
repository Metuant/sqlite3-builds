#include "sqlite3.h"

/* Atexit smoke: triggers enough tracker records for stats eligibility, exits
 * normally, expects the atexit-registered dump to emit either a
 * slow_query_stats line (successful dump) or a dump_skipped reason=atexit
 * diagnostic (mutex contention). Shell-capture wrapper asserts one of the two
 * appears. */
int slow_query_test_record_sql(const char *sql, sqlite3_int64 elapsed_ns);

int obs_trace_stmt_cb(unsigned trace, void *ctx, void *p, void *x) {
    (void)trace;
    (void)ctx;
    (void)p;
    (void)x;
    return 0;
}

int main(void) {
    int i;
    for (i = 0; i < 5; i++) {
        slow_query_test_record_sql("SELECT atexit_smoke", 1000000000LL);
    }
    return 0;
}
