#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmpdir="$(mktemp -d /tmp/assemble-library-pins-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/assemble-library-pins-test-$$; echo /tmp/assemble-library-pins-test-$$; })"
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

run_assemble() {
  local release_tag=$1 sha256sums=$2 output_file=$3 stderr_file=$4
  shift 4
  set +e
  bash "${repo_root}/tools/assemble-library-pins.sh" \
    --release-tag "${release_tag}" \
    --sha256sums "${sha256sums}" \
    "$@" >"${output_file}" 2>"${stderr_file}"
  local status=$?
  set -e
  run_status="${status}"
}

write_pre_fragment() {
  local pre_file=$1 plex_sha=$2 emby_sha=$3
  cat > "${pre_file}" <<EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_sha}
pre|1|emby|linux-x86_64-v2|lscr.io/linuxserver/emby:test|lscr.io/linuxserver/emby@sha256:test|/app/emby/lib/libsqlite3.so.3.49.2|runtime|${emby_sha}
EOF
}

stdout="${tmpdir}/stdout"
stderr="${tmpdir}/stderr"
sha256sums="${tmpdir}/SHA256SUMS"
pre_fragment="${tmpdir}/SQLITE_LIBRARY_PINS_PRE"

plex_pre_sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
emby_pre_sha="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
plex_post_sha="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
generic_post_sha="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
prior_a_sha="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
prior_b_sha="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

cat > "${sha256sums}" <<EOF
1111111111111111111111111111111111111111111111111111111111111111  sqlite-vtag-cli-linux-x86_64-v2.tar.gz
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
${generic_post_sha}  sqlite-vtag-library-linux-x86_64-v2.so
EOF
write_pre_fragment "${pre_fragment}" "${plex_pre_sha}" "${emby_pre_sha}"
run_assemble "vtag" "${sha256sums}" "${stdout}" "${stderr}" "${pre_fragment}"
assert_status "${run_status}" 0 "happy path assemble"
assert_contains "${stdout}" "version|1|managed_window|5|release_tag|vtag|generated_at|vtag" "happy path metadata"
assert_contains "${stdout}" "post|1|plex|linux-x86_64-v2|release|vtag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vtag-library-plex-linux-x86_64-v2.so|${plex_post_sha}" "happy path plex post row"
assert_contains "${stdout}" "post|1|emby|linux-x86_64-v2|release|vtag|/app/emby/lib/libsqlite3.so.3.49.2|sqlite-vtag-library-linux-x86_64-v2.so|${generic_post_sha}" "happy path emby post row"

prior_pins="${tmpdir}/prior-pins.txt"
cat > "${prior_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|vprior|generated_at|vprior
post|1|plex|linux-x86_64-v2|release|vprior|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vprior-library-plex-linux-x86_64-v2.so|${prior_a_sha}
post|1|plex|linux-x86_64-v2|release|vother|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vother-library-plex-linux-x86_64-v2.so|${prior_b_sha}
EOF
run_assemble "vtag" "${sha256sums}" "${stdout}" "${stderr}" --prior "${prior_pins}" "${pre_fragment}"
assert_status "${run_status}" 0 "prior carryover"
assert_contains "${stdout}" "post|1|plex|linux-x86_64-v2|release|vprior|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vprior-library-plex-linux-x86_64-v2.so|${prior_a_sha}" "prior owning-release carryover"
assert_not_contains "${stdout}" "sqlite-vother-library-plex-linux-x86_64-v2.so" "prior non-owning-release filtering"

count_pre="${tmpdir}/count-pre.txt"
cat > "${count_pre}" <<EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
pre|1|emby|linux-x86_64-v2|lscr.io/linuxserver/emby:test|lscr.io/linuxserver/emby@sha256:test|/app/emby/lib/libsqlite3.so.3.49.2|runtime|${emby_pre_sha}
EOF
count_sums="${tmpdir}/count-SHA256SUMS"
cat > "${count_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF
run_assemble "vtag" "${count_sums}" "${stdout}" "${stderr}" "${count_pre}"
assert_nonzero "${run_status}" "count parity guard"
assert_contains "${stderr}" "does not match" "count parity stderr"

bad_schema_pre="${tmpdir}/bad-schema-pre.txt"
cat > "${bad_schema_pre}" <<EOF
pre|2|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
EOF
bad_schema_sums="${tmpdir}/bad-schema-SHA256SUMS"
cat > "${bad_schema_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-cli-linux-x86_64-v2.tar.gz
EOF
run_assemble "vtag" "${bad_schema_sums}" "${stdout}" "${stderr}" "${bad_schema_pre}"
assert_nonzero "${run_status}" "validate_row schema"
assert_contains "${stderr}" "unsupported schema" "validate_row schema stderr"

bad_sha_pre="${tmpdir}/bad-sha-pre.txt"
cat > "${bad_sha_pre}" <<EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|notahexsha
EOF
bad_sha_sums="${tmpdir}/bad-sha-SHA256SUMS"
cat > "${bad_sha_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF
run_assemble "vtag" "${bad_sha_sums}" "${stdout}" "${stderr}" "${bad_sha_pre}"
assert_nonzero "${run_status}" "validate_row sha256"
assert_contains "${stderr}" "invalid sha256" "validate_row sha256 stderr"

invalid_kind_pre="${tmpdir}/invalid-kind-pre.txt"
cat > "${invalid_kind_pre}" <<EOF
garbage|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
EOF
invalid_kind_sums="${tmpdir}/invalid-kind-SHA256SUMS"
cat > "${invalid_kind_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF
run_assemble "vtag" "${invalid_kind_sums}" "${stdout}" "${stderr}" "${invalid_kind_pre}"
assert_nonzero "${run_status}" "S6_invalid_kind"
assert_contains "${stderr}" "invalid kind" "S6_invalid_kind stderr"

empty_target_pre="${tmpdir}/empty-target-pre.txt"
cat > "${empty_target_pre}" <<EOF
pre|1||linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
EOF
empty_target_sums="${tmpdir}/empty-target-SHA256SUMS"
cat > "${empty_target_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF
run_assemble "vtag" "${empty_target_sums}" "${stdout}" "${stderr}" "${empty_target_pre}"
assert_nonzero "${run_status}" "S7_empty_target"
assert_contains "${stderr}" "empty target" "S7_empty_target stderr"

invalid_target_path_pre="${tmpdir}/invalid-target-path-pre.txt"
cat > "${invalid_target_path_pre}" <<EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|no-leading-slash|runtime|${plex_pre_sha}
EOF
invalid_target_path_sums="${tmpdir}/invalid-target-path-SHA256SUMS"
cat > "${invalid_target_path_sums}" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF
run_assemble "vtag" "${invalid_target_path_sums}" "${stdout}" "${stderr}" "${invalid_target_path_pre}"
assert_nonzero "${run_status}" "S8_invalid_target_path"
assert_contains "${stderr}" "invalid target_path" "S8_invalid_target_path stderr"

echo "assemble library pins tests passed"
