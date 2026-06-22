#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-warn-pipefail.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-warn-pipefail.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  local message expected actual
  message="$1"
  expected="$2"
  actual="$3"
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$message" "$expected" "$actual" >&2
  exit 1
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

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3-fails" <<'EOF_SQLITE'
#!/usr/bin/env bash
printf 'simulated sqlite wrapper failure\n' >&2
exit 83
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3-fails"
PATH="$tmp/bin:$PATH"
export PATH

if ! run_foreign_key_check_warn sqlite3-fails "$tmp/missing.db" >"$tmp/fk.out" 2>"$tmp/fk.err"; then
  fail "foreign key warn helper rc under pipefail" "0" "nonzero"
fi
assert_contains "$(cat "$tmp/fk.err")" "WARNING: foreign_key_check failed to run" "foreign key warn helper diagnostic"

if ! run_post_swap_fts_maintenance sqlite3-fails "$tmp/missing.db" >"$tmp/fts.out" 2>"$tmp/fts.err"; then
  fail "post-swap FTS helper rc under pipefail" "0" "nonzero"
fi
assert_contains "$(cat "$tmp/fts.err")" "WARNING: post-swap FTS discovery failed" "post-swap FTS warn helper diagnostic"

if ! try_deflate_plex_statistics_bandwidth sqlite3-fails "$tmp/missing.db" 90 >"$tmp/deflate.out" 2>"$tmp/deflate.err"; then
  fail "deflate warn helper rc under pipefail" "0" "nonzero"
fi
assert_contains "$(cat "$tmp/deflate.err")" "WARNING: could not check for statistics_bandwidth" "deflate warn helper diagnostic"

printf 'optimize_media_servers warn helper pipefail tests passed\n'
