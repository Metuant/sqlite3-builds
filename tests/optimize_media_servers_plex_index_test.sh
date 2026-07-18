#!/usr/bin/env bash
set -euo pipefail

. ./tests/optimize_media_servers_index_test_lib.sh
index_test_init "plex-index"

PLEX_TAGGINGS_INDEX="idx_dshadow_taggings_tag_id_metadata_item_id"
PLEX_SETTINGS_INDEX="idx_dshadow_mis_account_updated_guid_cover"
PLEX_RECENT_INDEX="idx_dshadow_metadata_items_section_added"
PLEX_GUID_NOCASE_INDEX="idx_dshadow_metadata_items_guid_nocase"
PLEX_VIEWS_GRANDPARENT_GUID_INDEX="idx_dshadow_metadata_item_views_account_grandparent_guid"
PLEX_MEDIA_COUNT_INDEX="idx_dshadow_metadata_items_section_id_type"
PLEX_GUID_NOCASE_DDL="CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_guid_nocase ON metadata_items (guid COLLATE NOCASE);"
PLEX_VIEWS_GRANDPARENT_GUID_DDL="CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_item_views_account_grandparent_guid ON metadata_item_views (account_id, grandparent_guid);"
PLEX_MEDIA_COUNT_DDL="CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_section_id_type ON metadata_items (library_section_id, id, metadata_type);"
PLEX_INDEX_NAMES=(
  "$PLEX_TAGGINGS_INDEX"
  "$PLEX_SETTINGS_INDEX"
  "$PLEX_RECENT_INDEX"
  "$PLEX_GUID_NOCASE_INDEX"
  "$PLEX_VIEWS_GRANDPARENT_GUID_INDEX"
  "$PLEX_MEDIA_COUNT_INDEX"
)
plex_index_ddl="$(printf '%s\n' "${_PLEX_INDEXES[@]}")"
assert_contains "$plex_index_ddl" "$PLEX_GUID_NOCASE_DDL" "Plex guid NOCASE DDL"
assert_contains "$plex_index_ddl" "$PLEX_VIEWS_GRANDPARENT_GUID_DDL" "Plex views grandparent DDL"
assert_contains "$plex_index_ddl" "$PLEX_MEDIA_COUNT_DDL" "Plex media-count DDL"

create_plex_index_db() {
  local db page_size
  db="$1"
  page_size="${2:-4096}"

  "$real_sqlite" "$db" <<EOF_SQL
PRAGMA page_size=${page_size};
VACUUM;
PRAGMA user_version=4101;
PRAGMA application_id=123456789;
CREATE TABLE versioned_metadata_items(id INTEGER PRIMARY KEY);
INSERT INTO versioned_metadata_items(id) VALUES(1);
CREATE TABLE taggings(
  id INTEGER PRIMARY KEY,
  tag_id INTEGER NOT NULL,
  metadata_item_id INTEGER NOT NULL
);
CREATE INDEX index_taggings_on_tag_id ON taggings(tag_id);
CREATE INDEX index_taggings_on_metadata_item_id ON taggings(metadata_item_id);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 5000
)
INSERT INTO taggings(id, tag_id, metadata_item_id)
SELECT x, x % 25, x FROM seq;
CREATE TABLE metadata_item_settings(
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  guid TEXT NOT NULL,
  view_offset INTEGER NOT NULL,
  last_viewed_at INTEGER NOT NULL
);
CREATE INDEX index_metadata_item_settings_on_account_id ON metadata_item_settings(account_id);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 5000
)
INSERT INTO metadata_item_settings(id, account_id, updated_at, guid, view_offset, last_viewed_at)
SELECT
  x,
  CASE WHEN x <= 4500 THEN 42 ELSE x END,
  2000000 - x,
  'plex://fixture/' || x,
  x * 100,
  1000000 + x
FROM seq;
CREATE TABLE metadata_items(
  id INTEGER PRIMARY KEY,
  guid TEXT NOT NULL,
  library_section_id INTEGER NOT NULL,
  metadata_type INTEGER NOT NULL,
  parent_id INTEGER,
  added_at INTEGER,
  originally_available_at INTEGER
);
CREATE INDEX index_metadata_items_on_guid ON metadata_items(guid);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 5000
)
INSERT INTO metadata_items(id, guid, library_section_id, metadata_type, parent_id, added_at, originally_available_at)
SELECT
  x,
  CASE WHEN x % 2 = 0 THEN 'plex://movie/' ELSE 'plex://show/' END || printf('%06d', x),
  CASE WHEN x % 2 = 0 THEN 1 ELSE 2 END,
  CASE WHEN x % 3 = 0 THEN 10 ELSE 4 END,
  NULL,
  3000000 - x,
  2000000 - x
FROM seq;
CREATE TABLE media_items(
  id INTEGER PRIMARY KEY,
  metadata_item_id INTEGER NOT NULL
);
CREATE INDEX index_media_items_on_metadata_item_id ON media_items(metadata_item_id);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 4500
)
INSERT INTO media_items(id, metadata_item_id)
SELECT x, x FROM seq;
CREATE TABLE metadata_item_views(
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL,
  guid TEXT NOT NULL,
  grandparent_guid TEXT NOT NULL,
  viewed_at INTEGER
);
CREATE INDEX index_metadata_item_views_on_guid ON metadata_item_views(guid);
WITH RECURSIVE seq(x) AS (
  VALUES(1)
  UNION ALL
  SELECT x + 1 FROM seq WHERE x < 5000
)
INSERT INTO metadata_item_views(id, account_id, guid, grandparent_guid, viewed_at)
SELECT
  x,
  CASE WHEN x <= 4500 THEN 42 ELSE x END,
  'plex://episode/' || x,
  'plex://show/' || (x % 100),
  4000000 - x
FROM seq;
EOF_SQL
}

index_count() {
  local db index_name
  db="$1"
  index_name="$2"
  index_test_index_count "$db" "$index_name"
}

assert_plex_indexes_present() {
  local db index_name
  db="$1"

  for index_name in "${PLEX_INDEX_NAMES[@]}"; do
    assert_eq "1" "$(index_count "$db" "$index_name")" "$index_name count"
  done
}

assert_plex_indexes_absent() {
  local db index_name
  db="$1"

  for index_name in "${PLEX_INDEX_NAMES[@]}"; do
    assert_eq "0" "$(index_count "$db" "$index_name")" "$index_name absent count"
  done
}

taggings_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT metadata_item_id FROM taggings WHERE tag_id=7 AND metadata_item_id=1007;" | tr '\r\n' ' '
}

settings_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT guid, view_offset, last_viewed_at FROM metadata_item_settings WHERE account_id=42 AND view_offset > 0 AND last_viewed_at > 100000 ORDER BY updated_at DESC LIMIT 5;" | tr '\r\n' ' '
}

recent_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT id FROM metadata_items WHERE library_section_id=2 AND metadata_type IN (4,10) ORDER BY added_at DESC LIMIT 5;" | tr '\r\n' ' '
}

guid_like_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT id FROM metadata_items WHERE guid LIKE 'plex://movie/0001%';" | tr '\r\n' ' '
}

guid_equality_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT id FROM metadata_items WHERE guid='plex://movie/000100';" | tr '\r\n' ' '
}

views_grandparent_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT id FROM metadata_item_views WHERE account_id=42 AND grandparent_guid='plex://show/42';" | tr '\r\n' ' '
}

views_vendor_guid_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT id FROM metadata_item_views INDEXED BY index_metadata_item_views_on_guid WHERE guid='plex://episode/142' AND account_id=42 AND grandparent_guid='plex://show/42';" | tr '\r\n' ' '
}

media_count_eqp() {
  local db
  db="$1"
  "$real_sqlite" "$db" "EXPLAIN QUERY PLAN SELECT metadata_items.id, count(media_items.id) AS cnt FROM metadata_items LEFT JOIN media_items ON media_items.metadata_item_id=metadata_items.id WHERE metadata_items.metadata_type IN (4,10) AND metadata_items.library_section_id=2 GROUP BY metadata_items.id HAVING cnt=0;" | tr '\r\n' ' '
}

assert_eqp_uses_taggings_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase taggings EQP search"
  assert_contains "$eqp" "$PLEX_TAGGINGS_INDEX" "$phase taggings EQP index"
  assert_contains "$eqp" "COVERING" "$phase taggings EQP covering"
  assert_not_contains "$eqp" "SCAN" "$phase taggings EQP no scan"
}

assert_eqp_uses_settings_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase settings EQP search"
  assert_contains "$eqp" "$PLEX_SETTINGS_INDEX" "$phase settings EQP index"
  assert_contains "$eqp" "COVERING" "$phase settings EQP covering"
  assert_not_contains "$eqp" "USE TEMP B-TREE" "$phase settings EQP no sort"
}

assert_eqp_uses_recent_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase recent EQP search"
  assert_contains "$eqp" "$PLEX_RECENT_INDEX" "$phase recent EQP index"
  assert_not_contains "$eqp" "USE TEMP B-TREE" "$phase recent EQP no sort"
}

assert_eqp_uses_guid_nocase_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase guid LIKE EQP search"
  assert_contains "$eqp" "$PLEX_GUID_NOCASE_INDEX" "$phase guid LIKE EQP index"
  assert_not_contains "$eqp" "SCAN" "$phase guid LIKE EQP no scan"
}

assert_eqp_uses_vendor_guid_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase guid equality EQP search"
  assert_contains "$eqp" "index_metadata_items_on_guid" "$phase guid equality vendor index"
  assert_not_contains "$eqp" "$PLEX_GUID_NOCASE_INDEX" "$phase guid equality no NOCASE adoption"
}

assert_eqp_uses_views_grandparent_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH" "$phase views EQP search"
  assert_contains "$eqp" "$PLEX_VIEWS_GRANDPARENT_GUID_INDEX" "$phase views EQP index"
  assert_not_contains "$eqp" "SCAN" "$phase views EQP no scan"
}

assert_eqp_uses_views_vendor_guid_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "index_metadata_item_views_on_guid" "$phase views vendor EQP index"
  assert_not_contains "$eqp" "$PLEX_VIEWS_GRANDPARENT_GUID_INDEX" "$phase views vendor no g2 adoption"
}

assert_eqp_uses_media_count_index() {
  local eqp phase
  eqp="$1"
  phase="$2"
  assert_contains "$eqp" "SEARCH metadata_items USING COVERING INDEX" "$phase media-count EQP covering search"
  assert_contains "$eqp" "$PLEX_MEDIA_COUNT_INDEX" "$phase media-count EQP index"
  assert_not_contains "$eqp" "SCAN metadata_items" "$phase media-count EQP no metadata_items scan"
}

plex_dir="$tmp/plex-dbs"
mkdir -p "$plex_dir"
published="$plex_dir/$_PLEX_DB"
create_plex_index_db "$published"
index_test_run_plex_optimize_capture published-first "$plex_dir" "$_PLEX_DB" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" ""
assert_eq 0 "$(cat "$tmp/published-first.rc")" "first Plex index pipeline rc"
assert_plex_indexes_present "$published"
stat1_plex_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat1 WHERE tbl IN ('taggings','metadata_item_settings','metadata_items','metadata_item_views') OR idx IN ('$PLEX_TAGGINGS_INDEX','$PLEX_SETTINGS_INDEX','$PLEX_RECENT_INDEX','$PLEX_GUID_NOCASE_INDEX','$PLEX_VIEWS_GRANDPARENT_GUID_INDEX','$PLEX_MEDIA_COUNT_INDEX');")"
assert_int_gt "$stat1_plex_count" 0 "published sqlite_stat1 Plex index row count"
stat4_exists="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='sqlite_stat4';")"
if [ "$stat4_exists" = "1" ]; then
  stat4_count="$("$real_sqlite" "$published" "SELECT COUNT(*) FROM sqlite_stat4 WHERE tbl IN ('taggings','metadata_item_settings','metadata_items','metadata_item_views') OR idx IN ('$PLEX_TAGGINGS_INDEX','$PLEX_SETTINGS_INDEX','$PLEX_RECENT_INDEX','$PLEX_GUID_NOCASE_INDEX','$PLEX_VIEWS_GRANDPARENT_GUID_INDEX','$PLEX_MEDIA_COUNT_INDEX');")"
  printf 'sqlite_stat4 Plex index rows: %s\n' "$stat4_count"
else
  printf 'SKIP: sqlite_stat4 unavailable in this sqlite3 build\n'
fi
assert_eqp_uses_taggings_index "$(taggings_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_settings_index "$(settings_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_recent_index "$(recent_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_guid_nocase_index "$(guid_like_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_vendor_guid_index "$(guid_equality_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_views_grandparent_index "$(views_grandparent_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_views_vendor_guid_index "$(views_vendor_guid_eqp "$published")" "published post-ANALYZE"
assert_eqp_uses_media_count_index "$(media_count_eqp "$published")" "published post-ANALYZE"

index_test_run_plex_optimize_capture published-second "$plex_dir" "$_PLEX_DB" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" ""
assert_eq 0 "$(cat "$tmp/published-second.rc")" "second Plex index pipeline rc"
assert_eq "" "$(cat "$tmp/published-second.err")" "second Plex index pipeline stderr"
assert_plex_indexes_present "$published"

warn_dir="$tmp/plex-warn"
mkdir -p "$warn_dir"
warn_db="$warn_dir/$_PLEX_DB"
create_plex_index_db "$warn_db" 1024
assert_eq "1024" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort source page_size"
export INDEX_TEST_FAIL_SQL=1
export INDEX_TEST_FAIL_NEEDLE="$PLEX_TAGGINGS_INDEX"
index_test_run_plex_optimize_capture warn-not-abort "$warn_dir" "$_PLEX_DB" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" ""
unset INDEX_TEST_FAIL_SQL INDEX_TEST_FAIL_NEEDLE
assert_eq 0 "$(cat "$tmp/warn-not-abort.rc")" "warn-not-abort rc"
assert_contains "$(cat "$tmp/warn-not-abort.err")" "WARNING: staged maintenance SQL failed" "warn-not-abort warning"
assert_eq "16384" "$("$real_sqlite" "$warn_db" "PRAGMA page_size;")" "warn-not-abort published page_size"
assert_plex_indexes_absent "$warn_db"

gate_dir="$tmp/plex-gate"
mkdir -p "$gate_dir"
gate_db="$gate_dir/$_PLEX_DB"
create_plex_index_db "$gate_db"
gate_hash_before="$(sha256_file "$gate_db")"
export INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY=1
export INDEX_TEST_CORRUPT_STAGED_DB="$gate_db.new"
index_test_run_plex_optimize_capture staged-gate "$gate_dir" "$_PLEX_DB" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" ""
unset INDEX_TEST_CORRUPT_STAGED_ON_INTEGRITY INDEX_TEST_CORRUPT_STAGED_DB
assert_eq 1 "$(cat "$tmp/staged-gate.rc")" "staged integrity gate rc"
assert_eq "$gate_hash_before" "$(sha256_file "$gate_db")" "staged integrity gate live hash"
[ ! -e "$gate_db.new" ] || fail "staged integrity gate cleanup" "no staged file" "$gate_db.new exists"
assert_contains "$(cat "$tmp/staged-gate.err")" "final staged integrity_check failed to run" "final staged integrity gate diagnostic"

eqp_db="$tmp/plex-eqp.db"
create_plex_index_db "$eqp_db"
for ddl in "${_PLEX_INDEXES[@]}"; do
  "$real_sqlite" "$eqp_db" "$ddl"
done
assert_eqp_uses_taggings_index "$(taggings_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_settings_index "$(settings_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_recent_index "$(recent_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_guid_nocase_index "$(guid_like_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_vendor_guid_index "$(guid_equality_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_views_grandparent_index "$(views_grandparent_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_views_vendor_guid_index "$(views_vendor_guid_eqp "$eqp_db")" "pre-ANALYZE"
assert_eqp_uses_media_count_index "$(media_count_eqp "$eqp_db")" "pre-ANALYZE"
"$real_sqlite" "$eqp_db" "ANALYZE;"
assert_eqp_uses_taggings_index "$(taggings_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_settings_index "$(settings_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_recent_index "$(recent_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_guid_nocase_index "$(guid_like_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_vendor_guid_index "$(guid_equality_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_views_grandparent_index "$(views_grandparent_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_views_vendor_guid_index "$(views_vendor_guid_eqp "$eqp_db")" "post-ANALYZE"
assert_eqp_uses_media_count_index "$(media_count_eqp "$eqp_db")" "post-ANALYZE"
assert_eq "1" "$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM taggings INDEXED BY $PLEX_TAGGINGS_INDEX WHERE tag_id=7 AND metadata_item_id=1007;")" "taggings INDEXED BY usability probe"
settings_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM metadata_item_settings INDEXED BY $PLEX_SETTINGS_INDEX WHERE account_id=42 AND view_offset > 0 AND last_viewed_at > 100000;")"
assert_int_gt "$settings_indexed_count" 0 "settings INDEXED BY usability probe"
recent_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM metadata_items INDEXED BY $PLEX_RECENT_INDEX WHERE library_section_id=2 AND metadata_type IN (4,10);")"
assert_int_gt "$recent_indexed_count" 0 "recent INDEXED BY usability probe"
guid_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM metadata_items INDEXED BY $PLEX_GUID_NOCASE_INDEX WHERE guid LIKE 'plex://movie/0001%';")"
assert_int_gt "$guid_indexed_count" 0 "guid NOCASE INDEXED BY usability probe"
views_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM metadata_item_views INDEXED BY $PLEX_VIEWS_GRANDPARENT_GUID_INDEX WHERE account_id=42 AND grandparent_guid='plex://show/42';")"
assert_int_gt "$views_indexed_count" 0 "views grandparent INDEXED BY usability probe"
media_count_indexed_count="$("$real_sqlite" "$eqp_db" "SELECT COUNT(*) FROM metadata_items INDEXED BY $PLEX_MEDIA_COUNT_INDEX WHERE library_section_id=2 AND metadata_type IN (4,10);")"
assert_int_gt "$media_count_indexed_count" 0 "media-count INDEXED BY usability probe"

printf 'optimize_media_servers Plex index tests passed\n'
