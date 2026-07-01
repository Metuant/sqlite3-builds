#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fatal() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

cli="${1:-${SQLITE3_CLI:-cli/sqlite3}}"
if [ ! -x "$cli" ] && [ -x "release/cli/sqlite3" ]; then
  cli="release/cli/sqlite3"
fi
[ -x "$cli" ] || fatal "sqlite CLI not executable: $cli"

compile_options="$("$cli" :memory: 'PRAGMA compile_options;')"
printf '%s\n' "$compile_options" | grep -Fxq 'ENABLE_STAT4' || fatal "$cli lacks ENABLE_STAT4"
printf '%s\n' "$compile_options" | grep -Fxq 'ENABLE_FTS3' || fatal "$cli lacks ENABLE_FTS3/FTS4 support"

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/plex-fts-stat4-eqp.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT
db="$tmpdir/repro.db"

"$cli" "$db" <<'SQL'
PRAGMA analysis_limit=0;
CREATE TABLE tags(id INTEGER PRIMARY KEY, tag_type INTEGER NOT NULL);
CREATE INDEX index_tags_on_tag_type ON tags(tag_type);
CREATE TABLE metadata_items(
  id INTEGER PRIMARY KEY,
  library_section_id INTEGER NOT NULL,
  metadata_type INTEGER NOT NULL
);
CREATE INDEX index_metadata_items_section_type
  ON metadata_items(library_section_id, metadata_type, id);
CREATE TABLE taggings(metadata_item_id INTEGER NOT NULL, tag_id INTEGER NOT NULL);
CREATE INDEX index_taggings_tag_id_metadata_item_id
  ON taggings(tag_id, metadata_item_id);
CREATE INDEX index_taggings_metadata_item_id_tag_id
  ON taggings(metadata_item_id, tag_id);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);

WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<6000)
INSERT INTO tags(id, tag_type)
SELECT x, CASE WHEN x<=5 THEN 322 ELSE 1 END FROM n;

WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<6000)
INSERT INTO metadata_items(id, library_section_id, metadata_type)
SELECT x, 1, 1 FROM n;

WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<6000)
INSERT INTO taggings(metadata_item_id, tag_id)
SELECT x, x FROM n;

WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<6000)
INSERT INTO fts4_tag_titles_icu(rowid, tag)
SELECT x, CASE WHEN x<=105 THEN 'alpha item ' || x ELSE 'zzzz item ' || x END
FROM n;

ANALYZE;
SQL

original_plan="$("$cli" "$db" <<'SQL'
EXPLAIN QUERY PLAN
select distinct(tags.id) from metadata_items
join taggings on taggings.metadata_item_id=metadata_items.id
join tags on tags.id=taggings.tag_id
join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id
where fts4_tag_titles_icu.tag match 'alpha*'
  and tag_type=322
  and metadata_items.library_section_id in (1)
  and metadata_items.metadata_type=1
group by tags.id order by count(*) desc limit 100;
SQL
)"

unlikely_plan="$("$cli" "$db" <<'SQL'
EXPLAIN QUERY PLAN
select distinct(tags.id) from metadata_items
join taggings on taggings.metadata_item_id=metadata_items.id
join tags on tags.id=taggings.tag_id
join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id
where fts4_tag_titles_icu.tag match 'alpha*'
  and unlikely(tag_type=322)
  and metadata_items.library_section_id in (1)
  and metadata_items.metadata_type=1
group by tags.id order by count(*) desc limit 100;
SQL
)"

line_number() {
  local text="$1" pattern="$2"
  printf '%s\n' "$text" | awk -v pat="$pattern" 'index($0, pat) { print NR; found=1; exit } END { if (!found) print 0 }'
}

orig_tags_line="$(line_number "$original_plan" 'SEARCH tags USING COVERING INDEX index_tags_on_tag_type')"
orig_fts_line="$(line_number "$original_plan" 'SCAN fts4_tag_titles_icu VIRTUAL TABLE')"
unlikely_tags_line="$(line_number "$unlikely_plan" 'SEARCH tags USING COVERING INDEX index_tags_on_tag_type')"
unlikely_fts_line="$(line_number "$unlikely_plan" 'SCAN fts4_tag_titles_icu VIRTUAL TABLE')"

[ "$orig_tags_line" -gt 0 ] || fatal "original plan did not use tag_type index first-path candidate: $original_plan"
[ "$orig_fts_line" -gt 0 ] || fatal "original plan missing FTS scan: $original_plan"
[ "$orig_tags_line" -lt "$orig_fts_line" ] || fatal "original plan did not drive from tag_type before FTS: $original_plan"
[ "$unlikely_fts_line" -gt 0 ] || fatal "unlikely plan missing FTS scan: $unlikely_plan"
[ "$unlikely_tags_line" -gt 0 ] || fatal "unlikely plan missing tag lookup: $unlikely_plan"
[ "$unlikely_fts_line" -lt "$unlikely_tags_line" ] || fatal "unlikely plan did not drive from FTS before tag_type: $unlikely_plan"

printf 'plex fts stat4 eqp repro passed using %s\n' "$cli"
