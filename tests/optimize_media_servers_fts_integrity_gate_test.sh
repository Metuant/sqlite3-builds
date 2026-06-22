#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-fts-integrity.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-fts-integrity.XXXXXX)"
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

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
for arg in "$@"; do
  case "$arg" in
    *"VACUUM INTO "*)
      "$REAL_SQLITE" "$@"
      rc=$?
      if [ "$rc" -eq 0 ] && [ "${CORRUPT_STAGED_AFTER_VACUUM:-0}" = "1" ]; then
        "$REAL_SQLITE" "$CORRUPT_STAGED_DB" "UPDATE docs SET body='stage-mismatch' WHERE id=1;"
      fi
      exit "$rc"
      ;;
  esac
done
exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

create_healthy_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE VIRTUAL TABLE fts4_docs USING fts4(body);
INSERT INTO fts4_docs(rowid, body) VALUES(1, 'alpha');
CREATE VIRTUAL TABLE fts5_docs USING fts5(body);
INSERT INTO fts5_docs(rowid, body) VALUES(1, 'beta');
EOF_SQL
}

create_external_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE docs(id INTEGER PRIMARY KEY, body TEXT);
CREATE VIRTUAL TABLE fts5_external USING fts5(body, content='docs', content_rowid='id');
INSERT INTO docs(id, body) VALUES(1, 'alpha');
INSERT INTO fts5_external(rowid, body) VALUES(1, 'alpha');
EOF_SQL
}

run_rebuild_capture() {
  local name binary db backup_dir page_size auto_vacuum sanity post_sql hook
  name="$1"
  binary="$2"
  db="$3"
  backup_dir="$4"
  page_size="$5"
  auto_vacuum="$6"
  sanity="$7"
  post_sql="$8"
  hook="$9"
  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    rebuild_db_vacuum_into "$binary" "$db" "$backup_dir" "$page_size" "$auto_vacuum" "$sanity" "$post_sql" "$hook"
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

healthy="$tmp/healthy.db"
create_healthy_fts_db "$healthy"
run_fts_integrity_gate sqlite3 "$healthy" "source"

corrupt_source="$tmp/corrupt-source.db"
create_external_fts_db "$corrupt_source"
"$real_sqlite" "$corrupt_source" "UPDATE docs SET body='source-mismatch' WHERE id=1;"
source_hash_before="$(sha256_file "$corrupt_source")"

set +e
run_fts_integrity_gate sqlite3 "$corrupt_source" "source" >"$tmp/source-gate.out" 2>"$tmp/source-gate.err"
rc=$?
set -e
assert_eq 1 "$rc" "source FTS integrity gate rc on mismatched external content"
assert_contains "$(cat "$tmp/source-gate.err")" "ERROR: source FTS integrity-check failed" "source FTS integrity diagnostic"

backup_dir="$tmp/source-backup"
mkdir -p "$backup_dir"
run_rebuild_capture source-abort sqlite3 "$corrupt_source" "$backup_dir" 4096 NONE "" "" ""
assert_eq 1 "$(cat "$tmp/source-abort.rc")" "source FTS pipeline abort rc"
assert_eq "$source_hash_before" "$(sha256_file "$corrupt_source")" "source FTS abort live hash"
[ ! -e "$corrupt_source.new" ] || fail "source FTS abort staged cleanup" "no staged file" "$corrupt_source.new exists"
assert_contains "$(cat "$tmp/source-abort.err")" "source FTS integrity gate failed" "source FTS pipeline diagnostic"

staged_source="$tmp/staged-source.db"
create_external_fts_db "$staged_source"
staged_hash_before="$(sha256_file "$staged_source")"
export CORRUPT_STAGED_AFTER_VACUUM=1
export CORRUPT_STAGED_DB="$staged_source.new"
run_rebuild_capture staged-abort sqlite3 "$staged_source" "$tmp" 4096 NONE "" "" ""
unset CORRUPT_STAGED_AFTER_VACUUM CORRUPT_STAGED_DB
assert_eq 1 "$(cat "$tmp/staged-abort.rc")" "staged FTS pipeline abort rc"
assert_eq "$staged_hash_before" "$(sha256_file "$staged_source")" "staged FTS abort live hash"
[ ! -e "$staged_source.new" ] || fail "staged FTS abort staged cleanup" "no staged file" "$staged_source.new exists"
assert_contains "$(cat "$tmp/staged-abort.err")" "staged FTS integrity gate failed" "staged FTS pipeline diagnostic"

printf 'optimize_media_servers FTS integrity gate tests passed\n'
