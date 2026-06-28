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
  printf '%s\n' "$1" >> "$PLEX_BLOB_GATE_LOG"
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
  local name flag expected rc
  local plex_instance plex_path plex_databases_path
  name="$1"
  flag="$2"
  expected="$3"

  plex_instance="plex-blob-gate"
  plex_path="$tmp/$name/Library/Application Support/Plex Media Server"
  plex_databases_path="$plex_path/Plug-in Support/Databases"
  prepare_plex_tree "$plex_path"
  PLEX_BLOB_GATE_LOG="$tmp/$name.log"
  export PLEX_BLOB_GATE_LOG
  : > "$PLEX_BLOB_GATE_LOG"

  case "$flag" in
    __unset__)
      unset PLEX_PROCESS_BLOB_DB
      ;;
    *)
      PLEX_PROCESS_BLOB_DB="$flag"
      ;;
  esac

  set +e
  run_plex_maintenance_safely >"$tmp/$name.out" 2>"$tmp/$name.err"
  rc=$?
  set -e

  assert_eq 0 "$rc" "$name run_plex_maintenance_safely rc"
  assert_eq "$expected" "$(cat "$PLEX_BLOB_GATE_LOG")" "$name optimized database list"
}

run_blob_gate_case omitted __unset__ "$_PLEX_DB"
run_blob_gate_case disabled 0 "$_PLEX_DB"
run_blob_gate_case enabled 1 "$_PLEX_DB
$_PLEX_BLOB_DB"

printf 'optimize_media_servers Plex blob gate tests passed\n'
