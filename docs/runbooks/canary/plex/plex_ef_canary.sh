#!/usr/bin/env bash
set -euo pipefail

umask 077
export LC_ALL=C

HARNESS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RUNBOOK_DIR="$(cd -- "$HARNESS_DIR/.." && pwd)"
ENV_FILE="$RUNBOOK_DIR/env"
if [ ! -f "$ENV_FILE" ]; then
  printf 'error: missing %s; copy %s and fill local values
' "$ENV_FILE" "$RUNBOOK_DIR/env.example" >&2
  exit 2
fi
# shellcheck source=/dev/null
. "$ENV_FILE"

require_env() {
  local name value
  for name in "$@"; do
    value="${!name-}"
    [ -n "$value" ] || {
      printf 'error: %s is required in %s; see %s
' "$name" "$ENV_FILE" "$RUNBOOK_DIR/env.example" >&2
      exit 2
    }
  done
}

require_env SQLITE_BIN SCRATCH_ROOT PLEX_SQLITE_BIN PLEX_ORIG PLEX_TIMED_ITERS   PLEX_TAG_SECTION_ID PLEX_TAG_ID PLEX_TAG_INDEX PLEX_ONDECK_SECTION_ID   PLEX_ONDECK_ACCOUNT_ID PLEX_ONDECK_INDEX PLEX_ONDECK_IDLIST

GEN_BIN="$SQLITE_BIN"
PLEX_BIN="$PLEX_SQLITE_BIN"
PLEX_CAND="${SCRATCH_ROOT}/plex-cand.db"
RUN_ID="${RUN_ID:-plex-ef-canary-$(date -u +%Y%m%dT%H%M%SZ)}"
case "$RUN_ID" in
  ''|*/*|*..*) printf 'error: RUN_ID must be a single path segment without ..\n' >&2; exit 2 ;;
esac
RUN_DIR="${SCRATCH_ROOT}/${RUN_ID}"
SQL_DIR="${RUN_DIR}/sql"
OUT_DIR="${RUN_DIR}/out"
CAPTURE_LOG="${RUN_DIR}/capture.log"
RESULTS_TSV="${RUN_DIR}/results.tsv"
CONCERNS_TSV="${RUN_DIR}/concerns.tsv"
STATUS_FILE="${RUN_DIR}/STATUS.txt"
TIMED_ITERS="${TIMED_ITERS:-$PLEX_TIMED_ITERS}"
QUERY_TIMEOUT_S="${QUERY_TIMEOUT_S:-10}"
DISK_MARGIN_BYTES="${DISK_MARGIN_BYTES:-1073741824}"

TAG_SECTION_ID="$PLEX_TAG_SECTION_ID"
TAG_ID="$PLEX_TAG_ID"
TAG_INDEX="$PLEX_TAG_INDEX"
ONDECK_SECTION_ID="$PLEX_ONDECK_SECTION_ID"
ONDECK_ACCOUNT_ID="$PLEX_ONDECK_ACCOUNT_ID"
ONDECK_INDEX="$PLEX_ONDECK_INDEX"
ONDECK_IDLIST="$PLEX_ONDECK_IDLIST"

mkdir -p "$SCRATCH_ROOT"
case "$RUN_DIR" in
  "$SCRATCH_ROOT"/*) ;;
  *) printf 'error: RUN_DIR must be under %s\n' "$SCRATCH_ROOT" >&2; exit 2 ;;
esac
mkdir -p "$SQL_DIR" "$OUT_DIR"
: > "$CAPTURE_LOG"
: > "$STATUS_FILE"
printf 'family_shape\tstat_state\tarm\tadoption\tmedian_ms\tidentity\n' > "$RESULTS_TSV"
printf 'family_shape\tstat_state\tconcern\tdetail\n' > "$CONCERNS_TSV"

log() {
  printf '%s\n' "$*" | tee -a "$CAPTURE_LOG" >&2
}

write_status() {
  printf 'STATUS: %s\n' "$1" > "$STATUS_FILE"
}

is_sqlite_copy_argv() {
  local args=$1
  local copy_path=$2
  local sqlite_bin
  shift 2
  case " $args " in
    *" $copy_path "*) ;;
    *) return 1 ;;
  esac
  for sqlite_bin in "$@"; do
    case "$args" in
      "$sqlite_bin"|"$sqlite_bin "*) return 0 ;;
    esac
  done
  return 1
}

cleanup_orphans() {
  local pid args pids_file
  pids_file="${RUN_DIR}/orphan-pids.txt"
  if command -v pgrep >/dev/null 2>&1; then
    pgrep -af -- "$PLEX_CAND" > "$pids_file" 2>/dev/null || true
    while IFS= read -r line; do
      pid=${line%% *}
      args=${line#* }
      case "$pid" in
        ''|*[!0-9]*) continue ;;
      esac
      if [ "$pid" != "$$" ] && is_sqlite_copy_argv "$args" "$PLEX_CAND" "$GEN_BIN" "$PLEX_BIN"; then
        log "ORPHAN_CLEANUP exact_copy_path=${PLEX_CAND} pid=${pid}"
        kill "$pid" 2>/dev/null || true
      fi
    done < "$pids_file"
  fi
}

on_exit() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    cleanup_orphans
    if [ ! -s "$STATUS_FILE" ]; then
      write_status "BLOCKED"
    fi
  fi
  cleanup_copy_files
}

trap 'exit 130' INT
trap 'exit 143' TERM
trap on_exit EXIT

die() {
  local msg=$1
  local status=${2:-BLOCKED}
  log "ERROR ${msg}"
  write_status "$status"
  exit 1
}

cleanup_copy_files() {
  case "$PLEX_CAND" in
    "$SCRATCH_ROOT"/*) ;;
    *) return 0 ;;
  esac
  rm -f "$PLEX_CAND" "$PLEX_CAND-wal" "$PLEX_CAND-shm" "$PLEX_CAND-journal"
}

require_file() {
  [ -f "$1" ] || die "missing file: $1"
}

require_executable() {
  [ -x "$1" ] || die "missing executable: $1"
}

validate_iter_count() {
  case "$TIMED_ITERS" in
    ''|*[!0-9]*) die "TIMED_ITERS must be an integer >=3, got ${TIMED_ITERS}" ;;
  esac
  if [ "$TIMED_ITERS" -lt 3 ]; then
    die "TIMED_ITERS must be >=3, got ${TIMED_ITERS}"
  fi
  case "$QUERY_TIMEOUT_S" in
    ''|*[!0-9]*) die "QUERY_TIMEOUT_S must be a positive integer, got ${QUERY_TIMEOUT_S}" ;;
  esac
  if [ "$QUERY_TIMEOUT_S" -lt 1 ]; then
    die "QUERY_TIMEOUT_S must be a positive integer, got ${QUERY_TIMEOUT_S}"
  fi
  case "$DISK_MARGIN_BYTES" in
    ''|*[!0-9]*) die "DISK_MARGIN_BYTES must be a non-negative integer, got ${DISK_MARGIN_BYTES}" ;;
  esac
  command -v timeout >/dev/null 2>&1 || die "timeout command is required"
}

run_sql_file() {
  local label=$1
  local bin=$2
  local db=$3
  local sql=$4
  local out=$5
  local rc
  log "PHASE label=${label} bin=${bin} db=${db} sql=${sql} out=${out}"
  set +e
  "$bin" -batch "$db" < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    die "${label} failed rc=${rc} out=${out}"
  fi
}

run_timed_sql_file() {
  local label=$1
  local bin=$2
  local db=$3
  local sql=$4
  local out=$5
  local rc
  log "PHASE label=${label} bin=${bin} db=${db} sql=${sql} out=${out} timeout_s=${QUERY_TIMEOUT_S}"
  set +e
  timeout "${QUERY_TIMEOUT_S}s" "$bin" -batch "$db" < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  case "$rc" in
    0) return 0 ;;
    124) return 124 ;;
    *) die "${label} failed rc=${rc} out=${out}" ;;
  esac
}

run_sql_no_db() {
  local label=$1
  local bin=$2
  local sql=$3
  local out=$4
  local rc
  log "PHASE label=${label} bin=${bin} sql=${sql} out=${out}"
  set +e
  "$bin" -batch < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    die "${label} failed rc=${rc} out=${out}"
  fi
}

log_prefixed_file() {
  local prefix=$1
  local src=$2
  local tagged="${src}.tagged"
  sed "s/^/${prefix}/" "$src" > "$tagged" || true
  while IFS= read -r line; do
    log "$line"
  done < "$tagged"
}

append_result() {
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >> "$RESULTS_TSV"
}

record_concern() {
  local family_shape=$1
  local stat_state=$2
  local concern=$3
  local detail=$4
  printf '%s\t%s\t%s\t%s\n' "$family_shape" "$stat_state" "$concern" "$detail" >> "$CONCERNS_TSV"
  log "CONCERN stat_state=${stat_state} family_shape=${family_shape} concern=${concern} detail=${detail}"
}

idlist_count() {
  local csv=$1
  local old_ifs=$IFS
  local count=0
  local item
  IFS=,
  for item in $csv; do
    [ -n "$item" ] && count=$((count + 1))
  done
  IFS=$old_ifs
  printf '%s\n' "$count"
}

sqlite_preamble() {
  cat <<'SQL'
.bail on
.mode list
.separator "\t"
.headers off
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-1048576;
PRAGMA mmap_size=34359738368;
PRAGMA threads=8;
SQL
}

normalize_sqlite_text_output() {
  local src=$1
  awk '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function unquote_sqlite_text(s) {
      s = trim(s)
      if (length(s) >= 2 && substr(s, 1, 1) == "\"" && substr(s, length(s), 1) == "\"") {
        s = substr(s, 2, length(s) - 2)
        gsub(/""/, "\"", s)
      }
      return trim(s)
    }
    { print unquote_sqlite_text($0) }
  ' "$src"
}

sqlite_kv_value() {
  local src=$1
  local selector=$2
  local key=$3
  normalize_sqlite_text_output "$src" | awk -v selector="$selector" -v key="$key" '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function unquote_sqlite_text(s) {
      s = trim(s)
      if (length(s) >= 2 && substr(s, 1, 1) == "\"" && substr(s, length(s), 1) == "\"") {
        s = substr(s, 2, length(s) - 2)
        gsub(/""/, "\"", s)
      }
      return trim(s)
    }
    index($0, selector) {
      for (i = 1; i <= NF; i++) {
        if (index($i, key "=") == 1) {
          print unquote_sqlite_text(substr($i, length(key) + 2))
          exit
        }
      }
    }
  '
}

sqlite_uint_value() {
  local src=$1
  local selector=$2
  local key=$3
  local value
  value=$(sqlite_kv_value "$src" "$selector" "$key")
  case "$value" in
    ''|*[!0-9]*) return 1 ;;
  esac
  printf '%s\n' "$value"
}

first_normalized_line() {
  local src=$1
  normalize_sqlite_text_output "$src" | awk 'NF { print; exit }'
}

refresh_copy() {
  local backup_sql="${SQL_DIR}/refresh-copy.sql"
  local backup_out="${OUT_DIR}/refresh-copy.out"
  log "PHASE label=refresh_copy source=${PLEX_ORIG} dest=${PLEX_CAND}"
  rm -f "$PLEX_CAND" "$PLEX_CAND-wal" "$PLEX_CAND-shm"
  cat > "$backup_sql" <<SQL
.bail on
.open --readonly '${PLEX_ORIG}'
.backup '${PLEX_CAND}'
SQL
  run_sql_no_db "refresh_copy" "$GEN_BIN" "$backup_sql" "$backup_out"
}

file_size_bytes() {
  local path=$1
  local size
  if size=$(stat -c %s "$path" 2>/dev/null); then
    printf '%s\n' "$size"
  elif size=$(stat -f %z "$path" 2>/dev/null); then
    printf '%s\n' "$size"
  else
    die "could not stat file size: ${path}"
  fi
}

available_bytes() {
  local path=$1
  df -Pk "$path" | awk 'NR == 2 { printf "%.0f\n", $4 * 1024 }'
}

disk_preflight() {
  local source_bytes free_bytes required_bytes
  source_bytes=$(file_size_bytes "$PLEX_ORIG")
  free_bytes=$(available_bytes "$SCRATCH_ROOT")
  case "$free_bytes" in
    ''|*[!0-9]*) die "could not determine free bytes for ${SCRATCH_ROOT}" ;;
  esac
  required_bytes=$((source_bytes * 2 + DISK_MARGIN_BYTES))
  if [ "$free_bytes" -lt "$required_bytes" ]; then
    die "insufficient free space under ${SCRATCH_ROOT}: free=${free_bytes} required=${required_bytes} source_bytes=${source_bytes} margin=${DISK_MARGIN_BYTES}"
  fi
  log "PHASE label=disk_preflight scratch=${SCRATCH_ROOT} free=${free_bytes} required=${required_bytes} source_bytes=${source_bytes} margin=${DISK_MARGIN_BYTES}"
}

create_g2() {
  local sql="${SQL_DIR}/create-g2.sql"
  local out="${OUT_DIR}/create-g2.out"
  {
    sqlite_preamble
    cat <<SQL
CREATE INDEX IF NOT EXISTS ${ONDECK_INDEX}
ON metadata_item_views (account_id, grandparent_guid);
SQL
  } > "$sql"
  run_sql_file "create_g2" "$GEN_BIN" "$PLEX_CAND" "$sql" "$out"
}

write_e_queries() {
  cat > "${SQL_DIR}/plex-e07-baseline.query.sql" <<SQL
select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (${TAG_SECTION_ID}) and (metadata_items.metadata_type=1 and tags.id=${TAG_ID}) );
SQL
  cat > "${SQL_DIR}/plex-e07-candidate.query.sql" <<SQL
select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (${TAG_SECTION_ID}) and (metadata_items.metadata_type=1 and tags.id=${TAG_ID} AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=${TAG_ID})) );
SQL
}

emit_f01_baseline_query_body() {
  printf '%s' 'select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id='
  printf '%s' "$ONDECK_SECTION_ID"
  printf '%s' ' and grandparents.id in ('
  printf '%s' "$ONDECK_IDLIST"
  printf '%s' ') and metadata_item_settings.view_count>0  and metadata_item_views.account_id='
  printf '%s' "$ONDECK_ACCOUNT_ID"
  printf '%s\n' ' group by grandparents.id order by viewed_at desc;'
}

emit_f01_candidate_query_body() {
  {
    cat <<'SQL'
SELECT grandparents_id AS id,
       originally_available_at AS originally_available_at,
       parent_index AS parent_index,
       metadata_item_views_index AS "index",
       viewed_at AS "max(viewed_at)",
       library_section_id AS library_section_id,
       grandparents_extra_data AS extra_data
FROM (
  SELECT grandparents.id AS grandparents_id,
         metadata_item_views.originally_available_at AS originally_available_at,
         metadata_item_views.parent_index AS parent_index,
         metadata_item_views.`index` AS metadata_item_views_index,
         metadata_item_views.viewed_at AS viewed_at,
         grandparents.library_section_id AS library_section_id,
         grandparentsSettings.extra_data AS grandparents_extra_data,
         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS canary_on_deck_rank
  FROM metadata_items AS grandparents
  CROSS JOIN metadata_item_views
  JOIN metadata_item_settings
  JOIN metadata_item_settings AS grandparentsSettings
  WHERE grandparents.guid=metadata_item_views.grandparent_guid
    AND metadata_item_settings.guid=metadata_item_views.guid
    AND metadata_item_views.account_id=metadata_item_settings.account_id
    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid
    AND metadata_item_views.account_id=grandparentsSettings.account_id
SQL
    printf '%s' '    AND metadata_item_views.library_section_id='
    printf '%s' "$ONDECK_SECTION_ID"
    printf '%s' $'\n    AND grandparents.id IN ('
    printf '%s' "$ONDECK_IDLIST"
    printf '%s' $')\n    AND metadata_item_settings.view_count>0\n    AND metadata_item_views.account_id='
    printf '%s' "$ONDECK_ACCOUNT_ID"
    cat <<'SQL'

) AS canary_on_deck_ranked
WHERE canary_on_deck_rank=1
ORDER BY viewed_at DESC, grandparents_id DESC;
SQL
  }
}

write_f01_baseline_query() {
  emit_f01_baseline_query_body > "${SQL_DIR}/plex-f01-baseline.query.sql"
}

write_f01_candidate_query() {
  emit_f01_candidate_query_body > "${SQL_DIR}/plex-f01-candidate.query.sql"
}

write_query_files() {
  write_e_queries
  write_f01_baseline_query
  write_f01_candidate_query
}

prove_state() {
  local stat_state=$1
  local sql="${SQL_DIR}/${stat_state}-state.sql"
  local out="${OUT_DIR}/${stat_state}-state.out"
  local parsed="${OUT_DIR}/${stat_state}-state.normalized.out"
  local g2_stat1 g2_stat4
  {
    sqlite_preamble
    cat <<SQL
SELECT 'STATE stat_state=${stat_state} key=sqlite_version value=' || sqlite_version();
SELECT 'STATE stat_state=${stat_state} key=database name=' || name || ' file=' || file FROM pragma_database_list;
SELECT 'STATE stat_state=${stat_state} key=index name=' || name || ' sql=' || replace(replace(sql, char(10), ' '), char(13), ' ')
FROM sqlite_master
WHERE type='index'
  AND name IN ('${TAG_INDEX}', '${ONDECK_INDEX}', 'index_metadata_item_views_on_guid', 'index_metadata_items_on_guid', 'index_metadata_item_settings_on_account_id', 'index_metadata_item_settings_on_guid')
ORDER BY name;
SELECT 'STATE stat_state=${stat_state} key=stat1_total rows=' || count(*) FROM sqlite_stat1;
SELECT 'STATE stat_state=${stat_state} key=stat4_total rows=' || count(*) FROM sqlite_stat4;
SELECT 'STATE stat_state=${stat_state} key=g2_stat1_rows rows=' || count(*) FROM sqlite_stat1 WHERE idx='${ONDECK_INDEX}';
SELECT 'STATE stat_state=${stat_state} key=g2_stat4_rows rows=' || count(*) FROM sqlite_stat4 WHERE idx='${ONDECK_INDEX}';
SQL
  } > "$sql"
  run_sql_file "state_${stat_state}" "$GEN_BIN" "$PLEX_CAND" "$sql" "$out"
  normalize_sqlite_text_output "$out" > "$parsed"
  log_prefixed_file "" "$parsed"
  grep -F "key=index name=${TAG_INDEX}" "$parsed" >/dev/null ||
    record_concern "state" "$stat_state" "missing_index" "name=${TAG_INDEX}"
  grep -F "key=index name=${ONDECK_INDEX}" "$parsed" >/dev/null ||
    record_concern "state" "$stat_state" "missing_index" "name=${ONDECK_INDEX}"
  if ! g2_stat1=$(sqlite_uint_value "$parsed" "key=g2_stat1_rows" "rows"); then
    g2_stat1="missing"
    record_concern "state" "$stat_state" "missing_numeric_state" "key=g2_stat1_rows"
  fi
  if ! g2_stat4=$(sqlite_uint_value "$parsed" "key=g2_stat4_rows" "rows"); then
    g2_stat4="missing"
    record_concern "state" "$stat_state" "missing_numeric_state" "key=g2_stat4_rows"
  fi
  if [ "$stat_state" = "current" ] && [ "$g2_stat1" != "0" ]; then
    record_concern "state" "$stat_state" "unexpected_g2_analyzed_state" "stat1_rows=${g2_stat1}"
  fi
  if [ "$stat_state" = "current" ] && [ "$g2_stat4" != "0" ]; then
    record_concern "state" "$stat_state" "unexpected_g2_analyzed_state" "stat4_rows=${g2_stat4}"
  fi
  case "$g2_stat1" in
    ''|*[!0-9]*) ;;
    *)
      if [ "$stat_state" = "analyzed" ] && [ "$g2_stat1" -lt 1 ]; then
        record_concern "state" "$stat_state" "unexpected_g2_analyzed_state" "stat1_rows=${g2_stat1}"
      fi
      ;;
  esac
}

verify_literals() {
  local stat_state=$1
  local sql="${SQL_DIR}/${stat_state}-literal-verify.sql"
  local out="${OUT_DIR}/${stat_state}-literal-verify.out"
  local parsed="${OUT_DIR}/${stat_state}-literal-verify.normalized.out"
  local id_count tag_exists taggings_for_tag views_for_account_section grandparents_present
  id_count=$(idlist_count "$ONDECK_IDLIST")
  {
    sqlite_preamble
    cat <<SQL
SELECT 'LITERAL_SOURCE stat_state=${stat_state} family_shape=plex-e07 source=env tag_id=${TAG_ID} capture=docs/design/plex-slow-query-census-2026-07-07.sql';
SELECT 'LITERAL_SOURCE stat_state=${stat_state} family_shape=plex-e03 source=env tag_id=${TAG_ID} capture=docs/design/plex-slow-query-census-2026-07-07.sql hash=96c8be56';
SELECT 'LITERAL_SOURCE stat_state=${stat_state} family_shape=plex-f01 source=env section_id=${ONDECK_SECTION_ID} account_id=${ONDECK_ACCOUNT_ID} idlist_items=${id_count} capture=docs/design/plex-slow-query-census-2026-07-07.sql';
SELECT 'LITERAL_VERIFY stat_state=${stat_state} family_shape=plex-e07 key=tag_exists value=' || count(*) FROM tags WHERE id=${TAG_ID};
SELECT 'LITERAL_VERIFY stat_state=${stat_state} family_shape=plex-e07 key=taggings_for_tag value=' || count(*) FROM taggings WHERE tag_id=${TAG_ID};
SELECT 'LITERAL_VERIFY stat_state=${stat_state} family_shape=plex-f01 key=views_for_account_section value=' || count(*) FROM metadata_item_views WHERE library_section_id=${ONDECK_SECTION_ID} AND account_id=${ONDECK_ACCOUNT_ID};
SELECT 'LITERAL_VERIFY stat_state=${stat_state} family_shape=plex-f01 key=grandparents_present value=' || count(*) FROM metadata_items WHERE id IN (${ONDECK_IDLIST});
SQL
  } > "$sql"
  run_sql_file "literal_verify_${stat_state}" "$GEN_BIN" "$PLEX_CAND" "$sql" "$out"
  normalize_sqlite_text_output "$out" > "$parsed"
  log_prefixed_file "" "$parsed"
  if ! tag_exists=$(sqlite_uint_value "$parsed" "family_shape=plex-e07 key=tag_exists" "value"); then
    tag_exists="missing"
    record_concern "plex-e07" "$stat_state" "missing_numeric_literal" "key=tag_exists"
  fi
  if ! taggings_for_tag=$(sqlite_uint_value "$parsed" "family_shape=plex-e07 key=taggings_for_tag" "value"); then
    taggings_for_tag="missing"
    record_concern "plex-e07" "$stat_state" "missing_numeric_literal" "key=taggings_for_tag"
  fi
  if ! views_for_account_section=$(sqlite_uint_value "$parsed" "family_shape=plex-f01 key=views_for_account_section" "value"); then
    views_for_account_section="missing"
    record_concern "plex-f01" "$stat_state" "missing_numeric_literal" "key=views_for_account_section"
  fi
  if ! grandparents_present=$(sqlite_uint_value "$parsed" "family_shape=plex-f01 key=grandparents_present" "value"); then
    grandparents_present="missing"
    record_concern "plex-f01" "$stat_state" "missing_numeric_literal" "key=grandparents_present"
  fi
  [ "$tag_exists" != "0" ] ||
    record_concern "plex-e07" "$stat_state" "literal_count_empty" "key=tag_exists tag_id=${TAG_ID}"
  [ "$taggings_for_tag" != "0" ] ||
    record_concern "plex-e07" "$stat_state" "literal_count_empty" "key=taggings_for_tag tag_id=${TAG_ID}"
  [ "$views_for_account_section" != "0" ] ||
    record_concern "plex-f01" "$stat_state" "literal_count_empty" "key=views_for_account_section section_id=${ONDECK_SECTION_ID} account_id=${ONDECK_ACCOUNT_ID}"
  [ "$grandparents_present" != "0" ] ||
    record_concern "plex-f01" "$stat_state" "literal_count_empty" "key=grandparents_present"
}

write_e07_identity_sql() {
  local stat_state=$1
  local sql=$2
  {
    sqlite_preamble
    cat <<SQL
WITH baseline(v) AS (
  select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (${TAG_SECTION_ID}) and (metadata_items.metadata_type=1 and tags.id=${TAG_ID}) )
),
candidate(v) AS (
  select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (${TAG_SECTION_ID}) and (metadata_items.metadata_type=1 and tags.id=${TAG_ID} AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=${TAG_ID})) )
)
SELECT 'IDENTITY_RESULT stat_state=${stat_state} family_shape=plex-e07 identity=' ||
       CASE WHEN baseline.v IS candidate.v THEN 'PASS' ELSE 'FAIL' END ||
       ' baseline_count=' || baseline.v ||
       ' candidate_count=' || candidate.v
FROM baseline, candidate;
SQL
  } > "$sql"
}

write_e03_identity_sql() {
  local stat_state=$1
  local sql=$2
  {
    sqlite_preamble
    cat <<SQL
DROP TABLE IF EXISTS temp.e03_baseline_identity;
DROP TABLE IF EXISTS temp.e03_candidate_identity;
CREATE TEMP TABLE e03_baseline_identity AS
SELECT id, count(*) AS n
FROM (
  select metadata_items.id AS id
  from metadata_items
  left join taggings on taggings.metadata_item_id=metadata_items.id
  left join tags on taggings.tag_id=tags.id
  where metadata_items.library_section_id in (${TAG_SECTION_ID})
    and (metadata_items.metadata_type=1 and tags.id=${TAG_ID})
)
GROUP BY id;
CREATE TEMP TABLE e03_candidate_identity AS
SELECT id, count(*) AS n
FROM (
  select metadata_items.id AS id
  from metadata_items
  left join taggings on taggings.metadata_item_id=metadata_items.id
  left join tags on taggings.tag_id=tags.id
  where metadata_items.library_section_id in (${TAG_SECTION_ID})
    and (metadata_items.metadata_type=1 and tags.id=${TAG_ID} AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=${TAG_ID}))
)
GROUP BY id;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-e03 arm=baseline groups=' || count(*) || ' rows=' || coalesce(sum(n), 0) FROM e03_baseline_identity;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-e03 arm=candidate groups=' || count(*) || ' rows=' || coalesce(sum(n), 0) FROM e03_candidate_identity;
WITH diff AS (
  SELECT b.id AS id, b.n AS baseline_n, c.n AS candidate_n, 'missing_or_count_mismatch' AS kind
  FROM e03_baseline_identity AS b
  LEFT JOIN e03_candidate_identity AS c USING (id)
  WHERE c.id IS NULL OR c.n <> b.n
  UNION ALL
  SELECT c.id AS id, b.n AS baseline_n, c.n AS candidate_n, 'extra_candidate' AS kind
  FROM e03_candidate_identity AS c
  LEFT JOIN e03_baseline_identity AS b USING (id)
  WHERE b.id IS NULL
)
SELECT 'IDENTITY_RESULT stat_state=${stat_state} family_shape=plex-e03 identity=' ||
       CASE WHEN count(*) = 0 THEN 'PASS' ELSE 'FAIL' END ||
       ' diff_count=' || count(*)
FROM diff;
WITH diff AS (
  SELECT b.id AS id, b.n AS baseline_n, c.n AS candidate_n, 'missing_or_count_mismatch' AS kind
  FROM e03_baseline_identity AS b
  LEFT JOIN e03_candidate_identity AS c USING (id)
  WHERE c.id IS NULL OR c.n <> b.n
  UNION ALL
  SELECT c.id AS id, b.n AS baseline_n, c.n AS candidate_n, 'extra_candidate' AS kind
  FROM e03_candidate_identity AS c
  LEFT JOIN e03_baseline_identity AS b USING (id)
  WHERE b.id IS NULL
)
SELECT 'IDENTITY_DIFF stat_state=${stat_state} family_shape=plex-e03 kind=' || kind ||
       ' id=' || id ||
       ' baseline_n=' || coalesce(baseline_n, 'NULL') ||
       ' candidate_n=' || coalesce(candidate_n, 'NULL')
FROM diff
LIMIT 20;
SQL
  } > "$sql"
}

write_f01_identity_sql() {
  local stat_state=$1
  local sql=$2
  {
    sqlite_preamble
    cat <<'SQL'
DROP TABLE IF EXISTS temp.f01_original;
DROP TABLE IF EXISTS temp.f01_rewrite;
DROP TABLE IF EXISTS temp.f01_bound;
DROP TABLE IF EXISTS temp.f01_joined;
DROP TABLE IF EXISTS temp.f01_oracle;
DROP TABLE IF EXISTS temp.f01_tie_counts;
DROP TABLE IF EXISTS temp.f01_projection_diff;
CREATE TEMP TABLE f01_original AS
SQL
    emit_f01_baseline_query_body
    cat <<'SQL'
CREATE TEMP TABLE f01_rewrite AS
SQL
    emit_f01_candidate_query_body
    cat <<SQL
.parameter init
.parameter set ?1 ${ONDECK_SECTION_ID}
CREATE TEMP TABLE f01_bound AS
select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.\`index\`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=? and grandparents.id in (${ONDECK_IDLIST}) and metadata_item_settings.view_count>0  and metadata_item_views.account_id=${ONDECK_ACCOUNT_ID} group by grandparents.id order by viewed_at desc;
SQL
    cat <<'SQL'
CREATE TEMP TABLE f01_joined AS
SELECT grandparents.id AS id,
       metadata_item_views.originally_available_at AS originally_available_at,
       metadata_item_views.parent_index AS parent_index,
       metadata_item_views.`index` AS "index",
       metadata_item_views.viewed_at AS "max(viewed_at)",
       grandparents.library_section_id AS library_section_id,
       grandparentsSettings.extra_data AS extra_data,
       metadata_item_views.viewed_at AS viewed_at,
       metadata_item_views.id AS metadata_item_views_id,
       grandparentsSettings.id AS grandparents_settings_id,
       metadata_item_settings.id AS metadata_item_settings_id
FROM metadata_items AS grandparents
CROSS JOIN metadata_item_views
JOIN metadata_item_settings
JOIN metadata_item_settings AS grandparentsSettings
WHERE grandparents.guid=metadata_item_views.grandparent_guid
  AND metadata_item_settings.guid=metadata_item_views.guid
  AND metadata_item_views.account_id=metadata_item_settings.account_id
  AND grandparentsSettings.guid=metadata_item_views.grandparent_guid
  AND metadata_item_views.account_id=grandparentsSettings.account_id
SQL
    printf '%s' '  AND metadata_item_views.library_section_id='
    printf '%s' "$ONDECK_SECTION_ID"
    printf '%s' $'\n  AND grandparents.id IN ('
    printf '%s' "$ONDECK_IDLIST"
    printf '%s' $')\n  AND metadata_item_settings.view_count>0\n  AND metadata_item_views.account_id='
    printf '%s' "$ONDECK_ACCOUNT_ID"
    cat <<'SQL'
;
CREATE TEMP TABLE f01_oracle AS
SELECT id,
       originally_available_at,
       parent_index,
       "index",
       "max(viewed_at)",
       library_section_id,
       extra_data,
       viewed_at
FROM (
  SELECT f01_joined.*,
         row_number() OVER (
           PARTITION BY id
           ORDER BY viewed_at DESC,
                    metadata_item_views_id DESC,
                    grandparents_settings_id DESC,
                    metadata_item_settings_id DESC
         ) AS rn
  FROM f01_joined
)
WHERE rn=1;
CREATE TEMP TABLE f01_tie_counts AS
SELECT o.id, count(*) AS tie_count
FROM f01_oracle AS o
JOIN f01_joined AS j
  ON j.id=o.id
 AND j.viewed_at IS o.viewed_at
GROUP BY o.id;
CREATE TEMP TABLE f01_projection_diff AS
SELECT o.id
FROM f01_original AS o
JOIN f01_rewrite AS r USING (id)
WHERE NOT (
  o.originally_available_at IS r.originally_available_at
  AND o.parent_index IS r.parent_index
  AND o."index" IS r."index"
  AND o."max(viewed_at)" IS r."max(viewed_at)"
  AND o.library_section_id IS r.library_section_id
  AND o.extra_data IS r.extra_data
);
SQL
    cat <<SQL
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-f01 key=original_groups value=' || count(*) FROM f01_original;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-f01 key=rewrite_groups value=' || count(*) FROM f01_rewrite;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-f01 key=bound_groups value=' || count(*) FROM f01_bound;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-f01 key=projection_diff_groups value=' || count(*) FROM f01_projection_diff;
SELECT 'IDENTITY_DETAIL stat_state=${stat_state} family_shape=plex-f01 key=all_null_viewed_at_tie_groups value=' || count(*)
FROM f01_oracle AS o
JOIN f01_tie_counts AS t USING (id)
WHERE o.viewed_at IS NULL AND t.tie_count > 1;
WITH checks AS (
  SELECT
    (SELECT count(*) FROM f01_original AS o LEFT JOIN f01_rewrite AS r USING (id) WHERE r.id IS NULL) AS missing_rewrite,
    (SELECT count(*) FROM f01_rewrite AS r LEFT JOIN f01_original AS o USING (id) WHERE o.id IS NULL) AS extra_rewrite,
    (SELECT count(*) FROM f01_original AS o JOIN f01_rewrite AS r USING (id) WHERE NOT (o."max(viewed_at)" IS r."max(viewed_at)")) AS max_mismatch,
    (SELECT count(*) FROM f01_original AS o LEFT JOIN f01_bound AS b USING (id) WHERE b.id IS NULL) AS missing_bound,
    (SELECT count(*) FROM f01_bound AS b LEFT JOIN f01_original AS o USING (id) WHERE o.id IS NULL) AS extra_bound,
    (SELECT count(*) FROM f01_original AS o JOIN f01_bound AS b USING (id) WHERE NOT (
      o.originally_available_at IS b.originally_available_at
      AND o.parent_index IS b.parent_index
      AND o."index" IS b."index"
      AND o."max(viewed_at)" IS b."max(viewed_at)"
      AND o.library_section_id IS b.library_section_id
      AND o.extra_data IS b.extra_data
    )) AS bound_mismatch,
    (SELECT count(*) FROM f01_projection_diff AS d LEFT JOIN f01_tie_counts AS t USING (id) WHERE coalesce(t.tie_count, 0) <= 1) AS untied_projection_diff,
    (SELECT count(*)
       FROM f01_projection_diff AS d
       JOIN f01_rewrite AS r USING (id)
       JOIN f01_oracle AS x USING (id)
      WHERE NOT (
        r.originally_available_at IS x.originally_available_at
        AND r.parent_index IS x.parent_index
        AND r."index" IS x."index"
        AND r."max(viewed_at)" IS x."max(viewed_at)"
        AND r.library_section_id IS x.library_section_id
        AND r.extra_data IS x.extra_data
      )) AS differing_group_oracle_mismatch,
    (SELECT count(*)
       FROM f01_rewrite AS r
       JOIN f01_oracle AS x USING (id)
      WHERE NOT (
        r.originally_available_at IS x.originally_available_at
        AND r.parent_index IS x.parent_index
        AND r."index" IS x."index"
        AND r."max(viewed_at)" IS x."max(viewed_at)"
        AND r.library_section_id IS x.library_section_id
        AND r.extra_data IS x.extra_data
      )) AS rewrite_oracle_mismatch
)
SELECT 'IDENTITY_RESULT stat_state=${stat_state} family_shape=plex-f01 identity=' ||
       CASE
         WHEN (SELECT count(*) FROM f01_original)=0
          AND (SELECT count(*) FROM f01_rewrite)=0
          AND (SELECT count(*) FROM f01_bound)=0
         THEN 'VACUOUS'
         WHEN (SELECT count(*) FROM f01_original)>0
          AND (SELECT count(*) FROM f01_rewrite)>0
          AND (SELECT count(*) FROM f01_bound)>0
          AND missing_rewrite=0
          AND extra_rewrite=0
          AND max_mismatch=0
          AND missing_bound=0
          AND extra_bound=0
          AND bound_mismatch=0
          AND untied_projection_diff=0
          AND differing_group_oracle_mismatch=0
          AND rewrite_oracle_mismatch=0
         THEN 'PASS'
         ELSE 'FAIL'
       END ||
       ' missing_rewrite=' || missing_rewrite ||
       ' extra_rewrite=' || extra_rewrite ||
       ' max_mismatch=' || max_mismatch ||
       ' missing_bound=' || missing_bound ||
       ' extra_bound=' || extra_bound ||
       ' bound_mismatch=' || bound_mismatch ||
       ' projection_diff_untied=' || untied_projection_diff ||
       ' differing_group_oracle_mismatch=' || differing_group_oracle_mismatch ||
       ' rewrite_oracle_mismatch=' || rewrite_oracle_mismatch
FROM checks;
SELECT 'IDENTITY_DIFF stat_state=${stat_state} family_shape=plex-f01 kind=projection_diff id=' || d.id ||
       ' tie_count=' || coalesce(t.tie_count, 0)
FROM f01_projection_diff AS d
LEFT JOIN f01_tie_counts AS t USING (id)
LIMIT 20;
SQL
  } > "$sql"
}

run_identity_sql() {
  local stat_state=$1
  local family_shape=$2
  local bin=$3
  local writer=$4
  local sql="${SQL_DIR}/${stat_state}-${family_shape}-identity.sql"
  local out="${OUT_DIR}/${stat_state}-${family_shape}-identity.out"
  local tagged="${OUT_DIR}/${stat_state}-${family_shape}-identity-lines.out"
  local identity_result
  "$writer" "$stat_state" "$sql"
  run_sql_file "identity_${stat_state}_${family_shape}" "$bin" "$PLEX_CAND" "$sql" "$out"
  normalize_sqlite_text_output "$out" | grep '^IDENTITY_' > "$tagged" || true
  while IFS= read -r line; do
    log "$line"
  done < "$tagged"
  identity_result=$(sqlite_kv_value "$tagged" "IDENTITY_RESULT stat_state=${stat_state} family_shape=${family_shape}" "identity")
  if [ "$family_shape" = "plex-f01" ]; then
    f01_identity_result="$identity_result"
  fi
  case "$identity_result" in
    PASS|VACUOUS) return 0 ;;
    *) return 1 ;;
  esac
}

run_e_identities() {
  local stat_state=$1
  if run_identity_sql "$stat_state" "plex-e03" "$PLEX_BIN" "write_e03_identity_sql"; then
    append_result "plex-e03" "$stat_state" "baseline" "identity_only_plex_bin" "NA" "PASS"
    append_result "plex-e03" "$stat_state" "candidate" "identity_only_plex_bin" "NA" "PASS"
  else
    append_result "plex-e03" "$stat_state" "baseline" "identity_only_plex_bin" "NA" "FAIL"
    append_result "plex-e03" "$stat_state" "candidate" "identity_only_plex_bin" "NA" "FAIL"
    die "plex-e03 PLEX_BIN grouped identity failed for ${stat_state}" "DONE_WITH_CONCERNS"
  fi
  if ! run_identity_sql "$stat_state" "plex-e07" "$GEN_BIN" "write_e07_identity_sql"; then
    append_result "plex-e07" "$stat_state" "baseline" "not_timed_identity_failed" "NA" "FAIL"
    append_result "plex-e07" "$stat_state" "candidate" "not_timed_identity_failed" "NA" "FAIL"
    die "plex-e07 GEN_BIN count identity failed for ${stat_state}" "DONE_WITH_CONCERNS"
  fi
}

run_f_identity() {
  local stat_state=$1
  if ! run_identity_sql "$stat_state" "plex-f01" "$GEN_BIN" "write_f01_identity_sql"; then
    append_result "plex-f01" "$stat_state" "baseline" "not_timed_identity_failed" "NA" "FAIL"
    append_result "plex-f01" "$stat_state" "candidate" "not_timed_identity_failed" "NA" "FAIL"
    die "plex-f01 full-projection oracle identity failed for ${stat_state}" "DONE_WITH_CONCERNS"
  fi
}

write_eqp_sql() {
  local query_file=$1
  local sql=$2
  {
    sqlite_preamble
    printf 'EXPLAIN QUERY PLAN\n'
    cat "$query_file"
  } > "$sql"
}

detect_e_adoption() {
  local arm=$1
  local eqp=$2
  local has_list=0
  local has_idx=0
  local has_rowid=0
  grep -E 'LIST SUBQUERY|LIST-SUBQUERY' "$eqp" >/dev/null && has_list=1
  grep -F "$TAG_INDEX" "$eqp" >/dev/null && has_idx=1
  grep -E 'SEARCH metadata_items USING INTEGER PRIMARY KEY' "$eqp" >/dev/null && has_rowid=1
  if [ "$arm" = "candidate" ] && [ "$has_list" -eq 1 ] && [ "$has_idx" -eq 1 ]; then
    printf 'adopted\n'
    return
  fi
  printf 'not-adopted\n'
}

describe_e_adoption() {
  local eqp=$1
  local has_list=0
  local has_idx=0
  local has_rowid=0
  grep -E 'LIST SUBQUERY|LIST-SUBQUERY' "$eqp" >/dev/null && has_list=1
  grep -F "$TAG_INDEX" "$eqp" >/dev/null && has_idx=1
  grep -E 'SEARCH metadata_items USING INTEGER PRIMARY KEY' "$eqp" >/dev/null && has_rowid=1
  printf 'list=%s idx=%s rowid=%s index=%s' "$has_list" "$has_idx" "$has_rowid" "$TAG_INDEX"
}

detect_f_adoption() {
  local arm=$1
  local eqp=$2
  local has_g2=0
  local has_vendor_hint=0
  local first_driver
  grep -F "$ONDECK_INDEX" "$eqp" >/dev/null && has_g2=1
  grep -F "index_metadata_item_views_on_guid" "$eqp" >/dev/null && has_vendor_hint=1
  first_driver=$(awk '
    /(SEARCH|SCAN) (grandparents|metadata_item_views)( |$)/ {
      if ($0 ~ /(SEARCH|SCAN) grandparents( |$)/) { print "grandparents"; exit }
      if ($0 ~ /(SEARCH|SCAN) metadata_item_views( |$)/) { print "metadata_item_views"; exit }
    }
  ' "$eqp")
  if [ "$arm" = "candidate" ] && [ "$has_g2" -eq 1 ] && [ "${first_driver:-}" = "grandparents" ]; then
    printf 'adopted\n'
    return
  fi
  printf 'not-adopted\n'
}

describe_f_adoption() {
  local eqp=$1
  local has_g2=0
  local has_vendor_hint=0
  local first_driver
  grep -F "$ONDECK_INDEX" "$eqp" >/dev/null && has_g2=1
  grep -F "index_metadata_item_views_on_guid" "$eqp" >/dev/null && has_vendor_hint=1
  first_driver=$(awk '
    /(SEARCH|SCAN) (grandparents|metadata_item_views)( |$)/ {
      if ($0 ~ /(SEARCH|SCAN) grandparents( |$)/) { print "grandparents"; exit }
      if ($0 ~ /(SEARCH|SCAN) metadata_item_views( |$)/) { print "metadata_item_views"; exit }
    }
  ' "$eqp")
  printf 'first=%s g2=%s vendor_hint=%s index=%s' "${first_driver:-none}" "$has_g2" "$has_vendor_hint" "$ONDECK_INDEX"
}

report_adoption() {
  local stat_state=$1
  local family_shape=$2
  local arm=$3
  local adoption=$4
  local detail=$5
  log "NOTE stat_state=${stat_state} family_shape=${family_shape} arm=${arm} gate=adoption adoption=${adoption} detail=${detail}"
  if [ "$arm" = "candidate" ] && [ "$adoption" != "adopted" ]; then
    record_concern "$family_shape" "$stat_state" "candidate_not_adopted" "arm=${arm} detail=${detail}"
  fi
  if [ "$arm" = "baseline" ] && [ "$adoption" = "adopted" ]; then
    record_concern "$family_shape" "$stat_state" "baseline_adopted" "arm=${arm} detail=${detail}"
  fi
}

write_run_sql() {
  local query_file=$1
  local sql=$2
  local stat_state=$3
  local family_shape=$4
  local arm=$5
  local adoption=$6
  local phase=$7
  local iter=$8
  {
    sqlite_preamble
    printf '.print RUN stat_state=%s family_shape=%s arm=%s adoption=%s phase=%s iter=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$phase" "$iter"
    if [ "$phase" = "timed" ]; then
      printf '.timer on\n'
    fi
    cat "$query_file"
    if [ "$phase" = "timed" ]; then
      printf '.timer off\n'
    fi
  } > "$sql"
}

prewarm_copy() {
  log "PHASE label=prewarm copy=${PLEX_CAND}"
  cat "$PLEX_CAND" "$PLEX_CAND-wal" "$PLEX_CAND-shm" >/dev/null 2>&1 || true
}

extract_real_ms() {
  local out=$1
  awk '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function unquote_sqlite_text(s) {
      s = trim(s)
      if (length(s) >= 2 && substr(s, 1, 1) == "\"" && substr(s, length(s), 1) == "\"") {
        s = substr(s, 2, length(s) - 2)
        gsub(/""/, "\"", s)
      }
      return trim(s)
    }
    function is_number(s) {
      return s ~ /^[-+]?[0-9]+([.][0-9]+)?([eE][-+]?[0-9]+)?$|^[-+]?[.][0-9]+([eE][-+]?[0-9]+)?$/
    }
    {
      line = unquote_sqlite_text($0)
      if (line !~ /^Run Time:/) {
        next
      }
      $0 = line
      for (i=1; i<=NF; i++) {
        if ($i == "real") {
          value = unquote_sqlite_text($(i+1))
          if (is_number(value)) {
            real = value + 0.0
            found = 1
          }
        }
      }
    }
    END {
      if (!found) {
        exit 1
      }
      printf "%.3f\n", (real * 1000.0)
    }
  ' "$out"
}

median_file() {
  local src=$1
  awk '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function unquote_sqlite_text(s) {
      s = trim(s)
      if (length(s) >= 2 && substr(s, 1, 1) == "\"" && substr(s, length(s), 1) == "\"") {
        s = substr(s, 2, length(s) - 2)
        gsub(/""/, "\"", s)
      }
      return trim(s)
    }
    function is_number(s) {
      return s ~ /^[-+]?[0-9]+([.][0-9]+)?([eE][-+]?[0-9]+)?$|^[-+]?[.][0-9]+([eE][-+]?[0-9]+)?$/
    }
    NF {
      value = unquote_sqlite_text($1)
      if (!is_number(value)) {
        bad = 1
        exit
      }
      n++
      a[n]=value + 0.0
    }
    END {
      if (bad || n == 0) exit 1
      for (i=1; i<=n; i++) {
        for (j=i+1; j<=n; j++) {
          if (a[j] < a[i]) {
            t=a[i]; a[i]=a[j]; a[j]=t
          }
        }
      }
      if (n % 2) {
        printf "%.3f\n", a[(n+1)/2]
      } else {
        printf "%.3f\n", (a[n/2] + a[n/2+1]) / 2.0
      }
    }
  ' "$src"
}

timeout_verdict_for_pair() {
  local baseline_timed_out=$1
  local candidate_timed_out=$2
  if [ "$candidate_timed_out" -eq 1 ] && [ "$baseline_timed_out" -eq 0 ]; then
    printf 'REGRESSION\n'
  elif [ "$baseline_timed_out" -eq 1 ] && [ "$candidate_timed_out" -eq 0 ]; then
    printf 'WIN\n'
  elif [ "$baseline_timed_out" -eq 1 ] && [ "$candidate_timed_out" -eq 1 ]; then
    printf 'INCONCLUSIVE_BOTH_SLOW\n'
  else
    printf 'NONE\n'
  fi
}

run_timed_family() {
  local stat_state=$1
  local family_shape=$2
  local identity=$3
  local baseline_query="${SQL_DIR}/${family_shape}-baseline.query.sql"
  local candidate_query="${SQL_DIR}/${family_shape}-candidate.query.sql"
  local arm query eqp_sql eqp_out adoption adoption_detail
  local iter first second run_sql out real_ms timings median_ms rc
  local baseline_timed_out=0
  local candidate_timed_out=0
  local timeout_verdict

  for arm in baseline candidate; do
    if [ "$arm" = "baseline" ]; then
      query=$baseline_query
    else
      query=$candidate_query
    fi
    eqp_sql="${SQL_DIR}/${stat_state}-${family_shape}-${arm}-eqp.sql"
    eqp_out="${OUT_DIR}/${stat_state}-${family_shape}-${arm}-eqp.out"
    write_eqp_sql "$query" "$eqp_sql"
    run_sql_file "eqp_${stat_state}_${family_shape}_${arm}" "$GEN_BIN" "$PLEX_CAND" "$eqp_sql" "$eqp_out"
    if [ "$family_shape" = "plex-e07" ]; then
      adoption=$(detect_e_adoption "$arm" "$eqp_out")
      adoption_detail=$(describe_e_adoption "$eqp_out")
    else
      adoption=$(detect_f_adoption "$arm" "$eqp_out")
      adoption_detail=$(describe_f_adoption "$eqp_out")
    fi
    report_adoption "$stat_state" "$family_shape" "$arm" "$adoption" "$adoption_detail"
    log_prefixed_file "EQP stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} iter=NA " "$eqp_out"
    printf '%s\n' "$adoption" > "${OUT_DIR}/${stat_state}-${family_shape}-${arm}-adoption.txt"
  done

  for arm in baseline candidate; do
    : > "${OUT_DIR}/${stat_state}-${family_shape}-${arm}-timings.ms"
  done

  prewarm_copy
  for arm in baseline candidate; do
    if [ "$arm" = "baseline" ]; then
      query=$baseline_query
    else
      query=$candidate_query
    fi
    adoption=$(first_normalized_line "${OUT_DIR}/${stat_state}-${family_shape}-${arm}-adoption.txt")
    run_sql="${SQL_DIR}/${stat_state}-${family_shape}-${arm}-warm.sql"
    out="${OUT_DIR}/${stat_state}-${family_shape}-${arm}-warm.out"
    write_run_sql "$query" "$run_sql" "$stat_state" "$family_shape" "$arm" "$adoption" "warm" "0"
    if run_timed_sql_file "warm_${stat_state}_${family_shape}_${arm}" "$GEN_BIN" "$PLEX_CAND" "$run_sql" "$out"; then
      log "WARM stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} iter=0 out=${out}"
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then
        [ "$arm" = "baseline" ] && baseline_timed_out=1 || candidate_timed_out=1
        log "TIMEOUT_WARMUP stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} timeout_s=${QUERY_TIMEOUT_S} out=${out}"
        continue
      fi
      die "warm-up query returned unexpected rc=${rc} for ${stat_state} ${family_shape} ${arm}"
    fi
  done

  iter=1
  while [ "$iter" -le "$TIMED_ITERS" ]; do
    if [ $((iter % 2)) -eq 1 ]; then
      first=baseline
      second=candidate
    else
      first=candidate
      second=baseline
    fi
    for arm in "$first" "$second"; do
      if [ "$arm" = "baseline" ]; then
        query=$baseline_query
      else
        query=$candidate_query
      fi
      if [ "$arm" = "baseline" ] && [ "$baseline_timed_out" -eq 1 ]; then
        log "TIME_SKIP stat_state=${stat_state} family_shape=${family_shape} arm=${arm} iter=${iter} reason=prior_timeout"
        continue
      fi
      if [ "$arm" = "candidate" ] && [ "$candidate_timed_out" -eq 1 ]; then
        log "TIME_SKIP stat_state=${stat_state} family_shape=${family_shape} arm=${arm} iter=${iter} reason=prior_timeout"
        continue
      fi
      adoption=$(first_normalized_line "${OUT_DIR}/${stat_state}-${family_shape}-${arm}-adoption.txt")
      run_sql="${SQL_DIR}/${stat_state}-${family_shape}-${arm}-iter-${iter}.sql"
      out="${OUT_DIR}/${stat_state}-${family_shape}-${arm}-iter-${iter}.out"
      timings="${OUT_DIR}/${stat_state}-${family_shape}-${arm}-timings.ms"
      write_run_sql "$query" "$run_sql" "$stat_state" "$family_shape" "$arm" "$adoption" "timed" "$iter"
      if run_timed_sql_file "timed_${stat_state}_${family_shape}_${arm}_${iter}" "$GEN_BIN" "$PLEX_CAND" "$run_sql" "$out"; then
        :
      else
        rc=$?
        if [ "$rc" -eq 124 ]; then
          [ "$arm" = "baseline" ] && baseline_timed_out=1 || candidate_timed_out=1
          log "TIMEOUT stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} iter=${iter} timeout_s=${QUERY_TIMEOUT_S} out=${out}"
          continue
        fi
        die "timed query returned unexpected rc=${rc} for ${stat_state} ${family_shape} ${arm} iter=${iter}"
      fi
      if ! real_ms=$(extract_real_ms "$out"); then
        die "missing or invalid timer output for ${stat_state} ${family_shape} ${arm} iter=${iter}"
      fi
      [ -n "$real_ms" ] || die "missing or invalid timer output for ${stat_state} ${family_shape} ${arm} iter=${iter}"
      printf '%s\n' "$real_ms" >> "$timings"
      log "TIMING stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} iter=${iter} real_ms=${real_ms} out=${out}"
    done
    if [ "$baseline_timed_out" -eq 1 ] && [ "$candidate_timed_out" -eq 1 ]; then
      break
    fi
    iter=$((iter + 1))
  done

  for arm in baseline candidate; do
    adoption=$(first_normalized_line "${OUT_DIR}/${stat_state}-${family_shape}-${arm}-adoption.txt")
    timings="${OUT_DIR}/${stat_state}-${family_shape}-${arm}-timings.ms"
    if [ "$arm" = "baseline" ] && [ "$baseline_timed_out" -eq 1 ]; then
      median_ms=TIMEOUT
    elif [ "$arm" = "candidate" ] && [ "$candidate_timed_out" -eq 1 ]; then
      median_ms=TIMEOUT
    else
      if ! median_ms=$(median_file "$timings"); then
        die "missing or invalid median input for ${stat_state} ${family_shape} ${arm}"
      fi
    fi
    log "MEDIAN stat_state=${stat_state} family_shape=${family_shape} arm=${arm} adoption=${adoption} iter=median median_ms=${median_ms} runs=${TIMED_ITERS}"
    append_result "$family_shape" "$stat_state" "$arm" "$adoption" "$median_ms" "$identity"
  done
  timeout_verdict=$(timeout_verdict_for_pair "$baseline_timed_out" "$candidate_timed_out")
  if [ "$timeout_verdict" != NONE ]; then
    log "TIMEOUT_VERDICT stat_state=${stat_state} family_shape=${family_shape} verdict=${timeout_verdict} timeout_s=${QUERY_TIMEOUT_S}"
    record_concern "$family_shape" "$stat_state" "timeout_verdict" "verdict=${timeout_verdict} timeout_s=${QUERY_TIMEOUT_S}"
  fi
}

analyze_copy() {
  local sql="${SQL_DIR}/analyze-main.sql"
  local out="${OUT_DIR}/analyze-main.out"
  {
    sqlite_preamble
    cat <<'SQL'
PRAGMA analysis_limit=0;
ANALYZE main;
SQL
  } > "$sql"
  run_sql_file "analyze_main" "$GEN_BIN" "$PLEX_CAND" "$sql" "$out"
  log_prefixed_file "ANALYZE " "$out"
}

run_stat_state() {
  local stat_state=$1
  prove_state "$stat_state"
  verify_literals "$stat_state"
  run_e_identities "$stat_state"
  run_f_identity "$stat_state"
  run_timed_family "$stat_state" "plex-e07" "PASS"
  run_timed_family "$stat_state" "plex-f01" "$f01_identity_result"
}

main() {
  validate_iter_count
  require_executable "$GEN_BIN"
  require_executable "$PLEX_BIN"
  require_file "$PLEX_ORIG"
  log "START run_dir=${RUN_DIR} results=${RESULTS_TSV} timed_iters=${TIMED_ITERS}"
  log "HOST_CONSTRAINT scratch_root=${SCRATCH_ROOT} copy=${PLEX_CAND} originals_read_only=${PLEX_ORIG}"
  log "MEDIAN_POLICY timed_iters=${TIMED_ITERS} report=median_not_fastest order=counterbalanced"
  write_query_files
  disk_preflight
  refresh_copy
  create_g2
  run_stat_state "current"
  analyze_copy
  run_stat_state "analyzed"
  log "RESULTS_FILE ${RESULTS_TSV}"
  log "CAPTURE_LOG ${CAPTURE_LOG}"
  if [ -s "$CONCERNS_TSV" ] && [ "$(wc -l < "$CONCERNS_TSV" | awk '{print $1}')" -gt 1 ]; then
    log "CONCERNS_FILE ${CONCERNS_TSV}"
    write_status "DONE_WITH_CONCERNS"
    log "STATUS: DONE_WITH_CONCERNS"
  else
    write_status "DONE"
    log "STATUS: DONE"
  fi
}

main "$@"
