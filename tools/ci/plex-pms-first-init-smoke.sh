#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ci/lib/assertions.sh
. "${script_dir}/lib/assertions.sh"

plex_img=$1
arch_suffix=$2
run_id=$3
artifact_dir=$4
icu_version=$5

EXPECTED_ICU_MAJOR="${icu_version%%.*}"
: "Plex image ${plex_img} with expected ICU major ${EXPECTED_ICU_MAJOR}"
ARTIFACT_DIR_ABS="$(realpath "$artifact_dir")"
if [ ! -f "$ARTIFACT_DIR_ABS/libsqlite3.so" ]; then
  echo "FATAL: missing extracted Plex library at $ARTIFACT_DIR_ABS/libsqlite3.so"
  exit 1
fi

PMS_CONFIG_DIR="$(mktemp -d)"
PMS_DB_COPY_DIR="$(mktemp -d)"
PMS_DB_COPY="${PMS_DB_COPY_DIR}/com.plexapp.plugins.library.db"
PMS_CONTAINER="pms-smoke-${run_id}-${arch_suffix}"
# shellcheck disable=SC2154
trap 'status=$?; docker logs "$PMS_CONTAINER" 2>&1 || true; docker stop "$PMS_CONTAINER" 2>/dev/null || true; exit "$status"' EXIT

docker run --detach \
  --name "$PMS_CONTAINER" \
  -e PUID=1000 \
  -e PGID=1000 \
  -e VERSION=docker \
  -e SQLITE3_DISABLE_STMT_TRACE=0 \
  --mount "type=bind,src=${PMS_CONFIG_DIR},dst=/config" \
  --mount "type=bind,src=${ARTIFACT_DIR_ABS}/libsqlite3.so,dst=/usr/lib/plexmediaserver/lib/libsqlite3.so,readonly" \
  --network host \
  "$plex_img"

pms_ready=0
for _ in $(seq 1 30); do
  if bash -c 'echo > /dev/tcp/localhost/32400' 2>/dev/null; then
    pms_ready=1
    break
  fi
  sleep 3
done
if [ "$pms_ready" -ne 1 ]; then
  echo "FATAL: PMS did not open TCP 32400 within 90 seconds"
  docker logs "$PMS_CONTAINER" 2>&1 || true
  exit 1
fi

wait_for_pms_table_count() {
  local table_count
  table_count=0
  for _ in $(seq 1 30); do
    if docker cp "${PMS_CONTAINER}:/config/Library/Application Support/Plex Media Server/Plug-in Support/Databases/com.plexapp.plugins.library.db" "$PMS_DB_COPY" >/dev/null 2>&1; then
      table_count="$(sqlite3 "$PMS_DB_COPY" "SELECT COUNT(*) FROM sqlite_master WHERE type='table';" 2>/dev/null || true)"
      case "$table_count" in
        "" | *[!0-9]*) table_count=0 ;;
      esac
      if [ "$table_count" -ge 20 ]; then
        return 0
      fi
    fi
    sleep 1
  done
  echo "FATAL: expected at least 20 PMS tables within 30 seconds, got $table_count"
  return 1
}
wait_for_pms_table_count

bad_signal_re='\bSQLITE_(ERROR|INTERNAL|NOMEM|IOERR|CORRUPT|FULL|CANTOPEN|PROTOCOL|MISMATCH|MISUSE|NOTADB|FORMAT|AUTH)\b|database disk image is malformed|Cannot create table|no such function|no such collation|Failed to open'
pms_logs="$(docker logs "$PMS_CONTAINER" 2>&1 || true)"
scan_bad_signal "$pms_logs" "$bad_signal_re" "FATAL: PMS logs contain SQLite/runtime bad signal" || exit 1
assert_marker_present "$pms_logs" '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "FATAL: PMS logs missing SQLITE_TRACE_STMT observability marker" || exit 1
if ! pms_open_marker_count="$(assert_open_marker_count "$pms_logs" "PMS")"; then
  exit 1
fi
echo "PMS_OPEN_MARKER_COUNT(unpatched)=${pms_open_marker_count}"
assert_no_startup_config_rc21 "$pms_logs" "PMS" || exit 1
