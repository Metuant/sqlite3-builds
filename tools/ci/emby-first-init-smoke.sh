#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ci/lib/assertions.sh
. "${script_dir}/lib/assertions.sh"

emby_img=$1
arch_suffix=$2
run_id=$3
generic_artifact_dir=$4
sqlite_version_dotted=$5

: "Emby first-init smoke arch ${arch_suffix}"
# WHY: tag pin fixes the Emby runtime ABI checked below.
GENERIC_ARTIFACT_DIR_ABS="$(realpath "$generic_artifact_dir")"
if [ ! -f "$GENERIC_ARTIFACT_DIR_ABS/libsqlite3.so" ]; then
  echo "FATAL: missing extracted generic library at $GENERIC_ARTIFACT_DIR_ABS/libsqlite3.so"
  exit 1
fi

EMBY_CONFIG_DIR="$(mktemp -d)"
EMBY_CONTAINER="emby-smoke-${run_id}-${arch_suffix}"
# shellcheck disable=SC2154
trap 'status=$?; docker logs "$EMBY_CONTAINER" 2>&1 || true; docker stop "$EMBY_CONTAINER" 2>/dev/null || true; exit "$status"' EXIT

docker run --detach \
  --name "$EMBY_CONTAINER" \
  -e PUID=1000 \
  -e PGID=1000 \
  -e SQLITE3_DISABLE_STMT_TRACE=0 \
  --mount "type=bind,src=${EMBY_CONFIG_DIR},dst=/config" \
  --mount "type=bind,src=${GENERIC_ARTIFACT_DIR_ABS}/libsqlite3.so,dst=/app/emby/lib/libsqlite3.so.3.49.2,readonly" \
  --network host \
  "$emby_img"

sqlite_version_line=""
emby_logs=""
for _ in $(seq 1 30); do
  emby_logs="$(docker logs "$EMBY_CONTAINER" 2>&1 || true)"
  sqlite_version_line="$(printf "%s\n" "$emby_logs" | grep "Sqlite version:" | tail -n 1 || true)"
  if [ -n "$sqlite_version_line" ]; then
    break
  fi
  sleep 3
done
if [ -z "$sqlite_version_line" ]; then
  echo "FATAL: Emby did not log Sqlite version: within 90 seconds"
  docker logs "$EMBY_CONTAINER" 2>&1 || true
  exit 1
fi

assert_emby_sqlite_version "$sqlite_version_line" "$sqlite_version_dotted" || exit 1

emby_logs="$(docker logs "$EMBY_CONTAINER" 2>&1 || true)"
assert_emby_compile_options "$emby_logs" || exit 1

bad_signal_re='\bSQLITE_(ERROR|INTERNAL|NOMEM|IOERR|CORRUPT|FULL|CANTOPEN|PROTOCOL|MISMATCH|MISUSE|NOTADB|FORMAT|AUTH)\b|database disk image is malformed|Cannot create table|no such function|no such collation|Failed to open'
scan_bad_signal "$emby_logs" "$bad_signal_re" "FATAL: Emby logs contain SQLite/runtime bad signal" || exit 1
assert_marker_present "$emby_logs" '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "FATAL: Emby logs missing SQLITE_TRACE_STMT observability marker" || exit 1
assert_open_marker_count "$emby_logs" "Emby" >/dev/null || exit 1
assert_no_startup_config_rc21 "$emby_logs" "Emby" || exit 1
