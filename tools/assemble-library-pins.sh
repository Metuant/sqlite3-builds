#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: assemble-library-pins.sh --release-tag TAG --sha256sums SHA256SUMS [--prior PRIOR_PINS]... PRE_FRAGMENT...

Emits SQLITE_LIBRARY_PINS on stdout.
EOF
}

fatal() { echo "FATAL: $*" >&2; exit 1; }

release_tag=""
sha256sums=""
prior_files=()
pre_files=()
SQLITE_VERSION_DOTTED="${SQLITE_VERSION_DOTTED:-}"
MIMALLOC_VERSION="${MIMALLOC_VERSION:-}"
ICU_VERSION="${ICU_VERSION:-}"
PLEX_IMAGE_TAG="${PLEX_IMAGE_TAG:-}"
EMBY_IMAGE_TAG="${EMBY_IMAGE_TAG:-}"
generated_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release-tag)
      [[ $# -ge 2 ]] || fatal "--release-tag requires a value"
      release_tag=$2
      shift 2
      ;;
    --sha256sums)
      [[ $# -ge 2 ]] || fatal "--sha256sums requires a value"
      sha256sums=$2
      shift 2
      ;;
    --prior)
      [[ $# -ge 2 ]] || fatal "--prior requires a value"
      prior_files+=("$2")
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --*)
      fatal "unknown argument: $1"
      ;;
    *)
      pre_files+=("$1")
      shift
      ;;
  esac
done

[[ -n "${release_tag}" ]] || fatal "missing --release-tag"
[[ -n "${sha256sums}" ]] || fatal "missing --sha256sums"
[[ -f "${sha256sums}" ]] || fatal "SHA256SUMS not found: ${sha256sums}"
[[ -n "${SQLITE_VERSION_DOTTED:-}" ]] || fatal "SQLITE_VERSION_DOTTED not set"
[[ -n "${MIMALLOC_VERSION:-}" ]] || fatal "MIMALLOC_VERSION not set"
[[ -n "${ICU_VERSION:-}" ]] || fatal "ICU_VERSION not set"
[[ -n "${PLEX_IMAGE_TAG:-}" ]] || fatal "PLEX_IMAGE_TAG not set"
[[ -n "${EMBY_IMAGE_TAG:-}" ]] || fatal "EMBY_IMAGE_TAG not set"
[[ -n "${generated_at}" ]] || fatal "generated_at capture returned empty"
for pre_file in "${pre_files[@]}"; do
  [[ -f "${pre_file}" ]] || fatal "pre fragment not found: ${pre_file}"
done

validate_row() {
  local row=$1 label=$2 fields status
  fields="$(printf "%s\n" "${row}" | awk -F'|' '{ print NF }')"
  [[ "${fields}" == "9" ]] || fatal "${label} row has ${fields} fields, expected 9: ${row}"
  set +e
  printf "%s\n" "${row}" | awk -F'|' '
    $1 != "pre" && $1 != "post" { exit 3 }
    $2 != "1" { exit 1 }
    $3 == "" { exit 4 }
    $7 !~ /^\// { exit 5 }
    $9 !~ /^[0-9A-Fa-f]{64}$/ { exit 2 }
  '
  status=$?
  set -e
  case "${status}" in
    0) ;;
    1) fatal "${label} row has unsupported schema: ${row}" ;;
    2) fatal "${label} row has invalid sha256: ${row}" ;;
    3) fatal "${label} row has invalid kind (expected pre|post): ${row}" ;;
    4) fatal "${label} row has empty target: ${row}" ;;
    5) fatal "${label} row has invalid target_path (must start with /): ${row}" ;;
    *) fatal "${label} row validation failed: ${row}" ;;
  esac
}

emit_current_post_rows() {
  local sums_file=$1 tag=$2
  tr -d '\r' < "${sums_file}" | awk -v tag="${tag}" '
    function emit(kind, target, arch, source, source_digest, target_path, artifact, sha) {
      printf "%s|1|%s|%s|%s|%s|%s|%s|%s\n", kind, target, arch, source, source_digest, target_path, artifact, sha
    }
    NF >= 2 {
      sha = $1
      artifact = $2
      if (sha !~ /^[0-9A-Fa-f]{64}$/) next
      plex_prefix = "sqlite-" tag "-library-plex-"
      generic_prefix = "sqlite-" tag "-library-"
      if (index(artifact, plex_prefix) == 1 && artifact ~ /\.so$/) {
        arch = artifact
        sub("^" plex_prefix, "", arch)
        sub(/\.so$/, "", arch)
        emit("post", "plex", arch, "release", tag, "/usr/lib/plexmediaserver/lib/libsqlite3.so", artifact, sha)
      } else if (index(artifact, generic_prefix) == 1 && artifact !~ /-library-plex-/ && artifact ~ /\.so$/) {
        arch = artifact
        sub("^" generic_prefix, "", arch)
        sub(/\.so$/, "", arch)
        emit("post", "emby", arch, "release", tag, "/app/emby/lib/libsqlite3.so.3.49.2", artifact, sha)
      }
    }
  '
}

emit_prior_post_rows() {
  local pins_file=$1 metadata_tag
  metadata_tag="$(
    tr -d '\r' < "${pins_file}" | awk -F'|' '$1 == "version" && ($2 == "1" || $2 == "2") && $5 == "release_tag" { print $6; exit }'
  )"
  [[ -n "${metadata_tag}" ]] || fatal "prior pins missing metadata release tag: ${pins_file}"
  tr -d '\r' < "${pins_file}" | awk -F'|' -v tag="${metadata_tag}" '
    /^#/ || NF == 0 { next }
    $1 == "post" && NF == 9 && $2 == "1" && $5 == "release" && $6 == tag { print }
  '
}

pre_target_arch_count="$(
  for pre_file in "${pre_files[@]}"; do
    tr -d '\r' < "${pre_file}"
  done | awk -F'|' '
    /^#/ || NF == 0 { next }
    $1 == "pre" && NF == 9 && $2 == "1" { seen[$3 "|" $4] = 1 }
    END { for (k in seen) count++; print count + 0 }
  '
)"
current_post_rows="$(emit_current_post_rows "${sha256sums}" "${release_tag}")"
current_post_count="$(printf "%s\n" "${current_post_rows}" | awk 'NF { count++ } END { print count + 0 }')"
if [[ "${current_post_count}" -ne "${pre_target_arch_count}" ]]; then
  fatal "current post row count ${current_post_count} does not match pre target/arch count ${pre_target_arch_count}"
fi

emit_component_rows() {
  printf "component|sqlite|%s\n" "${SQLITE_VERSION_DOTTED}"
  printf "component|mimalloc|%s\n" "${MIMALLOC_VERSION}"
  printf "component|icu|%s\n" "${ICU_VERSION}"
  printf "component|plex|%s\n" "${PLEX_IMAGE_TAG}"
  printf "component|emby|%s\n" "${EMBY_IMAGE_TAG}"
}

echo "# SQLITE_LIBRARY_PINS schema=2"
printf "version|2|managed_window|5|release_tag|%s|generated_at|%s\n" "${release_tag}" "${generated_at}"
emit_component_rows

for pre_file in "${pre_files[@]}"; do
  while IFS= read -r row; do
    [[ -n "${row}" && "${row}" != \#* ]] || continue
    validate_row "${row}" "${pre_file}"
    printf "%s\n" "${row}"
  done < <(tr -d '\r' < "${pre_file}")
done

while IFS= read -r row; do
  [[ -n "${row}" ]] || continue
  validate_row "${row}" "${sha256sums}"
  printf "%s\n" "${row}"
done <<< "${current_post_rows}"

for prior_file in "${prior_files[@]}"; do
  prior_rows=""
  [[ -f "${prior_file}" ]] || fatal "prior pins not found: ${prior_file}"
  prior_rows="$(emit_prior_post_rows "${prior_file}")" || {
    fatal "emit_prior_post_rows failed for ${prior_file}"
  }
  while IFS= read -r row; do
    [[ -n "${row}" ]] || continue
    validate_row "${row}" "${prior_file}"
    printf "%s\n" "${row}"
  done <<< "${prior_rows}"
done
