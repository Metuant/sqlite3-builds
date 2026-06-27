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
if [ "${FAIL_REBUILD:-0}" = "1" ]; then
  for arg in "$@"; do
    case "$arg" in
      *"VALUES('rebuild')"*) exit 1 ;;
    esac
  done
fi

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

create_plex_tag_titles_icu_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE tags(id INTEGER PRIMARY KEY, tag TEXT);
INSERT INTO tags(id, tag) VALUES
  (1, 'Music'),
  (2, 'Movies'),
  (3, 'imdb://tt0133093'),
  (4, 'tmdb://603'),
  (5, ''),
  (6, NULL);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag, content='tags');
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('rebuild');
EOF_SQL
}

bad_plex_tag_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag GLOB '*://*' OR t.tag IS NULL OR trim(t.tag)='';"
}

name_plex_tag_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag IN ('Music','Movies');"
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

set +e
run_fts_integrity_gate sqlite3 "$corrupt_source" "source" "warn" >"$tmp/source-gate-warn.out" 2>"$tmp/source-gate-warn.err"
rc=$?
set -e
assert_eq 0 "$rc" "source FTS warn gate rc on mismatched external content"
source_warn_err="$(cat "$tmp/source-gate-warn.err")"
assert_contains "$source_warn_err" "WARNING:" "source FTS warn diagnostic level"
assert_contains "$source_warn_err" "continuing" "source FTS warn diagnostic continuation"

rebuild_unit="$tmp/rebuild-unit.db"
create_external_fts_db "$rebuild_unit"
"$real_sqlite" "$rebuild_unit" "UPDATE docs SET body='rebuild-unit-mismatch' WHERE id=1;"
set +e
run_fts_rebuild_hard sqlite3 "$rebuild_unit" >"$tmp/rebuild-unit.out" 2>"$tmp/rebuild-unit.err"
rc=$?
set -e
assert_eq 0 "$rc" "FTS rebuild hard rc on mismatched external content"

rebuild_fail_unit="$tmp/rebuild-fail-unit.db"
create_external_fts_db "$rebuild_fail_unit"
"$real_sqlite" "$rebuild_fail_unit" "UPDATE docs SET body='rebuild-fail-mismatch' WHERE id=1;"
export FAIL_REBUILD=1
set +e
run_fts_rebuild_hard sqlite3 "$rebuild_fail_unit" >"$tmp/rebuild-fail-unit.out" 2>"$tmp/rebuild-fail-unit.err"
rc=$?
set -e
unset FAIL_REBUILD
assert_eq 1 "$rc" "FTS rebuild hard rc when rebuild SQL fails"
assert_contains "$(cat "$tmp/rebuild-fail-unit.err")" "FTS rebuild failed" "FTS rebuild hard failure diagnostic"

backup_dir="$tmp/source-backup"
mkdir -p "$backup_dir"
run_rebuild_capture source-repair sqlite3 "$corrupt_source" "$backup_dir" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/source-repair.rc")" "source FTS repair pipeline rc"
source_backup_count="$(find "$backup_dir" -name 'corrupt-source.db-*.original' -print | wc -l | tr -d ' ')"
assert_eq "1" "$source_backup_count" "source FTS repair backup count"
[ ! -e "$corrupt_source.new" ] || fail "source FTS repair staged cleanup" "no staged file" "$corrupt_source.new exists"
source_hash_after="$(sha256_file "$corrupt_source")"
[ "$source_hash_after" != "$source_hash_before" ] || fail "source FTS repair live hash" "changed hash" "$source_hash_after"

staged_source="$tmp/staged-source.db"
create_external_fts_db "$staged_source"
staged_hash_before="$(sha256_file "$staged_source")"
export CORRUPT_STAGED_AFTER_VACUUM=1
export CORRUPT_STAGED_DB="$staged_source.new"
run_rebuild_capture staged-repair sqlite3 "$staged_source" "$tmp" 4096 NONE "" "" ""
unset CORRUPT_STAGED_AFTER_VACUUM CORRUPT_STAGED_DB
assert_eq 0 "$(cat "$tmp/staged-repair.rc")" "staged FTS repair pipeline rc"
[ ! -e "$staged_source.new" ] || fail "staged FTS repair staged cleanup" "no staged file" "$staged_source.new exists"
staged_hash_after="$(sha256_file "$staged_source")"
[ "$staged_hash_after" != "$staged_hash_before" ] || fail "staged FTS repair live hash" "changed hash" "$staged_hash_after"

recurate_source="$tmp/recurate-source.db"
create_plex_tag_titles_icu_db "$recurate_source"
assert_eq "4" "$(bad_plex_tag_doc_count "$recurate_source")" "pre-pipeline Plex tag-title bad doc count"
run_rebuild_capture recurate sqlite3 "$recurate_source" "$tmp" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/recurate.rc")" "Plex FTS re-curation pipeline rc"
[ ! -e "$recurate_source.new" ] || fail "Plex FTS re-curation staged cleanup" "no staged file" "$recurate_source.new exists"
assert_eq "0" "$(bad_plex_tag_doc_count "$recurate_source")" "Plex FTS re-curation bad doc count"
assert_eq "2" "$(name_plex_tag_doc_count "$recurate_source")" "Plex FTS re-curation name doc count"

rebuild_fail_source="$tmp/rebuild-fail-source.db"
create_external_fts_db "$rebuild_fail_source"
rebuild_fail_hash_before="$(sha256_file "$rebuild_fail_source")"
export FAIL_REBUILD=1
run_rebuild_capture staged-rebuild-fail sqlite3 "$rebuild_fail_source" "$tmp" 4096 NONE "" "" ""
unset FAIL_REBUILD
assert_eq 1 "$(cat "$tmp/staged-rebuild-fail.rc")" "staged FTS rebuild failure pipeline rc"
assert_eq "$rebuild_fail_hash_before" "$(sha256_file "$rebuild_fail_source")" "staged FTS rebuild failure live hash"
[ ! -e "$rebuild_fail_source.new" ] || fail "staged FTS rebuild failure staged cleanup" "no staged file" "$rebuild_fail_source.new exists"
assert_contains "$(cat "$tmp/staged-rebuild-fail.err")" "staged FTS rebuild failed" "staged FTS rebuild failure diagnostic"

printf 'optimize_media_servers FTS integrity gate tests passed\n'
