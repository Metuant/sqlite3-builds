#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

fail() {
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$3" "$1" "$2" >&2
  exit 1
}

assert_eq() {
  [ "$2" = "$1" ] || fail "$1" "$2" "$3"
}

assert_contains() {
  case "$1" in
    *"$2"*) ;;
    *) fail "contains $2" "$1" "$3" ;;
  esac
}

assert_not_contains() {
  case "$1" in
    *"$2"*) fail "does not contain $2" "$1" "$3" ;;
  esac
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 in PATH" "missing" "sqlite3 availability"

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-plex-blob-trim.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-plex-blob-trim.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/plex"

cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
stdin_sql=""
if [ ! -t 0 ]; then
  stdin_sql="$(cat)"
fi
{
  printf 'ARGV'
  printf '|%s' "$@"
  printf '\nSTDIN|%s\n' "$stdin_sql"
} >> "$TRIM_SQLITE_LOG"
for arg in "$@"; do
  if [ "$arg" = "-readonly" ] && [ "${FAIL_MAIN_READ:-0}" = "1" ]; then
    printf 'simulated main read failure\n' >&2
    exit 70
  fi
  if [ "$arg" = "-readonly" ]; then
    readonly_seen=1
  fi
done
case "$stdin_sql" in
  *"DELETE FROM blobs"*)
    if [ "${FAIL_TRIM_CONSERVATION:-0}" = "1" ]; then
      stdin_sql="${stdin_sql/"SELECT (SELECT value FROM trim_counts WHERE label='post_candidate')=0;"/"SELECT 0;"}"
      printf 'INJECT|CONSERVATION_CHECK_FAILURE\n' >> "$TRIM_SQLITE_LOG"
    fi
    ;;
esac
for arg in "$@"; do
  case "$arg" in
    *"VACUUM;"*)
      if [ "${FAIL_TRIM_VACUUM:-0}" = "1" ]; then
      printf 'simulated trim VACUUM failure\n' >&2
      exit 72
      fi
      ;;
    *"PRAGMA integrity_check;"*)
      if [ -n "${BAD_TRIM_INTEGRITY_MARKER:-}" ] && [ -e "$BAD_TRIM_INTEGRITY_MARKER" ]; then
        printf 'OUTPUT|not ok\n' >> "$TRIM_SQLITE_LOG"
        printf 'not ok\n'
        exit 0
      fi
      ;;
  esac
done
if [ -n "$stdin_sql" ]; then
  output="$(printf '%s\n' "$stdin_sql" | "$REAL_SQLITE" "$@")"
  rc=$?
  if [ "$rc" -eq 0 ] && [ -n "${BAD_TRIM_INTEGRITY_MARKER:-}" ]; then
    case "$stdin_sql" in
      *"DELETE FROM blobs"*) : > "$BAD_TRIM_INTEGRITY_MARKER" ;;
    esac
  fi
else
  output="$("$REAL_SQLITE" "$@")"
  rc=$?
fi
if [ "${readonly_seen:-0}" = "1" ] && [ "${CONTAMINATE_MAIN_STDOUT:-0}" = "1" ]; then
  output="${output}"$'\n''9999'
fi
printf 'OUTPUT|%s\n' "$output" >> "$TRIM_SQLITE_LOG"
printf '%s\n' "$output"
exit "$rc"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
export PATH="$tmp/bin:$PATH"
export REAL_SQLITE="$real_sqlite"
export TRIM_SQLITE_LOG="$tmp/sqlite.log"

plex_databases_path="$tmp/plex"
main_db="$plex_databases_path/$_PLEX_DB"
staged_db="$tmp/blobs.db.new"
cutoff="$($real_sqlite :memory: "SELECT CAST(strftime('%s','now','-24 months') AS INTEGER);")"

create_main_fixture() {
  rm -f "$main_db"
  "$real_sqlite" "$main_db" <<SQL
CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,parent_id INTEGER,metadata_type INTEGER,originally_available_at,added_at);
CREATE TABLE media_items(id INTEGER PRIMARY KEY,metadata_item_id INTEGER);
CREATE TABLE media_parts(id INTEGER PRIMARY KEY,media_item_id INTEGER);
INSERT INTO metadata_items VALUES
  (10,NULL,3,NULL,NULL),(11,10,4,$((cutoff - 1)),NULL),
  (20,NULL,3,NULL,NULL),(21,20,4,$cutoff,NULL),
  (30,NULL,3,NULL,NULL),(31,30,4,$((cutoff + 3600)),NULL),
  (40,NULL,2,NULL,NULL),(41,40,4,$((cutoff - 1)),NULL),
  (51,999,4,$((cutoff - 1)),NULL),
  (60,NULL,3,NULL,NULL),(61,60,4,'not-an-epoch',NULL),
  (70,NULL,3,NULL,NULL),(71,70,4,$((cutoff - 100)),NULL),(72,70,4,NULL,NULL);
INSERT INTO media_items VALUES (110,11),(210,21),(310,31),(410,41),(510,51),(610,61),(710,71);
INSERT INTO media_parts VALUES (100,110),(200,210),(300,310),(400,410),(500,510),(600,610),(700,710);
SQL
}

create_blob_fixture() {
  rm -f "$staged_db"
  "$real_sqlite" "$staged_db" <<'SQL'
CREATE TABLE blobs(
  id INTEGER PRIMARY KEY,
  blob BLOB,
  linked_type TEXT,
  linked_id INTEGER,
  linked_guid TEXT,
  created_at INTEGER,
  blob_type INTEGER,
  UNIQUE(linked_type,linked_id,blob_type),
  UNIQUE(linked_type,linked_guid,blob_type)
);
INSERT INTO blobs VALUES
  (1,X'01','media_part',100,'g100',1,5),
  (2,X'02','media_part',200,'g200',2,5),
  (3,X'03','media_part',300,'g300',3,5),
  (4,X'04','media_part',400,'g400',4,5),
  (5,X'05','media_part',500,'g500',5,5),
  (6,X'06','media_part',600,'g600',6,5),
  (7,X'07','media_part',700,'g700',7,5),
  (8,X'08','media_stream',100,'g800',8,5),
  (9,X'09','media_part',999,'g900',9,3),
  (10,X'0A','media_part',9999,'g1000',10,5);
SQL
}

create_main_fixture
create_blob_fixture
: > "$TRIM_SQLITE_LOG"
main_hash_before="$(sha256sum "$main_db" | awk '{print $1}')"
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/success.out" 2>"$tmp/success.err"
assert_eq "3,4,5,6,8,9,10" "$($real_sqlite "$staged_db" "SELECT group_concat(id, ',') FROM (SELECT id FROM blobs ORDER BY id);")" "eligible, boundary, and dateless-newest deletion"
assert_eq "$main_hash_before" "$(sha256sum "$main_db" | awk '{print $1}')" "main database immutability"
log="$(cat "$TRIM_SQLITE_LOG")"
assert_contains "$log" "|-readonly|-batch|-noheader|$main_db" "main read-only argv"
assert_contains "$log" "ARGV|-batch|-noheader|$staged_db" "staged write argv"
assert_contains "$log" "OUTPUT|DELETED|3" "deleted count"
assert_contains "$log" ".import '" "TEMP-table import"
assert_not_contains "$log" "ATTACH" "no ATTACH"
assert_not_contains "$log" "file:" "no file URI"
assert_contains "$log" "PRAGMA threads=8;" "trim VACUUM threads"
assert_not_contains "$log" "PRAGMA temp_store=MEMORY" "trim VACUUM file-backed temp"
assert_not_contains "$log" "PRAGMA temp_store=2" "trim VACUUM no numeric MEMORY"
if ls "$tmp"/blobs.db.new.trim.* >/dev/null 2>&1; then
  fail "no scratch directory" "scratch remains" "success scratch cleanup"
fi

create_main_fixture
create_blob_fixture
"$real_sqlite" "$main_db" "UPDATE metadata_items SET originally_available_at=$((cutoff + 3600)) WHERE metadata_type=4;"
: > "$TRIM_SQLITE_LOG"
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/zero.out" 2>"$tmp/zero.err"
assert_contains "$(cat "$tmp/zero.out")" "deleted 0 rows" "zero-candidate report"
assert_not_contains "$(cat "$TRIM_SQLITE_LOG")" "VACUUM;" "zero-candidate VACUUM skip"
assert_eq "ok" "$($real_sqlite "$staged_db" 'PRAGMA integrity_check;')" "post-trim integrity"

create_main_fixture
create_blob_fixture
: > "$TRIM_SQLITE_LOG"
staged_hash_before="$(sha256sum "$staged_db" | awk '{print $1}')"
export FAIL_MAIN_READ=1
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/read-fail.out" 2>"$tmp/read-fail.err"
unset FAIL_MAIN_READ
assert_contains "$(cat "$tmp/read-fail.err")" "could not read finished-season" "main read failure warning"
assert_eq "$staged_hash_before" "$(sha256sum "$staged_db" | awk '{print $1}')" "main read failure staged immutability"
assert_not_contains "$(cat "$TRIM_SQLITE_LOG")" "DELETE FROM blobs" "main read failure no staged DELETE"

create_main_fixture
create_blob_fixture
: > "$TRIM_SQLITE_LOG"
staged_hash_before="$(sha256sum "$staged_db" | awk '{print $1}')"
export CONTAMINATE_MAIN_STDOUT=1
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/contaminated-read.out" 2>"$tmp/contaminated-read.err"
unset CONTAMINATE_MAIN_STDOUT
assert_contains "$(cat "$tmp/contaminated-read.err")" "non-conforming finished-season media_part id output" "contaminated main read warning"
assert_contains "$(cat "$TRIM_SQLITE_LOG")" "ARGV|-init|/dev/null|-readonly|-batch|-noheader|$main_db" "contaminated main read init isolation"
assert_eq "$staged_hash_before" "$(sha256sum "$staged_db" | awk '{print $1}')" "contaminated main read staged byte identity"
assert_eq "0A" "$($real_sqlite "$staged_db" "SELECT hex(blob) FROM blobs WHERE id=10;")" "contaminated main read unrelated blob identity"
assert_not_contains "$(cat "$TRIM_SQLITE_LOG")" "DELETE FROM blobs" "contaminated main read no staged DELETE"

create_main_fixture
create_blob_fixture
staged_hash_before="$(sha256sum "$staged_db" | awk '{print $1}')"
staged_rows_before="$($real_sqlite "$staged_db" "SELECT group_concat(id || ':' || hex(blob), ',') FROM (SELECT id,blob FROM blobs ORDER BY id);")"
: > "$TRIM_SQLITE_LOG"
export FAIL_TRIM_CONSERVATION=1
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/conservation-fail.out" 2>"$tmp/conservation-fail.err"
unset FAIL_TRIM_CONSERVATION
assert_contains "$(cat "$tmp/conservation-fail.err")" "CHECK constraint failed" "conservation CHECK executed"
assert_contains "$(cat "$tmp/conservation-fail.err")" "trim DELETE failed" "conservation failure warning"
assert_contains "$(cat "$TRIM_SQLITE_LOG")" "INJECT|CONSERVATION_CHECK_FAILURE" "conservation failure injection"
assert_eq "$staged_hash_before" "$(sha256sum "$staged_db" | awk '{print $1}')" "conservation failure staged byte identity"
assert_eq "$staged_rows_before" "$($real_sqlite "$staged_db" "SELECT group_concat(id || ':' || hex(blob), ',') FROM (SELECT id,blob FROM blobs ORDER BY id);")" "conservation failure staged row identity"

create_main_fixture
create_blob_fixture
: > "$TRIM_SQLITE_LOG"
export FAIL_TRIM_VACUUM=1
try_trim_plex_finished_season_blobs sqlite3 "$staged_db" >"$tmp/vacuum-fail.out" 2>"$tmp/vacuum-fail.err"
unset FAIL_TRIM_VACUUM
assert_contains "$(cat "$tmp/vacuum-fail.err")" "post-trim VACUUM failed" "trim VACUUM failure warning"
assert_eq "3,4,5,6,8,9,10" "$($real_sqlite "$staged_db" "SELECT group_concat(id, ',') FROM (SELECT id FROM blobs ORDER BY id);")" "VACUUM failure keeps committed deletion"
assert_not_contains "$(cat "$TRIM_SQLITE_LOG")" "PRAGMA integrity_check;" "VACUUM failure defers integrity to pipeline"

create_main_fixture
staged_db="$plex_databases_path/$_PLEX_BLOB_DB"
create_blob_fixture
live_hash_before="$(sha256sum "$staged_db" | awk '{print $1}')"
mkdir -p "$tmp/backups"
export BAD_TRIM_INTEGRITY_MARKER="$tmp/bad-trim-integrity.marker"
STATS_BANDWIDTH_RETAIN_DAYS=90
set +e
(
  rebuild_db_vacuum_into sqlite3 "$staged_db" "$tmp/backups" 4096 NONE "" "" "try_trim_plex_finished_season_blobs"
) >"$tmp/integrity-fail.out" 2>"$tmp/integrity-fail.err"
rc=$?
set -e
unset BAD_TRIM_INTEGRITY_MARKER
assert_eq "1" "$rc" "final integrity after trim pipeline status"
assert_contains "$(cat "$tmp/integrity-fail.err")" "final staged DB failed integrity_check" "final integrity after trim diagnostic"
assert_eq "$live_hash_before" "$(sha256sum "$staged_db" | awk '{print $1}')" "final integrity after trim live hash"
[ ! -e "$staged_db.new" ] || fail "no staged DB" "$staged_db.new exists" "final integrity after trim staged cleanup"

if ls "$tmp"/blobs.db.new.trim.* >/dev/null 2>&1; then
  fail "no scratch directory" "scratch remains" "failure scratch cleanup"
fi

printf 'optimize_media_servers Plex blob trim tests passed\n'
