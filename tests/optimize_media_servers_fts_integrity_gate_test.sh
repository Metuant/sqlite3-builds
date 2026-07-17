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

assert_not_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) fail "$message" "not contains [$needle]" "$haystack" ;;
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
db_arg="${1:-}"
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
  case "$arg" in
    *"PRAGMA integrity_check;"*) printf '%s\n' "$db_arg" >> "$FTS_INTEGRITY_CALL_LOG" ;;
  esac
done

if [ -n "${SOURCE_INTEGRITY_HICCUP_DB:-}" ] && [ "$db_arg" = "$SOURCE_INTEGRITY_HICCUP_DB" ]; then
  case "$sql_text" in
    *"__sqlite3_builds_source_integrity_begin_v1__"*)
      case "${SOURCE_INTEGRITY_HICCUP_MODE:-execute}" in
        execute)
          printf 'simulated unrelated source integrity execution failure\n' >&2
          exit 70
          ;;
        interpret)
          printf 'unframed checker output\n'
          exit 0
          ;;
      esac
      ;;
  esac
fi

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
export FTS_INTEGRITY_CALL_LOG="$tmp/integrity-calls.log"
: > "$FTS_INTEGRITY_CALL_LOG"

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
DELETE FROM fts4_tag_titles_icu
WHERE docid IN (
  SELECT rowid FROM tags
  WHERE tag GLOB '*://*' OR tag IS NULL OR trim(tag)=''
);
EOF_SQL
}

create_btree_corrupt_db() {
  local db decoy_root
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA page_size=4096;
CREATE TABLE victim(id INTEGER PRIMARY KEY, payload TEXT);
CREATE TABLE decoy(id INTEGER PRIMARY KEY, payload TEXT);
CREATE TABLE fixture_proof(expected_victim_rows INTEGER NOT NULL);
INSERT INTO fixture_proof VALUES(5000);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x+1 FROM seq WHERE x<5000
)
INSERT INTO victim
SELECT x, printf('%0800d', x) FROM seq;
EOF_SQL
  decoy_root="$("$real_sqlite" "$db" "SELECT rootpage FROM sqlite_schema WHERE type='table' AND name='decoy';")"
  "$real_sqlite" "$db" ".dbconfig defensive off" \
    "PRAGMA writable_schema=ON; UPDATE sqlite_schema SET rootpage=${decoy_root} WHERE type='table' AND name='victim'; PRAGMA writable_schema=OFF;" \
    >/dev/null
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

lossy_source="$tmp/lossy-proof-source.db"
lossy_staged="$tmp/lossy-proof-staged.db"
create_btree_corrupt_db "$lossy_source"
lossy_integrity="$("$real_sqlite" "$lossy_source" "PRAGMA integrity_check;" | tr -d '\r')"
assert_contains "$lossy_integrity" "2nd reference to page" "lossy fixture demonstrates cross-btree page reuse"
assert_contains "$lossy_integrity" "never used" "lossy fixture demonstrates unreachable pages"
assert_eq "5000" "$("$real_sqlite" "$lossy_source" "SELECT expected_victim_rows FROM fixture_proof;")" "lossy fixture expected victim rows"
assert_eq "0" "$("$real_sqlite" "$lossy_source" "SELECT count(*) FROM victim;")" "lossy fixture source-visible victim rows"
"$real_sqlite" "$lossy_source" "VACUUM INTO '$lossy_staged';"
assert_eq "ok" "$("$real_sqlite" "$lossy_staged" "PRAGMA integrity_check;")" "lossy fixture staged integrity"
assert_eq "5000" "$("$real_sqlite" "$lossy_staged" "SELECT expected_victim_rows FROM fixture_proof;")" "lossy fixture staged expected victim rows"
assert_eq "0" "$("$real_sqlite" "$lossy_staged" "SELECT count(*) FROM victim;")" "lossy fixture staged lost victim rows"
printf 'NON-VACUITY PROOF vacuum-loss: expected_rows=5000 source_visible_rows=0 staged_integrity=ok staged_visible_rows=0\n'

btree_corrupt_source="$tmp/btree-corrupt-source.db"
btree_corrupt_backup_dir="$tmp/btree-corrupt-backup"
mkdir -p "$btree_corrupt_backup_dir"
create_btree_corrupt_db "$btree_corrupt_source"
btree_corrupt_hash_before="$(sha256_file "$btree_corrupt_source")"
: > "$FTS_INTEGRITY_CALL_LOG"
run_rebuild_capture btree-corrupt sqlite3 "$btree_corrupt_source" "$btree_corrupt_backup_dir" 4096 NONE "" "" ""
assert_eq 1 "$(cat "$tmp/btree-corrupt.rc")" "source b-tree corruption pipeline rc"
btree_corrupt_err="$(cat "$tmp/btree-corrupt.err")"
assert_contains "$btree_corrupt_err" "ERROR: source DB failed integrity_check" "source b-tree corruption diagnostic"
assert_contains "$btree_corrupt_err" "2nd reference to page" "source b-tree corruption detail"
assert_eq "$btree_corrupt_hash_before" "$(sha256_file "$btree_corrupt_source")" "source b-tree corruption live hash"
assert_eq "0" "$(find "$btree_corrupt_backup_dir" -name 'btree-corrupt-source.db-*.original' -print | wc -l | tr -d ' ')" "source b-tree corruption backup publication count"
[ ! -e "$btree_corrupt_source.new" ] || fail "source b-tree corruption staged cleanup" "no staged file" "$btree_corrupt_source.new exists"
assert_eq "1" "$(wc -l < "$FTS_INTEGRITY_CALL_LOG" | tr -d ' ')" "source b-tree corruption source integrity invocation count"
printf 'NON-VACUITY PROOF source-btree: maintenance_rc=1 diagnostic=[ERROR: source DB failed integrity_check] detail=[2nd reference to page] live_hash_preserved=yes backup_count=0 staged_absent=yes\n'

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
: > "$FTS_INTEGRITY_CALL_LOG"
run_rebuild_capture source-repair sqlite3 "$corrupt_source" "$backup_dir" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/source-repair.rc")" "source FTS repair pipeline rc"
assert_eq "2" "$(wc -l < "$FTS_INTEGRITY_CALL_LOG" | tr -d ' ')" "source FTS repair source and final integrity gates"
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
: > "$FTS_INTEGRITY_CALL_LOG"
run_rebuild_capture staged-repair sqlite3 "$staged_source" "$tmp" 4096 NONE "" "" ""
unset CORRUPT_STAGED_AFTER_VACUUM CORRUPT_STAGED_DB
assert_eq 0 "$(cat "$tmp/staged-repair.rc")" "staged FTS repair pipeline rc"
assert_eq "2" "$(wc -l < "$FTS_INTEGRITY_CALL_LOG" | tr -d ' ')" "staged FTS repair source and final integrity gates"
[ ! -e "$staged_source.new" ] || fail "staged FTS repair staged cleanup" "no staged file" "$staged_source.new exists"
staged_hash_after="$(sha256_file "$staged_source")"
[ "$staged_hash_after" != "$staged_hash_before" ] || fail "staged FTS repair live hash" "changed hash" "$staged_hash_after"

recurate_source="$tmp/recurate-source.db"
create_plex_tag_titles_icu_db "$recurate_source"
assert_eq "0" "$(bad_plex_tag_doc_count "$recurate_source")" "curated Plex source URI and empty FTS doc count"
assert_eq "2" "$(name_plex_tag_doc_count "$recurate_source")" "curated Plex source name FTS doc count"
curated_whole_integrity="$("$real_sqlite" "$recurate_source" "PRAGMA integrity_check;" | tr -d '\r')"
assert_eq "malformed inverted index for FTS4 table main.fts4_tag_titles_icu" "$curated_whole_integrity" "curated Plex source whole-DB xIntegrity result"
curated_hash_before="$(sha256_file "$recurate_source")"
set +e
run_source_integrity_gate sqlite3 "$recurate_source" >"$tmp/curated-source-gate.out" 2>"$tmp/curated-source-gate.err"
rc=$?
set -e
assert_eq 0 "$rc" "curated Plex source integrity gate rc"
assert_contains "$(cat "$tmp/curated-source-gate.err")" "accepted curated FTS4 exception" "curated Plex source integrity diagnostic"
assert_eq "$curated_hash_before" "$(sha256_file "$recurate_source")" "curated Plex source gate live hash"
assert_eq "0" "$(bad_plex_tag_doc_count "$recurate_source")" "curated Plex source gate retains excluded docs"
assert_eq "2" "$(name_plex_tag_doc_count "$recurate_source")" "curated Plex source gate retains name docs"
: > "$FTS_INTEGRITY_CALL_LOG"
run_rebuild_capture recurate sqlite3 "$recurate_source" "$tmp" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/recurate.rc")" "Plex pipeline with re-curation disabled rc"
[ ! -e "$recurate_source.new" ] || fail "Plex pipeline with re-curation disabled staged cleanup" "no staged file" "$recurate_source.new exists"
assert_eq "4" "$(bad_plex_tag_doc_count "$recurate_source")" "Plex pipeline staged rebuild restores URI and empty FTS documents"
assert_eq "2" "$(name_plex_tag_doc_count "$recurate_source")" "Plex pipeline retains name FTS documents"
assert_eq "2" "$(wc -l < "$FTS_INTEGRITY_CALL_LOG" | tr -d ' ')" "curated Plex pipeline source and final integrity gates"
printf 'NON-VACUITY PROOF curated-fts4: whole_result=[malformed inverted index for FTS4 table main.fts4_tag_titles_icu] source_gate_rc=0 live_hash_preserved=yes gate_bad_docs=0 gate_name_docs=2 pipeline_rc=0 post_pipeline_bad_docs=4\n'

source_hiccup_execute="$tmp/source-hiccup-execute.db"
create_healthy_fts_db "$source_hiccup_execute"
export SOURCE_INTEGRITY_HICCUP_DB="$source_hiccup_execute"
export SOURCE_INTEGRITY_HICCUP_MODE=execute
run_rebuild_capture source-hiccup-execute sqlite3 "$source_hiccup_execute" "$tmp" 4096 NONE "" "" ""
unset SOURCE_INTEGRITY_HICCUP_DB SOURCE_INTEGRITY_HICCUP_MODE
assert_eq 0 "$(cat "$tmp/source-hiccup-execute.rc")" "source integrity execution hiccup pipeline rc"
assert_contains "$(cat "$tmp/source-hiccup-execute.err")" "source integrity_check failed to run" "source integrity execution hiccup diagnostic"
assert_contains "$(cat "$tmp/source-hiccup-execute.err")" "corruption was not demonstrated" "source integrity execution hiccup classification"
assert_eq "1" "$("$real_sqlite" "$source_hiccup_execute" "SELECT count(*) FROM fts4_docs;")" "source integrity execution hiccup retained row"
printf 'NON-VACUITY PROOF checker-execution-hiccup: maintenance_rc=0 classification=[corruption was not demonstrated] retained_rows=1\n'

source_hiccup_interpret="$tmp/source-hiccup-interpret.db"
create_healthy_fts_db "$source_hiccup_interpret"
export SOURCE_INTEGRITY_HICCUP_DB="$source_hiccup_interpret"
export SOURCE_INTEGRITY_HICCUP_MODE=interpret
run_rebuild_capture source-hiccup-interpret sqlite3 "$source_hiccup_interpret" "$tmp" 4096 NONE "" "" ""
unset SOURCE_INTEGRITY_HICCUP_DB SOURCE_INTEGRITY_HICCUP_MODE
assert_eq 0 "$(cat "$tmp/source-hiccup-interpret.rc")" "source integrity interpretation hiccup pipeline rc"
assert_contains "$(cat "$tmp/source-hiccup-interpret.err")" "source integrity_check returned uninterpretable output" "source integrity interpretation hiccup diagnostic"
assert_contains "$(cat "$tmp/source-hiccup-interpret.err")" "corruption was not demonstrated" "source integrity interpretation hiccup classification"
assert_not_contains "$(cat "$tmp/source-hiccup-interpret.err")" "source DB failed integrity_check" "source integrity interpretation hiccup is not corruption"
assert_eq "1" "$("$real_sqlite" "$source_hiccup_interpret" "SELECT count(*) FROM fts4_docs;")" "source integrity interpretation hiccup retained row"
printf 'NON-VACUITY PROOF checker-interpretation-hiccup: maintenance_rc=0 classification=[corruption was not demonstrated] false_corruption_diagnostic=absent retained_rows=1\n'

rebuild_fail_source="$tmp/rebuild-fail-source.db"
create_external_fts_db "$rebuild_fail_source"
rebuild_fail_hash_before="$(sha256_file "$rebuild_fail_source")"
export FAIL_REBUILD=1
: > "$FTS_INTEGRITY_CALL_LOG"
run_rebuild_capture staged-rebuild-fail sqlite3 "$rebuild_fail_source" "$tmp" 4096 NONE "" "" ""
unset FAIL_REBUILD
assert_eq 1 "$(cat "$tmp/staged-rebuild-fail.rc")" "staged FTS rebuild failure pipeline rc"
assert_eq "1" "$(wc -l < "$FTS_INTEGRITY_CALL_LOG" | tr -d ' ')" "staged FTS rebuild failure has only the source integrity gate"
assert_eq "$rebuild_fail_hash_before" "$(sha256_file "$rebuild_fail_source")" "staged FTS rebuild failure live hash"
[ ! -e "$rebuild_fail_source.new" ] || fail "staged FTS rebuild failure staged cleanup" "no staged file" "$rebuild_fail_source.new exists"
assert_contains "$(cat "$tmp/staged-rebuild-fail.err")" "staged FTS rebuild failed" "staged FTS rebuild failure diagnostic"

printf 'optimize_media_servers FTS integrity gate tests passed\n'
