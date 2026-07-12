#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-plex-blob-gate.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-plex-blob-gate.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  local message expected actual
  message="$1"
  expected="$2"
  actual="$3"
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$message" "$expected" "$actual" >&2
  exit 1
}

assert_eq() {
  local expected actual message
  expected="$1"
  actual="$2"
  message="$3"
  [ "$actual" = "$expected" ] || fail "$message" "$expected" "$actual"
}

optimize_plex_db() {
  printf '%s|%s|%s|%s\n' "$1" "${2:-}" "${3:-}" "${4:-}" >> "$PLEX_BLOB_GATE_LOG"
}

prepare_plex_tree() {
  local plex_root
  plex_root="$1"
  mkdir -p \
    "$plex_root/Cache/PhotoTranscoder" \
    "$plex_root/Crash Reports" \
    "$plex_root/Codecs" \
    "$plex_root/Plug-in Support/Caches" \
    "$plex_root/Plug-in Support/Databases"
}

run_blob_gate_case() {
  local name process_flag trim_flag expected rc
  local plex_instance plex_path plex_databases_path
  name="$1"
  process_flag="$2"
  trim_flag="$3"
  expected="$4"

  plex_instance="plex-blob-gate"
  plex_path="$tmp/$name/Library/Application Support/Plex Media Server"
  plex_databases_path="$plex_path/Plug-in Support/Databases"
  prepare_plex_tree "$plex_path"
  PLEX_BLOB_GATE_LOG="$tmp/$name.log"
  export PLEX_BLOB_GATE_LOG
  : > "$PLEX_BLOB_GATE_LOG"

  case "$process_flag" in
    __unset__)
      unset PLEX_PROCESS_BLOB_DB
      ;;
    *)
      PLEX_PROCESS_BLOB_DB="$process_flag"
      ;;
  esac
  case "$trim_flag" in
    __unset__)
      unset PLEX_TRIM_FINISHED_SEASON_BLOBS
      ;;
    *)
      PLEX_TRIM_FINISHED_SEASON_BLOBS="$trim_flag"
      ;;
  esac

  set +e
  run_plex_maintenance_safely >"$tmp/$name.out" 2>"$tmp/$name.err"
  rc=$?
  set -e

  assert_eq 0 "$rc" "$name run_plex_maintenance_safely rc"
  assert_eq "$expected" "$(cat "$PLEX_BLOB_GATE_LOG")" "$name optimized database list"
}

main_log="${_PLEX_DB}|SELECT 1 FROM versioned_metadata_items LIMIT 1;|try_deflate_plex_statistics_bandwidth|"
run_blob_gate_case process-unset __unset__ 1 "$main_log"
run_blob_gate_case process-zero 0 1 "$main_log"
run_blob_gate_case process-other other 1 "$main_log"
run_blob_gate_case trim-unset 1 __unset__ "$main_log
${_PLEX_BLOB_DB}|||"
run_blob_gate_case trim-zero 1 0 "$main_log
${_PLEX_BLOB_DB}|||"
run_blob_gate_case trim-other 1 other "$main_log
${_PLEX_BLOB_DB}|||"
run_blob_gate_case both-enabled 1 1 "$main_log
${_PLEX_BLOB_DB}|||try_trim_plex_finished_season_blobs"

printf 'optimize_media_servers Plex blob gate tests passed\n'
