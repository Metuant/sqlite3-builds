#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmpdir="$(mktemp -d /tmp/assemble-library-pins-fatal-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/assemble-library-pins-fatal-test-$$; echo /tmp/assemble-library-pins-fatal-test-$$; })"
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

run_assemble() {
  local output_file=$1 stderr_file=$2
  shift 2
  set +e
  SQLITE_VERSION_DOTTED="3.53.1" \
    MIMALLOC_VERSION="3.3.2" \
    ICU_VERSION="69.1" \
    PLEX_IMAGE_TAG="1.42.2" \
    EMBY_IMAGE_TAG="version-4.9.3.0" \
    bash "${repo_root}/tools/assemble-library-pins.sh" \
      --release-tag vtag \
      --sha256sums "${tmpdir}/SHA256SUMS" \
      "$@" \
      "${tmpdir}/pre.txt" >"${output_file}" 2>"${stderr_file}"
  local status=$?
  set -e
  run_status="${status}"
}

stdout="${tmpdir}/stdout"
stderr="${tmpdir}/stderr"
plex_pre_sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
plex_post_sha="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

cat > "${tmpdir}/pre.txt" <<EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${plex_pre_sha}
EOF

cat > "${tmpdir}/SHA256SUMS" <<EOF
${plex_post_sha}  sqlite-vtag-library-plex-linux-x86_64-v2.so
EOF

cat > "${tmpdir}/prior-missing-metadata.txt" <<EOF
post|1|plex|linux-x86_64-v2|release|vprior|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vprior-library-plex-linux-x86_64-v2.so|${plex_post_sha}
EOF
run_assemble "${stdout}" "${stderr}" --prior "${tmpdir}/prior-missing-metadata.txt"
assert_nonzero "${run_status}" "missing prior metadata"
assert_contains "${stderr}" "FATAL: prior pins missing metadata release tag" "missing prior metadata stderr"

cat > "${tmpdir}/prior-invalid-sha.txt" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|vprior|generated_at|vprior
post|1|plex|linux-x86_64-v2|release|vprior|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vprior-library-plex-linux-x86_64-v2.so|not-a-sha
EOF
run_assemble "${stdout}" "${stderr}" --prior "${tmpdir}/prior-invalid-sha.txt"
assert_nonzero "${run_status}" "invalid prior sha"
assert_contains "${stderr}" "invalid sha256" "invalid prior sha stderr"

cat > "${tmpdir}/prior-valid.txt" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|vprior|generated_at|vprior
post|1|plex|linux-x86_64-v2|release|vprior|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-vprior-library-plex-linux-x86_64-v2.so|${plex_post_sha}
EOF
run_assemble "${stdout}" "${stderr}" --prior "${tmpdir}/prior-valid.txt"
assert_status "${run_status}" 0 "valid prior carryover"
assert_contains "${stdout}" "sqlite-vprior-library-plex-linux-x86_64-v2.so" "valid prior carryover stdout"

echo "assemble library pins fatal propagation tests passed"
