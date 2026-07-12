/* obs_* stubs for smokes that omit observability.c. */

#include "observability.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int obs_is_disabled(void) {
    const char *v = getenv("SQLITE3_DISABLE_OBSERVABILITY");
    return v && strcmp(v, "1") == 0;
}

void obs_logf(const char *fn, const char *fmt, ...) {
    va_list ap;
    if (obs_is_disabled()) return;
    fprintf(stderr, "[sqlite3-builds-obs] test 0 0 %s", fn);
    if (fmt && fmt[0]) {
        fputc(' ', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}
