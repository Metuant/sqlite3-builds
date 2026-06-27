#!/usr/bin/env bash
set -euo pipefail

. ./tests/optimize_media_servers_index_test_lib.sh
index_test_init "emby-index"

EMBY_INDEX_NAME="idx_dshadow_mediaitems_parent_type"

create_mediaitems_db() {
  local db page_size
  db="$1"
  page_size="${2:-4096}"

  "$real_sqlite" "$db" <<EOF_SQL
PRAGMA page_size=${page_size};
VACUUM;
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE MediaItems(
  Id INTEGER PRIMARY KEY,
  ParentId INTEGER NOT NULL,
  Type INTEGER NOT NULL,
  Name TEXT
);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 5000
)
INSERT INTO MediaItems(Id, ParentId, Type, Name)
SELECT
  x,
  CASE WHEN x <= 4500 THEN x % 25 ELSE x END,
  CASE WHEN x % 20 = 0 THEN 8 ELSE 1 END,
  'item-' || x
FROM seq;
CREATE TABLE SyncJobs2(Id INTEGER PRIMARY KEY, Name TEXT);
INSERT INTO SyncJobs2(Id, Name) VALUES(1, 'fixture');
EOF_SQL
}

run_rebuild_capture() {
  local name db page_size optimize_sql
  name="$1"
  db="$2"
  page_size="$3"
  optimize_sql="$4"

  index_test_run_rebuild_capture "$name" sqlite3 "$db" "$page_size" "SELECT 1 FROM SyncJobs2 LIMIT 1;" "$optimize_sql"
}

index_count() {
  local db
  db="$1"
  index_test_index_count "$db" "$EMBY_INDEX_NAME"
}

mediaitems_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT Id FROM MediaItems WHERE ParentId=4600 AND Type=8;" | tr '\r\n' ' '
}

assert_eqp_uses_emby_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase EQP search"
  assert_contains "$eqp" "$EMBY_INDEX_NAME" "$phase EQP index"
  assert_not_contains "$eqp" "SCAN" "$phase EQP no scan"
}

optimize_sql="$(build_emby_optimize_sql)"

published="$tmp/published.db"
create_mediaitems_db "$published"
run_rebuild_capture published-first "$published" 4096 "$optimize_sql"
assert_eq 0 "$(cat "$tmp/published-first.rc")" "first Emby index pipeline rc"
assert_eq "1" "$(index_count "$published")" "published Emby index count"
stat1_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat1;")"
assert_int_gt "$stat1_count" 0 "published sqlite_stat1 row count"
stat1_mediaitems_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat1 WHERE tbl='MediaItems' OR idx='$EMBY_INDEX_NAME';")"
assert_int_gt "$stat1_mediaitems_count" 0 "published sqlite_stat1 MediaItems/index row count"
stat4_exists="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='sqlite_stat4';")"
if [ "$stat4_exists" = "1" ]; then
  stat4_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat4 WHERE tbl='MediaItems' OR idx='$EMBY_INDEX_NAME';")"
  printf 'sqlite_stat4 MediaItems/index rows: %s\n' "$stat4_count"
else
  printf 'SKIP: sqlite_stat4 unavailable in this sqlite3 build\n'
fi
assert_eqp_uses_emby_index "$(mediaitems_eqp "$published")" "published post-ANALYZE"

run_rebuild_capture published-second "$published" 4096 "$optimize_sql"
assert_eq 0 "$(cat "$tmp/published-second.rc")" "second Emby index pipeline rc"
assert_eq "" "$(cat "$tmp/published-second.err")" "second Emby index pipeline stderr"
assert_eq "1" "$(index_count "$published")" "idempotent Emby index count"

warn_db="$tmp/warn-not-abort.db"
create_mediaitems_db "$warn_db" 1024
assert_eq "1024" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort source page_size"
export INDEX_TEST_FAIL_SQL=1
export INDEX_TEST_FAIL_NEEDLE="$EMBY_INDEX_NAME"
run_rebuild_capture warn-not-abort "$warn_db" 4096 "$optimize_sql"
unset INDEX_TEST_FAIL_SQL INDEX_TEST_FAIL_NEEDLE
assert_eq 0 "$(cat "$tmp/warn-not-abort.rc")" "warn-not-abort rc"
assert_contains "$(cat "$tmp/warn-not-abort.err")" "WARNING: staged maintenance SQL failed" "warn-not-abort warning"
assert_eq "4096" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort published page_size"
assert_eq "0" "$(index_count "$warn_db")" "warn-not-abort no index"

gate_db="$tmp/staged-gate.db"
create_mediaitems_db "$gate_db"
gate_hash_before="$(sha256_file "$gate_db")"
export INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY=1
export INDEX_TEST_CORRUPT_STAGED_DB="$gate_db.new"
run_rebuild_capture staged-gate "$gate_db" 4096 "$optimize_sql"
unset INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY INDEX_TEST_CORRUPT_STAGED_DB
assert_eq 1 "$(cat "$tmp/staged-gate.rc")" "staged integrity gate rc"
assert_eq "$gate_hash_before" "$(sha256_file "$gate_db")" "staged integrity gate live hash"
[ ! -e "$gate_db.new" ] || fail "staged integrity gate cleanup" "no staged file" "$gate_db.new exists"
assert_contains "$(cat "$tmp/staged-gate.err")" "integrity check failed" "staged integrity gate diagnostic"

eqp_db="$tmp/eqp.db"
create_mediaitems_db "$eqp_db"
for ddl in "${EMBY_INDEXES[@]}"; do
  "$real_sqlite" "$eqp_db" "$ddl"
done
assert_eqp_uses_emby_index "$(mediaitems_eqp "$eqp_db")" "pre-ANALYZE"
"$real_sqlite" "$eqp_db" "ANALYZE;"
assert_eqp_uses_emby_index "$(mediaitems_eqp "$eqp_db")" "post-ANALYZE"
assert_eq "1" "$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM MediaItems INDEXED BY $EMBY_INDEX_NAME WHERE ParentId=4600 AND Type=8;")" "INDEXED BY usability probe"

printf 'optimize_media_servers Emby index tests passed\n'
