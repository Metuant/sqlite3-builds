#!/usr/bin/env bash
set -euo pipefail

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

fatal() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_contains() {
  local file=$1 needle=$2 label=$3
  if ! grep -Fq "${needle}" "${file}"; then
    echo "FATAL: ${label}: expected ${file} to contain ${needle}" >&2
    echo "---- ${file} ----" >&2
    cat "${file}" >&2
    exit 1
  fi
}

assert_status() {
  local actual=$1 expected=$2 label=$3
  if [[ "${actual}" -ne "${expected}" ]]; then
    echo "FATAL: ${label}: expected exit ${expected}, got ${actual}" >&2
    exit 1
  fi
}

assert_nonzero() {
  local actual=$1 label=$2
  if [[ "${actual}" -eq 0 ]]; then
    echo "FATAL: ${label}: expected nonzero exit, got 0" >&2
    exit 1
  fi
}

. "$(dirname "$0")/../scripts/lib/deploy-assertion.sh"
fatal() { echo "FATAL: $*" >&2; exit 1; }
sha256_of() { sha256sum "$1" | awk '{print $1}'; }
export -f emit_component_summary assert_pre_replacement_sha fatal sha256_of

write_file() {
  local path=$1 content=$2
  printf "%s" "${content}" > "${path}"
}

pre_file="${tmpdir}/pre.so"
window_file="${tmpdir}/window.so"
current_file="${tmpdir}/current.so"
unknown_file="${tmpdir}/unknown.so"
emby_only_file="${tmpdir}/emby-only.so"
bad_source_post_file="${tmpdir}/bad-source-post.so"
emby_window_file="${tmpdir}/emby-window.so"
emby_unknown_file="${tmpdir}/emby-unknown.so"
missing_icu_dir="${tmpdir}/missing-icu"
missing_icu_file="${missing_icu_dir}/libsqlite3.so"
icu_uc_file="${tmpdir}/libicuucplex.so.69"
icu_i18n_file="${tmpdir}/libicui18nplex.so.69"
icu_data_file="${tmpdir}/libicudataplex.so.69"

mkdir -p "${missing_icu_dir}"
write_file "${pre_file}" "managed pre image"
write_file "${window_file}" "managed prior release"
write_file "${current_file}" "managed current release"
write_file "${unknown_file}" "unmanaged runtime"
write_file "${emby_only_file}" "emby row only"
write_file "${bad_source_post_file}" "post row with non-release source"
write_file "${emby_window_file}" "emby prior release"
write_file "${emby_unknown_file}" "emby unmanaged"
write_file "${missing_icu_file}" "managed pre image with missing ICU"
write_file "${icu_uc_file}" "plex icu uc"
write_file "${icu_i18n_file}" "plex icu i18n"
write_file "${icu_data_file}" "plex icu data"

pre_sha="$(sha256_of "${pre_file}")"
window_sha="$(sha256_of "${window_file}")"
current_sha="$(sha256_of "${current_file}")"
emby_only_sha="$(sha256_of "${emby_only_file}")"
bad_source_post_sha="$(sha256_of "${bad_source_post_file}")"
emby_window_sha="$(sha256_of "${emby_window_file}")"
emby_unknown_sha="$(sha256_of "${emby_unknown_file}")"
missing_icu_sha="$(sha256_of "${missing_icu_file}")"
icu_uc_sha="$(sha256_of "${icu_uc_file}")"
icu_i18n_sha="$(sha256_of "${icu_i18n_file}")"
icu_data_sha="$(sha256_of "${icu_data_file}")"

manifest="${tmpdir}/SQLITE_LIBRARY_PINS"
cat > "${manifest}" <<EOF
# SQLITE_LIBRARY_PINS schema=2
version|2|managed_window|5|release_tag|2026.05.26-r1|generated_at|2026-05-26T18:44:37Z
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
pre|1|plex|test-arch|lscr.io/linuxserver/plex:fixture|lscr.io/linuxserver/plex@sha256:fixture|${pre_file}|runtime|${pre_sha}
pre|1|plex|test-arch|lscr.io/linuxserver/plex:fixture|lscr.io/linuxserver/plex@sha256:fixture|${missing_icu_file}|runtime|${missing_icu_sha}
pre|1|plex|test-arch|lscr.io/linuxserver/plex:fixture|lscr.io/linuxserver/plex@sha256:fixture|${icu_uc_file}|runtime|${icu_uc_sha}
pre|1|plex|test-arch|lscr.io/linuxserver/plex:fixture|lscr.io/linuxserver/plex@sha256:fixture|${icu_i18n_file}|runtime|${icu_i18n_sha}
pre|1|plex|test-arch|lscr.io/linuxserver/plex:fixture|lscr.io/linuxserver/plex@sha256:fixture|${icu_data_file}|runtime|${icu_data_sha}
post|1|plex|test-arch|release|2026.05.25-r1|${window_file}|sqlite-2026.05.25-r1-library-plex-test-arch.so|${window_sha}
post|1|plex|test-arch|release|2026.05.26-r1|${current_file}|sqlite-2026.05.26-r1-library-plex-test-arch.so|${current_sha}
post|1|plex|test-arch|runtime|2026.05.20-r1|${bad_source_post_file}|sqlite-2026.05.20-r1-library-plex-test-arch.so|${bad_source_post_sha}
pre|1|emby|test-arch|lscr.io/linuxserver/emby:fixture|lscr.io/linuxserver/emby@sha256:fixture|${emby_only_file}|runtime|${emby_only_sha}
post|1|emby|test-arch|release|2026.05.25-r1|${emby_window_file}|sqlite-2026.05.25-r1-library-test-arch.so|${emby_window_sha}
EOF

run_assertion() {
  local target=$1 file=$2 stdout_file=$3 stderr_file=$4
  set +e
  MANIFEST_FILE="${manifest}" \
    TARGET_NAME="${target}" \
    TARGET_FILE="${file}" \
    bash -c '
      set -euo pipefail
      curl() { echo "FATAL: network should not be used by assertion test" >&2; exit 97; }
      SQLITE_LIBRARY_PINS="$(cat "${MANIFEST_FILE}")"
      SQLITE_LIBRARY_PINS_SCHEMA_VERSION="2"
      SQLITE_LIBRARY_PINS_RELEASE_TAG="2026.05.26-r1"
      SQLITE_LIBRARY_PINS_WINDOW_N="5"
      arch="test-arch"
      TAG="2026.05.26-r1"
      target_path="${TARGET_FILE}"
      assert_pre_replacement_sha "${TARGET_NAME}"
    ' >"${stdout_file}" 2>"${stderr_file}"
  local status=$?
  set -e
  run_status="${status}"
  return 0
}

stdout="${tmpdir}/stdout"
stderr="${tmpdir}/stderr"

run_assertion plex "${unknown_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_nonzero "${status}" "unknown SHA"
assert_contains "${stderr}" "unknown pre-replacement SHA" "unknown SHA stderr"
assert_contains "${stderr}" "schema=2" "unknown SHA schema stderr"
assert_contains "${stderr}" "release=2026.05.26-r1" "unknown SHA release stderr"
assert_contains "${stderr}" "window=5" "unknown SHA window stderr"
assert_contains "${stderr}" "expected pre/post SHAs:" "unknown SHA expected-list stderr"

run_assertion plex "${pre_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "pre-row SHA"
assert_contains "${stdout}" "Verified managed pre-replacement" "pre-row stdout"
assert_contains "${stdout}" "Verified managed Plex ICU libicuucplex.so.69" "pre-row ICU uc stdout"
assert_contains "${stdout}" "Verified managed Plex ICU libicui18nplex.so.69" "pre-row ICU i18n stdout"
assert_contains "${stdout}" "Verified managed Plex ICU libicudataplex.so.69" "pre-row ICU data stdout"

run_assertion plex "${window_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "window post-row SHA"
assert_contains "${stdout}" "Verified managed prior-release" "window post-row stdout"

run_assertion plex "${current_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "current-release post SHA"
assert_contains "${stdout}" "already deployed" "current-release post stdout"
assert_contains "${stdout}" "Deployed sqlite=3.53.1 mimalloc=3.3.2 icu=69.1 (plex variant)" "current-release summary stdout"

run_assertion plex "${emby_only_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_nonzero "${status}" "kind isolation"
assert_contains "${stderr}" "FATAL" "kind isolation stderr"

run_assertion plex "${bad_source_post_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_nonzero "${status}" "non-release post source rejection"
assert_contains "${stderr}" "FATAL" "non-release post source stderr"

run_assertion plex "${missing_icu_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_nonzero "${status}" "missing Plex ICU sibling"
assert_contains "${stderr}" "missing Plex ICU runtime file" "missing Plex ICU sibling stderr"

run_assertion jellyfin "${unknown_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "jellyfin skip"
assert_contains "${stderr}" "WARNING" "jellyfin skip stderr"

run_assertion emby "${emby_unknown_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_nonzero "${status}" "emby unknown SHA"
assert_contains "${stderr}" "unknown pre-replacement SHA" "emby unknown SHA stderr"

run_assertion emby "${emby_only_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "emby pre-row SHA"
assert_contains "${stdout}" "Verified managed pre-replacement" "emby pre-row stdout"

run_assertion emby "${emby_window_file}" "${stdout}" "${stderr}"
status="${run_status}"
assert_status "${status}" 0 "emby window post-row SHA"
assert_contains "${stdout}" "Verified managed prior-release" "emby window post-row stdout"

echo "deploy assertion tests passed"
