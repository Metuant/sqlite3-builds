#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmpdir="$(mktemp -d /tmp/regen-deploy-pins-custom-template-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/regen-deploy-pins-custom-template-test-$$; echo /tmp/regen-deploy-pins-custom-template-test-$$; })"
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

assert_not_contains() {
  local file=$1 needle=$2 label=$3
  if grep -Fq "${needle}" "${file}"; then
    echo "FATAL: ${label}: expected ${file} not to contain ${needle}" >&2
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

run_regen() {
  local tool_path=$1 pins_file=$2 output_file=$3 stderr_file=$4 template_path=$5
  set +e
  bash "${tool_path}" "${pins_file}" "${output_file}" custom-tag "${template_path}" >"${tmpdir}/stdout" 2>"${stderr_file}"
  local status=$?
  set -e
  run_status="${status}"
}

pins_file="${tmpdir}/pins.txt"
stderr="${tmpdir}/stderr"
rendered="${tmpdir}/rendered.sh"
external_template="${tmpdir}/update-sqlite-library.sh.template"
pre_sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
post_sha="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

cat > "${pins_file}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|custom-tag|generated_at|custom-tag
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|custom-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-custom-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF

cp "${repo_root}/scripts/update-sqlite-library.sh.template" "${external_template}"
run_regen "${repo_root}/tools/regen-deploy-pins.sh" "${pins_file}" "${rendered}" "${stderr}" "${external_template}"
assert_status "${run_status}" 0 "external template happy path"
assert_contains "${rendered}" 'assert_pre_replacement_sha()' "external template happy path assertion body"
assert_not_contains "${rendered}" "__ASSERT_PRE_REPLACEMENT_SHA__" "external template happy path assertion placeholder"

missing_placeholder_template="${tmpdir}/missing-placeholder.template"
grep -vF "__ASSERT_PRE_REPLACEMENT_SHA__" "${external_template}" > "${missing_placeholder_template}"
run_regen "${repo_root}/tools/regen-deploy-pins.sh" "${pins_file}" "${tmpdir}/missing.sh" "${stderr}" "${missing_placeholder_template}"
assert_nonzero "${run_status}" "missing assertion placeholder"
assert_contains "${stderr}" "FATAL: template missing or has duplicate __ASSERT_PRE_REPLACEMENT_SHA__ placeholder" "missing assertion placeholder stderr"

copied_tool_dir="${tmpdir}/copied-tool"
mkdir -p "${copied_tool_dir}"
cp "${repo_root}/tools/regen-deploy-pins.sh" "${copied_tool_dir}/regen-deploy-pins.sh"
chmod +x "${copied_tool_dir}/regen-deploy-pins.sh"
run_regen "${copied_tool_dir}/regen-deploy-pins.sh" "${pins_file}" "${tmpdir}/copied-tool-output.sh" "${stderr}" "${external_template}"
assert_nonzero "${run_status}" "missing deploy assertion source"
assert_contains "${stderr}" "FATAL: deploy-assertion.sh not found at" "missing deploy assertion source stderr"

echo "regen deploy pins custom template tests passed"
