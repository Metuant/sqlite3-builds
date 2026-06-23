#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-pipeline.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-pipeline.XXXXXX)"
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
cat > "$tmp/bin/docker" <<'EOF_DOCKER'
#!/usr/bin/env bash
printf 'SKIP: Docker stopped-container gate is not exercised by local helper smoke tests\n' >&2
exit 75
EOF_DOCKER
chmod +x "$tmp/bin/docker"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"
export SQLITE_CALL_LOG="$tmp/sqlite-calls.log"
: > "$SQLITE_CALL_LOG"

backup_dir="$tmp/backups"
mkdir -p "$backup_dir"

run_rebuild_capture() {
  local name binary db page_size auto_vacuum sanity post_sql hook
  name="$1"
  binary="$2"
  db="$3"
  page_size="$4"
  auto_vacuum="$5"
  sanity="$6"
  post_sql="$7"
  hook="$8"
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

create_normal_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE media(id INTEGER PRIMARY KEY, title TEXT);
INSERT INTO media(id, title) VALUES(1, 'alpha');
CREATE VIRTUAL TABLE fts_search9 USING fts5(title);
INSERT INTO fts_search9(rowid, title) VALUES(1, 'alpha');
EOF_SQL
}

create_external_fts_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE docs(id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO docs(id, body) VALUES(1, 'alpha');
CREATE VIRTUAL TABLE fts5_external USING fts5(body, content='docs', content_rowid='id');
INSERT INTO fts5_external(rowid, body) VALUES(1, 'alpha');
EOF_SQL
}

create_fk_orphan_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
PRAGMA foreign_keys=OFF;
CREATE TABLE parent(id INTEGER PRIMARY KEY);
CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id));
INSERT INTO child(id, parent_id) VALUES(1, 99);
EOF_SQL
}

create_stats_db() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE media(id INTEGER PRIMARY KEY, title TEXT);
INSERT INTO media(id, title) VALUES(1, 'plex');
CREATE TABLE statistics_bandwidth(
  id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
  account_id integer,
  device_id integer,
  timespan integer,
  at dt_integer(8),
  lan boolean,
  bytes integer(8)
);
CREATE INDEX index_statistics_bandwidth_on_at ON statistics_bandwidth(at);
CREATE INDEX index_statistics_bandwidth_on_account_id_and_timespan_and_at ON statistics_bandwidth(account_id,timespan,at);
INSERT INTO statistics_bandwidth(id, account_id, device_id, timespan, at, lan, bytes)
VALUES
  (1, 10, 100, 86400, CAST(STRFTIME('%s','now','-120 days') AS INTEGER), 0, 1000),
  (2, 10, 100, 86400, CAST(STRFTIME('%s','now','-10 days') AS INTEGER), 0, 2000),
  (3, NULL, 100, 86400, CAST(STRFTIME('%s','now','-10 days') AS INTEGER), 0, 3000);
EOF_SQL
}

healthy="$tmp/healthy.db"
create_normal_fts_db "$healthy"
run_rebuild_capture healthy sqlite3 "$healthy" 4096 NONE "" "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002;" ""
assert_eq 0 "$(cat "$tmp/healthy.rc")" "healthy pipeline rc"
assert_eq "1" "$("$real_sqlite" "$healthy" "SELECT COUNT(*) FROM media WHERE title='alpha';")" "healthy pipeline live row"
healthy_backup_count="$(find "$backup_dir" -name 'healthy.db-*.original' -print | wc -l | tr -d ' ')"
assert_eq "1" "$healthy_backup_count" "healthy pipeline backup count"
[ ! -e "$healthy.new" ] || fail "healthy pipeline staged cleanup" "no staged file" "$healthy.new exists"

source_corrupt="$tmp/source-corrupt.db"
create_external_fts_db "$source_corrupt"
"$real_sqlite" "$source_corrupt" "UPDATE docs SET body='source-mismatch' WHERE id=1;"
source_hash_before="$(sha256_file "$source_corrupt")"
run_rebuild_capture source-corrupt sqlite3 "$source_corrupt" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/source-corrupt.rc")" "source FTS corruption pipeline rc"
source_corrupt_backup_count="$(find "$backup_dir" -name 'source-corrupt.db-*.original' -print | wc -l | tr -d ' ')"
assert_eq "1" "$source_corrupt_backup_count" "source FTS corruption backup count"
[ ! -e "$source_corrupt.new" ] || fail "source FTS corruption staged cleanup" "no staged file" "$source_corrupt.new exists"
source_hash_after="$(sha256_file "$source_corrupt")"
[ "$source_hash_after" != "$source_hash_before" ] || fail "source FTS corruption live hash" "changed hash" "$source_hash_after"

staged_corrupt="$tmp/staged-corrupt.db"
create_external_fts_db "$staged_corrupt"
staged_hash_before="$(sha256_file "$staged_corrupt")"
export CORRUPT_STAGED_AFTER_VACUUM=1
export CORRUPT_STAGED_DB="$staged_corrupt.new"
run_rebuild_capture staged-corrupt sqlite3 "$staged_corrupt" 4096 NONE "" "" ""
unset CORRUPT_STAGED_AFTER_VACUUM CORRUPT_STAGED_DB
assert_eq 0 "$(cat "$tmp/staged-corrupt.rc")" "staged FTS corruption pipeline rc"
[ ! -e "$staged_corrupt.new" ] || fail "staged FTS corruption staged cleanup" "no staged file" "$staged_corrupt.new exists"
staged_hash_after="$(sha256_file "$staged_corrupt")"
[ "$staged_hash_after" != "$staged_hash_before" ] || fail "staged FTS corruption live hash" "changed hash" "$staged_hash_after"

staged_rebuild_fail="$tmp/staged-rebuild-fail.db"
create_external_fts_db "$staged_rebuild_fail"
staged_rebuild_fail_hash_before="$(sha256_file "$staged_rebuild_fail")"
export FAIL_REBUILD=1
run_rebuild_capture staged-rebuild-fail sqlite3 "$staged_rebuild_fail" 4096 NONE "" "" ""
unset FAIL_REBUILD
assert_eq 1 "$(cat "$tmp/staged-rebuild-fail.rc")" "staged FTS rebuild failure pipeline rc"
assert_eq "$staged_rebuild_fail_hash_before" "$(sha256_file "$staged_rebuild_fail")" "staged FTS rebuild failure live hash"
[ ! -e "$staged_rebuild_fail.new" ] || fail "staged FTS rebuild failure staged cleanup" "no staged file" "$staged_rebuild_fail.new exists"
assert_contains "$(cat "$tmp/staged-rebuild-fail.err")" "staged FTS rebuild failed" "staged FTS rebuild failure diagnostic"

fk_db="$tmp/fk-warning.db"
create_fk_orphan_db "$fk_db"
run_rebuild_capture fk-warning sqlite3 "$fk_db" 4096 NONE "" "" ""
assert_eq 0 "$(cat "$tmp/fk-warning.rc")" "FK warning pipeline rc"
assert_contains "$(cat "$tmp/fk-warning.err")" "WARNING: foreign_key_check returned rows" "FK warning pipeline diagnostic"
assert_eq "1" "$("$real_sqlite" "$fk_db" "SELECT COUNT(*) FROM child;")" "FK warning pipeline swapped row"

plex_dir="$tmp/plex-dbs"
mkdir -p "$plex_dir"
PLEX_DATABASES_PATH="$plex_dir"
PLEX_INSTANCE="plex-fixture"
PLEX_BINARY="sqlite3"
BACKUP_PATH="$tmp/no-global-backup-root"

plex_main="$plex_dir/$PLEX_DB"
create_stats_db "$plex_main"
optimize_plex_db "$PLEX_DB" "" "try_deflate_plex_statistics_bandwidth" >"$tmp/plex-main.out" 2>"$tmp/plex-main.err"
assert_contains "$(cat "$tmp/plex-main.out")" "Deflating Plex statistics_bandwidth" "Plex main deflate log"
assert_eq "2" "$("$real_sqlite" "$plex_main" "SELECT group_concat(id, ',') FROM (SELECT id FROM statistics_bandwidth ORDER BY id);")" "Plex main deflated ids"

plex_blob="$plex_dir/$PLEX_BLOB_DB"
create_stats_db "$plex_blob"
optimize_plex_db "$PLEX_BLOB_DB" "" "" >"$tmp/plex-blob.out" 2>"$tmp/plex-blob.err"
assert_not_contains "$(cat "$tmp/plex-blob.out")" "Deflating Plex statistics_bandwidth" "Plex blob no deflate log"
assert_eq "1,2,3" "$("$real_sqlite" "$plex_blob" "SELECT group_concat(id, ',') FROM (SELECT id FROM statistics_bandwidth ORDER BY id);")" "Plex blob retained stats ids"

emby="$tmp/emby-library.db"
create_normal_fts_db "$emby"
: > "$SQLITE_CALL_LOG"
run_rebuild_capture emby sqlite3 "$emby" 4096 NONE "SELECT 1 FROM media LIMIT 1;" "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002;" ""
assert_eq 0 "$(cat "$tmp/emby.rc")" "Emby fts_search9 pipeline rc"
emby_log="$(cat "$SQLITE_CALL_LOG")"
assert_contains "$emby_log" 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''rebuild'\'');' "Emby fts_search9 rebuild"
assert_contains "$emby_log" 'INSERT INTO "fts_search9"("fts_search9") VALUES('\''optimize'\'');' "Emby fts_search9 optimize"

printf 'SKIP: Docker stopped-container gate cases require a live Docker daemon and are intentionally not run by this local helper smoke test\n'
printf 'optimize_media_servers pipeline smoke tests passed\n'
