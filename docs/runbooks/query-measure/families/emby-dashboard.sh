# shellcheck shell=bash disable=SC2034
# Query-family data for Emby dashboard Latest. Sourced by query-measure.sh.

family_configure() {
  require_env EMBY_DB_SOURCE
  SOURCE_DB=$EMBY_DB_SOURCE
  DASHBOARD_ANCESTOR_SIZE=${DASHBOARD_ANCESTOR_SIZE:-12}
  PREPARE_DASHBOARD_INDEXES=${PREPARE_DASHBOARD_INDEXES:-0}
  require_uint DASHBOARD_ANCESTOR_SIZE
  require_uint PREPARE_DASHBOARD_INDEXES
  [ "$DASHBOARD_ANCESTOR_SIZE" -ge 1 ] || die 'DASHBOARD_ANCESTOR_SIZE must be at least 1'
  case "$PREPARE_DASHBOARD_INDEXES" in 0|1) ;; *) die 'PREPARE_DASHBOARD_INDEXES must be 0 or 1' ;; esac
  DASHBOARD_USER_ID=
  DASHBOARD_ANCESTORS=
}

family_all_cases() {
  printf '%s\n' \
    movies-latest \
    mixed-latest \
    episodes-latest \
    episodes-latest-single-index
}
family_cases() {
  local -a selected
  if [ -n "${CASES:-}" ]; then read -r -a selected <<< "$CASES"; printf '%s\n' "${selected[@]}"; else family_all_cases; fi
}

# The shared engine owns fixed vendor/candidate labels. The comparison case puts
# the shipped two-index arm in the vendor slot for a direct paired-to-single A-B.
family_case_role() {
  case "$1" in
    movies-latest|episodes-latest) printf 'SHIPPED\n' ;;
    mixed-latest|episodes-latest-single-index) printf 'COMPARISON\n' ;;
    *) die "unknown Emby dashboard case role: $1" ;;
  esac
}

family_contract_check() {
  local _role
  grep -Fx '    X(EMBY_EPISODES_LATEST, "emby", "dashboard+episodes_latest", "emby_fts_rewrite", 1) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby Episodes Latest mode catalogue contract drifted'
  grep -Fx '    X(EMBY_MOVIES_LATEST, "emby", "dashboard+movies_latest", "emby_fts_rewrite", 1) \' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby movies Latest mode catalogue contract drifted'
  grep -Fx '    X(EMBY_MIXED_LATEST, "emby", "dashboard+mixed_latest", "emby_fts_rewrite", 1)' "${REPO_ROOT}/src/rewrite_modes.h" >/dev/null || die 'Emby mixed Latest mode catalogue contract drifted'
  [ "$(family_all_cases | wc -l | tr -d ' ')" -eq 4 ] || die 'Emby dashboard arm matrix incomplete'
  for _case in movies-latest mixed-latest episodes-latest episodes-latest-single-index; do
    family_all_cases | grep -Fx "$_case" >/dev/null || die "missing Emby dashboard measurement arm: $_case"
    _role=$(family_case_role "$_case")
    case "$_case:$_role" in
      movies-latest:SHIPPED|mixed-latest:COMPARISON|episodes-latest:SHIPPED|episodes-latest-single-index:COMPARISON) ;;
      *) die "Emby dashboard arm role drifted: case=$_case role=$_role" ;;
    esac
  done
  while IFS= read -r _case; do family_all_cases | grep -Fx "$_case" >/dev/null || die "unknown CASES entry for emby-dashboard: $_case"; done < <(family_cases)
}

# These production matcher shapes reject bind parameters. The derived user and
# ancestor cells therefore remain literals in the raw prepare form by contract.
family_parameters() { :; }

family_derive_sql() {
  cat <<SQL
SELECT 'USER',UserId,count(*) AS n FROM UserDatas GROUP BY UserId ORDER BY n DESC,UserId LIMIT 1;
SELECT 'ANCESTOR',X.AncestorId,count(*) AS n
FROM AncestorIds2 AS X JOIN MediaItems AS A ON A.Id=X.ItemId
WHERE A.Type IN (5,8)
GROUP BY X.AncestorId ORDER BY n DESC,X.AncestorId LIMIT ${DASHBOARD_ANCESTOR_SIZE};
SQL
}

family_parse_derived() {
  local out=$1
  DASHBOARD_USER_ID=$(awk -F '\t' '$1=="USER" && $2~/^[0-9]+$/{print $2;exit}' "$out")
  DASHBOARD_ANCESTORS=$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{print $2}' "$out" | paste -sd, -)
  [ -n "$DASHBOARD_USER_ID" ] || die 'Emby dashboard user derivation returned no numeric user'
  [ -n "$DASHBOARD_ANCESTORS" ] || die 'Emby dashboard ancestor derivation returned no numeric ancestors'
  [ "$(awk -F '\t' '$1=="ANCESTOR"{n++} END{print n+0}' "$out")" -eq "$(awk -F '\t' '$1=="ANCESTOR" && $2~/^[0-9]+$/{n++} END{print n+0}' "$out")" ] || die 'Emby dashboard ancestor derivation returned non-numeric data'
}

family_record_literals() {
  printf 'user\tid=%s\tderived=1\tselection=most_UserDatas_rows\n' "$DASHBOARD_USER_ID"
  printf 'ancestors\tcount=%s\tvalues=%s\tderived=1\tselection=most_type_5_or_8_items\n' "$(printf '%s' "$DASHBOARD_ANCESTORS" | awk -F, '{print NF}')" "$DASHBOARD_ANCESTORS"
}

family_prepare_sql() {
  [ "$PREPARE_DASHBOARD_INDEXES" -eq 1 ] || return 0
  cat <<'SQL'
.bail on
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type = 8;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_episodes_dcn_gk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) WHERE Type = 8;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_dcn_puk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) WHERE Type = 5;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_puk_dc_cover ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) WHERE Type = 5;
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_mixed_dcn_gk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) WHERE Type IN (8,5);
CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_mixed_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type IN (8,5);
SQL
}

dashboard_projection() {
  case "$1" in
    movies-latest) printf '%s' 'A.Id,A.Name,A.Path,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.Images,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ' ;;
    mixed-latest) printf '%s' 'A.type,A.Id,A.IndexNumber,A.Name,A.ParentIndexNumber,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ' ;;
    episodes-latest|episodes-latest-single-index) printf '%s' 'A.Id,A.EndDate,A.IndexNumber,A.Name,A.Path,A.ParentIndexNumber,A.ProductionYear,A.RunTimeTicks,A.ParentId,A.SeriesName,A.AlbumId,A.SeriesId,A.Images,A.SortIndexNumber,A.SortParentIndexNumber,A.IndexNumberEnd,UserDatas.IsFavorite,UserDatas.Played,UserDatas.PlaybackPositionTicks,UserDatas.AudioStreamIndex,UserDatas.SubtitleStreamIndex ' ;;
    *) die "unknown Emby dashboard case: $1" ;;
  esac
}

family_render() {
  local case_id=$1 arm=$2 projection
  projection=$(dashboard_projection "$case_id")
  case "$case_id:$arm" in
    movies-latest:vendor)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type=5 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by A.PresentationUniqueKey ORDER BY A.DateCreated DESC LIMIT 12;\n' "$DASHBOARD_ANCESTORS" "$projection" "$DASHBOARD_USER_ID"
      ;;
    movies-latest:candidate)
      cat <<SQL
WITH ranked(id, dc, puk) AS MATERIALIZED (
  SELECT A.Id,A.DateCreated,A.PresentationUniqueKey
  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_movies_dcn_puk
  WHERE A.Type=5
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
    AND NOT EXISTS (
      SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_movies_puk_dc_cover
      WHERE B.Type=5 AND B.PresentationUniqueKey IS A.PresentationUniqueKey
        AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id))
        AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN ($DASHBOARD_ANCESTORS))
        AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=$DASHBOARD_USER_ID AND UB.played<>0)
    )
  ORDER BY (A.DateCreated IS NULL) ASC,A.DateCreated DESC,A.PresentationUniqueKey ASC LIMIT 12
)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=$DASHBOARD_USER_ID
ORDER BY (R.dc IS NULL) ASC,R.dc DESC,R.puk ASC LIMIT 12;
SQL
      ;;
    mixed-latest:vendor)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type in (8,5) AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 3;\n' "$DASHBOARD_ANCESTORS" "$projection" "$DASHBOARD_USER_ID"
      ;;
    mixed-latest:candidate)
      cat <<SQL
WITH mixed_latest_args(user_id,row_limit) AS MATERIALIZED (VALUES ($DASHBOARD_USER_ID,3)),
ranked(id,dc,gk) AS MATERIALIZED (
  SELECT A.Id,A.DateCreated,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey)
  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_mixed_dcn_gk
  CROSS JOIN mixed_latest_args AS Args
  WHERE A.Type IN (8,5)
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=Args.user_id AND U0.played<>0)
    AND NOT EXISTS (
      SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_mixed_gk_dc
      WHERE B.Type IN (8,5)
        AND coalesce(B.SeriesPresentationUniqueKey,B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey)
        AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id))
        AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN ($DASHBOARD_ANCESTORS))
        AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=Args.user_id AND UB.played<>0)
    )
  ORDER BY (A.DateCreated IS NULL) ASC,A.DateCreated DESC,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey) ASC
  LIMIT (SELECT row_limit FROM mixed_latest_args)
)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
CROSS JOIN mixed_latest_args AS Args
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=Args.user_id
ORDER BY (R.dc IS NULL) ASC,R.dc DESC,R.gk ASC
LIMIT (SELECT row_limit FROM mixed_latest_args);
SQL
      ;;
    episodes-latest:vendor)
      printf 'with WithAncestors AS (SELECT itemid FROM AncestorIds2 WHERE AncestorId in (%s) )select %sfrom mediaitems A left join UserDatas on A.UserDataKeyId=UserDatas.UserDataKeyId And UserDatas.UserId=%s where A.Type=8 AND Coalesce(UserDatas.played, 0)=0 AND A.Id in WithAncestors Group by coalesce(A.SeriesPresentationUniqueKey, A.PresentationUniqueKey) ORDER BY MAX(A.DateCreated) DESC LIMIT 12;\n' "$DASHBOARD_ANCESTORS" "$projection" "$DASHBOARD_USER_ID"
      ;;
    episodes-latest-single-index:candidate)
      cat <<SQL
WITH exact_groups(gk,id,maxdc) AS MATERIALIZED (
  SELECT coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey),A.Id,A.DateCreated
  FROM MediaItems AS A
  WHERE A.Type=8
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
    AND NOT EXISTS (
      SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_gk_dc
      WHERE B.Type=8
        AND coalesce(B.SeriesPresentationUniqueKey,B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey)
        AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id))
        AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN ($DASHBOARD_ANCESTORS))
        AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=$DASHBOARD_USER_ID AND UB.played<>0)
    )
), ranked AS MATERIALIZED (
  SELECT gk,id,maxdc FROM exact_groups ORDER BY maxdc DESC LIMIT 12
)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=$DASHBOARD_USER_ID
ORDER BY R.maxdc DESC LIMIT 12;
SQL
      ;;
    episodes-latest:candidate|episodes-latest-single-index:vendor)
      cat <<SQL
WITH ranked(id,dc,gk) AS MATERIALIZED (
  SELECT A.Id,A.DateCreated,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey)
  FROM MediaItems AS A INDEXED BY idx_dshadow_emby_latest_episodes_dcn_gk
  WHERE A.Type=8
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
    AND NOT EXISTS (
      SELECT 1 FROM MediaItems AS B INDEXED BY idx_dshadow_emby_latest_gk_dc
      WHERE B.Type=8
        AND coalesce(B.SeriesPresentationUniqueKey,B.PresentationUniqueKey) IS coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey)
        AND ((B.DateCreated IS NOT NULL AND A.DateCreated IS NULL) OR B.DateCreated>A.DateCreated OR (B.DateCreated IS A.DateCreated AND B.Id<A.Id))
        AND EXISTS (SELECT 1 FROM AncestorIds2 AS XB WHERE XB.ItemId=B.Id AND XB.AncestorId IN ($DASHBOARD_ANCESTORS))
        AND NOT EXISTS (SELECT 1 FROM UserDatas AS UB WHERE UB.UserDataKeyId=B.UserDataKeyId AND UB.UserId=$DASHBOARD_USER_ID AND UB.played<>0)
    )
  ORDER BY (A.DateCreated IS NULL) ASC,A.DateCreated DESC,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey) ASC LIMIT 12
)
SELECT ${projection}FROM ranked AS R JOIN MediaItems AS A ON A.Id=R.id
LEFT JOIN UserDatas ON A.UserDataKeyId=UserDatas.UserDataKeyId AND UserDatas.UserId=$DASHBOARD_USER_ID
ORDER BY (R.dc IS NULL) ASC,R.dc DESC,R.gk ASC LIMIT 12;
SQL
      ;;
    *) die "unknown Emby dashboard arm: $case_id/$arm" ;;
  esac
}

dashboard_query_core() { sed '$s/;[[:space:]]*$//' "$1"; }
dashboard_render_core() { family_render "$1" "$2" | sed '$s/;[[:space:]]*$//'; }

family_identity_sql() {
  local case_id=$1 vendor_file=$2 candidate_file=$3
  local comparison candidate_kind require_canonical vendor_core reference_core candidate_core

  if [ "$case_id" = movies-latest ] || [ "$case_id" = mixed-latest ]; then
    write_readonly_preamble
    cat <<SQL
WITH vendor_identity AS MATERIALIZED ($(dashboard_query_core "$vendor_file")),
candidate_identity AS MATERIALIZED ($(dashboard_query_core "$candidate_file")),
metrics AS (
 SELECT (SELECT count(*) FROM vendor_identity) vendor_rows,
        (SELECT count(*) FROM candidate_identity) candidate_rows,
        (SELECT count(*) FROM (SELECT * FROM vendor_identity EXCEPT SELECT * FROM candidate_identity)) missing_candidate,
        (SELECT count(*) FROM (SELECT * FROM candidate_identity EXCEPT SELECT * FROM vendor_identity)) extra_candidate
)
SELECT 'IDENTITY',CASE WHEN vendor_rows>0 AND vendor_rows=candidate_rows AND missing_candidate=0 AND extra_candidate=0 THEN 'PASS' ELSE 'FAIL' END,
       vendor_rows,candidate_rows,missing_candidate,extra_candidate FROM metrics;
SQL
    return 0
  fi

  case "$case_id" in
    episodes-latest)
      comparison=vendor_vs_shipped_two_index
      candidate_kind=shipped_two_index
      require_canonical=1
      vendor_core=$(dashboard_query_core "$vendor_file")
      reference_core=$vendor_core
      ;;
    episodes-latest-single-index)
      comparison=shipped_two_index_vs_single_index_antijoin
      candidate_kind=single_index_antijoin
      require_canonical=1
      vendor_core=$(dashboard_render_core episodes-latest vendor)
      reference_core=$(dashboard_query_core "$vendor_file")
      ;;
    *) die "unknown Emby dashboard identity case: $case_id" ;;
  esac
  candidate_core=$(dashboard_query_core "$candidate_file")

  write_readonly_preamble
  cat <<SQL
WITH eligible(id,gk,dc) AS MATERIALIZED (
  SELECT A.Id,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey),A.DateCreated
  FROM MediaItems AS A
  WHERE A.Type=8
    AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId=A.Id AND X.AncestorId IN ($DASHBOARD_ANCESTORS))
    AND NOT EXISTS (SELECT 1 FROM UserDatas AS U0 WHERE U0.UserDataKeyId=A.UserDataKeyId AND U0.UserId=$DASHBOARD_USER_ID AND U0.played<>0)
), eligible_groups(gk,maxdc) AS MATERIALIZED (
  SELECT gk,MAX(dc) FROM eligible GROUP BY gk
), eligible_maxima(gk,maxdc,canonical_id,tie_count) AS MATERIALIZED (
  SELECT G.gk,G.maxdc,MIN(E.id),count(*)
  FROM eligible_groups AS G JOIN eligible AS E ON E.gk IS G.gk AND E.dc IS G.maxdc
  GROUP BY G.gk,G.maxdc
), universe(group_count,expected_rows) AS (
  SELECT count(*),CASE WHEN count(*)<12 THEN count(*) ELSE 12 END FROM eligible_maxima
), canonical_boundary(has_cut,maxdc) AS (
  SELECT group_count>12,
         (SELECT maxdc FROM eligible_maxima ORDER BY (maxdc IS NULL) ASC,maxdc DESC LIMIT 1 OFFSET 11)
  FROM universe
), vendor_output AS MATERIALIZED ($vendor_core),
reference_output AS MATERIALIZED ($reference_core),
candidate_output AS MATERIALIZED ($candidate_core),
vendor_rows(id,gk,maxdc) AS MATERIALIZED (
  SELECT A.Id,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey),A.DateCreated
  FROM vendor_output AS O JOIN MediaItems AS A ON A.Id=O.Id
), reference_rows(id,gk,maxdc) AS MATERIALIZED (
  SELECT A.Id,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey),A.DateCreated
  FROM reference_output AS O JOIN MediaItems AS A ON A.Id=O.Id
), candidate_rows(id,gk,maxdc) AS MATERIALIZED (
  SELECT A.Id,coalesce(A.SeriesPresentationUniqueKey,A.PresentationUniqueKey),A.DateCreated
  FROM candidate_output AS O JOIN MediaItems AS A ON A.Id=O.Id
), arm_names(arm) AS (VALUES('vendor'),('reference'),('candidate')),
arm_rows(arm,id,gk,maxdc) AS (
  SELECT 'vendor',id,gk,maxdc FROM vendor_rows
  UNION ALL SELECT 'reference',id,gk,maxdc FROM reference_rows
  UNION ALL SELECT 'candidate',id,gk,maxdc FROM candidate_rows
), arm_quality(arm,row_count,group_count,invalid_max,missing_above_boundary,below_boundary) AS (
  SELECT N.arm,
         (SELECT count(*) FROM arm_rows AS R WHERE R.arm=N.arm),
         (SELECT count(*) FROM (SELECT R.gk FROM arm_rows AS R WHERE R.arm=N.arm GROUP BY R.gk)),
         (SELECT count(*) FROM arm_rows AS R
          WHERE R.arm=N.arm AND NOT EXISTS (
            SELECT 1 FROM eligible_maxima AS G
            WHERE G.gk IS R.gk AND G.maxdc IS R.maxdc
              AND EXISTS (SELECT 1 FROM eligible AS E WHERE E.id=R.id AND E.gk IS R.gk AND E.dc IS R.maxdc)
          )),
         (SELECT count(*) FROM eligible_maxima AS G CROSS JOIN canonical_boundary AS B
          WHERE B.has_cut=1
            AND ((B.maxdc IS NULL AND G.maxdc IS NOT NULL) OR (B.maxdc IS NOT NULL AND G.maxdc>B.maxdc))
            AND NOT EXISTS (SELECT 1 FROM arm_rows AS R WHERE R.arm=N.arm AND R.gk IS G.gk)),
         (SELECT count(*) FROM arm_rows AS R CROSS JOIN canonical_boundary AS B
          WHERE R.arm=N.arm AND B.has_cut=1 AND B.maxdc IS NOT NULL
            AND (R.maxdc IS NULL OR R.maxdc<B.maxdc))
  FROM arm_names AS N
), pair_metrics AS (
  SELECT
    (SELECT count(*) FROM reference_rows AS R CROSS JOIN canonical_boundary AS B
     WHERE NOT EXISTS (SELECT 1 FROM candidate_rows AS C WHERE C.gk IS R.gk)
       AND (B.has_cut=0 OR NOT (R.maxdc IS B.maxdc))) AS missing_candidate_group,
    (SELECT count(*) FROM candidate_rows AS C CROSS JOIN canonical_boundary AS B
     WHERE NOT EXISTS (SELECT 1 FROM reference_rows AS R WHERE R.gk IS C.gk)
       AND (B.has_cut=0 OR NOT (C.maxdc IS B.maxdc))) AS extra_candidate_group,
    (SELECT count(*) FROM reference_rows AS R CROSS JOIN canonical_boundary AS B
     WHERE B.has_cut=1 AND R.maxdc IS B.maxdc
       AND NOT EXISTS (SELECT 1 FROM candidate_rows AS C WHERE C.gk IS R.gk)) AS boundary_missing_candidate_group,
    (SELECT count(*) FROM candidate_rows AS C CROSS JOIN canonical_boundary AS B
     WHERE B.has_cut=1 AND C.maxdc IS B.maxdc
       AND NOT EXISTS (SELECT 1 FROM reference_rows AS R WHERE R.gk IS C.gk)) AS boundary_extra_candidate_group,
    (SELECT count(*) FROM reference_rows AS R JOIN candidate_rows AS C ON C.gk IS R.gk
     WHERE NOT (C.maxdc IS R.maxdc)) AS common_maxdc_mismatch,
    (SELECT count(*) FROM reference_rows AS R
       JOIN candidate_rows AS C ON C.gk IS R.gk
       JOIN eligible_maxima AS G ON G.gk IS R.gk
     WHERE G.tie_count=1 AND C.id<>R.id) AS non_tied_id_mismatch,
    (SELECT count(*) FROM reference_rows AS R
       JOIN candidate_rows AS C ON C.gk IS R.gk
       JOIN eligible_maxima AS G ON G.gk IS R.gk
     WHERE G.tie_count>1 AND C.id<>R.id) AS tied_id_divergence,
    (SELECT count(*) FROM candidate_rows AS C
     WHERE NOT EXISTS (SELECT 1 FROM eligible_maxima AS G WHERE G.gk IS C.gk AND G.canonical_id=C.id)) AS candidate_not_lower_id
), vendor_metrics AS (
  SELECT
    (SELECT count(*) FROM vendor_rows AS V CROSS JOIN canonical_boundary AS B
     WHERE NOT EXISTS (SELECT 1 FROM candidate_rows AS C WHERE C.gk IS V.gk)
       AND (B.has_cut=0 OR NOT (V.maxdc IS B.maxdc))) AS vendor_missing_candidate_group,
    (SELECT count(*) FROM candidate_rows AS C CROSS JOIN canonical_boundary AS B
     WHERE NOT EXISTS (SELECT 1 FROM vendor_rows AS V WHERE V.gk IS C.gk)
       AND (B.has_cut=0 OR NOT (C.maxdc IS B.maxdc))) AS vendor_extra_candidate_group,
    (SELECT count(*) FROM vendor_rows AS V CROSS JOIN canonical_boundary AS B
     WHERE B.has_cut=1 AND V.maxdc IS B.maxdc
       AND NOT EXISTS (SELECT 1 FROM candidate_rows AS C WHERE C.gk IS V.gk)) AS vendor_boundary_missing_candidate_group,
    (SELECT count(*) FROM candidate_rows AS C CROSS JOIN canonical_boundary AS B
     WHERE B.has_cut=1 AND C.maxdc IS B.maxdc
       AND NOT EXISTS (SELECT 1 FROM vendor_rows AS V WHERE V.gk IS C.gk)) AS vendor_boundary_extra_candidate_group,
    (SELECT count(*) FROM vendor_rows AS V JOIN candidate_rows AS C ON C.gk IS V.gk
     WHERE NOT (C.maxdc IS V.maxdc)) AS vendor_common_maxdc_mismatch,
    (SELECT count(*) FROM vendor_rows AS V
       JOIN candidate_rows AS C ON C.gk IS V.gk
       JOIN eligible_maxima AS G ON G.gk IS V.gk
     WHERE G.tie_count=1 AND C.id<>V.id) AS vendor_non_tied_id_mismatch,
    (SELECT count(*) FROM vendor_rows AS V
       JOIN candidate_rows AS C ON C.gk IS V.gk
       JOIN eligible_maxima AS G ON G.gk IS V.gk
     WHERE G.tie_count>1 AND C.id<>V.id) AS vendor_tied_id_divergence
), metrics AS (
  SELECT U.expected_rows,
         R.row_count AS reference_row_count,R.group_count AS reference_group_count,
         R.invalid_max AS reference_invalid_max,
         R.missing_above_boundary AS reference_missing_above,R.below_boundary AS reference_below_boundary,
         C.row_count AS candidate_row_count,C.group_count AS candidate_group_count,
         C.invalid_max AS candidate_invalid_max,
         C.missing_above_boundary AS candidate_missing_above,C.below_boundary AS candidate_below_boundary,
         V.row_count AS vendor_row_count,V.group_count AS vendor_group_count,V.invalid_max AS vendor_invalid_max,
         V.missing_above_boundary AS vendor_missing_above,V.below_boundary AS vendor_below_boundary,
         P.*,VM.*
  FROM universe AS U
  JOIN arm_quality AS R ON R.arm='reference'
  JOIN arm_quality AS C ON C.arm='candidate'
  JOIN arm_quality AS V ON V.arm='vendor'
  CROSS JOIN pair_metrics AS P CROSS JOIN vendor_metrics AS VM
)
SELECT 'IDENTITY',
       CASE WHEN expected_rows>0
                  AND reference_row_count=expected_rows AND candidate_row_count=expected_rows
                  AND reference_group_count=reference_row_count AND candidate_group_count=candidate_row_count
                  AND reference_invalid_max=0 AND candidate_invalid_max=0
                  AND reference_missing_above=0 AND candidate_missing_above=0
                  AND reference_below_boundary=0 AND candidate_below_boundary=0
                  AND missing_candidate_group=0 AND extra_candidate_group=0
                  AND common_maxdc_mismatch=0 AND non_tied_id_mismatch=0
                  AND ($require_canonical=0 OR candidate_not_lower_id=0)
            THEN 'PASS' ELSE 'FAIL' END,
       'comparison=$comparison','expected_rows='||expected_rows,
       'reference_rows='||reference_row_count,'candidate_rows='||candidate_row_count,
       'reference_groups='||reference_group_count,'candidate_groups='||candidate_group_count,
       'reference_invalid_max='||reference_invalid_max,'candidate_invalid_max='||candidate_invalid_max,
       'reference_rank_errors='||(reference_missing_above+reference_below_boundary),
       'candidate_rank_errors='||(candidate_missing_above+candidate_below_boundary),
       'normalized_missing='||missing_candidate_group,'normalized_extra='||extra_candidate_group,
       'boundary_missing='||boundary_missing_candidate_group,'boundary_extra='||boundary_extra_candidate_group,
       'common_maxdc_mismatch='||common_maxdc_mismatch,
       'non_tied_id_mismatch='||non_tied_id_mismatch,
       'tied_id_divergence='||tied_id_divergence,
       'candidate_not_lower_id='||candidate_not_lower_id
FROM metrics
UNION ALL
SELECT 'VENDOR_INFO','INFO','comparison=${candidate_kind}_vs_vendor',
       'expected_rows='||expected_rows,
       'vendor_rows='||vendor_row_count,'candidate_rows='||candidate_row_count,
       'vendor_groups='||vendor_group_count,'candidate_groups='||candidate_group_count,
       'vendor_invalid_max='||vendor_invalid_max,'candidate_invalid_max='||candidate_invalid_max,
       'vendor_rank_errors='||(vendor_missing_above+vendor_below_boundary),
       'candidate_rank_errors='||(candidate_missing_above+candidate_below_boundary),
       'normalized_missing='||vendor_missing_candidate_group,
       'normalized_extra='||vendor_extra_candidate_group,
       'boundary_missing='||vendor_boundary_missing_candidate_group,
       'boundary_extra='||vendor_boundary_extra_candidate_group,
       'common_maxdc_mismatch='||vendor_common_maxdc_mismatch,
       'non_tied_id_mismatch='||vendor_non_tied_id_mismatch,
       'tied_id_divergence='||vendor_tied_id_divergence,
       'candidate_not_lower_id='||candidate_not_lower_id
FROM metrics;
SQL
}

family_identity_pass() { awk -F '\t' '$1=="IDENTITY" && $2=="PASS"{ok=1} END{exit !ok}' "$2"; }
family_identity_detail() { tr '\n' ';' < "$2" | sed 's/;$//'; }
family_fixture_check() { :; }
