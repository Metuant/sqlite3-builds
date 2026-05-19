#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmpdir="$(mktemp -d /tmp/regen-deploy-pins-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/regen-deploy-pins-test-$$; echo /tmp/regen-deploy-pins-test-$$; })"
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
  local pins_file=$1 output_file=$2 release_tag=$3 stderr_file=$4 template_path=${5:-}
  set +e
  if [[ -n "${template_path}" ]]; then
    bash "${repo_root}/tools/regen-deploy-pins.sh" "${pins_file}" "${output_file}" "${release_tag}" "${template_path}" >"${tmpdir}/regen.stdout" 2>"${stderr_file}"
  else
    bash "${repo_root}/tools/regen-deploy-pins.sh" "${pins_file}" "${output_file}" "${release_tag}" >"${tmpdir}/regen.stdout" 2>"${stderr_file}"
  fi
  local status=$?
  set -e
  run_status="${status}"
}

write_happy_pins() {
  local pins_file=$1 tag=$2 pre_sha=$3 post_sha=$4
  cat > "${pins_file}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|${tag}|generated_at|${tag}
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|${tag}|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-${tag}-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
}

stderr="${tmpdir}/stderr"
pins="${tmpdir}/pins.txt"
rendered="${tmpdir}/rendered.sh"
pre_sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
post_sha="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

write_happy_pins "${pins}" "test-tag" "${pre_sha}" "${post_sha}"
run_regen "${pins}" "${rendered}" "test-tag" "${stderr}"
assert_status "${run_status}" 0 "happy path regen"
bash -n "${rendered}"
assert_not_contains "${rendered}" "__SQLITE_LIBRARY_PINS_BLOCK__" "happy path pins placeholder"
assert_not_contains "${rendered}" "__ASSERT_PRE_REPLACEMENT_SHA__" "happy path assertion placeholder"
assert_contains "${rendered}" 'SQLITE_LIBRARY_PINS_RELEASE_TAG="test-tag"' "happy path release tag"
assert_contains "${rendered}" 'SQLITE_LIBRARY_PINS_GENERATED_AT="test-tag"' "happy path generated_at"
assert_contains "${rendered}" 'assert_pre_replacement_sha()' "happy path assertion function"

tmpl_no_assert="${tmpdir}/tmpl-no-assert.sh"
grep -vF "__ASSERT_PRE_REPLACEMENT_SHA__" "${repo_root}/scripts/update-sqlite-library.sh.template" > "${tmpl_no_assert}"
run_regen "${pins}" "${tmpdir}/missing-assert.sh" "test-tag" "${stderr}" "${tmpl_no_assert}"
assert_nonzero "${run_status}" "missing assert placeholder"
assert_contains "${stderr}" "FATAL: template missing or has duplicate __ASSERT_PRE_REPLACEMENT_SHA__ placeholder" "missing assert placeholder stderr"

placeholder_pins="${tmpdir}/placeholder-pins.txt"
cat > "${placeholder_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|test-tag|generated_at|test-tag
pre|1|plex|linux-x86_64-v2|__SQLITE_LIBRARY_PINS_BLOCK__|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${placeholder_pins}" "${tmpdir}/placeholder.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "pins placeholder collision"
assert_contains "${stderr}" "FATAL: pins manifest contains placeholder token" "pins placeholder collision stderr"

eof_pins="${tmpdir}/eof-pins.txt"
cat > "${eof_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|test-tag|generated_at|test-tag
PINS_EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${eof_pins}" "${tmpdir}/eof.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "PINS_EOF collision"
assert_contains "${stderr}" "FATAL: pins manifest contains literal PINS_EOF line" "PINS_EOF collision stderr"

snap_pins="${tmpdir}/snap-pins.txt"
awk '
  $0 == "SQLITE_LIBRARY_PINS=$(cat <<'\''PINS_EOF'\''" { in_pins = 1; next }
  in_pins && $0 == "PINS_EOF" { exit }
  in_pins { print }
' "${repo_root}/scripts/update-sqlite-library.sh" > "${snap_pins}"
run_regen "${snap_pins}" "${tmpdir}/roundtrip.sh" "unreleased" "${stderr}"
assert_status "${run_status}" 0 "round-trip regen"
diff "${tmpdir}/roundtrip.sh" "${repo_root}/scripts/update-sqlite-library.sh"

rendered2="${tmpdir}/rendered-atomic.sh"
rendered2_first="${tmpdir}/rendered-atomic-first.sh"
atomic_pins="${tmpdir}/atomic-pins.txt"
write_happy_pins "${atomic_pins}" "atomic-tag" "${pre_sha}" "${post_sha}"
run_regen "${atomic_pins}" "${rendered2}" "atomic-tag" "${stderr}"
assert_status "${run_status}" 0 "S6_atomic_write first render"
if [[ ! -s "${rendered2}" ]]; then
  fatal "S6_atomic_write first render: expected non-empty ${rendered2}"
fi
if compgen -G "${rendered2}.tmp.*" >/dev/null; then
  fatal "S6_atomic_write first render: leaked tmp file for ${rendered2}"
fi
cp "${rendered2}" "${rendered2_first}"
run_regen "${atomic_pins}" "${rendered2}" "atomic-tag" "${stderr}"
assert_status "${run_status}" 0 "S6_atomic_write second render"
if [[ ! -s "${rendered2}" ]]; then
  fatal "S6_atomic_write second render: expected non-empty ${rendered2}"
fi
if compgen -G "${rendered2}.tmp.*" >/dev/null; then
  fatal "S6_atomic_write second render: leaked tmp file for ${rendered2}"
fi
diff "${rendered2_first}" "${rendered2}"

echo "regen deploy pins tests passed"
