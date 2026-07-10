#!/bin/sh
set -eu

HARNESS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ENV_FILE=$HARNESS_DIR/env
if [ ! -f "$ENV_FILE" ]; then
  printf 'error: missing %s; copy %s and fill local values
' "$ENV_FILE" "$HARNESS_DIR/env.example" >&2
  exit 2
fi
# shellcheck source=/dev/null
. "$ENV_FILE"

require_env() {
  for name in "$@"; do
    eval "value=\${$name-}"
    [ -n "$value" ] || {
      printf 'error: %s is required in %s; see %s
' "$name" "$ENV_FILE" "$HARNESS_DIR/env.example" >&2
      exit 2
    }
  done
}

require_env SQLITE_BIN EMBY_DB SCRATCH_ROOT EMBY_USER_ID EMBY_ANCESTORS   MATCH_CASE1_OR MATCH_CASE1_AND MATCH_CASE2_OR MATCH_CASE2_AND   MATCH_PHRASE MATCH_PHRASE_PREFIX MATCH_CASE3_AND

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

validate_run_id() {
  case "$1" in
    ''|*/*|*..*) die "RUN_ID must be a single path segment without '/' or '..'" ;;
  esac
}

BIN=$SQLITE_BIN
DB=$EMBY_DB
SCRATCH=$SCRATCH_ROOT
RUN_ID=${RUN_ID:-emby-search-measure-timing-$(date -u +%Y%m%dT%H%M%SZ)}
validate_run_id "$RUN_ID"
RUN_DIR=$SCRATCH/$RUN_ID
ITERS=${ITERS:-4}
QUERY_TIMEOUT_S=${QUERY_TIMEOUT_S:-10}

CANDIDATES=${CANDIDATES:-"COMBINED B8 B1 B2 B3 B4 B5 B5_INLINE B7"}
VARIANTS=${VARIANTS:-"type presentation"}
MATCH_CASES=${MATCH_CASES:-"case1_or case1_and case2_or case2_and"}

cleanup_on_exit() {
  rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${RUN_DIR:-}" ]; then
    case "$RUN_DIR" in
      "$SCRATCH"/*) rm -rf "$RUN_DIR" ;;
    esac
  fi
}
trap cleanup_on_exit EXIT

match_literal() {
  case "$1" in
    case1_or) printf '%s
' "$MATCH_CASE1_OR" ;;
    case1_and) printf '%s
' "$MATCH_CASE1_AND" ;;
    case2_or) printf '%s
' "$MATCH_CASE2_OR" ;;
    case2_and) printf '%s
' "$MATCH_CASE2_AND" ;;
    phrase) printf '%s
' "$MATCH_PHRASE" ;;
    phrase_prefix) printf '%s
' "$MATCH_PHRASE_PREFIX" ;;
    case3_and) printf '%s
' "$MATCH_CASE3_AND" ;;
    *) die "unknown match case: $1" ;;
  esac
}

replace_literal() {
  old=$1
  new=$2
  awk -v old="$old" -v new="$new" '
    BEGIN { if (old == "") exit 2 }
    {
      out = ""
      rest = $0
      while ((pos = index(rest, old)) > 0) {
        out = out substr(rest, 1, pos - 1) new
        rest = substr(rest, pos + length(old))
      }
      print out rest
    }
  '
}

render_placeholders() {
  replace_literal "__ANCESTORS__" "$EMBY_ANCESTORS" |
    replace_literal "__EMBY_USER_ID__" "$EMBY_USER_ID"
}

OLD_WITH_ITEM='WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))'
NEW_WITH_ITEM_UNION_ALL='WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union all select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))'
OLD_B2='A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors)'
NEW_B2='EXISTS (SELECT 1 FROM itemPeople2 WHERE itemPeople2.PersonId = A.Id AND itemPeople2.ItemId in WithAncestors)'
OLD_B3='A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__))'
NEW_B3='EXISTS (SELECT 1 FROM ListItems ListItemsExemptionForPlaylists JOIN ancestorids2 AncestorIdExemptionForPlaylists ON ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid AND AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__) WHERE ListItemsExemptionForPlaylists.ListId = A.Id)'
OLD_B4='A.Id in WithAncestors'
NEW_B4='EXISTS (SELECT 1 FROM WithAncestors WHERE WithAncestors.itemid = A.Id)'
OLD_B5='A.Id in WithItemLinkItemIds'
NEW_B5='EXISTS (SELECT 1 FROM WithItemLinkItemIds WHERE WithItemLinkItemIds.LinkedId = A.Id)'
OLD_OR_MEMBERSHIP='(A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds)'
EMBY_EXISTS_ARMS='(EXISTS (SELECT 1 FROM AncestorIds2 WHERE AncestorIds2.itemid = A.Id AND AncestorIds2.AncestorId in (__ANCESTORS__)) OR  exists (select 1 from ListItems join ancestorids2 on ListItems.ListItemId=ancestorids2.itemid and ancestorids2.AncestorId in (__ANCESTORS__) where ListItems.ListId=A.Id) OR EXISTS (SELECT 1 FROM itemPeople2 JOIN AncestorIds2 ON AncestorIds2.itemid = itemPeople2.ItemId WHERE itemPeople2.PersonId = A.Id and AncestorIds2.AncestorId in (__ANCESTORS__)) OR Exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (__ANCESTORS__)  where ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6,4,7,10,3,2,0,1,5)) OR Exists (select 1 from ItemLinks2 ItemLinks2TwoLevel where exists (select 1 From ItemLinks2 join ancestorids2 on ancestorids2.itemid=itemlinks2.itemid and ancestorids2.ancestorid in (__ANCESTORS__)  where itemlinks2.linkedid = itemlinks2twolevel.itemid AND ItemLinks2.Type in (7,0,1,5,6,4,2)) and ItemLinks2TwoLevel.LinkedId=A.Id))'
NEW_A3_UNION='(A.Id in (select itemid from WithAncestors union select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__) union select PersonId from itemPeople2 where ItemId in WithAncestors union select LinkedId from WithItemLinkItemIds))'
NEW_A3_UNIONALL='(A.Id in (select itemid from WithAncestors union all select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__) union all select PersonId from itemPeople2 where ItemId in WithAncestors union all select LinkedId from WithItemLinkItemIds))'
OLD_WITH_ITEM_COMMA=',WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))'
NEW_B5_INLINE='(EXISTS (SELECT 1 FROM ItemLinks2 JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2.itemid WHERE ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6,4,7,10,3,2,0,1,5)) OR EXISTS (SELECT 1 FROM ItemLinks2 ItemLinks2TwoLevel JOIN ItemLinks2 ItemLinks2Seed ON ItemLinks2Seed.LinkedId = ItemLinks2TwoLevel.itemid JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2Seed.itemid WHERE ItemLinks2TwoLevel.LinkedId = A.Id AND ItemLinks2Seed.Type in (7,0,1,5,6,4,2)))'
OLD_CTE_END=')))select count(*) OVER()'
NEW_CTE_END="))),FtsCandidates AS MATERIALIZED (SELECT rowid AS RowId, rank AS Rank FROM fts_search9 WHERE fts_search9 MATCH '__MATCH_LITERAL__')select count(*) OVER()"
OLD_FTS_FROM="from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match '__MATCH_LITERAL__'"
NEW_FTS_FROM='from FtsCandidates join mediaitems A on A.Id=FtsCandidates.RowId'

base_query() {
  case "$1" in
    type)
      cat <<'SQL'
with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (__ANCESTORS__) ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))select count(*) OVER() AS TotalRecordCount,A.type,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=__EMBY_USER_ID__ and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match '__MATCH_LITERAL__' where A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) AND A.ExtraType is null AND (A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) Group by A.Type ORDER BY Rank ASC LIMIT 50
SQL
      ;;
    presentation)
      cat <<'SQL'
with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (__ANCESTORS__) ),WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.PresentationUniqueKey,A.Images,A.Status,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=__EMBY_USER_ID__ and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from mediaitems A join fts_search9 on A.Id=fts_search9.RowId and fts_search9 match '__MATCH_LITERAL__' left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=__EMBY_USER_ID__ where A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26,29,34) AND (Coalesce(ShareLevel, 0) > 0 OR A.Type in (1,2,5,6,8,9,10,11,12,13,14,15,18,19,20,21,22,23,24,25,26,29,34) OR A.IsPublic=1) AND A.ExtraType is null AND (A.Id in WithAncestors OR A.Id in (select ListItemsExemptionForPlaylists.ListId from ListItems ListItemsExemptionForPlaylists join ancestorids2 AncestorIdExemptionForPlaylists on ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid and AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__)) OR A.Id in (select PersonId from itemPeople2 where ItemId in WithAncestors) OR A.Id in WithItemLinkItemIds) Group by A.PresentationUniqueKey ORDER BY Rank ASC LIMIT 50
SQL
      ;;
    *) die "unknown variant: $1" ;;
  esac
}

transform_B1() { replace_literal "$OLD_WITH_ITEM" "$NEW_WITH_ITEM_UNION_ALL"; }
transform_B2() { replace_literal "$OLD_B2" "$NEW_B2"; }
transform_B3() { replace_literal "$OLD_B3" "$NEW_B3"; }
transform_B4() { replace_literal "$OLD_B4" "$NEW_B4"; }
transform_B5() { replace_literal "$OLD_B5" "$NEW_B5"; }
transform_EMBY_EXISTS() { replace_literal "$OLD_OR_MEMBERSHIP" "$EMBY_EXISTS_ARMS"; }
transform_A3_UNION() { replace_literal "$OLD_OR_MEMBERSHIP" "$NEW_A3_UNION"; }
transform_A3_UNIONALL() { replace_literal "$OLD_OR_MEMBERSHIP" "$NEW_A3_UNIONALL"; }
transform_B5_INLINE() { replace_literal "$OLD_WITH_ITEM_COMMA" ""; }
transform_B5_INLINE_ARM() { replace_literal "$OLD_B5" "$NEW_B5_INLINE"; }
transform_B7A() { replace_literal 'WithAncestors AS (' 'WithAncestors AS NOT MATERIALIZED ('; }
transform_B7B() { replace_literal 'WithItemLinkItemIds AS (' 'WithItemLinkItemIds AS NOT MATERIALIZED ('; }
transform_B8A() { replace_literal "$OLD_CTE_END" "$NEW_CTE_END"; }
transform_B8B() { replace_literal "$OLD_FTS_FROM" "$NEW_FTS_FROM"; }

apply_candidate() {
  case "$1" in
    BASE) cat ;;
    B1) transform_B1 ;;
    B2) transform_B2 ;;
    B3) transform_B3 ;;
    B4) transform_B4 ;;
    B5) transform_B5 ;;
    EMBY_EXISTS) transform_EMBY_EXISTS ;;
    A3_UNION) transform_A3_UNION ;;
    A3_UNIONALL) transform_A3_UNIONALL ;;
    B5_INLINE) transform_B5_INLINE | transform_B5_INLINE_ARM ;;
    B7) transform_B7A | transform_B7B ;;
    B8) transform_B8A | transform_B8B ;;
    COMBINED) transform_B1 | transform_B2 | transform_B3 | transform_B4 | transform_B5 | transform_B7A | transform_B7B ;;
    *) die "unknown candidate: $1" ;;
  esac
}

render_query() {
  variant=$1
  candidate=$2
  match=$3
  base_query "$variant" | apply_candidate "$candidate" | replace_literal "__MATCH_LITERAL__" "$match" | render_placeholders
}

write_eqp_sql() {
  sql=$1
  variant=$2
  candidate=$3
  match=$4
  {
    printf 'PRAGMA query_only=1;\n'
    printf 'EXPLAIN QUERY PLAN\n'
    render_query "$variant" "$candidate" "$match"
    printf ';\n'
  } > "$sql"
}

write_timed_sql() {
  sql=$1
  variant=$2
  candidate=$3
  match=$4
  arm=$5
  phase=$6
  iter=$7
  {
    printf 'PRAGMA query_only=1;\n'
    printf '.timer on\n'
    printf '.print TIMER stat_state=current variant=%s match_case=%s candidate=%s arm=%s phase=%s iter=%s\n' "$variant" "$match_case" "$candidate" "$arm" "$phase" "$iter"
    render_query "$variant" "$candidate" "$match"
    printf ';\n.timer off\n'
  } > "$sql"
}

write_schema_audit_sql() {
  sql=$1
  {
    printf '.headers on\n.mode tabs\n'
    printf 'PRAGMA query_only=1;\n'
    printf "SELECT type, name, tbl_name, replace(replace(sql, char(10), ' '), char(13), ' ') AS sql FROM sqlite_master WHERE type = 'index' AND tbl_name IN ('itemPeople2', 'ItemLinks2', 'ListItems', 'AncestorIds2') ORDER BY tbl_name, name;\n"
    printf 'EXPLAIN QUERY PLAN\n'
    printf 'WITH '
    printf 'WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) ),%s\n' "$EMBY_ANCESTORS" "$OLD_WITH_ITEM"
    printf 'SELECT LinkedId FROM WithItemLinkItemIds;\n'
  } > "$sql"
}

run_sql() {
  sql=$1
  out=$2
  label=$3
  "$BIN" -readonly -batch "$DB" < "$sql" > "$out" 2>&1 || die "$label failed; see $out"
}

run_timed_sql() {
  sql=$1
  out=$2
  label=$3
  set +e
  timeout "${QUERY_TIMEOUT_S}s" "$BIN" -readonly -batch "$DB" < "$sql" > "$out" 2>&1
  rc=$?
  set -e
  case "$rc" in
    0) return 0 ;;
    124) return 124 ;;
    *) die "$label failed rc=$rc; see $out" ;;
  esac
}

prewarm_db() {
  cat "$DB" "$DB-wal" "$DB-shm" 2>/dev/null >/dev/null || :
}

summarize_eqp() {
  eqp=$1
  variant=$2
  match_case=$3
  candidate=$4
  arm=$5
  materialization=$(grep -Ei 'MATERIALIZE|USE TEMP B-TREE|UNION USING TEMP B-TREE|LIST SUBQUERY|CO-ROUTINE' "$eqp" 2>/dev/null | tr '\n' ';')
  if [ -z "$materialization" ]; then
    materialization=none
  fi
  printf 'EQP_SUMMARY\tstat_state=current\tvariant=%s\tmatch_case=%s\tcandidate=%s\tarm=%s\tmaterialization=%s\tfile=%s\n' "$variant" "$match_case" "$candidate" "$arm" "$materialization" "$eqp" | tee -a "$RUN_DIR/eqp-summary.tsv" >/dev/null
}

parse_time() {
  out=$1
  variant=$2
  match_case=$3
  candidate=$4
  arm=$5
  iter=$6
  awk -v v="$variant" -v m="$match_case" -v c="$candidate" -v a="$arm" -v i="$iter" -v f="$out" '
    /Run Time/ {
      real = ""
      for (n = 1; n <= NF; n++) {
        if ($n == "real") {
          real = $(n + 1)
        }
      }
      if (real != "") {
        print "TIME\tstat_state=current\tvariant=" v "\tmatch_case=" m "\tcandidate=" c "\tarm=" a "\titer=" i "\treal=" real "\tfile=" f
      }
    }
  ' "$out" >> "$RUN_DIR/timings.tsv"
}

record_timeout() {
  timeout_variant=$1
  timeout_match_case=$2
  timeout_target=$3
  timeout_arm=$4
  timeout_phase=$5
  timeout_iter=$6
  timeout_out=$7
  printf 'TIMEOUT\tstat_state=current\tvariant=%s\tmatch_case=%s\tcandidate=%s\tarm=%s\tphase=%s\titer=%s\ttimeout_s=%s\tfile=%s\n' "$timeout_variant" "$timeout_match_case" "$timeout_target" "$timeout_arm" "$timeout_phase" "$timeout_iter" "$QUERY_TIMEOUT_S" "$timeout_out" >> "$RUN_DIR/timings.tsv"
}

write_medians() {
  awk -F '\t' '
    /^TIME\t/ {
      key = ""
      real = ""
      for (i = 2; i <= NF; i++) {
        if ($i ~ /^real=/) {
          real = substr($i, 6)
        } else if ($i ~ /^iter=/ || $i ~ /^file=/) {
          continue
        } else {
          key = key ? key FS $i : $i
        }
      }
      if (key != "" && real != "") {
        n[key]++
        vals[key, n[key]] = real + 0
      }
    }
    END {
      for (key in n) {
        for (i = 1; i <= n[key]; i++) {
          for (j = i + 1; j <= n[key]; j++) {
            if (vals[key, j] < vals[key, i]) {
              t = vals[key, i]
              vals[key, i] = vals[key, j]
              vals[key, j] = t
            }
          }
        }
        if (n[key] % 2) {
          med = vals[key, int((n[key] + 1) / 2)]
        } else {
          med = (vals[key, n[key] / 2] + vals[key, n[key] / 2 + 1]) / 2
        }
        printf "MEDIAN\t%s\truns=%d\treal=%.6f\n", key, n[key], med
      }
    }
  ' "$RUN_DIR/timings.tsv" > "$RUN_DIR/medians.tsv"
}

run_pair() {
  # Use `target` (not `candidate`): write_eqp_sql/write_timed_sql/render_query all
  # assign to a GLOBAL `candidate` (no `local` in POSIX sh), so reading `$candidate`
  # after the baseline arm would yield BASE and make the candidate arm time BASE too.
  pair_variant=$1
  pair_match_case=$2
  target=$3
  pair_match=$(match_literal "$pair_match_case")
  pair_dir=$RUN_DIR/$target/$pair_variant/$pair_match_case
  baseline_timed_out=0
  candidate_timed_out=0
  mkdir -p "$pair_dir"

  for arm in baseline candidate; do
    if [ "$arm" = baseline ]; then
      arm_candidate=BASE
    else
      arm_candidate=$target
    fi
    eqp_sql=$pair_dir/eqp-$arm.sql
    eqp_out=$pair_dir/eqp-$arm.out
    write_eqp_sql "$eqp_sql" "$pair_variant" "$arm_candidate" "$pair_match"
    run_sql "$eqp_sql" "$eqp_out" "eqp $target $pair_variant $pair_match_case $arm"
    summarize_eqp "$eqp_out" "$pair_variant" "$pair_match_case" "$target" "$arm"
  done

  prewarm_db
  for arm in baseline candidate; do
    if [ "$arm" = baseline ]; then
      arm_candidate=BASE
    else
      arm_candidate=$target
    fi
    warm_sql=$pair_dir/warm-$arm.sql
    warm_out=$pair_dir/warm-$arm.out
    write_timed_sql "$warm_sql" "$pair_variant" "$arm_candidate" "$pair_match" "$arm" warm 0
    if run_timed_sql "$warm_sql" "$warm_out" "warm $target $pair_variant $pair_match_case $arm"; then
      :
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then
        [ "$arm" = baseline ] && baseline_timed_out=1 || candidate_timed_out=1
        record_timeout "$pair_variant" "$pair_match_case" "$target" "$arm" warm 0 "$warm_out"
        continue
      fi
      die "warm query returned unexpected rc=$rc for $target $pair_variant $pair_match_case $arm"
    fi
  done

  iter=1
  while [ "$iter" -le "$ITERS" ]; do
    if [ $((iter % 2)) -eq 1 ]; then
      order="baseline candidate"
    else
      order="candidate baseline"
    fi
    prewarm_db
    for arm in $order; do
      if [ "$arm" = baseline ] && [ "$baseline_timed_out" -eq 1 ]; then
        printf 'TIME_SKIP\tstat_state=current\tvariant=%s\tmatch_case=%s\tcandidate=%s\tarm=%s\titer=%s\treason=prior_timeout\n' "$pair_variant" "$pair_match_case" "$target" "$arm" "$iter" >> "$RUN_DIR/timings.tsv"
        continue
      fi
      if [ "$arm" = candidate ] && [ "$candidate_timed_out" -eq 1 ]; then
        printf 'TIME_SKIP\tstat_state=current\tvariant=%s\tmatch_case=%s\tcandidate=%s\tarm=%s\titer=%s\treason=prior_timeout\n' "$pair_variant" "$pair_match_case" "$target" "$arm" "$iter" >> "$RUN_DIR/timings.tsv"
        continue
      fi
      if [ "$arm" = baseline ]; then
        arm_candidate=BASE
      else
        arm_candidate=$target
      fi
      timed_sql=$pair_dir/timed-$iter-$arm.sql
      timed_out=$pair_dir/timed-$iter-$arm.out
      write_timed_sql "$timed_sql" "$pair_variant" "$arm_candidate" "$pair_match" "$arm" timed "$iter"
      if run_timed_sql "$timed_sql" "$timed_out" "timed $target $pair_variant $pair_match_case iter $iter $arm"; then
        :
      else
        rc=$?
        if [ "$rc" -eq 124 ]; then
          [ "$arm" = baseline ] && baseline_timed_out=1 || candidate_timed_out=1
          record_timeout "$pair_variant" "$pair_match_case" "$target" "$arm" timed "$iter" "$timed_out"
          continue
        fi
        die "timed query returned unexpected rc=$rc for $target $pair_variant $pair_match_case iter $iter $arm"
      fi
      parse_time "$timed_out" "$pair_variant" "$pair_match_case" "$target" "$arm" "$iter"
    done
    if [ "$baseline_timed_out" -eq 1 ] && [ "$candidate_timed_out" -eq 1 ]; then
      break
    fi
    iter=$((iter + 1))
  done
}

[ -x "$BIN" ] || die "sqlite3 binary not executable: $BIN"
[ -r "$DB" ] || die "database copy not readable: $DB"
[ "$ITERS" -ge 3 ] || die "ITERS must be at least 3"
case "$QUERY_TIMEOUT_S" in
  ''|*[!0-9]*) die "QUERY_TIMEOUT_S must be a positive integer, got $QUERY_TIMEOUT_S" ;;
esac
[ "$QUERY_TIMEOUT_S" -ge 1 ] || die "QUERY_TIMEOUT_S must be a positive integer, got $QUERY_TIMEOUT_S"
command -v timeout >/dev/null 2>&1 || die "timeout command is required"
mkdir -p "$RUN_DIR"

printf 'emby-search-measure timing run\nBIN=%s\nDB=%s\nSCRATCH=%s\nRUN_DIR=%s\nstat_state=current\nquery_only=1\niters=%s\n' "$BIN" "$DB" "$SCRATCH" "$RUN_DIR" "$ITERS" > "$RUN_DIR/manifest.txt"
: > "$RUN_DIR/timings.tsv"
: > "$RUN_DIR/eqp-summary.tsv"

schema_sql=$RUN_DIR/schema-audit.sql
schema_out=$RUN_DIR/schema-audit.out
write_schema_audit_sql "$schema_sql"
run_sql "$schema_sql" "$schema_out" "schema audit"

for candidate in $CANDIDATES; do
  for variant in $VARIANTS; do
    for match_case in $MATCH_CASES; do
      run_pair "$variant" "$match_case" "$candidate"
    done
  done
done

write_medians
printf 'STATUS\ttiming_complete\trun_dir=%s\tmedians=%s\n' "$RUN_DIR" "$RUN_DIR/medians.tsv" | tee -a "$RUN_DIR/timings.tsv"
