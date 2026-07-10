#!/usr/bin/env bash
set -euo pipefail

HARNESS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RUNBOOK_DIR="$(cd -- "$HARNESS_DIR/.." && pwd)"
ENV_FILE="$RUNBOOK_DIR/env"
if [ ! -f "$ENV_FILE" ]; then
  printf 'ERROR: missing %s; copy %s and fill local values
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
      printf 'ERROR: %s is required in %s; see %s
' "$name" "$ENV_FILE" "$RUNBOOK_DIR/env.example" >&2
      exit 2
    }
  done
}

require_env SQLITE_BIN SCRATCH_ROOT EMBY_ORIG EMBY_ITERATIONS EMBY_LATEST_INDEX   CANARY_EMBY_A_SHAPE01_USER_ID CANARY_EMBY_A_SHAPE01_ANCESTORS CANARY_EMBY_A_SHAPE01_EXPECTED_ROWS   CANARY_EMBY_A_SHAPE04_USER_ID CANARY_EMBY_A_SHAPE04_ANCESTORS CANARY_EMBY_A_SHAPE04_EXPECTED_ROWS   CANARY_EMBY_C_EMPTY_CAPTURE_FILE CANARY_EMBY_C_EMPTY_CAPTURE_HASH CANARY_EMBY_C_EMPTY_USER_ID CANARY_EMBY_C_EMPTY_ANCESTORS CANARY_EMBY_C_EMPTY_EXPECTED_GROUPS   CANARY_EMBY_C_TWO_GROUP_CAPTURE_FILE CANARY_EMBY_C_TWO_GROUP_CAPTURE_HASH CANARY_EMBY_C_TWO_GROUP_USER_ID CANARY_EMBY_C_TWO_GROUP_ANCESTORS CANARY_EMBY_C_TWO_GROUP_EXPECTED_GROUPS   CANARY_EMBY_C_LARGE_GROUP_CAPTURE_FILE CANARY_EMBY_C_LARGE_GROUP_CAPTURE_HASH CANARY_EMBY_C_LARGE_GROUP_USER_ID CANARY_EMBY_C_LARGE_GROUP_ANCESTORS CANARY_EMBY_C_LARGE_GROUP_EXPECTED_GROUPS   CANARY_EMBY_D_EP_LATEST_USER_ID CANARY_EMBY_D_EP_LATEST_ANCESTORS CANARY_EMBY_D_EP_LATEST_EXPECTED_DISTINCT_IDS

EMBY_COPY="$SCRATCH_ROOT/emby-cand.db"
STATIC_CAPTURE_DIR="$HARNESS_DIR/static-captures"
LATEST_INDEX="$EMBY_LATEST_INDEX"
ITERATIONS="${ITERATIONS:-$EMBY_ITERATIONS}"
QUERY_TIMEOUT_S="${QUERY_TIMEOUT_S:-10}"
DISK_MARGIN_BYTES="${DISK_MARGIN_BYTES:-1073741824}"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
RUN_DIR="$SCRATCH_ROOT/emby-canary-$RUN_ID"
QUERY_DIR="$RUN_DIR/queries"
OUT_DIR="$RUN_DIR/out"
EQP_LOG="$RUN_DIR/eqp-tagged.log"
IDENTITY_LOG="$RUN_DIR/identity.log"
TIMINGS_TSV="$RUN_DIR/timings.tsv"
MEDIANS_LOG="$RUN_DIR/medians.log"
RESULTS_TSV="$RUN_DIR/results.tsv"
LITERALS_TSV="$RUN_DIR/literals.tsv"
CONCERNS_TSV="$RUN_DIR/concerns.tsv"
NOTES_LOG="$RUN_DIR/notes.log"

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

log() {
  printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"
}

record_concern() {
  family_shape="$1"
  stat_state="$2"
  concern="$3"
  detail="$4"
  printf '%s\t%s\t%s\t%s\n' "$family_shape" "$stat_state" "$concern" "$detail" >> "$CONCERNS_TSV"
  printf 'CONCERN stat_state=%s family_shape=%s concern=%s detail=%s\n' "$stat_state" "$family_shape" "$concern" "$detail" >> "$NOTES_LOG"
}

cleanup_orphan_sqlite() {
  [ -n "${EMBY_COPY:-}" ] || return 0
  pgrep -af -- "$EMBY_COPY" 2>/dev/null | while IFS= read -r line; do
    pid="${line%% *}"
    cmd="${line#* }"
    case "$pid" in
      ''|*[!0-9]*) continue ;;
    esac
    if [ "$pid" != "$$" ] && is_sqlite_copy_argv "$cmd" "$EMBY_COPY" "$SQLITE_BIN"; then
      printf 'KILL_ORPHAN exact_db=%s pid=%s cmd=%s\n' "$EMBY_COPY" "$pid" "$cmd" >&2
      kill "$pid" 2>/dev/null || true
    fi
  done
}

is_sqlite_copy_argv() {
  args="$1"
  copy_path="$2"
  shift 2
  case " $args " in
    *" $copy_path "*) ;;
    *) return 1 ;;
  esac
  while [ "$#" -gt 0 ]; do
    sqlite_bin="$1"
    shift
    case "$args" in
      "$sqlite_bin"|"$sqlite_bin "*) return 0 ;;
    esac
  done
  return 1
}

on_exit() {
  rc=$?
  if [ "$rc" -ne 0 ]; then
    cleanup_orphan_sqlite
    printf 'HARNESS_STATUS failed rc=%s run_dir=%s results=%s\n' "$rc" "$RUN_DIR" "$RESULTS_TSV" >&2
  fi
  cleanup_copy_files
  exit "$rc"
}
trap on_exit EXIT

cleanup_copy_files() {
  [ -n "${EMBY_COPY:-}" ] || return 0
  case "$EMBY_COPY" in
    "$SCRATCH_ROOT"/*) ;;
    *) return 0 ;;
  esac
  rm -f "$EMBY_COPY" "$EMBY_COPY-wal" "$EMBY_COPY-shm" "$EMBY_COPY-journal"
}

require_host() {
  [ -x "$SQLITE_BIN" ] || die "GEN_BIN is not executable: $SQLITE_BIN"
  [ -f "$EMBY_ORIG" ] || die "missing Emby original DB: $EMBY_ORIG"
  mkdir -p "$SCRATCH_ROOT"
  [ -d "$SCRATCH_ROOT" ] || die "missing scratch root after mkdir: $SCRATCH_ROOT"
  case "$EMBY_COPY" in
    "$SCRATCH_ROOT"/*) ;;
    *) die "copy path escapes scratch root: $EMBY_COPY" ;;
  esac
  case "$RUN_DIR" in
    "$SCRATCH_ROOT"/*) ;;
    *) die "run dir escapes scratch root: $RUN_DIR" ;;
  esac
  case "$ITERATIONS" in
    ''|*[!0-9]*) die "ITERATIONS must be an integer >=3, got $ITERATIONS" ;;
  esac
  if [ "$ITERATIONS" -lt 3 ]; then
    die "ITERATIONS must be >=3, got $ITERATIONS"
  fi
  case "$QUERY_TIMEOUT_S" in
    ''|*[!0-9]*) die "QUERY_TIMEOUT_S must be a positive integer, got $QUERY_TIMEOUT_S" ;;
  esac
  if [ "$QUERY_TIMEOUT_S" -lt 1 ]; then
    die "QUERY_TIMEOUT_S must be a positive integer, got $QUERY_TIMEOUT_S"
  fi
  case "$DISK_MARGIN_BYTES" in
    ''|*[!0-9]*) die "DISK_MARGIN_BYTES must be a non-negative integer, got $DISK_MARGIN_BYTES" ;;
  esac
  command -v timeout >/dev/null 2>&1 || die "timeout command is required"
}

init_run_dir() {
  mkdir -p "$QUERY_DIR" "$OUT_DIR"
  : > "$EQP_LOG"
  : > "$IDENTITY_LOG"
  : > "$MEDIANS_LOG"
  : > "$NOTES_LOG"
  printf 'stat_state\tfamily_shape\tarm\tadoption\titer\tms\n' > "$TIMINGS_TSV"
  printf 'family_shape\tstat_state\tarm\tadoption\tmedian_ms\tidentity\n' > "$RESULTS_TSV"
  printf 'family_shape\tsource\tdetail\n' > "$LITERALS_TSV"
  printf 'family_shape\tstat_state\tconcern\tdetail\n' > "$CONCERNS_TSV"
}

refresh_copy() {
  backup_sql="$RUN_DIR/refresh-copy.sql"
  backup_out="$RUN_DIR/refresh-copy.out"
  log "copy_refresh start source=$EMBY_ORIG dest=$EMBY_COPY"
  rm -f "$EMBY_COPY" "$EMBY_COPY-wal" "$EMBY_COPY-shm" "$EMBY_COPY-journal"
  cat > "$backup_sql" <<SQL
.bail on
.open --readonly '${EMBY_ORIG}'
.backup '${EMBY_COPY}'
SQL
  run_sql_no_db "$backup_sql" "$backup_out" "copy_refresh"
  [ -f "$EMBY_COPY" ] || die "copy did not materialize: $EMBY_COPY"
  log "copy_refresh done dest=$EMBY_COPY"
}

file_size_bytes() {
  path="$1"
  if size="$(stat -c %s "$path" 2>/dev/null)"; then
    printf '%s\n' "$size"
  elif size="$(stat -f %z "$path" 2>/dev/null)"; then
    printf '%s\n' "$size"
  else
    die "could not stat file size: $path"
  fi
}

available_bytes() {
  path="$1"
  df -Pk "$path" | awk 'NR == 2 { printf "%.0f\n", $4 * 1024 }'
}

disk_preflight() {
  source_bytes="$(file_size_bytes "$EMBY_ORIG")"
  free_bytes="$(available_bytes "$SCRATCH_ROOT")"
  case "$free_bytes" in
    ''|*[!0-9]*) die "could not determine free bytes for $SCRATCH_ROOT" ;;
  esac
  required_bytes=$((source_bytes * 2 + DISK_MARGIN_BYTES))
  if [ "$free_bytes" -lt "$required_bytes" ]; then
    die "insufficient free space under $SCRATCH_ROOT: free=$free_bytes required=$required_bytes source_bytes=$source_bytes margin=$DISK_MARGIN_BYTES"
  fi
  log "disk_preflight ok scratch=$SCRATCH_ROOT free=$free_bytes required=$required_bytes source_bytes=$source_bytes margin=$DISK_MARGIN_BYTES"
}

run_sql_file() {
  db="$1"
  sql_file="$2"
  out_file="$3"
  label="$4"
  "$SQLITE_BIN" -batch "$db" < "$sql_file" > "$out_file" 2>&1 ||
    die "$label failed; see $out_file"
}

run_timed_sql_file() {
  db="$1"
  sql_file="$2"
  out_file="$3"
  label="$4"
  set +e
  timeout "${QUERY_TIMEOUT_S}s" "$SQLITE_BIN" -batch "$db" < "$sql_file" > "$out_file" 2>&1
  rc=$?
  set -e
  case "$rc" in
    0) return 0 ;;
    124) return 124 ;;
    *) die "$label failed rc=$rc; see $out_file" ;;
  esac
}

run_sql_no_db() {
  sql_file="$1"
  out_file="$2"
  label="$3"
  "$SQLITE_BIN" -batch < "$sql_file" > "$out_file" 2>&1 ||
    die "$label failed; see $out_file"
}

write_state_probe() {
  file="$1"
  cat > "$file" <<SQL
.mode tabs
.headers off
SELECT 'sqlite_version', sqlite_version();
SELECT 'db_path', file FROM pragma_database_list WHERE name='main';
PRAGMA page_size;
PRAGMA cache_size;
PRAGMA mmap_size;
PRAGMA journal_mode;
SELECT 'stat1_rows', count(*) FROM sqlite_stat1;
SELECT 'stat4_rows', count(*) FROM sqlite_stat4;
SELECT 'latest_index_present', count(*) FROM sqlite_master WHERE type='index' AND name='${LATEST_INDEX}';
SELECT 'mediaitems_rows', count(*) FROM MediaItems;
SELECT 'mediaitems_type8_rows', count(*) FROM MediaItems WHERE Type=8;
SQL
}

state_probe() {
  stat_state="$1"
  sql="$RUN_DIR/state-$stat_state.sql"
  out="$RUN_DIR/state-$stat_state.tsv"
  write_state_probe "$sql"
  run_sql_file "$EMBY_COPY" "$sql" "$out" "state probe $stat_state"
  sed "s/^/STATE stat_state=$stat_state /" "$out" >> "$RUN_DIR/state-tagged.log"
  grep -F $'latest_index_present\t1' "$out" >/dev/null ||
    record_concern "state" "$stat_state" "missing_index" "name=$LATEST_INDEX out=$out"
}

analyze_copy() {
  sql="$RUN_DIR/analyze.sql"
  out="$RUN_DIR/analyze.out"
  cat > "$sql" <<'SQL'
PRAGMA analysis_limit=0;
ANALYZE main;
SQL
  log "analyze start db=$EMBY_COPY"
  run_sql_file "$EMBY_COPY" "$sql" "$out" "ANALYZE main"
  log "analyze done db=$EMBY_COPY"
}

prewarm() {
  db="$1"
  cat "$db" "$db-wal" "$db-shm" 2>/dev/null >/dev/null || true
}

write_query_file() {
  file="$1"
  sql="$2"
  printf '%s\n' "$sql" > "$file"
}

render_emby_template() {
  sql="$1"
  uid="$2"
  ancestors="$3"
  printf '%s
' "$sql" | awk -v uid="$uid" -v ancestors="$ancestors" '{ gsub(/__EMBY_USER_ID__/, uid); gsub(/__ANCESTORS__/, ancestors); print }'
}

replace_once() {
  haystack="$1"
  needle="$2"
  replacement="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) die "replace_once could not find needle: $needle" ;;
  esac
  before="${haystack%%"$needle"*}"
  after="${haystack#*"$needle"}"
  printf '%s%s%s' "$before" "$replacement" "$after"
}

assert_occurs_once() {
  haystack="$1"
  needle="$2"
  label="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) die "$label missing expected splice bytes" ;;
  esac
  rest="${haystack#*"$needle"}"
  case "$rest" in
    *"$needle"*) die "$label has duplicate splice bytes" ;;
  esac
}

ensure_sql_semicolon() {
  sql="$1"
  sql="${sql%$'\r'}"
  case "$sql" in
    *';') printf '%s' "$sql" ;;
    *) printf '%s;' "$sql" ;;
  esac
}

static_capture_path() {
  family_shape="$1"
  case "$family_shape" in
    c-empty) printf '%s/%s' "$STATIC_CAPTURE_DIR" "$CANARY_EMBY_C_EMPTY_CAPTURE_FILE" ;;
    c-two-group) printf '%s/%s' "$STATIC_CAPTURE_DIR" "$CANARY_EMBY_C_TWO_GROUP_CAPTURE_FILE" ;;
    c-large-group) printf '%s/%s' "$STATIC_CAPTURE_DIR" "$CANARY_EMBY_C_LARGE_GROUP_CAPTURE_FILE" ;;
    *) return 1 ;;
  esac
}

read_static_captured_sql() {
  family_shape="$1"
  hash="$2"
  uid="$3"
  l1="$4"
  if ! capture_path="$(static_capture_path "$family_shape")"; then
    die "missing static captured SQL mapping for $family_shape"
  fi
  [ -f "$capture_path" ] ||
    die "missing static captured SQL hash=$hash for $family_shape path=$capture_path"
  sql="$(cat "$capture_path")" ||
    die "could not read static captured SQL hash=$hash for $family_shape path=$capture_path"
  [ -n "$sql" ] || die "empty static captured SQL hash=$hash for $family_shape path=$capture_path"
  sql="$(render_emby_template "$sql" "$uid" "$l1")"
  printf '%s\tcaptured_sql\thash=%s source=static:%s\n' "$family_shape" "$hash" "$capture_path" >> "$LITERALS_TSV"
  ensure_sql_semicolon "$sql"
}

ancestor_exists_clause() {
  l1="$1"
  printf 'EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (%s))' "$l1"
}

resd_baseline_from_captured_exists() {
  family_shape="$1"
  hash="$2"
  uid="$3"
  l1="$4"
  captured="$(read_static_captured_sql "$family_shape" "$hash" "$uid" "$l1")"
  exists_clause="$(ancestor_exists_clause "$l1")"
  assert_occurs_once "$captured" "$exists_clause" "$family_shape captured EXISTS"
  baseline="$(replace_once "$captured" "$exists_clause" "A.Id in WithAncestors")"
  assert_occurs_once "$baseline" "A.Id in WithAncestors" "$family_shape inverse baseline"
  roundtrip="$(candidate_ancestor_splice "$baseline" "$l1")"
  [ "$roundtrip" = "$captured" ] ||
    die "$family_shape inverse splice is not byte-identical outside the intended membership arm"
  printf '%s\tinverse_splice\tverified_byte_identity_outside_membership hash=%s\n' "$family_shape" "$hash" >> "$LITERALS_TSV"
  printf '%s' "$baseline"
}

resd_baseline_from_captured_in() {
  family_shape="$1"
  hash="$2"
  uid="$3"
  l1="$4"
  captured="$(read_static_captured_sql "$family_shape" "$hash" "$uid" "$l1")"
  assert_occurs_once "$captured" "A.Id in WithAncestors" "$family_shape captured IN"
  assert_occurs_once "$captured" "AncestorId in ($l1)" "$family_shape captured L1"
  exists_clause="$(ancestor_exists_clause "$l1")"
  spliced="$(candidate_ancestor_splice "$captured" "$l1")"
  roundtrip="$(replace_once "$spliced" "$exists_clause" "A.Id in WithAncestors")"
  [ "$roundtrip" = "$captured" ] ||
    die "$family_shape captured IN splice is not byte-identical outside the intended membership arm"
  printf '%s\tancestor_splice\tverified_byte_identity_outside_membership hash=%s\n' "$family_shape" "$hash" >> "$LITERALS_TSV"
  printf '%s' "$captured"
}

resd_conjunct() {
  uid="$1"
  printf 'AND ((A.Type=5 AND A.UserDataKeyId IN (SELECT UserDataKeyId FROM UserDatas WHERE UserId=%s AND playbackPositionTicks>0)) OR (A.Type=8 AND A.SeriesPresentationUniqueKey IN (SELECT N2.SeriesPresentationUniqueKey FROM MediaItems N2 JOIN UserDatas UN2 ON N2.UserDataKeyId=UN2.UserDataKeyId AND UN2.UserId=%s WHERE N2.Type=8 AND Coalesce(N2.SortParentIndexNumber,N2.ParentIndexNumber,-1) <> 0 AND (UN2.Played=1 OR UN2.playbackPositionTicks>0))))' "$uid" "$uid"
}

candidate_ancestor_splice() {
  baseline="$1"
  l1="$2"
  exists_clause="$(ancestor_exists_clause "$l1")"
  replace_once "$baseline" "A.Id in WithAncestors" "$exists_clause"
}

candidate_resd() {
  baseline="$1"
  l1="$2"
  uid="$3"
  spliced="$(candidate_ancestor_splice "$baseline" "$l1")"
  conjunct="$(resd_conjunct "$uid")"
  replace_once "$spliced" " Group by coalesce(" "$conjunct Group by coalesce("
}

epi_expr_a() {
  printf '((Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, 1) * 1000000) + Coalesce(A.SortIndexNumber, A.IndexNumber, 0) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(A.ParentIndexNumber,1)=0 Then (Cast(Coalesce(A.IndexNumber, 0) as REAL) / 100000) Else 0 End))'
}

epi_expr_n() {
  printf '((Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber, 1) * 1000000) + Coalesce(N.SortIndexNumber, N.IndexNumber, 0) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then 0 Else 0.5 End) + (Select Case When Coalesce(N.ParentIndexNumber,1)=0 Then (Cast(Coalesce(N.IndexNumber, 0) as REAL) / 100000) Else 0 End))'
}

resd_from_join() {
  uid="$1"
  epi_n="$(epi_expr_n)"
  printf 'from mediaitems A left join (Select N.SeriesPresentationUniqueKey,%s AbsoluteIndexNumber,max(UserDatas_N.LastPlayedDateInt) LastPlayedDateInt,UserDatas_N.playbackPositionTicks from MediaItems N join UserDatas UserDatas_N on N.UserDataKeyId=UserDatas_N.UserDataKeyId And UserDatas_N.UserId=%s where N.Type=8 and Coalesce(N.SortParentIndexNumber,N.ParentIndexNumber,-1) <> 0 and (UserDatas_N.Played=1 or UserDatas_N.playbackPositionTicks > 0) Group By N.SeriesPresentationUniqueKey ORDER BY UserDatas_N.LastPlayedDateInt desc, AbsoluteIndexNumber desc) LastWatchedEpisodes on LastWatchedEpisodes.SeriesPresentationUniqueKey=A.SeriesPresentationUniqueKey left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s' "$epi_n" "$uid" "$uid"
}

resd_where_mixed() {
  uid="$1"
  printf "((A.Type=5 and UserDatas.playbackPositionTicks > 0) OR (A.Type=8 AND (UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null)) AND (A.Type=5 OR Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0) AND A.Type in (5,8) AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=%s and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0" "$uid"
}

resd_where_0065() {
  uid="$1"
  printf "((UserDatas.playbackPositionTicks > 0 or Coalesce(UserDatas.played,0) = 0) AND (select case when LastWatchedEpisodes.playbackPositionTicks > 0 then EpisodeAbsoluteIndexNumber >= Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) else EpisodeAbsoluteIndexNumber > Coalesce(LastWatchedEpisodes.AbsoluteIndexNumber,EpisodeAbsoluteIndexNumber) end) AND LastWatchedEpisodes.LastPlayedDateInt not null) AND Coalesce(A.SortParentIndexNumber,A.ParentIndexNumber, -1) <> 0 AND A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=%s and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0" "$uid"
}

make_resd_identity_query() {
  l1="$1"
  uid="$2"
  where_kind="$3"
  membership="$4"
  add_conjunct="$5"

  epi="$(epi_expr_a)"
  from_join="$(resd_from_join "$uid")"
  if [ "$where_kind" = "0065" ]; then
    where_clause="$(resd_where_0065 "$uid")"
  else
    where_clause="$(resd_where_mixed "$uid")"
  fi
  if [ "$add_conjunct" = "yes" ]; then
    conjunct=" $(resd_conjunct "$uid")"
  else
    conjunct=""
  fi
  cat <<SQL
WITH WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in ($l1)),
identity_rows AS (
  SELECT coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) AS gk,
         COALESCE(lastwatchedepisodes.lastplayeddateint, userdatas.lastplayeddateint, 0) AS orderkey,
         $epi AS EpisodeAbsoluteIndexNumber
  $from_join
  WHERE $where_clause AND $membership$conjunct
)
SELECT quote(gk) AS gk, max(orderkey) AS max_orderkey, min(EpisodeAbsoluteIndexNumber) AS min_absindex, count(*) AS n
FROM identity_rows
GROUP BY gk
ORDER BY quote(gk);
SQL
}

make_shape_a01_baseline() {
  cat <<'SQL'
with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (__ANCESTORS__) )select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=__EMBY_USER_ID__ where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=__EMBY_USER_ID__ and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 12;
SQL
}

make_shape_a04_baseline() {
  cat <<'SQL'
with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (__ANCESTORS__) )select A.type,A.Id,A.EndDate,A.CommunityRating,A.IndexNumber,A.Name,A.Path,A.PremiereDate,A.ParentIndexNumber,A.ProductionYear,A.OfficialRating,A.RunTimeTicks,A.guid,A.ParentId,A.CriticRating,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex from mediaitems A join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=__EMBY_USER_ID__ where A.Type in (5,8) AND UserDatas.playbackPositionTicks > 0 AND Coalesce(UserDatas.HideFromResume,0)=0 AND COALESCE((select hidefromresume from userdatas where userdatas.userid=__EMBY_USER_ID__ and userdatas.userdatakeyid=(select userdatakeyid from mediaitems where mediaitems.id=A.seriesid)),0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY UserDatas.LastPlayedDateInt DESC LIMIT 24;
SQL
}

make_shape_d_baseline() {
  cat <<'SQL'
WITH keys(gk) AS MATERIALIZED (SELECT DISTINCT coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey) FROM MediaItems WHERE Type = 8), picked AS MATERIALIZED (SELECT K.gk, (SELECT A2.Id FROM MediaItems AS A2 WHERE A2.Type = 8 AND coalesce(A2.SeriesPresentationUniqueKey, A2.PresentationUniqueKey) IS K.gk AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A2.Id AND X.AncestorId IN (__ANCESTORS__)) AND NOT EXISTS (SELECT 1 FROM UserDatas AS U2 WHERE U2.UserDataKeyId = A2.UserDataKeyId AND U2.UserId = __EMBY_USER_ID__ AND U2.played <> 0) ORDER BY A2.DateCreated DESC LIMIT 1) AS id FROM keys AS K), exact_groups AS MATERIALIZED (SELECT P.gk, P.id, Amax.DateCreated AS maxdc FROM picked AS P JOIN MediaItems AS Amax ON Amax.Id = P.id WHERE P.id IS NOT NULL), ranked AS MATERIALIZED (SELECT gk, id, maxdc FROM exact_groups ORDER BY maxdc DESC LIMIT 12) SELECT A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex  FROM ranked AS R JOIN MediaItems AS A ON A.Id = R.id LEFT JOIN UserDatas ON A.UserDataKeyId = UserDatas.UserDataKeyId AND UserDatas.UserId = __EMBY_USER_ID__ ORDER BY R.maxdc DESC LIMIT 12;
SQL
}

write_capture_wrapper() {
  query_file="$1"
  wrapper_file="$2"
  cat > "$wrapper_file" <<SQL
.mode tabs
.headers off
.nullvalue <NULL>
$(cat "$query_file")
SQL
}

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

line_count() {
  wc -l < "$1" | awk '{print $1}'
}

sum_group_counts() {
  awk -F '\t' '{ sum += $2 + 0 } END { print sum + 0 }' "$1"
}

make_ep_latest_identity_query() {
  query_file="$1"
  presentation_suffix=" ORDER BY R.maxdc DESC LIMIT 12;"
  sql="$(cat "$query_file")" ||
    die "could not read EP-LATEST-A query file: $query_file"
  assert_occurs_once "$sql" "$presentation_suffix" "EP-LATEST-A presentation suffix"
  core_sql="$(replace_once "$sql" "$presentation_suffix" "")"
  cat <<SQL
SELECT Id, count(*) AS n
FROM (
  $core_sql
)
GROUP BY Id
ORDER BY Id;
SQL
}

record_count_expectation() {
  family_shape="$1"
  stat_state="$2"
  grain="$3"
  arm="$4"
  actual_count="$5"
  expected_count="$6"
  [ -n "$expected_count" ] || return 0
  result="PASS"
  case "$expected_count" in
    nonempty)
      if [ "$actual_count" -le 0 ]; then
        result="FAIL"
      fi
      ;;
    *[!0-9]*)
      die "invalid expected count for $family_shape: $expected_count"
      ;;
    *)
      if [ "$actual_count" != "$expected_count" ]; then
        result="FAIL"
      fi
      ;;
  esac
  printf 'LITERAL_PRESENCE stat_state=%s family_shape=%s grain=%s arm=%s actual=%s expected=%s result=%s\n' "$stat_state" "$family_shape" "$grain" "$arm" "$actual_count" "$expected_count" "$result" >> "$IDENTITY_LOG"
  if [ "$result" != "PASS" ]; then
    record_concern "$family_shape" "$stat_state" "literal_count_expectation" "grain=$grain arm=$arm actual=$actual_count expected=$expected_count"
  fi
  return 0
}

identity_full_ordered() {
  family_shape="$1"
  stat_state="$2"
  baseline_query="$3"
  candidate_query="$4"
  expected_rows="$5"

  bwrap="$OUT_DIR/$stat_state-$family_shape-baseline-identity.sql"
  cwrap="$OUT_DIR/$stat_state-$family_shape-candidate-identity.sql"
  bout="$OUT_DIR/$stat_state-$family_shape-baseline-identity.tsv"
  cout="$OUT_DIR/$stat_state-$family_shape-candidate-identity.tsv"
  write_capture_wrapper "$baseline_query" "$bwrap"
  write_capture_wrapper "$candidate_query" "$cwrap"
  run_sql_file "$EMBY_COPY" "$bwrap" "$bout" "identity baseline $family_shape $stat_state"
  run_sql_file "$EMBY_COPY" "$cwrap" "$cout" "identity candidate $family_shape $stat_state"
  bhash="$(sha_file "$bout")"
  chash="$(sha_file "$cout")"
  brows="$(line_count "$bout")"
  crows="$(line_count "$cout")"
  pass="PASS"
  if [ "$bhash" != "$chash" ]; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "full_ordered_rows" "baseline" "$brows" "$expected_rows"; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "full_ordered_rows" "candidate" "$crows" "$expected_rows"; then
    pass="FAIL"
  fi
  printf 'IDENTITY stat_state=%s family_shape=%s grain=full_ordered baseline_rows=%s candidate_rows=%s baseline_sha256=%s candidate_sha256=%s result=%s\n' "$stat_state" "$family_shape" "$brows" "$crows" "$bhash" "$chash" "$pass" >> "$IDENTITY_LOG"
  [ "$pass" = "PASS" ] || die "identity failed for $family_shape $stat_state; see $bout and $cout"
}

identity_ep_latest_grouped() {
  family_shape="$1"
  stat_state="$2"
  baseline_query="$3"
  candidate_query="$4"
  expected_distinct_ids="$5"

  case "$expected_distinct_ids" in
    nonempty)
      ;;
    ''|*[!0-9]*)
      die "EP-LATEST-A expected distinct-id count must be a positive integer, got $expected_distinct_ids"
      ;;
  esac
  if [ "$expected_distinct_ids" != "nonempty" ] && [ "$expected_distinct_ids" -le 0 ]; then
    die "EP-LATEST-A expected distinct-id count must be >0, got $expected_distinct_ids"
  fi

  raw_bwrap="$OUT_DIR/$stat_state-$family_shape-baseline-raw-identity.sql"
  raw_cwrap="$OUT_DIR/$stat_state-$family_shape-candidate-raw-identity.sql"
  raw_bout="$OUT_DIR/$stat_state-$family_shape-baseline-raw-identity.tsv"
  raw_cout="$OUT_DIR/$stat_state-$family_shape-candidate-raw-identity.tsv"
  bsql="$OUT_DIR/$stat_state-$family_shape-baseline-grouped-identity.sql"
  csql="$OUT_DIR/$stat_state-$family_shape-candidate-grouped-identity.sql"
  bout="$OUT_DIR/$stat_state-$family_shape-baseline-grouped-identity.tsv"
  cout="$OUT_DIR/$stat_state-$family_shape-candidate-grouped-identity.tsv"

  write_capture_wrapper "$baseline_query" "$raw_bwrap"
  write_capture_wrapper "$candidate_query" "$raw_cwrap"
  run_sql_file "$EMBY_COPY" "$raw_bwrap" "$raw_bout" "raw identity baseline $family_shape $stat_state"
  run_sql_file "$EMBY_COPY" "$raw_cwrap" "$raw_cout" "raw identity candidate $family_shape $stat_state"

  {
    printf '.mode tabs\n.headers off\n.nullvalue <NULL>\n'
    make_ep_latest_identity_query "$baseline_query"
  } > "$bsql"
  {
    printf '.mode tabs\n.headers off\n.nullvalue <NULL>\n'
    make_ep_latest_identity_query "$candidate_query"
  } > "$csql"
  run_sql_file "$EMBY_COPY" "$bsql" "$bout" "grouped identity baseline $family_shape $stat_state"
  run_sql_file "$EMBY_COPY" "$csql" "$cout" "grouped identity candidate $family_shape $stat_state"

  raw_bhash="$(sha_file "$raw_bout")"
  raw_chash="$(sha_file "$raw_cout")"
  bhash="$(sha_file "$bout")"
  chash="$(sha_file "$cout")"
  bdistinct="$(line_count "$bout")"
  cdistinct="$(line_count "$cout")"
  brows="$(sum_group_counts "$bout")"
  crows="$(sum_group_counts "$cout")"
  pass="PASS"
  if [ "$bhash" != "$chash" ]; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "ep_latest_distinct_ids" "baseline" "$bdistinct" "$expected_distinct_ids"; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "ep_latest_distinct_ids" "candidate" "$cdistinct" "$expected_distinct_ids"; then
    pass="FAIL"
  fi
  if [ "$pass" = "PASS" ] && [ "$raw_bhash" != "$raw_chash" ]; then
    printf 'IDENTITY_NOTE stat_state=%s family_shape=%s note=raw_order_diff_only baseline_raw_sha256=%s candidate_raw_sha256=%s\n' "$stat_state" "$family_shape" "$raw_bhash" "$raw_chash" >> "$IDENTITY_LOG"
  fi
  printf 'IDENTITY stat_state=%s family_shape=%s grain=id_count_digest baseline_distinct_ids=%s candidate_distinct_ids=%s baseline_rows=%s candidate_rows=%s baseline_sha256=%s candidate_sha256=%s result=%s\n' "$stat_state" "$family_shape" "$bdistinct" "$cdistinct" "$brows" "$crows" "$bhash" "$chash" "$pass" >> "$IDENTITY_LOG"
  [ "$pass" = "PASS" ] || die "EP-LATEST-A identity failed for $family_shape $stat_state; see $bout and $cout"
}

identity_resd_grouped() {
  family_shape="$1"
  stat_state="$2"
  l1="$3"
  uid="$4"
  where_kind="$5"
  expected_groups="$6"

  bsql="$OUT_DIR/$stat_state-$family_shape-baseline-identity.sql"
  csql="$OUT_DIR/$stat_state-$family_shape-candidate-identity.sql"
  bout="$OUT_DIR/$stat_state-$family_shape-baseline-identity.tsv"
  cout="$OUT_DIR/$stat_state-$family_shape-candidate-identity.tsv"
  membership_baseline="A.Id in WithAncestors"
  membership_candidate="$(ancestor_exists_clause "$l1")"
  {
    printf '.mode tabs\n.headers off\n.nullvalue <NULL>\n'
    make_resd_identity_query "$l1" "$uid" "$where_kind" "$membership_baseline" "no"
  } > "$bsql"
  {
    printf '.mode tabs\n.headers off\n.nullvalue <NULL>\n'
    make_resd_identity_query "$l1" "$uid" "$where_kind" "$membership_candidate" "yes"
  } > "$csql"
  run_sql_file "$EMBY_COPY" "$bsql" "$bout" "identity baseline $family_shape $stat_state"
  run_sql_file "$EMBY_COPY" "$csql" "$cout" "identity candidate $family_shape $stat_state"
  bhash="$(sha_file "$bout")"
  chash="$(sha_file "$cout")"
  brows="$(line_count "$bout")"
  crows="$(line_count "$cout")"
  pass="PASS"
  if [ "$bhash" != "$chash" ]; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "resd_groups" "baseline" "$brows" "$expected_groups"; then
    pass="FAIL"
  fi
  if ! record_count_expectation "$family_shape" "$stat_state" "resd_groups" "candidate" "$crows" "$expected_groups"; then
    pass="FAIL"
  fi
  printf 'IDENTITY stat_state=%s family_shape=%s grain=resd_gk_maxorder_minabsindex_n baseline_groups=%s candidate_groups=%s baseline_sha256=%s candidate_sha256=%s result=%s\n' "$stat_state" "$family_shape" "$brows" "$crows" "$bhash" "$chash" "$pass" >> "$IDENTITY_LOG"
  [ "$pass" = "PASS" ] || die "RES-D identity failed for $family_shape $stat_state; see $bout and $cout"
}

write_eqp_sql() {
  query_file="$1"
  eqp_sql="$2"
  {
    printf 'EXPLAIN QUERY PLAN\n'
    cat "$query_file"
  } > "$eqp_sql"
}

detect_adoption() {
  family_shape="$1"
  arm="$2"
  eqp_raw="$3"
  if [ "$arm" = "baseline" ]; then
    printf 'baseline'
    return 0
  fi
  case "$family_shape" in
    a-*)
      if grep -E 'CORRELATED|SEARCH AncestorIds2' "$eqp_raw" >/dev/null; then
        printf 'adopted'
      else
        printf 'not-adopted'
      fi
      ;;
    c-*)
      if grep -F 'MULTI-INDEX OR' "$eqp_raw" >/dev/null; then
        printf 'adopted'
      else
        printf 'not-adopted'
      fi
      ;;
    d-*)
      if grep -F "$LATEST_INDEX" "$eqp_raw" >/dev/null; then
        printf 'adopted'
      else
        printf 'not-adopted'
      fi
      ;;
    *)
      printf 'unknown'
      ;;
  esac
}

run_eqp() {
  family_shape="$1"
  stat_state="$2"
  arm="$3"
  query_file="$4"

  eqp_sql="$OUT_DIR/$stat_state-$family_shape-$arm-eqp.sql"
  eqp_raw="$OUT_DIR/$stat_state-$family_shape-$arm-eqp.raw"
  write_eqp_sql "$query_file" "$eqp_sql"
  run_sql_file "$EMBY_COPY" "$eqp_sql" "$eqp_raw" "EQP $family_shape $stat_state $arm"
  adoption="$(detect_adoption "$family_shape" "$arm" "$eqp_raw")"
  sed "s/^/EQP stat_state=$stat_state family_shape=$family_shape arm=$arm adoption=$adoption iter=0 /" "$eqp_raw" >> "$EQP_LOG"
  printf 'NOTE stat_state=%s family_shape=%s arm=%s gate=adoption adoption=%s detail=eqp_raw=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$eqp_raw" >> "$NOTES_LOG"
  if [ "$arm" = "candidate" ] && [ "$adoption" != "adopted" ]; then
    record_concern "$family_shape" "$stat_state" "candidate_not_adopted" "arm=$arm adoption=$adoption"
  fi
  printf '%s' "$adoption"
}

write_timed_sql() {
  query_file="$1"
  timed_sql="$2"
  stat_state="$3"
  family_shape="$4"
  arm="$5"
  adoption="$6"
  phase="$7"
  iter="$8"
  {
    sqlite_timing_preamble
    printf '.print TAG stat_state=%s family_shape=%s arm=%s adoption=%s phase=%s iter=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$phase" "$iter"
    if [ "$phase" = "timed" ]; then
      printf '.timer on\n'
    fi
    cat "$query_file"
    if [ "$phase" = "timed" ]; then
      printf '\n.timer off\n'
    fi
  } > "$timed_sql"
}

sqlite_timing_preamble() {
  cat <<'SQL'
.mode tabs
.headers off
.nullvalue <NULL>
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-1048576;
PRAGMA mmap_size=34359738368;
PRAGMA threads=8;
SQL
}

extract_timer_ms() {
  out_file="$1"
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
      for (i = 1; i < NF; i++) {
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
      printf "%.3f\n", real * 1000.0
    }
  ' "$out_file"
}

run_timed_arm() {
  family_shape="$1"
  stat_state="$2"
  arm="$3"
  adoption="$4"
  query_file="$5"
  iter="$6"
  sql="$OUT_DIR/$stat_state-$family_shape-$arm-$iter.sql"
  out="$OUT_DIR/$stat_state-$family_shape-$arm-$iter.out"
  write_timed_sql "$query_file" "$sql" "$stat_state" "$family_shape" "$arm" "$adoption" "timed" "$iter"
  if run_timed_sql_file "$EMBY_COPY" "$sql" "$out" "timing $family_shape $stat_state $arm iter=$iter"; then
    :
  else
    rc=$?
    if [ "$rc" -eq 124 ]; then
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$iter" "TIMEOUT" >> "$TIMINGS_TSV"
      printf 'TIMEOUT stat_state=%s family_shape=%s arm=%s adoption=%s iter=%s timeout_s=%s out=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$iter" "$QUERY_TIMEOUT_S" "$out" >> "$NOTES_LOG"
      return 124
    fi
    die "timing query returned unexpected rc=$rc for $family_shape $stat_state $arm iter=$iter; see $out"
  fi
  ms="$(extract_timer_ms "$out")" || die "missing timer output for $family_shape $stat_state $arm iter=$iter; see $out"
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$iter" "$ms" >> "$TIMINGS_TSV"
  printf 'TIME stat_state=%s family_shape=%s arm=%s adoption=%s iter=%s ms=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$iter" "$ms" >> "$RUN_DIR/timing-tagged.log"
}

median_for() {
  stat_state="$1"
  family_shape="$2"
  arm="$3"
  awk -F '\t' -v s="$stat_state" -v f="$family_shape" -v a="$arm" '
    function is_number(s) {
      return s ~ /^[-+]?[0-9]+([.][0-9]+)?([eE][-+]?[0-9]+)?$|^[-+]?[.][0-9]+([eE][-+]?[0-9]+)?$/
    }
    NR > 1 && $1 == s && $2 == f && $3 == a && is_number($6) {
      vals[++n] = $6 + 0
    }
    END {
      if (n == 0) exit 1
      for (i = 1; i <= n; i++) {
        for (j = i + 1; j <= n; j++) {
          if (vals[j] < vals[i]) {
            t = vals[i]; vals[i] = vals[j]; vals[j] = t
          }
        }
      }
      if (n % 2) {
        printf "%.3f\n", vals[(n + 1) / 2]
      } else {
        printf "%.3f\n", (vals[n / 2] + vals[n / 2 + 1]) / 2.0
      }
    }
  ' "$TIMINGS_TSV"
}

timeout_verdict_for_pair() {
  baseline_timed_out="$1"
  candidate_timed_out="$2"
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

run_warm_and_timing() {
  family_shape="$1"
  stat_state="$2"
  baseline_query="$3"
  candidate_query="$4"
  identity_result="$5"
  baseline_adoption="$6"
  candidate_adoption="$7"
  baseline_timed_out=0
  candidate_timed_out=0

  prewarm "$EMBY_COPY"
  for arm in baseline candidate; do
    if [ "$arm" = "baseline" ]; then
      query_file="$baseline_query"
      adoption="$baseline_adoption"
    else
      query_file="$candidate_query"
      adoption="$candidate_adoption"
    fi
    sql="$OUT_DIR/$stat_state-$family_shape-$arm-warm.sql"
    out="$OUT_DIR/$stat_state-$family_shape-$arm-warm.out"
    write_timed_sql "$query_file" "$sql" "$stat_state" "$family_shape" "$arm" "$adoption" "warm" "0"
    if run_timed_sql_file "$EMBY_COPY" "$sql" "$out" "warm-up $family_shape $stat_state $arm"; then
      :
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then
        [ "$arm" = "baseline" ] && baseline_timed_out=1 || candidate_timed_out=1
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "warm" "TIMEOUT" >> "$TIMINGS_TSV"
        printf 'TIMEOUT_WARMUP stat_state=%s family_shape=%s arm=%s adoption=%s timeout_s=%s out=%s\n' "$stat_state" "$family_shape" "$arm" "$adoption" "$QUERY_TIMEOUT_S" "$out" >> "$NOTES_LOG"
        continue
      fi
      die "warm-up query returned unexpected rc=$rc for $family_shape $stat_state $arm; see $out"
    fi
  done

  iter=1
  while [ "$iter" -le "$ITERATIONS" ]; do
    if [ $((iter % 2)) -eq 1 ]; then
      order="baseline candidate"
    else
      order="candidate baseline"
    fi
    for arm in $order; do
      if [ "$arm" = "baseline" ] && [ "$baseline_timed_out" -eq 1 ]; then
        printf 'TIME_SKIP stat_state=%s family_shape=%s arm=%s iter=%s reason=prior_timeout\n' "$stat_state" "$family_shape" "$arm" "$iter" >> "$NOTES_LOG"
        continue
      fi
      if [ "$arm" = "candidate" ] && [ "$candidate_timed_out" -eq 1 ]; then
        printf 'TIME_SKIP stat_state=%s family_shape=%s arm=%s iter=%s reason=prior_timeout\n' "$stat_state" "$family_shape" "$arm" "$iter" >> "$NOTES_LOG"
        continue
      fi
      if [ "$arm" = "baseline" ]; then
        if run_timed_arm "$family_shape" "$stat_state" "$arm" "$baseline_adoption" "$baseline_query" "$iter"; then
          :
        else
          rc=$?
          [ "$rc" -eq 124 ] || die "timing $family_shape $stat_state $arm returned unexpected rc=$rc"
          baseline_timed_out=1
        fi
      else
        if run_timed_arm "$family_shape" "$stat_state" "$arm" "$candidate_adoption" "$candidate_query" "$iter"; then
          :
        else
          rc=$?
          [ "$rc" -eq 124 ] || die "timing $family_shape $stat_state $arm returned unexpected rc=$rc"
          candidate_timed_out=1
        fi
      fi
    done
    if [ "$baseline_timed_out" -eq 1 ] && [ "$candidate_timed_out" -eq 1 ]; then
      break
    fi
    iter=$((iter + 1))
  done

  if [ "$baseline_timed_out" -eq 1 ]; then
    bmedian=TIMEOUT
  else
    bmedian="$(median_for "$stat_state" "$family_shape" "baseline")" ||
      die "could not compute baseline median for $family_shape $stat_state"
  fi
  if [ "$candidate_timed_out" -eq 1 ]; then
    cmedian=TIMEOUT
  else
    cmedian="$(median_for "$stat_state" "$family_shape" "candidate")" ||
      die "could not compute candidate median for $family_shape $stat_state"
  fi
  timeout_verdict="$(timeout_verdict_for_pair "$baseline_timed_out" "$candidate_timed_out")"
  if [ "$timeout_verdict" != NONE ]; then
    printf 'TIMEOUT_VERDICT stat_state=%s family_shape=%s verdict=%s timeout_s=%s\n' "$stat_state" "$family_shape" "$timeout_verdict" "$QUERY_TIMEOUT_S" >> "$NOTES_LOG"
    record_concern "$family_shape" "$stat_state" "timeout_verdict" "verdict=$timeout_verdict timeout_s=$QUERY_TIMEOUT_S"
  fi
  printf 'MEDIAN stat_state=%s family_shape=%s arm=baseline adoption=%s iter=median median_ms=%s identity=%s\n' "$stat_state" "$family_shape" "$baseline_adoption" "$bmedian" "$identity_result" >> "$MEDIANS_LOG"
  printf 'MEDIAN stat_state=%s family_shape=%s arm=candidate adoption=%s iter=median median_ms=%s identity=%s\n' "$stat_state" "$family_shape" "$candidate_adoption" "$cmedian" "$identity_result" >> "$MEDIANS_LOG"
  printf '%s\t%s\tbaseline\t%s\t%s\t%s\n' "$family_shape" "$stat_state" "$baseline_adoption" "$bmedian" "$identity_result" >> "$RESULTS_TSV"
  printf '%s\t%s\tcandidate\t%s\t%s\t%s\n' "$family_shape" "$stat_state" "$candidate_adoption" "$cmedian" "$identity_result" >> "$RESULTS_TSV"
}

run_shape_full() {
  family_shape="$1"
  stat_state="$2"
  baseline_query="$3"
  candidate_query="$4"
  expected_rows="$5"
  identity_full_ordered "$family_shape" "$stat_state" "$baseline_query" "$candidate_query" "$expected_rows"
  baseline_adoption="$(run_eqp "$family_shape" "$stat_state" "baseline" "$baseline_query")"
  candidate_adoption="$(run_eqp "$family_shape" "$stat_state" "candidate" "$candidate_query")"
  run_warm_and_timing "$family_shape" "$stat_state" "$baseline_query" "$candidate_query" "PASS" "$baseline_adoption" "$candidate_adoption"
}

run_shape_resd() {
  family_shape="$1"
  stat_state="$2"
  l1="$3"
  uid="$4"
  where_kind="$5"
  baseline_query="$6"
  candidate_query="$7"
  expected_groups="$8"
  identity_resd_grouped "$family_shape" "$stat_state" "$l1" "$uid" "$where_kind" "$expected_groups"
  baseline_adoption="$(run_eqp "$family_shape" "$stat_state" "baseline" "$baseline_query")"
  candidate_adoption="$(run_eqp "$family_shape" "$stat_state" "candidate" "$candidate_query")"
  run_warm_and_timing "$family_shape" "$stat_state" "$baseline_query" "$candidate_query" "PASS" "$baseline_adoption" "$candidate_adoption"
}

run_shape_ep_latest() {
  family_shape="$1"
  stat_state="$2"
  baseline_query="$3"
  candidate_query="$4"
  expected_distinct_ids="$5"
  identity_ep_latest_grouped "$family_shape" "$stat_state" "$baseline_query" "$candidate_query" "$expected_distinct_ids"
  baseline_adoption="$(run_eqp "$family_shape" "$stat_state" "baseline" "$baseline_query")"
  candidate_adoption="$(run_eqp "$family_shape" "$stat_state" "candidate" "$candidate_query")"
  run_warm_and_timing "$family_shape" "$stat_state" "$baseline_query" "$candidate_query" "PASS" "$baseline_adoption" "$candidate_adoption"
}

materialize_queries() {
  a01_baseline="$(render_emby_template "$(make_shape_a01_baseline)" "$CANARY_EMBY_A_SHAPE01_USER_ID" "$CANARY_EMBY_A_SHAPE01_ANCESTORS")"
  a01_candidate="$(candidate_ancestor_splice "$a01_baseline" "$CANARY_EMBY_A_SHAPE01_ANCESTORS")"
  write_query_file "$QUERY_DIR/a-shape01.baseline.sql" "$a01_baseline"
  write_query_file "$QUERY_DIR/a-shape01.candidate.sql" "$a01_candidate"
  printf 'a-shape01	baked	env-configured shape 01; L1 reused
' >> "$LITERALS_TSV"

  a04_baseline="$(render_emby_template "$(make_shape_a04_baseline)" "$CANARY_EMBY_A_SHAPE04_USER_ID" "$CANARY_EMBY_A_SHAPE04_ANCESTORS")"
  a04_candidate="$(candidate_ancestor_splice "$a04_baseline" "$CANARY_EMBY_A_SHAPE04_ANCESTORS")"
  write_query_file "$QUERY_DIR/a-shape04.baseline.sql" "$a04_baseline"
  write_query_file "$QUERY_DIR/a-shape04.candidate.sql" "$a04_candidate"
  printf 'a-shape04	baked	env-configured shape 04; L1 reused
' >> "$LITERALS_TSV"

  c_empty_baseline="$(resd_baseline_from_captured_exists "c-empty" "$CANARY_EMBY_C_EMPTY_CAPTURE_HASH" "$CANARY_EMBY_C_EMPTY_USER_ID" "$CANARY_EMBY_C_EMPTY_ANCESTORS")"
  c_empty_candidate="$(candidate_resd "$c_empty_baseline" "$CANARY_EMBY_C_EMPTY_ANCESTORS" "$CANARY_EMBY_C_EMPTY_USER_ID")"
  write_query_file "$QUERY_DIR/c-empty.baseline.sql" "$c_empty_baseline"
  write_query_file "$QUERY_DIR/c-empty.candidate.sql" "$c_empty_candidate"
  printf 'c-empty	captured	static-captures/%s hash %s; baseline reconstructed by inverse EXISTS splice
' "$CANARY_EMBY_C_EMPTY_CAPTURE_FILE" "$CANARY_EMBY_C_EMPTY_CAPTURE_HASH" >> "$LITERALS_TSV"

  c_two_baseline="$(resd_baseline_from_captured_in "c-two-group" "$CANARY_EMBY_C_TWO_GROUP_CAPTURE_HASH" "$CANARY_EMBY_C_TWO_GROUP_USER_ID" "$CANARY_EMBY_C_TWO_GROUP_ANCESTORS")"
  c_two_candidate="$(candidate_resd "$c_two_baseline" "$CANARY_EMBY_C_TWO_GROUP_ANCESTORS" "$CANARY_EMBY_C_TWO_GROUP_USER_ID")"
  write_query_file "$QUERY_DIR/c-two-group.baseline.sql" "$c_two_baseline"
  write_query_file "$QUERY_DIR/c-two-group.candidate.sql" "$c_two_candidate"
  printf 'c-two-group	captured	static-captures/%s hash %s; collapsed variant; captured original-form baseline
' "$CANARY_EMBY_C_TWO_GROUP_CAPTURE_FILE" "$CANARY_EMBY_C_TWO_GROUP_CAPTURE_HASH" >> "$LITERALS_TSV"

  c_large_baseline="$(resd_baseline_from_captured_exists "c-large-group" "$CANARY_EMBY_C_LARGE_GROUP_CAPTURE_HASH" "$CANARY_EMBY_C_LARGE_GROUP_USER_ID" "$CANARY_EMBY_C_LARGE_GROUP_ANCESTORS")"
  c_large_candidate="$(candidate_resd "$c_large_baseline" "$CANARY_EMBY_C_LARGE_GROUP_ANCESTORS" "$CANARY_EMBY_C_LARGE_GROUP_USER_ID")"
  write_query_file "$QUERY_DIR/c-large-group.baseline.sql" "$c_large_baseline"
  write_query_file "$QUERY_DIR/c-large-group.candidate.sql" "$c_large_candidate"
  printf 'c-large-group	captured	static-captures/%s hash %s; baseline reconstructed by inverse EXISTS splice
' "$CANARY_EMBY_C_LARGE_GROUP_CAPTURE_FILE" "$CANARY_EMBY_C_LARGE_GROUP_CAPTURE_HASH" >> "$LITERALS_TSV"

  d_baseline="$(render_emby_template "$(make_shape_d_baseline)" "$CANARY_EMBY_D_EP_LATEST_USER_ID" "$CANARY_EMBY_D_EP_LATEST_ANCESTORS")"
  d_candidate="$(replace_once "$d_baseline" "FROM MediaItems WHERE Type = 8" "FROM MediaItems INDEXED BY $LATEST_INDEX WHERE Type = 8")"
  write_query_file "$QUERY_DIR/d-ep-latest-a.baseline.sql" "$d_baseline"
  write_query_file "$QUERY_DIR/d-ep-latest-a.candidate.sql" "$d_candidate"
  printf 'd-ep-latest-a	baked	env-configured latest-episode shape; candidate adds INDEXED BY %s
' "$LATEST_INDEX" >> "$LITERALS_TSV"
}

run_all_for_state() {
  stat_state="$1"
  run_shape_full "a-shape01" "$stat_state" "$QUERY_DIR/a-shape01.baseline.sql" "$QUERY_DIR/a-shape01.candidate.sql" "$CANARY_EMBY_A_SHAPE01_EXPECTED_ROWS"
  run_shape_full "a-shape04" "$stat_state" "$QUERY_DIR/a-shape04.baseline.sql" "$QUERY_DIR/a-shape04.candidate.sql" "$CANARY_EMBY_A_SHAPE04_EXPECTED_ROWS"
  run_shape_resd "c-empty" "$stat_state" "$CANARY_EMBY_C_EMPTY_ANCESTORS" "$CANARY_EMBY_C_EMPTY_USER_ID" "mixed" "$QUERY_DIR/c-empty.baseline.sql" "$QUERY_DIR/c-empty.candidate.sql" "$CANARY_EMBY_C_EMPTY_EXPECTED_GROUPS"
  run_shape_resd "c-two-group" "$stat_state" "$CANARY_EMBY_C_TWO_GROUP_ANCESTORS" "$CANARY_EMBY_C_TWO_GROUP_USER_ID" "0065" "$QUERY_DIR/c-two-group.baseline.sql" "$QUERY_DIR/c-two-group.candidate.sql" "$CANARY_EMBY_C_TWO_GROUP_EXPECTED_GROUPS"
  run_shape_resd "c-large-group" "$stat_state" "$CANARY_EMBY_C_LARGE_GROUP_ANCESTORS" "$CANARY_EMBY_C_LARGE_GROUP_USER_ID" "mixed" "$QUERY_DIR/c-large-group.baseline.sql" "$QUERY_DIR/c-large-group.candidate.sql" "$CANARY_EMBY_C_LARGE_GROUP_EXPECTED_GROUPS"
  run_shape_ep_latest "d-ep-latest-a" "$stat_state" "$QUERY_DIR/d-ep-latest-a.baseline.sql" "$QUERY_DIR/d-ep-latest-a.candidate.sql" "$CANARY_EMBY_D_EP_LATEST_EXPECTED_DISTINCT_IDS"
}

main() {
  require_host
  init_run_dir
  disk_preflight
  refresh_copy
  materialize_queries

  state_probe "current"
  run_all_for_state "current"

  analyze_copy
  state_probe "analyzed"
  run_all_for_state "analyzed"

  if [ -s "$CONCERNS_TSV" ] && [ "$(wc -l < "$CONCERNS_TSV" | awk '{print $1}')" -gt 1 ]; then
    printf 'HARNESS_STATUS done_with_concerns run_dir=%s results=%s concerns=%s\n' "$RUN_DIR" "$RESULTS_TSV" "$CONCERNS_TSV"
  else
    printf 'HARNESS_STATUS done run_dir=%s results=%s\n' "$RUN_DIR" "$RESULTS_TSV"
  fi
}

main "$@"
