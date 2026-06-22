#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ci/lib/assertions.sh
. "${script_dir}/lib/assertions.sh"

docker_library_image=$1
arch_suffix=$2

: "Slow-query smoke arch ${arch_suffix}"
out=""
set +e
out="$(docker run --rm "${docker_library_image}" /bin/sh -c '
  LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
  LD_LIBRARY_PATH="$LIB_DIR" /app/slow_query_smoke
' 2>&1)"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  printf "%s\n" "$out"
  exit "$status"
fi
printf "%s\n" "$out"
assert_marker_present "$out" 'slow query smoke passed' "FATAL: slow-query smoke did not report success" || exit 1
assert_marker_present "$out" '\[sqlite3-builds-obs\].* slow_query ' "FATAL: slow-query smoke missing slow_query marker" || exit 1

out=""
set +e
out="$(docker run --rm \
  -e SQLITE3_SLOW_QUERY_THRESHOLD_MS=0 \
  -e SQLITE3_DISABLE_SLOW_QUERY=1 \
  -e SQLITE3_DISABLE_STMT_TRACE=0 \
  "${docker_library_image}" \
  /bin/sh -c '
    LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
    LD_LIBRARY_PATH="$LIB_DIR" /app/slow_query_smoke child-kill
  ' 2>&1)"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  printf "%s\n" "$out"
  exit "$status"
fi
printf "%s\n" "$out"
assert_marker_present "$out" '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "FATAL: slow-query disabled-mode smoke missing SQLITE_TRACE_STMT marker" || exit 1
assert_marker_absent "$out" '\[sqlite3-builds-obs\].* slow_query(_stats)?( |$)' "FATAL: slow-query disabled-mode smoke emitted slow_query output" || exit 1

out=""
set +e
out="$(docker run --rm "${docker_library_image}" /bin/sh -c '
  LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
  AMALG_DIR=$(dirname "$LIB_DIR")
  gcc -O0 -DSLOW_QUERY_TRACKER_TEST_API -o /tmp/stmt_trace_smoke \
    /app/tests/stmt_trace_smoke.c \
    /app/auto_extension.c \
    /app/observability.c \
    /app/slow_query_tracker.c \
    -I"$AMALG_DIR" -lpthread -lm
  /tmp/stmt_trace_smoke
' 2>&1)"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  printf "%s\n" "$out"
  exit "$status"
fi
printf "%s\n" "$out"
assert_marker_present "$out" 'stmt trace smoke passed' "FATAL: stmt trace smoke did not report success" || exit 1

out=""
set +e
out="$(docker run --rm "${docker_library_image}" /bin/sh -c '
  LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
  LD_LIBRARY_PATH="$LIB_DIR" /app/slow_query_atexit_smoke
' 2>&1)"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  printf "%s\n" "$out"
  exit "$status"
fi
printf "%s\n" "$out"
assert_marker_present "$out" '\[sqlite3-builds-obs\].*(slow_query_stats|dump_skipped)' "FATAL: slow-query atexit smoke missing stats or contention diagnostic" || exit 1

out=""
set +e
out="$(docker run --rm "${docker_library_image}" /bin/sh -c '
  LIB_DIR=$(dirname "$(ls /app/sqlite-amalgamation-*/dist/libsqlite3.so)")
  LD_LIBRARY_PATH="$LIB_DIR" /app/slow_query_concurrency_smoke
' 2>&1)"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  printf "%s\n" "$out"
  exit "$status"
fi
printf "%s\n" "$out"
assert_marker_present "$out" 'slow query concurrency smoke passed' "FATAL: slow-query concurrency smoke did not report success" || exit 1
