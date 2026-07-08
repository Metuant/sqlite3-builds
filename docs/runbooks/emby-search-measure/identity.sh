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

BIN=$SQLITE_BIN
DB=$EMBY_DB
SCRATCH=$SCRATCH_ROOT
RUN_ID=${RUN_ID:-emby-search-measure-identity-$(date -u +%Y%m%dT%H%M%SZ)}
RUN_DIR=$SCRATCH/$RUN_ID

CANDIDATES=${CANDIDATES:-"COMBINED B8 B1 B2 B3 B4 B5 B5_INLINE B7"}
VARIANTS=${VARIANTS:-"type presentation"}
MATCH_CASES=${MATCH_CASES:-"case1_or case1_and case2_or case2_and"}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

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
OLD_SELECT_TYPE='select count(*) OVER() AS TotalRecordCount,A.type,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=__EMBY_USER_ID__ and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from'
OLD_SELECT_PRESENTATION='select count(*) OVER() AS TotalRecordCount,A.type,A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.PresentationUniqueKey,A.Images,A.Status,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=__EMBY_USER_ID__ and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from'
NEW_SELECT_ID='select A.Id AS id,(Select ShareLevel from UserItemShares join AncestorIds2 on AncestorIds2.AncestorId=UserItemShares.ItemId where UserItemShares.UserId=__EMBY_USER_ID__ and UserItemShares.ShareLevel not null and AncestorIds2.ItemId=A.Id order by Distance limit 1) as ShareLevel from'

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

render_identity_core() {
  variant=$1
  candidate=$2
  match=$3
  if [ "$variant" = type ]; then
    render_query "$variant" "$candidate" "$match" |
      replace_literal "$OLD_SELECT_TYPE" "$NEW_SELECT_ID" |
      replace_literal " ORDER BY Rank ASC LIMIT 50" ""
  else
    render_query "$variant" "$candidate" "$match" |
      replace_literal "$OLD_SELECT_PRESENTATION" "$NEW_SELECT_ID" |
      replace_literal " ORDER BY Rank ASC LIMIT 50" ""
  fi
}

write_identity_sql() {
  sql=$1
  variant=$2
  candidate=$3
  match=$4
  has_sha3=$5
  # Capture each arm in a command-substitution subshell FIRST: render_identity_core
  # / render_query assign positional params to GLOBALS (no `local` in POSIX sh), so
  # calling them inline would clobber $candidate to BASE and make the rewrite arm
  # render BASE too (vacuous BASE-vs-BASE identity). Subshell capture isolates that.
  orig_core=$(render_identity_core "$variant" BASE "$match")
  rew_core=$(render_identity_core "$variant" "$candidate" "$match")
  {
    printf '.headers off\n.mode tabs\n'
    printf 'PRAGMA temp_store=2;\n'
    printf 'DROP TABLE IF EXISTS temp.original_identity;\n'
    printf 'DROP TABLE IF EXISTS temp.rewrite_identity;\n'
    printf 'CREATE TEMP TABLE original_identity AS\n'
    printf 'SELECT id, count(*) AS n FROM (\n'
    printf '%s\n' "$orig_core"
    printf ') GROUP BY id;\n'
    printf 'CREATE TEMP TABLE rewrite_identity AS\n'
    printf 'SELECT id, count(*) AS n FROM (\n'
    printf '%s\n' "$rew_core"
    printf ') GROUP BY id;\n'
    if [ "$has_sha3" = 1 ]; then
      printf "SELECT 'SUMMARY' AS tag, 'original' AS arm, count(*) AS distinct_ids, coalesce(sum(n), 0) AS row_count, lower(hex(sha3_query('SELECT id, n FROM temp.original_identity ORDER BY id', 256))) AS digest FROM temp.original_identity;\n"
      printf "SELECT 'SUMMARY' AS tag, 'rewrite' AS arm, count(*) AS distinct_ids, coalesce(sum(n), 0) AS row_count, lower(hex(sha3_query('SELECT id, n FROM temp.rewrite_identity ORDER BY id', 256))) AS digest FROM temp.rewrite_identity;\n"
    else
      printf "SELECT 'SUMMARY' AS tag, 'original' AS arm, count(*) AS distinct_ids, coalesce(sum(n), 0) AS row_count, 'sha3_unavailable' AS digest FROM temp.original_identity;\n"
      printf "SELECT 'SUMMARY' AS tag, 'rewrite' AS arm, count(*) AS distinct_ids, coalesce(sum(n), 0) AS row_count, 'sha3_unavailable' AS digest FROM temp.rewrite_identity;\n"
    fi
    printf "SELECT 'DIFF' AS tag, o.id, o.n AS original_n, r.n AS rewrite_n FROM temp.original_identity AS o LEFT JOIN temp.rewrite_identity AS r USING (id) WHERE r.id IS NULL OR r.n <> o.n\n"
    printf "UNION ALL\n"
    printf "SELECT 'DIFF' AS tag, r.id, o.n AS original_n, r.n AS rewrite_n FROM temp.rewrite_identity AS r LEFT JOIN temp.original_identity AS o USING (id) WHERE o.id IS NULL\n"
    printf "ORDER BY id LIMIT 200;\n"
    printf "SELECT 'EXCEPTCHK' AS tag, (SELECT count(*) FROM (SELECT id,n FROM temp.original_identity EXCEPT SELECT id,n FROM temp.rewrite_identity)) AS orig_only, (SELECT count(*) FROM (SELECT id,n FROM temp.rewrite_identity EXCEPT SELECT id,n FROM temp.original_identity)) AS rewrite_only;\n"
  } > "$sql"
}

write_table_sql() {
  sql=$1
  variant=$2
  candidate=$3
  match=$4
  arm=$5
  {
    printf '.headers off\n.mode tabs\n'
    printf 'PRAGMA temp_store=2;\n'
    printf 'SELECT id, count(*) AS n FROM (\n'
    if [ "$arm" = original ]; then
      render_identity_core "$variant" BASE "$match"
    else
      render_identity_core "$variant" "$candidate" "$match"
    fi
    printf '\n) GROUP BY id ORDER BY id;\n'
  } > "$sql"
}

run_sql() {
  sql=$1
  out=$2
  label=$3
  "$BIN" -readonly -batch "$DB" < "$sql" > "$out" 2>&1 || die "$label failed; see $out"
}

[ -x "$BIN" ] || die "sqlite3 binary not executable: $BIN"
[ -r "$DB" ] || die "database copy not readable: $DB"
mkdir -p "$RUN_DIR"

printf 'emby-search-measure identity run\nBIN=%s\nDB=%s\nSCRATCH=%s\nRUN_DIR=%s\nstat_state=current\nquery_only=1\n' "$BIN" "$DB" "$SCRATCH" "$RUN_DIR" > "$RUN_DIR/manifest.txt"

sha3_probe=$RUN_DIR/sha3-probe.sql
sha3_out=$RUN_DIR/sha3-probe.out
printf "PRAGMA query_only=1;\nSELECT lower(hex(sha3_query('SELECT 1', 256)));\n" > "$sha3_probe"
if "$BIN" -readonly -batch "$DB" < "$sha3_probe" > "$sha3_out" 2>&1; then
  HAS_SHA3=1
else
  HAS_SHA3=0
fi
printf 'HAS_SHA3=%s\n' "$HAS_SHA3" | tee -a "$RUN_DIR/manifest.txt"

failures=0
for variant in $VARIANTS; do
  for match_case in $MATCH_CASES; do
    match=$(match_literal "$match_case")
    for candidate in $CANDIDATES; do
      stem=$variant-$match_case-$candidate
      sql=$RUN_DIR/identity-$stem.sql
      out=$RUN_DIR/identity-$stem.out
      write_identity_sql "$sql" "$variant" "$candidate" "$match" "$HAS_SHA3"
      run_sql "$sql" "$out" "identity $stem"
      if grep '^DIFF	' "$out" >/dev/null 2>&1; then
        printf 'IDENTITY_MISMATCH\tvariant=%s\tmatch_case=%s\tcandidate=%s\tout=%s\n' "$variant" "$match_case" "$candidate" "$out" | tee -a "$RUN_DIR/identity-summary.tsv"
        failures=$((failures + 1))
      else
        printf 'IDENTITY_OK\tvariant=%s\tmatch_case=%s\tcandidate=%s\tout=%s\n' "$variant" "$match_case" "$candidate" "$out" | tee -a "$RUN_DIR/identity-summary.tsv"
      fi
      if [ "$HAS_SHA3" = 0 ]; then
        orig_sql=$RUN_DIR/table-original-$stem.sql
        rew_sql=$RUN_DIR/table-rewrite-$stem.sql
        orig_tsv=$RUN_DIR/table-original-$stem.tsv
        rew_tsv=$RUN_DIR/table-rewrite-$stem.tsv
        write_table_sql "$orig_sql" "$variant" "$candidate" "$match" original
        write_table_sql "$rew_sql" "$variant" "$candidate" "$match" rewrite
        run_sql "$orig_sql" "$orig_tsv" "original table $stem"
        run_sql "$rew_sql" "$rew_tsv" "rewrite table $stem"
        sha256sum "$orig_tsv" "$rew_tsv" > "$RUN_DIR/table-$stem.sha256"
      fi
    done
  done
done

if [ "$failures" -eq 0 ]; then
  printf 'STATUS\tidentity_complete\tmismatches=0\trun_dir=%s\n' "$RUN_DIR" | tee -a "$RUN_DIR/identity-summary.tsv"
else
  printf 'STATUS\tidentity_failed\tmismatches=%s\trun_dir=%s\n' "$failures" "$RUN_DIR" | tee -a "$RUN_DIR/identity-summary.tsv"
  exit 1
fi
