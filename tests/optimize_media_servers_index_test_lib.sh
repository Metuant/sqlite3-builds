#!/usr/bin/env bash

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

assert_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) fail "$message" "contains [$needle]" "$haystack" ;;
  esac
}

assert_not_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) fail "$message" "not contains [$needle]" "$haystack" ;;
  esac
}

assert_int_gt() {
  local actual floor message
  actual="$1"
  floor="$2"
  message="$3"
  case "$actual" in
    ''|*[!0-9]*) fail "$message" "integer > $floor" "$actual" ;;
  esac
  [ "$actual" -gt "$floor" ] || fail "$message" "> $floor" "$actual"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

index_test_install_sqlite_wrapper() {
  cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
db_arg="${1:-}"
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

if [ "${INDEX_TEST_FAIL_SQL:-0}" = "1" ] && [ -n "${INDEX_TEST_FAIL_NEEDLE:-}" ]; then
  case "$sql_text" in
    *"$INDEX_TEST_FAIL_NEEDLE"*)
      printf 'injected staged maintenance failure\n' >&2
      exit 1
      ;;
  esac
fi

if [ "${INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY:-0}" = "1" ] &&
   [ -n "${INDEX_TEST_CORRUPT_STAGED_DB:-}" ] &&
   [ "$db_arg" = "$INDEX_TEST_CORRUPT_STAGED_DB" ]; then
  case "$sql_text" in
    *"PRAGMA integrity_check;"*)
      printf 'not a sqlite database' > "$db_arg"
      ;;
  esac
fi

exec "$REAL_SQLITE" "$@"
EOF_SQLITE
  chmod +x "$tmp/bin/sqlite3"
}

index_test_init() {
  local slug tmp_parent
  slug="$1"

  repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[1]}")/.." && pwd -P)"
  cd "$repo_root"

  . ./scripts/optimize_media_servers.sh

  tmp_parent="${TMPDIR:-/tmp}"
  tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-${slug}.XXXXXX" 2>/dev/null || mktemp -d "/tmp/sqlite3-optimize-${slug}.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT

  real_sqlite="$(command -v sqlite3 || true)"
  [ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

  mkdir -p "$tmp/bin"
  index_test_install_sqlite_wrapper
  PATH="$tmp/bin:$PATH"
  export PATH REAL_SQLITE="$real_sqlite"

  backup_dir="$tmp/backups"
  mkdir -p "$backup_dir"
}

index_test_run_rebuild_capture() {
  local name binary db page_size sanity optimize_sql rc
  name="$1"
  binary="$2"
  db="$3"
  page_size="$4"
  sanity="$5"
  optimize_sql="$6"

  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    rebuild_db_vacuum_into "$binary" "$db" "$backup_dir" "$page_size" NONE "$sanity" "$optimize_sql" ""
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

index_test_run_plex_optimize_capture() {
  local name db_dir db_file sanity pre_swap_hook rc
  name="$1"
  db_dir="$2"
  db_file="$3"
  sanity="$4"
  pre_swap_hook="${5:-}"

  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    PLEX_BINARY="sqlite3"
    PLEX_DATABASES_PATH="$db_dir"
    PLEX_INSTANCE="plex-index-fixture"
    BACKUP_PATH="$backup_dir"
    optimize_plex_db "$db_file" "$sanity" "$pre_swap_hook"
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

index_test_index_count() {
  local db index_name
  db="$1"
  index_name="$2"

  case "$index_name" in
    *"'"*) fail "index name quoting" "no single quote" "$index_name" ;;
  esac

  "$real_sqlite" "$db" "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='${index_name}';"
}
