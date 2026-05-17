#!/bin/bash
set -euo pipefail

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: missing required command: $1" >&2
    echo "       LSIO startup hooks require this deploy helper's preflighted tool surface" >&2
    exit 1
  }
}
# WHY: LSIO startup images expose a small tool surface; every non-shell tool
# used below is checked before deployment starts.
for cmd in curl sha256sum awk tar uname grep sed; do require_cmd "$cmd"; done

TAG="${TAG:-}"
if [[ -z "${TAG}" ]]; then
  # WHY: The release redirect avoids jq and API-token dependencies inside LSIO
  # startup hooks.
  redirect_url="$(curl -fsSL -o /dev/null -w '%{url_effective}' \
    'https://github.com/darthShadow/sqlite3-builds/releases/latest' || true)"
  TAG="$(printf '%s' "${redirect_url}" | sed -n 's|.*/releases/tag/\([^/]*\)$|\1|p')"
fi
if [[ -z "${TAG}" ]]; then
  echo "ERROR: could not resolve release tag (no releases yet? network failure?)" >&2
  echo "       set TAG=<release-tag> explicitly to override auto-detection" >&2
  exit 1
fi
echo "Deploying release tag: ${TAG}"

tmpdir=""
target_path=""
archive=""

usage() { echo "Usage: $0 {emby|jellyfin|plex}" >&2; }

fatal() { echo "FATAL: $*" >&2; exit 1; }

cleanup() { [[ -z "${tmpdir}" || ! -d "${tmpdir}" ]] || rm -rf "${tmpdir}"; }

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

resolve_arch() {
  local machine flags=""

  machine="$(uname -m)"
  case "${machine}" in
    x86_64)
      if [[ -r /proc/cpuinfo ]]; then
        flags="$(awk -F: '/^flags[[:space:]]*:/{print $2; exit}' /proc/cpuinfo)"
      fi
      has_all_flags() { local needed="$1"; for f in $needed; do printf " %s " "$flags" | grep -q " $f " || return 1; done; }

      # WHY: Strict psABI checks select the fastest compatible artifact and
      # avoid illegal-instruction failures on older hosts.
      if has_all_flags "cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3 avx avx2 bmi1 bmi2 f16c fma lzcnt movbe osxsave"; then
        echo "linux-x86_64-v3"
      elif has_all_flags "cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3"; then
        echo "linux-x86_64-v2"
      else
        fatal "unsupported CPU: does not meet x86-64-v2 baseline (need cx16, lahf_lm, popcnt, sse3, sse4_1, sse4_2, ssse3). flags: $flags"
      fi
      ;;
    aarch64 | arm64)
      echo "linux-arm64"
      ;;
    *)
      fatal "unsupported architecture: ${machine}"
      ;;
  esac
}

lookup_sha() {
  local sums_file=$1 name=$2
  local sha
  if ! sha="$(awk -v name="${name}" '$2 == name { print $1; found = 1; exit } END { if (!found) exit 1 }' "${sums_file}")"; then
    fatal "missing SHA256SUMS entry for ${name}"
  fi
  if [[ ! "${sha}" =~ ^[0-9A-Fa-f]{64}$ ]]; then
    fatal "invalid SHA256SUMS entry for ${name}: ${sha}"
  fi
  echo "${sha}"
}

verify_sha() {
  local file=$1 expected=$2 label=$3
  local actual
  actual="$(sha256_of "${file}")"
  if [[ "${actual}" != "${expected}" ]]; then
    fatal "SHA-256 mismatch for ${label}: expected ${expected}, got ${actual}"
  fi
}

rollback_target() {
  local backup_path=$1
  if cp -f "${backup_path}" "${target_path}"; then
    echo "Rolled back ${target_path} from ${backup_path}." >&2
  else
    echo "FATAL: rollback failed; inspect ${target_path} and ${backup_path}." >&2
  fi
}

deploy() {
  local release_base_url="https://github.com/darthShadow/sqlite3-builds/releases/download/${TAG}"
  local sha256sums_url="${release_base_url}/SHA256SUMS"
  local synthetic_so sums_file archive_path archive_url
  local extract_dir extracted_so current_sha backup_path post_copy_sha
  local members tmp_path tmp_sha member_check target
  local expected_archive_sha expected_so_sha

  target=$1
  synthetic_so="${archive%.tar.gz}.so"
  tmpdir="$(mktemp -d)"
  trap cleanup EXIT
  sums_file="${tmpdir}/SHA256SUMS"
  archive_path="${tmpdir}/${archive}"
  archive_url="${release_base_url}/${archive}"
  extract_dir="${tmpdir}/extract"
  extracted_so="${extract_dir}/libsqlite3.so"
  backup_path="${target_path}.backup"
  echo "Downloading SHA256SUMS from ${sha256sums_url}."
  curl -fsSL "${sha256sums_url}" -o "${sums_file}"
  expected_archive_sha="$(lookup_sha "${sums_file}" "${archive}")"
  expected_so_sha="$(lookup_sha "${sums_file}" "${synthetic_so}")"

  if [[ -f "${target_path}" ]]; then
    current_sha="$(sha256_of "${target_path}")"
    if [[ "${current_sha}" == "${expected_so_sha}" ]]; then
      echo "${target_path} already deployed at ${expected_so_sha}."
      exit 0
    fi
  else
    # WHY: Shared LSIO hooks can run in images where another app's bundled
    # SQLite path is not present.
    echo "WARNING: target path does not exist: ${target_path}" >&2
    echo "WARNING: skipping ${target} deploy (LSIO image may have moved/removed bundled SQLite)" >&2
    exit 0
  fi
  echo "Downloading ${archive_url}."
  curl -fsSL "${archive_url}" -o "${archive_path}"
  verify_sha "${archive_path}" "${expected_archive_sha}" "${archive}"

  mkdir -p "${extract_dir}"
  if ! members="$(tar -tzf "${archive_path}")"; then
    fatal "archive listing failed"
  fi
  # WHY: Validate tar members before extraction because this runs during
  # container startup with the target filesystem mounted.
  if printf "%s\n" "${members}" | grep -qE '^/|(^|/)\.\.($|/)'; then
    fatal "archive contains unsafe member paths"
  fi
  if ! printf "%s\n" "${members}" | grep -qE '^(\./)?libsqlite3\.so$'; then
    fatal "archive missing libsqlite3.so or contains extra members"
  fi
  member_check="$(
    printf "%s\n" "${members}" | awk '
      /^\.\/?$/ { next }
      /^(\.\/)?libsqlite3\.so$/ { found++; next }
      { extra++ }
      END { if (found == 1 && extra == 0) print "ok"; else print "bad" }
    '
  )"
  if [[ "${member_check}" != "ok" ]]; then
    fatal "archive missing libsqlite3.so or contains extra members"
  fi
  tar -xzf "${archive_path}" -C "${extract_dir}" "./libsqlite3.so" 2>/dev/null || tar -xzf "${archive_path}" -C "${extract_dir}" "libsqlite3.so"
  if [[ ! -f "${extracted_so}" ]]; then
    fatal "archive ${archive} did not contain libsqlite3.so"
  fi
  verify_sha "${extracted_so}" "${expected_so_sha}" "${synthetic_so}"

  cp -f "${target_path}" "${backup_path}"
  echo "Copying ${extracted_so} to ${target_path}."
  tmp_path="${target_path}.deploying.$$"
  # WHY: Same-directory temp plus mv keeps replacement atomic on the target filesystem.
  cp -f "${extracted_so}" "${tmp_path}"
  tmp_sha="$(sha256sum "${tmp_path}" | awk '{print $1}')"
  if [[ "${tmp_sha}" != "${expected_so_sha}" ]]; then
    echo "FATAL: tmp_path SHA mismatch before mv for ${tmp_path}: expected ${expected_so_sha}, got ${tmp_sha}" >&2
    rm -f "${tmp_path}"
    rollback_target "${backup_path}"
    exit 1
  fi
  if ! mv -f "${tmp_path}" "${target_path}"; then
    echo "FATAL: failed to move ${tmp_path} to ${target_path}" >&2
    rollback_target "${backup_path}"
    exit 1
  fi
  post_copy_sha="$(sha256_of "${target_path}")"
  if [[ "${post_copy_sha}" != "${expected_so_sha}" ]]; then
    echo "FATAL: post-copy SHA-256 mismatch for ${target_path}: expected ${expected_so_sha}, got ${post_copy_sha}" >&2
    rollback_target "${backup_path}"
    exit 1
  fi
  echo "Deployed ${target_path} at ${expected_so_sha}."
}

if [[ $# -ne 1 ]]; then
  usage
  exit 2
fi
arch="$(resolve_arch)"
case "$1" in
  emby)
    archive="sqlite-${TAG}-library-${arch}.tar.gz"
    target_path="/app/emby/lib/libsqlite3.so.3.49.2"
    ;;
  # NOTE: Jellyfin support is dormant in this cycle.
  # Re-validate the current Jellyfin target contract before production use.
  jellyfin)
    archive="sqlite-${TAG}-library-${arch}.tar.gz"
    target_path="/usr/lib/jellyfin/bin/libe_sqlite3.so"
    ;;
  plex)
    archive="sqlite-${TAG}-library-plex-${arch}.tar.gz"
    target_path="/usr/lib/plexmediaserver/lib/libsqlite3.so"
    target_dir="${target_path%/*}"
    missing_icu=()
    for so in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
      [[ -f "${target_dir}/${so}" ]] || missing_icu+=("${so}")
    done
    if (( ${#missing_icu[@]} > 0 )); then
      echo "WARNING: missing Plex ICU runtime file(s) beside ${target_path}: ${missing_icu[*]}" >&2
      echo "WARNING: skipping plex deploy (Plex ICU 69 runtime files are required)" >&2
      exit 0
    fi
    ;;
  *)
    usage
    exit 2
    ;;
esac

deploy "$1"
