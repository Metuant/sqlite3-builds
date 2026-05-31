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

GENERIC_ARTIFACT_DIR_ABS="$(realpath "$generic_artifact_dir")"
if [ ! -f "$GENERIC_ARTIFACT_DIR_ABS/libsqlite3.so" ]; then
  echo "FATAL: missing extracted generic library at $GENERIC_ARTIFACT_DIR_ABS/libsqlite3.so"
  exit 1
fi

EMBY_CONFIG_DIR="$(mktemp -d)"
EMBY_CONTAINER="emby-obs-off-smoke-${run_id}-${arch_suffix}"
# shellcheck disable=SC2154
trap 'status=$?; docker logs "$EMBY_CONTAINER" 2>&1 || true; docker stop "$EMBY_CONTAINER" 2>/dev/null || true; exit "$status"' EXIT

docker run --detach \
  --name "$EMBY_CONTAINER" \
  -e PUID=1000 \
  -e PGID=1000 \
  -e SQLITE3_DISABLE_OBSERVABILITY=1 \
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

docker stop "$EMBY_CONTAINER" >/dev/null 2>&1 || true
emby_logs="$(docker logs "$EMBY_CONTAINER" 2>&1 || true)"
assert_marker_absent "$emby_logs" '\[sqlite3-builds-obs\]' "FATAL: Emby observability kill switch emitted marker lines" || exit 1
