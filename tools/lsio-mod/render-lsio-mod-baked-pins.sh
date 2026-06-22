#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage:
  render-lsio-mod-baked-pins.sh --mod plex|emby --release-tag TAG --generated-at ISO \
    --sha256sums SHA256SUMS --pre-fragment FILE --output FILE \
    --artifact ARCH:COMPAT_GROUP:NAME:PATH:TARGET_PATH [--artifact ...]

Input pin files default to pins/*. Override with RENDER_LSIO_MOD_* paths for
render-specific fixtures.
USAGE
}

fatal() {
  echo "FATAL: $*" >&2
  exit 1
}

require_value() {
  local flag=$1
  if [ "$#" -lt 2 ] || [ -z "${2:-}" ] || [[ "${2:-}" == --* ]]; then
    echo "FATAL: missing value for $flag" >&2
    exit 2
  fi
}

valid_sha256() {
  [[ "$1" =~ ^[0-9A-Fa-f]{64}$ ]]
}

valid_hex_context() {
  [[ "$1" =~ ^[0-9A-Fa-f]{32}$ ]]
}

lower_hex() {
  local value
  value="$1"
  value="${value//A/a}"
  value="${value//B/b}"
  value="${value//C/c}"
  value="${value//D/d}"
  value="${value//E/e}"
  value="${value//F/f}"
  printf '%s\n' "$value"
}

pool_site_context_matches_write() {
  local original_hex patched_hex delta index original_byte patched_byte diff_count diff_index
  original_hex="$(lower_hex "$1")"
  patched_hex="$(lower_hex "$2")"
  delta="$3"
  diff_count=0
  diff_index=-1
  for index in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    original_byte="${original_hex:$((index * 2)):2}"
    patched_byte="${patched_hex:$((index * 2)):2}"
    if [ "$original_byte" != "$patched_byte" ]; then
      diff_count=$((diff_count + 1))
      diff_index="$index"
    fi
  done
  [ "$diff_count" -eq 1 ] && [ "$diff_index" -eq "$delta" ]
}

valid_arch() {
  case "$1" in
    linux-x86_64-v2|linux-x86_64-v3|linux-arm64) return 0 ;;
    *) return 1 ;;
  esac
}

valid_mod() {
  case "$1" in
    plex|emby) return 0 ;;
    *) return 1 ;;
  esac
}

valid_status() {
  case "$1" in
    supported|unsupported|pending_pool_review) return 0 ;;
    *) return 1 ;;
  esac
}

valid_detect_role() {
  local row_mod role
  row_mod="$1"
  role="$2"
  case "${row_mod}|${role}" in
    emby\|emby_deps|emby\|emby_dll) return 0 ;;
    plex\|plex_pms:pristine|plex\|plex_pms:patched|plex\|plex_scanner:pristine|plex\|plex_scanner:patched) return 0 ;;
    *) return 1 ;;
  esac
}

valid_pre_role() {
  local row_mod role
  row_mod="$1"
  role="$2"
  case "${row_mod}|${role}" in
    emby\|target_sqlite|plex\|target_sqlite) return 0 ;;
    plex\|plex_icu_linked:libicuucplex.so.69) return 0 ;;
    plex\|plex_icu_linked:libicui18nplex.so.69) return 0 ;;
    plex\|plex_icu_linked:libicudataplex.so.69) return 0 ;;
    *) return 1 ;;
  esac
}

valid_plex_icu_soname() {
  case "$1" in
    libicuucplex.so.69|libicui18nplex.so.69|libicudataplex.so.69) return 0 ;;
    *) return 1 ;;
  esac
}

valid_emby_target_path() {
  [[ "$1" =~ ^/app/emby/lib/libsqlite3\.so\.[0-9]+[.][0-9]+[.][0-9]+$ ]]
}

require_nonempty_fields() {
  local context value
  context="$1"
  shift
  for value in "$@"; do
    [ -n "$value" ] || fatal "malformed ${context}: empty field"
  done
}

mod=""
release_tag=""
generated_at=""
sha256sums=""
output=""
pre_fragments=()
artifacts=()

runtime_support="${RENDER_LSIO_MOD_RUNTIME_SUPPORT:-pins/runtime-support.tsv}"
compat_groups="${RENDER_LSIO_MOD_COMPAT_GROUPS:-pins/library-compat-groups.tsv}"
runtime_baselines="${RENDER_LSIO_MOD_RUNTIME_BASELINES:-pins/runtime-baselines.tsv}"
pool_sites_file="${RENDER_LSIO_MOD_POOL_SITES:-pins/plex-pool-patch-sites.tsv}"
pool_reviews_file="${RENDER_LSIO_MOD_POOL_REVIEWS:-pins/plex-pool-patch-reviews.tsv}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --mod) require_value "$1" "${2:-}"; mod="$2"; shift 2 ;;
    --release-tag) require_value "$1" "${2:-}"; release_tag="$2"; shift 2 ;;
    --generated-at) require_value "$1" "${2:-}"; generated_at="$2"; shift 2 ;;
    --sha256sums) require_value "$1" "${2:-}"; sha256sums="$2"; shift 2 ;;
    --pre-fragment) require_value "$1" "${2:-}"; pre_fragments+=("$2"); shift 2 ;;
    --pool-baselines) require_value "$1" "${2:-}"; shift 2 ;;
    --output) require_value "$1" "${2:-}"; output="$2"; shift 2 ;;
    --artifact) require_value "$1" "${2:-}"; artifacts+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

valid_mod "$mod" || { usage; exit 2; }
[ -n "$release_tag" ] || { echo "FATAL: missing --release-tag" >&2; exit 2; }
[ -n "$generated_at" ] || { echo "FATAL: missing --generated-at" >&2; exit 2; }
[ -f "$sha256sums" ] || { echo "FATAL: missing --sha256sums file" >&2; exit 2; }
[ -n "$output" ] || { echo "FATAL: missing --output" >&2; exit 2; }
[ "${#artifacts[@]}" -gt 0 ] || fatal "at least one --artifact is required"

for required_pin in "$runtime_support" "$compat_groups" "$runtime_baselines" "$pool_sites_file" "$pool_reviews_file"; do
  [ -f "$required_pin" ] || fatal "missing M3 prerequisite: $required_pin"
done

declare -a support_ids support_groups artifact_arches pool_records
declare -A group_mod group_artifact_stem
declare -A support_image_ref support_compat support_review supported_server support_group_seen
declare -A artifact_name artifact_path artifact_target artifact_sha
declare -A artifact_arch_seen artifact_group_has_v2 artifact_group_has_v3
declare -A detect_line detect_sha detect_path pre_path pre_sha pre_digest pre_image_ref
declare -A baseline_pre_sha_by_fragment_key pre_digest_override icu_runtime_sha review_approved review_count
declare -A pool_count group_arch_target_path group_arch_target_warned group_arch_artifact_sha

load_compat_groups() {
  local line_no compat row_mod build_variant artifact_stem sqlite_guard icu_version icu_sha linked smoke extra
  line_no=0
  while IFS=$'\t' read -r compat row_mod build_variant artifact_stem sqlite_guard icu_version icu_sha linked smoke extra || [ -n "${compat:-}" ]; do
    line_no=$((line_no + 1))
    compat="${compat%$'\r'}"
    case "$compat" in
      ""|\#*) continue ;;
      compat_group) continue ;;
    esac
    [ -z "${extra:-}" ] || fatal "malformed compat group row at line $line_no"
    require_nonempty_fields "compat group row at line $line_no" "$compat" "$row_mod" "$build_variant" "$artifact_stem" "$sqlite_guard" "$icu_version" "$icu_sha" "$linked" "$smoke"
    valid_mod "$row_mod" || fatal "invalid compat group mod at line $line_no: $row_mod"
    [ -z "${group_mod[$compat]:-}" ] || fatal "duplicate compat group: $compat"
    group_mod[$compat]="$row_mod"
    group_artifact_stem[$compat]="$artifact_stem"
    if [ "$row_mod" = "plex" ]; then
      [ "$linked" = "libicuucplex.so.69;libicui18nplex.so.69;libicudataplex.so.69" ] || fatal "unsupported Plex ICU soname set for $compat: $linked"
    fi
  done < "$compat_groups"
}

load_support() {
  local line_no row_mod server_id image_ref compat status review_ref notes extra
  line_no=0
  while IFS=$'\t' read -r row_mod server_id image_ref compat status review_ref notes extra || [ -n "${row_mod:-}" ]; do
    line_no=$((line_no + 1))
    row_mod="${row_mod%$'\r'}"
    case "$row_mod" in
      ""|\#*) continue ;;
      mod) continue ;;
    esac
    [ -z "${extra:-}" ] || fatal "malformed runtime support row at line $line_no"
    require_nonempty_fields "runtime support row at line $line_no" "$row_mod" "$server_id" "$image_ref" "$compat" "$status" "$review_ref" "$notes"
    valid_mod "$row_mod" || fatal "invalid runtime support mod at line $line_no: $row_mod"
    valid_status "$status" || fatal "invalid runtime support status at line $line_no: $status"
    [ -n "${group_mod[$compat]:-}" ] || fatal "runtime support references unknown compat group at line $line_no: $compat"
    [ "${group_mod[$compat]}" = "$row_mod" ] || fatal "runtime support compat group/mod mismatch at line $line_no: $compat"
    [ "$row_mod" = "$mod" ] || continue
    [ "$status" = "supported" ] || continue
    [ -z "${supported_server[$server_id]:-}" ] || fatal "duplicate supported server_id: $server_id"
    if [ -z "${support_group_seen[$compat]:-}" ]; then
      support_group_seen[$compat]=1
      support_groups+=("$compat")
    fi
    supported_server[$server_id]=1
    support_ids+=("$server_id")
    support_image_ref[$server_id]="$image_ref"
    support_compat[$server_id]="$compat"
    support_review[$server_id]="$review_ref"
  done < "$runtime_support"
  [ "${#support_ids[@]}" -gt 0 ] || fatal "no supported runtime rows for mod: $mod"
}

parse_artifacts() {
  local artifact arch compat name path target_path extra key
  for artifact in "${artifacts[@]}"; do
    IFS=':' read -r arch compat name path target_path extra <<EOF_ART
$artifact
EOF_ART
    [ -z "${extra:-}" ] || fatal "malformed --artifact: $artifact"
    require_nonempty_fields "--artifact" "$arch" "$compat" "$name" "$path" "$target_path"
    valid_arch "$arch" || fatal "unsupported artifact arch: $arch"
    [ -n "${group_mod[$compat]:-}" ] || fatal "artifact references unknown compat group: $compat"
    [ "${group_mod[$compat]}" = "$mod" ] || fatal "artifact compat group/mod mismatch: $compat"
    [ -n "${support_group_seen[$compat]:-}" ] || fatal "artifact compat group not used by support window: $compat"
    key="$compat|$arch"
    [ -z "${artifact_path[$key]:-}" ] || fatal "duplicate --artifact for compat_group=$compat arch=$arch"
    if [ -z "${artifact_arch_seen[$arch]:-}" ]; then
      artifact_arch_seen[$arch]=1
      artifact_arches+=("$arch")
    fi
    artifact_name[$key]="$name"
    artifact_path[$key]="$path"
    artifact_target[$key]="$target_path"
    case "$arch" in
      linux-x86_64-v2) artifact_group_has_v2[$compat]=1 ;;
      linux-x86_64-v3) artifact_group_has_v3[$compat]=1 ;;
    esac
  done
  for compat in "${support_groups[@]}"; do
    if [ "${artifact_group_has_v2[$compat]:-0}" -ne "${artifact_group_has_v3[$compat]:-0}" ]; then
      if [ "${artifact_group_has_v2[$compat]:-0}" -eq 0 ]; then
        fatal "missing paired x86_64 artifact class for compat_group=$compat: linux-x86_64-v2"
      fi
      fatal "missing paired x86_64 artifact class for compat_group=$compat: linux-x86_64-v3"
    fi
  done
}

lookup_sha_candidate() {
  local candidate
  candidate="$1"
  awk -v name="$candidate" '$2 == name { print $1; found=1; exit } END { if (!found) exit 1 }' "$sha256sums"
}

lookup_artifact_sha() {
  local arch compat name stem canonical candidate sha seen_candidates
  arch="$1"
  compat="$2"
  name="${artifact_name[$compat|$arch]}"
  stem="${group_artifact_stem[$compat]#library-}"
  canonical="sqlite-${release_tag}-library-${stem}-${arch}.so"
  seen_candidates=""

  for candidate in "$canonical" "$name"; do
    case " $seen_candidates " in
      *" $candidate "*) continue ;;
    esac
    seen_candidates="${seen_candidates} ${candidate}"
    if sha="$(lookup_sha_candidate "$candidate")"; then
      valid_sha256 "$sha" || fatal "invalid SHA256SUMS entry for $candidate: $sha"
      printf '%s\n' "$sha"
      return 0
    fi
  done

  fatal "missing SHA256SUMS entry for $canonical"
}

validate_artifacts() {
  local compat arch path sha actual v2_key v3_key
  for compat in "${support_groups[@]}"; do
    for arch in "${artifact_arches[@]}"; do
      path="${artifact_path[$compat|$arch]:-}"
      [ -n "$path" ] || fatal "missing --artifact for compat_group=$compat arch=$arch"
      [ -f "$path" ] || fatal "missing sqlite artifact: $path"
      sha="$(lookup_artifact_sha "$arch" "$compat")"
      actual="$(sha256sum "$path" | awk '{print $1}')"
      [ "$actual" = "$sha" ] || fatal "artifact SHA mismatch for $path: expected $sha got $actual"
      artifact_sha["$arch|$compat"]="$sha"
    done

    v2_key="linux-x86_64-v2|$compat"
    v3_key="linux-x86_64-v3|$compat"
    if [ -n "${artifact_sha[$v2_key]:-}" ] && [ -n "${artifact_sha[$v3_key]:-}" ]; then
      [ "${artifact_name[$compat|linux-x86_64-v2]}" != "${artifact_name[$compat|linux-x86_64-v3]}" ] || fatal "x86_64 artifact classes alias the same artifact name"
      [ "${artifact_path[$compat|linux-x86_64-v2]}" != "${artifact_path[$compat|linux-x86_64-v3]}" ] || fatal "x86_64 artifact classes alias the same artifact path"
      [ "${artifact_sha[$v2_key]}" != "${artifact_sha[$v3_key]}" ] || fatal "x86_64 artifact classes must have distinct SHA256SUMS entries"
    fi
  done
}

load_runtime_baselines() {
  local line_no kind row_mod server_id arch role image_ref image_digest file_path compat icu_role soname sha extra
  local key fragment_key required_path
  line_no=0
  while IFS=$'\t' read -r kind row_mod server_id arch role image_ref image_digest file_path compat icu_role soname sha extra || [ -n "${kind:-}" ]; do
    line_no=$((line_no + 1))
    kind="${kind%$'\r'}"
    case "$kind" in
      ""|\#*) continue ;;
      kind) continue ;;
    esac
    [ -z "${extra:-}" ] || fatal "malformed runtime baseline row at line $line_no"
    require_nonempty_fields "runtime baseline row at line $line_no" "$kind" "$row_mod" "$server_id" "$arch" "$role" "$image_ref" "$image_digest" "$file_path" "$compat" "$icu_role" "$soname" "$sha"
    valid_arch "$arch" || fatal "invalid runtime baseline arch at line $line_no: $arch"
    valid_sha256 "$sha" || fatal "invalid runtime baseline SHA at line $line_no: $sha"
    case "$kind" in
      detect)
        [ "$row_mod" = "$mod" ] || continue
        [ -n "${supported_server[$server_id]:-}" ] || continue
        valid_detect_role "$row_mod" "$role" || fatal "invalid detect role at line $line_no: $role"
        key="$server_id|$arch|$role"
        [ -z "${detect_line[$key]:-}" ] || fatal "duplicate detect baseline: $key"
        detect_line[$key]="detect|1|$row_mod|$server_id|$arch|$role|$file_path|$sha"
        detect_sha[$key]="$sha"
        detect_path[$key]="$file_path"
        ;;
      pre)
        [ "$row_mod" = "$mod" ] || continue
        [ -n "${supported_server[$server_id]:-}" ] || continue
        valid_pre_role "$row_mod" "$role" || fatal "invalid pre role at line $line_no: $role"
        [ "$image_ref" = "${support_image_ref[$server_id]}" ] || fatal "pre image_ref mismatch for $server_id $arch $role"
        key="$server_id|$arch|$role"
        [ -z "${pre_path[$key]:-}" ] || fatal "duplicate pre baseline: $key"
        if [ "$role" = "target_sqlite" ]; then
          if [ "$row_mod" = "plex" ] && [ "$file_path" != "/usr/lib/plexmediaserver/lib/libsqlite3.so" ]; then
            fatal "invalid Plex target_sqlite path for $server_id $arch: $file_path"
          fi
          if [ "$row_mod" = "emby" ] && ! valid_emby_target_path "$file_path"; then
            fatal "invalid Emby target_sqlite path for $server_id $arch: $file_path"
          fi
        else
          soname="${role#plex_icu_linked:}"
          valid_plex_icu_soname "$soname" || fatal "invalid Plex ICU soname at line $line_no: $soname"
          required_path="/usr/lib/plexmediaserver/lib/${soname}"
          [ "$file_path" = "$required_path" ] || fatal "invalid Plex ICU path at line $line_no: $file_path"
        fi
        pre_path[$key]="$file_path"
        pre_sha[$key]="$sha"
        pre_digest[$key]="$image_digest"
        pre_image_ref[$key]="$image_ref"
        fragment_key="$row_mod|$arch|$image_ref|$file_path"
        if [ -n "${baseline_pre_sha_by_fragment_key[$fragment_key]:-}" ] && [ "${baseline_pre_sha_by_fragment_key[$fragment_key]}" != "$sha" ]; then
          fatal "ambiguous pre fragment baseline key: $fragment_key"
        fi
        baseline_pre_sha_by_fragment_key[$fragment_key]="$sha"
        ;;
      icu-runtime)
        [ "$mod" = "plex" ] || continue
        [ "$row_mod" = "-" ] || fatal "icu-runtime row has unexpected mod at line $line_no: $row_mod"
        [ "$icu_role" = "linked" ] || continue
        [ -n "${group_mod[$compat]:-}" ] || fatal "icu-runtime references unknown compat group at line $line_no: $compat"
        [ "${group_mod[$compat]}" = "plex" ] || continue
        valid_plex_icu_soname "$soname" || fatal "invalid icu-runtime soname at line $line_no: $soname"
        key="$compat|$arch|$soname"
        [ -z "${icu_runtime_sha[$key]:-}" ] || fatal "duplicate icu-runtime baseline: $key"
        icu_runtime_sha[$key]="$sha"
        ;;
      *)
        fatal "unknown runtime baseline kind at line $line_no: $kind"
        ;;
    esac
  done < "$runtime_baselines"
}

load_pre_fragments() {
  local fragment line_no line kind version row_mod arch image_ref image_digest file_path source_kind sha extra key expected
  for fragment in "${pre_fragments[@]}"; do
    [ -f "$fragment" ] || fatal "missing pre fragment: $fragment"
    line_no=0
    while IFS= read -r line || [ -n "$line" ]; do
      line_no=$((line_no + 1))
      line="${line%$'\r'}"
      case "$line" in
        ""|\#*) continue ;;
      esac
      IFS='|' read -r kind version row_mod arch image_ref image_digest file_path source_kind sha extra <<EOF_PRE
$line
EOF_PRE
      [ "$kind" = "pre" ] || continue
      [ -z "${extra:-}" ] || fatal "malformed pre fragment row at $fragment:$line_no"
      require_nonempty_fields "pre fragment row at $fragment:$line_no" "$version" "$row_mod" "$arch" "$image_ref" "$image_digest" "$file_path" "$source_kind" "$sha"
      [ "$version" = "1" ] || fatal "unsupported pre fragment version at $fragment:$line_no: $version"
      [ "$source_kind" = "runtime" ] || fatal "unsupported pre fragment source kind at $fragment:$line_no: $source_kind"
      valid_arch "$arch" || fatal "invalid pre fragment arch at $fragment:$line_no: $arch"
      valid_sha256 "$sha" || fatal "invalid pre fragment SHA: $line"
      [ "$row_mod" = "$mod" ] || continue
      key="$row_mod|$arch|$image_ref|$file_path"
      expected="${baseline_pre_sha_by_fragment_key[$key]:-}"
      [ -n "$expected" ] || fatal "pre fragment row does not match supported baseline: mod=$row_mod arch=$arch image_ref=$image_ref path=$file_path"
      [ "$sha" = "$expected" ] || fatal "pre fragment SHA mismatch for $key: expected $expected got $sha"
      pre_digest_override[$key]="$image_digest"
    done < "$fragment"
  done
}

load_pool_reviews() {
  local line_no server_id arch binary_path label offset write_seek original_hex patched_hex review_ref reviewer status extra key reviewer_key
  local site_line_no site_server_id site_arch site_binary_path site_baseline_sha site_label site_offset site_write_seek site_original_hex site_patched_hex site_extra
  local site_key site_tuple_digest
  local -A site_identity_seen site_tuple_seen
  [ "$mod" = "plex" ] || return 0
  site_line_no=0
  while IFS=$'\t' read -r site_server_id site_arch site_binary_path site_baseline_sha site_label site_offset site_write_seek site_original_hex site_patched_hex site_extra || [ -n "${site_server_id:-}" ]; do
    site_line_no=$((site_line_no + 1))
    site_server_id="${site_server_id%$'\r'}"
    case "$site_server_id" in
      ""|\#*) continue ;;
      server_id) continue ;;
    esac
    [ -z "${site_extra:-}" ] || fatal "malformed pool-site row at line $site_line_no"
    require_nonempty_fields "pool-site row at line $site_line_no" "$site_server_id" "$site_arch" "$site_binary_path" "$site_baseline_sha" "$site_label" "$site_offset" "$site_write_seek" "$site_original_hex" "$site_patched_hex"
    [ -n "${supported_server[$site_server_id]:-}" ] || continue
    site_key="$site_server_id|$site_arch|$site_binary_path|$site_label|$site_offset"
    [ -z "${site_identity_seen[$site_key]:-}" ] || fatal "duplicate pool-site review key at line $site_line_no: $site_server_id $site_arch $site_binary_path $site_label $site_offset"
    site_identity_seen[$site_key]=1
    site_tuple_digest="$(printf '%s' "$site_server_id|$site_arch|$site_binary_path|$site_label|$site_offset|$site_write_seek|$site_original_hex|$site_patched_hex" | sha256sum | awk '{print $1}')"
    site_tuple_seen[$site_tuple_digest]=1
  done < "$pool_sites_file"

  line_no=0
  while IFS=$'\t' read -r server_id arch binary_path label offset write_seek original_hex patched_hex review_ref reviewer status extra || [ -n "${server_id:-}" ]; do
    line_no=$((line_no + 1))
    server_id="${server_id%$'\r'}"
    case "$server_id" in
      ""|\#*) continue ;;
      server_id) continue ;;
    esac
    [ -z "${extra:-}" ] || fatal "malformed pool review row at line $line_no"
    require_nonempty_fields "pool review row at line $line_no" "$server_id" "$arch" "$binary_path" "$label" "$offset" "$write_seek" "$original_hex" "$patched_hex" "$review_ref" "$reviewer" "$status"
    valid_arch "$arch" || fatal "invalid pool review arch at line $line_no: $arch"
    [[ "$offset" =~ ^[0-9]+$ ]] || fatal "invalid pool review offset at line $line_no: $offset"
    [[ "$write_seek" =~ ^[0-9]+$ ]] || fatal "invalid pool review write_seek at line $line_no: $write_seek"
    valid_hex_context "$original_hex" || fatal "invalid pool review original_hex at line $line_no: $original_hex"
    valid_hex_context "$patched_hex" || fatal "invalid pool review patched_hex at line $line_no: $patched_hex"
    [ -n "${supported_server[$server_id]:-}" ] || continue
    site_key="$server_id|$arch|$binary_path|$label|$offset"
    [ -n "${site_identity_seen[$site_key]:-}" ] || fatal "pool review has no pool-site row for $server_id $arch $binary_path $label $offset"
    [ "$review_ref" = "${support_review[$server_id]}" ] || fatal "pool review_ref mismatch for $server_id $arch $binary_path $label $offset"
    [ "$status" = "approved" ] || fatal "pool review not approved for $server_id $arch $binary_path $label $offset"
    site_tuple_digest="$(printf '%s' "$server_id|$arch|$binary_path|$label|$offset|$write_seek|$original_hex|$patched_hex" | sha256sum | awk '{print $1}')"
    [ -n "${site_tuple_seen[$site_tuple_digest]:-}" ] || continue
    key="$server_id|$arch|$binary_path|$site_tuple_digest"
    reviewer_key="$key|$reviewer"
    if [ -z "${review_approved[$reviewer_key]:-}" ]; then
      review_approved[$reviewer_key]=1
      review_count[$key]=$((${review_count[$key]:-0} + 1))
    fi
  done < "$pool_reviews_file"
}

load_pool_sites() {
  local line_no server_id arch binary_path baseline_sha label offset write_seek original_hex patched_hex extra
  local key review_key site_tuple_digest pms_key scanner_key pristine_sha
  local offset_num write_seek_num delta
  [ "$mod" = "plex" ] || return 0
  line_no=0
  while IFS=$'\t' read -r server_id arch binary_path baseline_sha label offset write_seek original_hex patched_hex extra || [ -n "${server_id:-}" ]; do
    line_no=$((line_no + 1))
    server_id="${server_id%$'\r'}"
    case "$server_id" in
      ""|\#*) continue ;;
      server_id) continue ;;
    esac
    [ -z "${extra:-}" ] || fatal "malformed pool-site row at line $line_no"
    require_nonempty_fields "pool-site row at line $line_no" "$server_id" "$arch" "$binary_path" "$baseline_sha" "$label" "$offset" "$write_seek" "$original_hex" "$patched_hex"
    valid_arch "$arch" || fatal "invalid pool-site arch at line $line_no: $arch"
    valid_sha256 "$baseline_sha" || fatal "invalid pool-site baseline SHA at line $line_no: $baseline_sha"
    [[ "$offset" =~ ^[0-9]+$ ]] || fatal "invalid pool-site offset at line $line_no: $offset"
    [[ "$write_seek" =~ ^[0-9]+$ ]] || fatal "invalid pool-site write_seek at line $line_no: $write_seek"
    valid_hex_context "$original_hex" || fatal "invalid pool-site original_hex at line $line_no: $original_hex"
    valid_hex_context "$patched_hex" || fatal "invalid pool-site patched_hex at line $line_no: $patched_hex"
    offset_num=$((10#$offset))
    write_seek_num=$((10#$write_seek))
    delta=$((write_seek_num - offset_num))
    [ "$delta" -ge 0 ] || fatal "pool-site write_seek before offset at line $line_no"
    [ "$delta" -le 15 ] || fatal "pool-site write_seek out of range at line $line_no"
    pool_site_context_matches_write "$original_hex" "$patched_hex" "$delta" || fatal "pool-site patched_hex must differ from original_hex at exactly write_seek-offset byte at line $line_no"
    [ -n "${supported_server[$server_id]:-}" ] || continue
    pms_key="$server_id|$arch|plex_pms:pristine"
    scanner_key="$server_id|$arch|plex_scanner:pristine"
    pristine_sha=""
    if [ "$binary_path" = "${detect_path[$pms_key]:-}" ]; then
      pristine_sha="${detect_sha[$pms_key]:-}"
    elif [ "$binary_path" = "${detect_path[$scanner_key]:-}" ]; then
      pristine_sha="${detect_sha[$scanner_key]:-}"
    fi
    [ -n "$pristine_sha" ] || fatal "pool-site row does not match a pristine detector at line $line_no"
    [ "$baseline_sha" = "$pristine_sha" ] || fatal "pool-site baseline mismatch for $server_id $arch $binary_path"
    site_tuple_digest="$(printf '%s' "$server_id|$arch|$binary_path|$label|$offset|$write_seek|$original_hex|$patched_hex" | sha256sum | awk '{print $1}')"
    review_key="$server_id|$arch|$binary_path|$site_tuple_digest"
    [ "${review_count[$review_key]:-0}" -ge 2 ] || fatal "missing approved pool-site reviews for $server_id $arch $binary_path $label $offset"
    key="$server_id|$arch|$binary_path"
    pool_count[$key]=$((${pool_count[$key]:-0} + 1))
    pool_records+=("$server_id|$arch|$binary_path|$label|$offset|$write_seek|$original_hex|$patched_hex")
  done < "$pool_sites_file"
}

require_tuple() {
  local server_id arch compat role key target_key target_path soname pms_path scanner_path
  local artifact_key artifact_target_path artifact_group_sha first_target first_sha
  server_id="$1"
  arch="$2"
  compat="${support_compat[$server_id]}"
  case "$mod" in
    emby)
      for role in emby_deps emby_dll; do
        key="$server_id|$arch|$role"
        [ -n "${detect_line[$key]:-}" ] || fatal "missing detector baseline: server_id=$server_id arch=$arch role=$role"
      done
      ;;
    plex)
      for role in plex_pms:pristine plex_pms:patched plex_scanner:pristine plex_scanner:patched; do
        key="$server_id|$arch|$role"
        [ -n "${detect_line[$key]:-}" ] || fatal "missing detector baseline: server_id=$server_id arch=$arch role=$role"
      done
      ;;
  esac

  target_key="$server_id|$arch|target_sqlite"
  target_path="${pre_path[$target_key]:-}"
  [ -n "$target_path" ] || fatal "missing target_sqlite baseline: server_id=$server_id arch=$arch"
  artifact_key="$compat|$arch"
  artifact_target_path="${artifact_target[$artifact_key]:-}"
  artifact_group_sha="${artifact_sha[$arch|$compat]:-}"
  [ -n "$artifact_target_path" ] || fatal "missing artifact target path: compat_group=$compat arch=$arch"
  [ -n "$artifact_group_sha" ] || fatal "missing artifact SHA: compat_group=$compat arch=$arch"
  if [ "$artifact_target_path" != "$target_path" ]; then
    echo "WARN: artifact target path differs from baseline for $server_id $arch compat_group=$compat: artifact=$artifact_target_path baseline=$target_path; emitting baseline target_path" >&2
  fi
  first_target="${group_arch_target_path[$artifact_key]:-}"
  first_sha="${group_arch_artifact_sha[$artifact_key]:-}"
  if [ -z "$first_target" ]; then
    group_arch_target_path[$artifact_key]="$target_path"
    group_arch_artifact_sha[$artifact_key]="$artifact_group_sha"
  elif [ "$first_target" != "$target_path" ]; then
    [ "$first_sha" = "$artifact_group_sha" ] || fatal "artifact SHA mismatch across divergent target paths: compat_group=$compat arch=$arch first_sha=$first_sha current_sha=$artifact_group_sha"
    if [ -z "${group_arch_target_warned[$artifact_key]:-}" ]; then
      echo "WARN: divergent target paths share one artifact: compat_group=$compat arch=$arch first=$first_target current=$target_path sha=$artifact_group_sha" >&2
      group_arch_target_warned[$artifact_key]=1
    fi
  fi

  if [ "$mod" = "plex" ]; then
    for soname in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
      role="plex_icu_linked:${soname}"
      key="$server_id|$arch|$role"
      [ -n "${pre_path[$key]:-}" ] || fatal "missing Plex ICU baseline: server_id=$server_id arch=$arch soname=$soname"
      [ "${icu_runtime_sha[$compat|$arch|$soname]:-}" = "${pre_sha[$key]}" ] || fatal "Plex ICU runtime baseline mismatch: compat=$compat arch=$arch soname=$soname"
    done
    pms_path="${detect_path[$server_id|$arch|plex_pms:pristine]}"
    scanner_path="${detect_path[$server_id|$arch|plex_scanner:pristine]}"
    [ "${pool_count[$server_id|$arch|$pms_path]:-0}" -gt 0 ] || fatal "missing Plex pool-site rows: server_id=$server_id arch=$arch path=$pms_path"
    [ "${pool_count[$server_id|$arch|$scanner_path]:-0}" -gt 0 ] || fatal "missing Plex pool-site rows: server_id=$server_id arch=$arch path=$scanner_path"
  fi
}

emit_pre_row() {
  local server_id arch role key image_ref digest file_path sha override_key
  server_id="$1"
  arch="$2"
  role="$3"
  key="$server_id|$arch|$role"
  image_ref="${pre_image_ref[$key]}"
  digest="${pre_digest[$key]}"
  file_path="${pre_path[$key]}"
  sha="${pre_sha[$key]}"
  override_key="$mod|$arch|$image_ref|$file_path"
  if [ -n "${pre_digest_override[$override_key]:-}" ]; then
    digest="${pre_digest_override[$override_key]}"
  fi
  printf 'pre|2|%s|%s|%s|%s|%s|%s|%s|%s\n' "$mod" "$server_id" "$arch" "$role" "$image_ref" "$digest" "$file_path" "$sha"
}

emit_pool_rows() {
  local server_id arch record rec_server rec_arch binary_path label offset write_seek original_hex patched_hex
  server_id="$1"
  arch="$2"
  for record in "${pool_records[@]}"; do
    IFS='|' read -r rec_server rec_arch binary_path label offset write_seek original_hex patched_hex <<EOF_POOL
$record
EOF_POOL
    [ "$rec_server" = "$server_id" ] || continue
    [ "$rec_arch" = "$arch" ] || continue
    printf 'pool-site|1|plex|%s|%s|%s|%s|%s|%s|%s|%s\n' "$server_id" "$arch" "$binary_path" "$label" "$offset" "$write_seek" "$original_hex" "$patched_hex"
  done
}

load_compat_groups
load_support
parse_artifacts
load_runtime_baselines
load_pre_fragments
load_pool_reviews
load_pool_sites
validate_artifacts

tmp="${output}.tmp.$$"
trap 'rm -f "$tmp"' EXIT

{
  printf '# baked-pins schema=3\n'
  printf 'meta|3|release_tag|%s|generated_at|%s\n' "$release_tag" "$generated_at"

  for server_id in "${support_ids[@]}"; do
    compat="${support_compat[$server_id]}"
    for arch in "${artifact_arches[@]}"; do
      require_tuple "$server_id" "$arch"
      case "$mod" in
        plex)
          printf '%s\n' "${detect_line[$server_id|$arch|plex_pms:pristine]}"
          printf '%s\n' "${detect_line[$server_id|$arch|plex_pms:patched]}"
          printf '%s\n' "${detect_line[$server_id|$arch|plex_scanner:pristine]}"
          printf '%s\n' "${detect_line[$server_id|$arch|plex_scanner:patched]}"
          ;;
        emby)
          printf '%s\n' "${detect_line[$server_id|$arch|emby_deps]}"
          printf '%s\n' "${detect_line[$server_id|$arch|emby_dll]}"
          ;;
      esac

      printf 'artifact|1|%s|%s|%s|%s|artifacts/%s/%s/libsqlite3.so|%s|%s\n' \
        "$mod" "$server_id" "$arch" "$compat" "$arch" "$compat" "${pre_path[$server_id|$arch|target_sqlite]}" "${artifact_sha[$arch|$compat]}"

      emit_pre_row "$server_id" "$arch" target_sqlite
      if [ "$mod" = "plex" ]; then
        emit_pre_row "$server_id" "$arch" plex_icu_linked:libicuucplex.so.69
        emit_pre_row "$server_id" "$arch" plex_icu_linked:libicui18nplex.so.69
        emit_pre_row "$server_id" "$arch" plex_icu_linked:libicudataplex.so.69
        emit_pool_rows "$server_id" "$arch"
      fi
    done
  done
} > "$tmp"

mv -f "$tmp" "$output"
trap - EXIT
