#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-deflate.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-deflate.XXXXXX)"
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
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

if [ "${FAIL_DELETE:-0}" = "1" ]; then
  case "$sql_text" in
    *"DELETE FROM statistics_bandwidth"*)
      printf 'simulated DELETE failure\n' >&2
      exit 70
      ;;
  esac
fi

if [ "${FAIL_VACUUM:-0}" = "1" ]; then
  case "$sql_text" in
    *"VACUUM;"*)
      printf 'simulated VACUUM failure\n' >&2
      exit 71
      ;;
  esac
fi

if [ -n "${BAD_INTEGRITY_MARKER:-}" ] && [ -e "$BAD_INTEGRITY_MARKER" ]; then
  case "$sql_text" in
    *"PRAGMA integrity_check(1);"*)
      printf 'not ok\n'
      exit 0
      ;;
  esac
fi

delete_seen=0
case "$sql_text" in
  *"DELETE FROM statistics_bandwidth"*) delete_seen=1 ;;
esac

"$REAL_SQLITE" "$@"
rc=$?
if [ "$rc" -eq 0 ] && [ "$delete_seen" = "1" ] && [ -n "${BAD_INTEGRITY_MARKER:-}" ]; then
  : > "$BAD_INTEGRITY_MARKER"
fi
exit "$rc"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

create_stats_fixture() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
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
  (3, NULL, 100, 86400, CAST(STRFTIME('%s','now','-10 days') AS INTEGER), 0, 3000),
  (4, NULL, 100, 86400, CAST(STRFTIME('%s','now','-120 days') AS INTEGER), 0, 4000),
  (5, 20, 200, 86400, NULL, 0, 5000),
  (6, 30, 300, 3600, CAST(STRFTIME('%s','now','-1 days') AS INTEGER), 1, 6000);
EOF_SQL
}

run_invalid_retention_case() {
  local name retain_days db hash_before rc
  name="$1"
  retain_days="$2"
  db="$tmp/${name}.db"

  create_stats_fixture "$db"
  hash_before="$(sha256_file "$db")"
  set +e
  try_deflate_plex_statistics_bandwidth sqlite3 "$db" "$retain_days" >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  assert_eq 0 "$rc" "$name invalid retention rc"
  assert_contains "$(cat "$tmp/${name}.err")" "WARNING: invalid statistics_bandwidth retention" "$name invalid retention warning"
  assert_eq "$hash_before" "$(sha256_file "$db")" "$name invalid retention DB hash"
}

schema_sql="SELECT type || '|' || name || '|' || tbl_name || '|' || sql FROM sqlite_master WHERE name IN ('statistics_bandwidth','index_statistics_bandwidth_on_at','index_statistics_bandwidth_on_account_id_and_timespan_and_at') ORDER BY name;"
index_sql="SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='statistics_bandwidth' ORDER BY name;"
remaining_sql="SELECT group_concat(id, ',') FROM (SELECT id FROM statistics_bandwidth ORDER BY id);"

positive="$tmp/positive.db"
create_stats_fixture "$positive"
schema_before="$("$real_sqlite" "$positive" "$schema_sql")"
try_deflate_plex_statistics_bandwidth sqlite3 "$positive" 90 >"$tmp/positive.out" 2>"$tmp/positive.err"
assert_contains "$(cat "$tmp/positive.out")" "Deflating Plex statistics_bandwidth" "deflate log"
assert_eq "2,5,6" "$("$real_sqlite" "$positive" "$remaining_sql")" "remaining statistics_bandwidth ids"
assert_eq "index_statistics_bandwidth_on_account_id_and_timespan_and_at
index_statistics_bandwidth_on_at" "$("$real_sqlite" "$positive" "$index_sql")" "statistics_bandwidth indexes"
assert_eq "$schema_before" "$("$real_sqlite" "$positive" "$schema_sql")" "statistics_bandwidth schema after deflate"
assert_eq "ok" "$("$real_sqlite" "$positive" "PRAGMA integrity_check(1);")" "post-deflate integrity"

run_invalid_retention_case invalid-retention-empty ""
run_invalid_retention_case invalid-retention-alpha "abc"

missing="$tmp/missing-table.db"
"$real_sqlite" "$missing" "CREATE TABLE other(id INTEGER PRIMARY KEY);"
try_deflate_plex_statistics_bandwidth sqlite3 "$missing" 90 >"$tmp/missing.out" 2>"$tmp/missing.err"
assert_contains "$(cat "$tmp/missing.err")" "WARNING: statistics_bandwidth is absent" "missing table warning"
assert_eq "0" "$("$real_sqlite" "$missing" "SELECT COUNT(*) FROM sqlite_master WHERE name='statistics_bandwidth';")" "missing table remains absent"

delete_fail="$tmp/delete-fail.db"
create_stats_fixture "$delete_fail"
delete_hash_before="$(sha256_file "$delete_fail")"
export FAIL_DELETE=1
try_deflate_plex_statistics_bandwidth sqlite3 "$delete_fail" 90 >"$tmp/delete-fail.out" 2>"$tmp/delete-fail.err"
unset FAIL_DELETE
assert_contains "$(cat "$tmp/delete-fail.err")" "WARNING: statistics_bandwidth deflate DELETE failed" "DELETE failure warning"
assert_eq "$delete_hash_before" "$(sha256_file "$delete_fail")" "DELETE failure DB hash"
assert_eq "ok" "$("$real_sqlite" "$delete_fail" "PRAGMA integrity_check(1);")" "DELETE failure DB integrity"

vacuum_fail="$tmp/vacuum-fail.db"
create_stats_fixture "$vacuum_fail"
export FAIL_VACUUM=1
try_deflate_plex_statistics_bandwidth sqlite3 "$vacuum_fail" 90 >"$tmp/vacuum-fail.out" 2>"$tmp/vacuum-fail.err"
unset FAIL_VACUUM
assert_contains "$(cat "$tmp/vacuum-fail.err")" "WARNING: post-deflate VACUUM failed" "VACUUM failure warning"
assert_eq "2,5,6" "$("$real_sqlite" "$vacuum_fail" "$remaining_sql")" "VACUUM failure retained deflated rows"
assert_eq "ok" "$("$real_sqlite" "$vacuum_fail" "PRAGMA integrity_check(1);")" "VACUUM failure DB integrity"

integrity_live="$tmp/integrity-live.db"
create_stats_fixture "$integrity_live"
live_hash_before="$(sha256_file "$integrity_live")"
mkdir -p "$tmp/backups"
export BAD_INTEGRITY_MARKER="$tmp/bad-integrity-marker"
set +e
(
  cd "$repo_root"
  . ./scripts/optimize_media_servers.sh
  rebuild_db_vacuum_into sqlite3 "$integrity_live" "$tmp/backups" 4096 NONE "" "" "try_deflate_plex_statistics_bandwidth"
) >"$tmp/integrity.out" 2>"$tmp/integrity.err"
rc=$?
set -e
unset BAD_INTEGRITY_MARKER
assert_eq 1 "$rc" "post-deflate integrity pipeline rc"
assert_contains "$(cat "$tmp/integrity.err")" "post-deflate DB failed integrity_check" "post-deflate integrity diagnostic"
assert_eq "$live_hash_before" "$(sha256_file "$integrity_live")" "post-deflate integrity abort live hash"
[ ! -e "$integrity_live.new" ] || fail "post-deflate integrity staged cleanup" "no staged file" "$integrity_live.new exists"

printf 'optimize_media_servers deflate tests passed\n'
