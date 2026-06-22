#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-fts-maint.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-fts-maint.XXXXXX)"
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

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done
case "$sql_text" in
  *"VALUES('rebuild')"*|*"VALUES('optimize')"*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g; s/[[:space:]]$//' >> "$SQLITE_CALL_LOG"
    printf '\n' >> "$SQLITE_CALL_LOG"
    ;;
esac
if [ -n "${FAIL_REBUILD_TABLE:-}" ]; then
  case "$sql_text" in
    *"INSERT INTO \"${FAIL_REBUILD_TABLE}\"(\"${FAIL_REBUILD_TABLE}\") VALUES('rebuild');"*)
      printf 'simulated rebuild failure for %s\n' "$FAIL_REBUILD_TABLE" >&2
      exit 64
      ;;
  esac
fi
exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

create_fts_fixture() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE content_docs(id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO content_docs(id, body) VALUES(1, 'external alpha');
CREATE VIRTUAL TABLE fts4_docs USING fts4(body);
INSERT INTO fts4_docs(rowid, body) VALUES(1, 'fts4 alpha');
CREATE VIRTUAL TABLE fts5_contentless USING fts5(body, content='');
INSERT INTO fts5_contentless(rowid, body) VALUES(1, 'contentless alpha');
CREATE VIRTUAL TABLE fts5_external USING fts5(body, content='content_docs', content_rowid='id');
INSERT INTO fts5_external(rowid, body) VALUES(1, 'external alpha');
CREATE VIRTUAL TABLE fts5_normal USING fts5(body);
INSERT INTO fts5_normal(rowid, body) VALUES(1, 'normal alpha');
EOF_SQL
}

db="$tmp/fts-maint.db"
create_fts_fixture "$db"
export SQLITE_CALL_LOG="$tmp/positive.log"
: > "$SQLITE_CALL_LOG"
run_post_swap_fts_maintenance sqlite3 "$db" >"$tmp/positive.out" 2>"$tmp/positive.err"
assert_eq "" "$(cat "$tmp/positive.err")" "positive FTS maintenance stderr"

expected="$tmp/expected.log"
cat > "$expected" <<'EOF_EXPECTED'
/fts-maint.db INSERT INTO "fts4_docs"("fts4_docs") VALUES('rebuild');
/fts-maint.db INSERT INTO "fts4_docs"("fts4_docs") VALUES('optimize');
/fts-maint.db INSERT INTO "fts5_contentless"("fts5_contentless") VALUES('optimize');
/fts-maint.db INSERT INTO "fts5_external"("fts5_external") VALUES('rebuild');
/fts-maint.db INSERT INTO "fts5_external"("fts5_external") VALUES('optimize');
/fts-maint.db INSERT INTO "fts5_normal"("fts5_normal") VALUES('rebuild');
/fts-maint.db INSERT INTO "fts5_normal"("fts5_normal") VALUES('optimize');
EOF_EXPECTED
actual_normalized="$tmp/actual-normalized.log"
sed "s#${tmp}##g" "$SQLITE_CALL_LOG" > "$actual_normalized"
assert_eq "$(cat "$expected")" "$(cat "$actual_normalized")" "post-swap FTS rebuild/optimize call order"
assert_not_contains "$(cat "$actual_normalized")" 'INSERT INTO "fts5_contentless"("fts5_contentless") VALUES('\''rebuild'\'');' "contentless FTS5 rebuild skip"

fail_db="$tmp/fts-maint-fail.db"
create_fts_fixture "$fail_db"
export SQLITE_CALL_LOG="$tmp/failure.log"
export FAIL_REBUILD_TABLE="fts5_external"
: > "$SQLITE_CALL_LOG"
if ! run_post_swap_fts_maintenance sqlite3 "$fail_db" >"$tmp/failure.out" 2>"$tmp/failure.err"; then
  fail "post-swap FTS maintenance failure rc" "0" "nonzero"
fi
unset FAIL_REBUILD_TABLE
failure_err="$(cat "$tmp/failure.err")"
failure_log="$(cat "$SQLITE_CALL_LOG")"
assert_contains "$failure_err" "WARNING: post-swap FTS rebuild failed for fts5_external" "post-swap FTS rebuild warning"
assert_contains "$failure_log" 'INSERT INTO "fts5_normal"("fts5_normal") VALUES('\''optimize'\'');' "post-swap FTS continued after rebuild failure"
assert_not_contains "$failure_log" 'INSERT INTO "fts5_external"("fts5_external") VALUES('\''optimize'\'');' "failed rebuild skips optimize for that table"

printf 'optimize_media_servers FTS rebuild optimize tests passed\n'
