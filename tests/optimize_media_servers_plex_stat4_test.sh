#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-plex-stat4.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-plex-stat4.XXXXXX)"
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

assert_int_gt() {
  local actual floor message
  actual="$1"
  floor="$2"
  message="$3"
  case "$actual" in
    ''|*[!0-9]*) fail "$message" "integer > $floor" "$actual" ;;
  esac
  [ "$actual" -gt "$floor" ] || fail "$message" "> $floor" "$actual"
}

assert_line_lt() {
  local left right message
  left="$1"
  right="$2"
  message="$3"
  [ -n "$left" ] || fail "$message" "left line number set" "$left:$right"
  [ -n "$right" ] || fail "$message" "right line number set" "$left:$right"
  [ "$left" -lt "$right" ] || fail "$message" "$left < $right" "$left >= $right"
}

first_line_of() {
  local file needle line
  file="$1"
  needle="$2"
  line="$(awk -v needle="$needle" 'index($0, needle) { print NR; exit }' "$file")"
  printf '%s' "$line"
}

first_line_after() {
  local file needle min_line line
  file="$1"
  needle="$2"
  min_line="$3"
  line="$(awk -v needle="$needle" -v min_line="$min_line" 'NR > min_line && index($0, needle) { print NR ":" $0; exit }' "$file")"
  printf '%s' "$line"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"
python_bin="$(command -v python3 || true)"
[ -n "$python_bin" ] || fail "python3 availability" "python3 in PATH" "missing"
stat4_available="$("$real_sqlite" ":memory:" "SELECT sqlite_compileoption_used('ENABLE_STAT4');")"
case "$stat4_available" in
  0|1) ;;
  *) fail "test sqlite3 ENABLE_STAT4 capability" "0 or 1" "$stat4_available" ;;
esac

[ "${GENERIC_SQLITE_BINARY:-}" = "${HOME}/bin/sqlite3" ] || fail "GENERIC_SQLITE_BINARY default" "${HOME}/bin/sqlite3" "${GENERIC_SQLITE_BINARY:-unset}"
old_binary_name="EMBY""_BINARY"
[ -z "${!old_binary_name+x}" ] || fail "$old_binary_name hard rename" "unset" "${!old_binary_name}"

mkdir -p "$tmp/bin" "$tmp/backups"
export REAL_SQLITE="$real_sqlite"

cat > "$tmp/bin/stat4-ok" <<'EOF_STAT4_OK'
#!/usr/bin/env bash
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done
case "$sql_text" in
  *"sqlite_compileoption_used('ENABLE_STAT4')"*) printf '1\n'; exit 0 ;;
  *"SELECT 1"*) printf '1\n'; exit 0 ;;
esac
exec "$REAL_SQLITE" "$@"
EOF_STAT4_OK
chmod +x "$tmp/bin/stat4-ok"

cat > "$tmp/bin/stat4-no" <<'EOF_STAT4_NO'
#!/usr/bin/env bash
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done
case "$sql_text" in
  *"sqlite_compileoption_used('ENABLE_STAT4')"*) printf '0\n'; exit 0 ;;
  *"SELECT 1"*) printf '1\n'; exit 0 ;;
esac
exec "$REAL_SQLITE" "$@"
EOF_STAT4_NO
chmod +x "$tmp/bin/stat4-no"

plex_stat4_preflight "$tmp/bin/stat4-ok" >"$tmp/preflight-ok.out" 2>"$tmp/preflight-ok.err"
assert_eq "" "$(cat "$tmp/preflight-ok.err")" "STAT4 positive preflight stderr"

set +e
plex_stat4_preflight "$tmp/bin/stat4-missing" >"$tmp/preflight-missing.out" 2>"$tmp/preflight-missing.err"
missing_rc=$?
plex_stat4_preflight "$tmp/bin/stat4-no" >"$tmp/preflight-no.out" 2>"$tmp/preflight-no.err"
no_rc=$?
set -e
assert_eq "1" "$missing_rc" "STAT4 missing binary preflight rc"
assert_contains "$(cat "$tmp/preflight-missing.err")" "WARNING: Plex STAT4 pre-flight failed" "STAT4 missing binary warning"
assert_eq "1" "$no_rc" "STAT4 no compile option preflight rc"
assert_contains "$(cat "$tmp/preflight-no.err")" "ENABLE_STAT4" "STAT4 no compile option warning"

create_plex_stat4_fixture() {
  local db include_control_names include_icu_names include_fts_tables
  db="$1"
  include_control_names="${2:-1}"
  include_icu_names="${3:-1}"
  include_fts_tables="${4:-0}"

  TEST_DB="$db" INCLUDE_CONTROL_NAMES="$include_control_names" INCLUDE_ICU_NAMES="$include_icu_names" INCLUDE_FTS_TABLES="$include_fts_tables" "$python_bin" <<'PY'
import os
import sqlite3

path = os.environ["TEST_DB"]
include_control_names = os.environ.get("INCLUDE_CONTROL_NAMES") == "1"
include_icu_names = os.environ.get("INCLUDE_ICU_NAMES") == "1"
include_fts_tables = os.environ.get("INCLUDE_FTS_TABLES") == "1"
con = sqlite3.connect(path)
con.create_collation("icu_root", lambda left, right: (left > right) - (left < right))
cur = con.cursor()
cur.executescript(
    """
    PRAGMA page_size=4096;
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
    CREATE TABLE metadata_item_settings(
      id INTEGER PRIMARY KEY,
      account_id INTEGER NOT NULL,
      updated_at INTEGER NOT NULL,
      guid TEXT NOT NULL,
      view_offset INTEGER NOT NULL,
      last_viewed_at INTEGER NOT NULL
    );
    CREATE INDEX index_metadata_item_settings_on_account_id ON metadata_item_settings(account_id);
    CREATE TABLE metadata_items(
      id INTEGER PRIMARY KEY,
      title_sort TEXT,
      library_section_id INTEGER NOT NULL,
      metadata_type INTEGER NOT NULL,
      added_at INTEGER,
      originally_available_at INTEGER
    );
    CREATE INDEX index_metadata_items_on_library_section_id ON metadata_items(library_section_id);
    CREATE TABLE "quote""table"(id INTEGER PRIMARY KEY, value TEXT NOT NULL);
    CREATE INDEX "quote""idx" ON "quote""table"(value);
    """
)
if include_icu_names:
    cur.executescript(
        """
        CREATE INDEX index_title_sort_custom_icu ON metadata_items(title_sort COLLATE icu_root);
        CREATE TABLE auto_icu(
          id INTEGER PRIMARY KEY,
          title TEXT COLLATE icu_root UNIQUE,
          safe_key INTEGER NOT NULL
        );
        CREATE INDEX ix_auto_icu_safe ON auto_icu(safe_key);
        """
    )
if include_control_names:
    cur.executescript(
        """
        CREATE TABLE "control
name"(id INTEGER PRIMARY KEY, value TEXT NOT NULL);
        CREATE INDEX "control
idx" ON "control
name"(value);
        """
    )
if include_fts_tables:
    cur.executescript(
        """
        CREATE VIRTUAL TABLE fts5_metadata_titles USING fts5(title);
        """
    )
cur.executemany(
    "INSERT INTO taggings(id, tag_id, metadata_item_id) VALUES(?, ?, ?)",
    ((i, i % 25, i) for i in range(1, 5001)),
)
cur.executemany(
    "INSERT INTO metadata_item_settings(id, account_id, updated_at, guid, view_offset, last_viewed_at) VALUES(?, ?, ?, ?, ?, ?)",
    (
        (i, 42 if i <= 4500 else i, 2000000 - i, f"plex://fixture/{i}", i * 100, 1000000 + i)
        for i in range(1, 5001)
    ),
)
cur.executemany(
    "INSERT INTO metadata_items(id, title_sort, library_section_id, metadata_type, added_at, originally_available_at) VALUES(?, ?, ?, ?, ?, ?)",
    ((i, f"title-{i % 100}", i % 12, i % 5, 9999999999 if i == 1 else 1000000 + i, 1 if i == 2 else 1000000 + i) for i in range(1, 5001)),
)
if include_icu_names:
    cur.executemany(
        "INSERT INTO auto_icu(id, title, safe_key) VALUES(?, ?, ?)",
        ((i, f"auto-{i}", i % 17) for i in range(1, 101)),
    )
cur.executemany(
    'INSERT INTO "quote""table"(id, value) VALUES(?, ?)',
    ((i, f"quoted-{i % 9}") for i in range(1, 101)),
)
if include_control_names:
    cur.executemany(
        'INSERT INTO "control\nname"(id, value) VALUES(?, ?)',
        ((i, f"control-{i % 9}") for i in range(1, 101)),
    )
if include_fts_tables:
    cur.executemany(
        "INSERT INTO fts5_metadata_titles(rowid, title) VALUES(?, ?)",
        ((i, f"fts-title-{i % 11}") for i in range(1, 101)),
    )
con.commit()
con.close()
PY

  local ddl
  for ddl in "${PLEX_INDEXES[@]}"; do
    "$real_sqlite" "$db" "$ddl"
  done
}

fixture="$tmp/plex-stat4-fixture.db"
create_plex_stat4_fixture "$fixture"
targets="$(discover_plex_stat4_analyze_targets "$real_sqlite" "$fixture")"
assert_contains "$targets" $'T\ttaggings' "STAT4 worklist taggings table target"
assert_contains "$targets" $'T\tmetadata_item_settings' "STAT4 worklist metadata_item_settings table target"
assert_contains "$targets" $'I\tindex_metadata_items_on_library_section_id' "STAT4 worklist safe metadata_items index target"
assert_contains "$targets" $'I\tix_auto_icu_safe' "STAT4 worklist safe explicit index beside ICU autoindex"
assert_contains "$targets" $'T\tquote"table' "STAT4 worklist quote-bearing table target"
assert_not_contains "$targets" $'T\tmetadata_items' "STAT4 worklist skips table-level metadata_items"
assert_not_contains "$targets" "index_title_sort_custom_icu" "STAT4 worklist skips unsafe ICU-collated index"
assert_not_contains "$targets" "sqlite_autoindex" "STAT4 worklist skips autoindexes as targets"
assert_not_contains "$targets" "control" "STAT4 worklist skips control-character identifiers before shell parsing"

cat > "$tmp/bin/stat4-discovery-fails" <<'EOF_STAT4_DISCOVERY_FAILS'
#!/usr/bin/env bash
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

case "$sql_text" in
  *"pragma_index_list"*)
    printf 'simulated discovery failure\n' >&2
    exit 81
    ;;
esac

exec "$REAL_SQLITE" "$@"
EOF_STAT4_DISCOVERY_FAILS
chmod +x "$tmp/bin/stat4-discovery-fails"
set +e
run_plex_stat4_analyze "$tmp/bin/stat4-discovery-fails" "$fixture" >"$tmp/stat4-discovery-fails.out" 2>"$tmp/stat4-discovery-fails.err"
discovery_fails_rc=$?
set -e
assert_eq "0" "$discovery_fails_rc" "STAT4 discovery failure rc"
assert_contains "$(cat "$tmp/stat4-discovery-fails.err")" "WARNING: Plex STAT4 worklist discovery failed" "STAT4 discovery failure warning"

cat > "$tmp/bin/stat4-sqlite" <<'EOF_STAT4_SQLITE'
#!/usr/bin/env bash
db_arg="${1:-}"
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

case "$sql_text" in
  *"ANALYZE "*)
    printf '%s\n' "$sql_text" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g; s/[[:space:]]$//' >> "$STAT4_ANALYZE_LOG"
    if [ -n "${ORDER_LOG:-}" ]; then
      printf 'stat4\n' >> "$ORDER_LOG"
      : > "$STAT4_MARKER"
    fi
    if [ "${STAT4_FAIL_NEEDLE:-}" != "" ]; then
      case "$sql_text" in
        *"$STAT4_FAIL_NEEDLE"*)
          printf 'simulated STAT4 ANALYZE failure\n' >&2
          exit 80
          ;;
      esac
    fi
    "$REAL_SQLITE" "$@"
    rc=$?
    if [ "$rc" -eq 0 ] && [ "${CORRUPT_AFTER_STAT4:-0}" = "1" ]; then
      printf 'not a sqlite database' > "$db_arg"
    fi
    exit "$rc"
    ;;
esac

exec "$REAL_SQLITE" "$@"
EOF_STAT4_SQLITE
chmod +x "$tmp/bin/stat4-sqlite"

export STAT4_ANALYZE_LOG="$tmp/stat4-analyze.log"
: > "$STAT4_ANALYZE_LOG"
run_plex_stat4_analyze "$tmp/bin/stat4-sqlite" "$fixture" >"$tmp/stat4-run.out" 2>"$tmp/stat4-run.err"
assert_not_contains "$(cat "$tmp/stat4-run.err")" "no such collation sequence" "STAT4 run avoids ICU collation failure"
assert_contains "$(cat "$tmp/stat4-run.out")" "Plex STAT4 analyze targets:" "STAT4 run progress"
stat4_log="$(cat "$STAT4_ANALYZE_LOG")"
assert_not_contains "$stat4_log" 'ANALYZE "metadata_items"' "STAT4 run skips metadata_items table ANALYZE"
assert_contains "$stat4_log" 'ANALYZE "index_metadata_items_on_library_section_id"' "STAT4 run analyzes safe metadata_items index"
assert_not_contains "$stat4_log" 'ANALYZE "index_title_sort_custom_icu"' "STAT4 run skips ICU index"
assert_contains "$stat4_log" 'ANALYZE "quote""table"' "STAT4 run quotes identifiers"
if [ "$stat4_available" = "1" ]; then
  assert_contains "$(cat "$tmp/stat4-run.out")" "Plex STAT4 sqlite_stat4 rows:" "STAT4 run final row count"
  taggings_stat4="$("$real_sqlite" "$fixture" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx='idx_dshadow_taggings_tag_id_metadata_item_id';")"
  settings_stat4="$("$real_sqlite" "$fixture" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx='idx_dshadow_mis_account_updated_guid_cover';")"
  assert_int_gt "$taggings_stat4" 0 "taggings leader index sqlite_stat4 rows"
  assert_int_gt "$settings_stat4" 0 "metadata_item_settings leader index sqlite_stat4 rows"
else
  printf 'SKIP: sqlite_stat4 unavailable in this sqlite3 build\n'
fi

failure_fixture="$tmp/plex-stat4-failure.db"
create_plex_stat4_fixture "$failure_fixture"
: > "$STAT4_ANALYZE_LOG"
export STAT4_FAIL_NEEDLE="ix_auto_icu_safe"
run_plex_stat4_analyze "$tmp/bin/stat4-sqlite" "$failure_fixture" >"$tmp/stat4-failure.out" 2>"$tmp/stat4-failure.err"
unset STAT4_FAIL_NEEDLE
assert_contains "$(cat "$tmp/stat4-failure.err")" "WARNING: Plex STAT4 ANALYZE failed for ix_auto_icu_safe" "STAT4 per-target failure warning"
if [ "$stat4_available" = "1" ]; then
  assert_int_gt "$("$real_sqlite" "$failure_fixture" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx='idx_dshadow_taggings_tag_id_metadata_item_id';")" 0 "STAT4 per-target failure preserves earlier safe target rows"
fi

cat > "$tmp/bin/plex-sqlite" <<'EOF_PLEX_SQLITE'
#!/usr/bin/env bash
db_arg="${1:-}"
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

case "$db_arg" in
  *.new)
    case "$sql_text" in
      *"ANALYZE;"*|*"PRAGMA optimize=0x10002;"*) printf 'plex-maintenance\n' >> "$ORDER_LOG" ;;
      *"UPDATE metadata_items SET"*) printf 'metadata-repair\n' >> "$ORDER_LOG" ;;
      *"PRAGMA integrity_check;"*)
        if [ -e "$STAT4_MARKER" ]; then
          printf 'post-stat4-integrity\n' >> "$ORDER_LOG"
        fi
        ;;
      *"PRAGMA user_version;"*) printf 'user-version\n' >> "$ORDER_LOG" ;;
      *"PRAGMA application_id;"*) printf 'application-id\n' >> "$ORDER_LOG"; : > "$POST_SWAP_SQL_GUARD_MARKER" ;;
    esac
    ;;
  *)
    if [ -n "${POST_SWAP_SQL_GUARD_MARKER:-}" ] && [ -e "$POST_SWAP_SQL_GUARD_MARKER" ]; then
      case "$sql_text" in
        *"UPDATE metadata_items SET"*) printf 'post-swap-metadata-repair\n' >> "$ORDER_LOG" ;;
        *"VALUES('optimize')"*) printf 'post-swap-fts-optimize\n' >> "$ORDER_LOG" ;;
      esac
    fi
    ;;
esac

exec "$REAL_SQLITE" "$@"
EOF_PLEX_SQLITE
chmod +x "$tmp/bin/plex-sqlite"

run_plex_optimize_with_stat4_capture() {
  local name db_dir rc
  name="$1"
  db_dir="$2"

  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    PLEX_BINARY="$tmp/bin/plex-sqlite"
    GENERIC_SQLITE_BINARY="$tmp/bin/stat4-sqlite"
    plex_stat4_enabled=1
    PLEX_DATABASES_PATH="$db_dir"
    PLEX_INSTANCE="plex-stat4-fixture"
    BACKUP_PATH="$tmp/backups"
    optimize_plex_db "$PLEX_DB" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" ""
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

integration_dir="$tmp/integration"
mkdir -p "$integration_dir"
integration_db="$integration_dir/$PLEX_DB"
create_plex_stat4_fixture "$integration_db" 0 0 1
export ORDER_LOG="$tmp/order.log"
export STAT4_MARKER="$tmp/stat4-marker"
export POST_SWAP_SQL_GUARD_MARKER="$tmp/post-swap-sql-guard-marker"
: > "$ORDER_LOG"
rm -f "$STAT4_MARKER" "$POST_SWAP_SQL_GUARD_MARKER"
run_plex_optimize_with_stat4_capture ordered "$integration_dir"
ordered_rc="$(cat "$tmp/ordered.rc")"
[ "$ordered_rc" = "0" ] || fail "ordered Plex STAT4 pipeline rc" "0" "rc=$ordered_rc stderr=$(cat "$tmp/ordered.err")"
plex_line="$(first_line_of "$ORDER_LOG" "plex-maintenance")"
repair_line="$(first_line_of "$ORDER_LOG" "metadata-repair")"
stat4_line="$(first_line_of "$ORDER_LOG" "stat4")"
integrity_line="$(first_line_of "$ORDER_LOG" "post-stat4-integrity")"
user_version_line="$(first_line_of "$ORDER_LOG" "user-version")"
application_id_line="$(first_line_of "$ORDER_LOG" "application-id")"
post_swap_fts_line="$(first_line_of "$ORDER_LOG" "post-swap-fts-optimize")"
assert_line_lt "$plex_line" "$repair_line" "Plex staged maintenance before metadata repair"
assert_line_lt "$repair_line" "$stat4_line" "metadata repair before STAT4"
assert_line_lt "$stat4_line" "$integrity_line" "STAT4 before post-STAT4 integrity"
assert_line_lt "$integrity_line" "$user_version_line" "post-STAT4 integrity before metadata preservation"
assert_line_lt "$user_version_line" "$application_id_line" "user_version before application_id preservation"
assert_line_lt "$application_id_line" "$post_swap_fts_line" "metadata preservation before post-swap FTS optimize"
later_plex_line="$(awk -v stat4_line="$stat4_line" 'NR > stat4_line && index($0, "plex-maintenance") { print NR; exit }' "$ORDER_LOG")"
assert_eq "" "$later_plex_line" "no Plex staged maintenance SQL after STAT4"
later_repair_line="$(first_line_after "$ORDER_LOG" "metadata-repair" "$stat4_line")"
assert_eq "" "$later_repair_line" "no metadata_items repair after STAT4"
post_swap_repair_line="$(first_line_of "$ORDER_LOG" "post-swap-metadata-repair")"
assert_eq "" "$post_swap_repair_line" "no metadata_items repair in post-swap SQL path"
if [ "$stat4_available" = "1" ]; then
  assert_int_gt "$("$real_sqlite" "$integration_db" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx='idx_dshadow_taggings_tag_id_metadata_item_id';")" 0 "pipeline taggings STAT4 rows"
  assert_int_gt "$("$real_sqlite" "$integration_db" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx='idx_dshadow_mis_account_updated_guid_cover';")" 0 "pipeline metadata_item_settings STAT4 rows"
fi

corrupt_dir="$tmp/corrupt"
mkdir -p "$corrupt_dir"
corrupt_db="$corrupt_dir/$PLEX_DB"
create_plex_stat4_fixture "$corrupt_db" 0 0
corrupt_hash_before="$(sha256_file "$corrupt_db")"
: > "$ORDER_LOG"
rm -f "$STAT4_MARKER" "$POST_SWAP_SQL_GUARD_MARKER"
export CORRUPT_AFTER_STAT4=1
run_plex_optimize_with_stat4_capture corrupt-after-stat4 "$corrupt_dir"
unset CORRUPT_AFTER_STAT4
assert_eq "1" "$(cat "$tmp/corrupt-after-stat4.rc")" "post-STAT4 integrity gate rc"
assert_contains "$(cat "$tmp/corrupt-after-stat4.err")" "post-STAT4 staged integrity_check" "post-STAT4 integrity gate diagnostic"
assert_eq "$corrupt_hash_before" "$(sha256_file "$corrupt_db")" "post-STAT4 integrity gate live hash unchanged"
[ ! -e "$corrupt_db.new" ] || fail "post-STAT4 integrity gate cleanup" "no staged file" "$corrupt_db.new exists"

printf 'optimize_media_servers Plex STAT4 tests passed\n'
