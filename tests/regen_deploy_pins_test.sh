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
# SQLITE_LIBRARY_PINS schema=2
version|2|managed_window|5|release_tag|${tag}|generated_at|2026-05-26T18:44:37Z
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
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
[[ -x "${rendered}" ]] || fatal "happy path regen: expected rendered deploy script to be executable"
assert_not_contains "${rendered}" "__SQLITE_LIBRARY_PINS_BLOCK__" "happy path pins placeholder"
assert_not_contains "${rendered}" "__INJECTED_HELPERS__" "happy path helpers placeholder"
assert_contains "${rendered}" 'SQLITE_LIBRARY_PINS_RELEASE_TAG="test-tag"' "happy path release tag"
assert_contains "${rendered}" 'SQLITE_LIBRARY_PINS_SCHEMA_VERSION="2"' "happy path schema version"
assert_contains "${rendered}" 'SQLITE_LIBRARY_PINS_GENERATED_AT="2026-05-26T18:44:37Z"' "happy path generated_at"
assert_contains "${rendered}" 'component|emby|version-4.9.3.0' "happy path component passthrough"
assert_contains "${rendered}" 'emit_component_summary()' "happy path summary function"
assert_contains "${rendered}" 'assert_pre_replacement_sha()' "happy path assertion function"
bash -c '
  set -euo pipefail
  source <(awk "
    /^emit_component_summary\(\) \{/ { print; in_fn = 1; next }
    /^assert_pre_replacement_sha\(\) \{/ { print; in_fn = 1; next }
    in_fn { print }
    in_fn && /^\}/ { in_fn = 0 }
  " "$1")
  declare -F emit_component_summary >/dev/null
  declare -F assert_pre_replacement_sha >/dev/null
' bash "${rendered}"

schema1_pins="${tmpdir}/schema1-pins.txt"
cat > "${schema1_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|test-tag|generated_at|test-tag
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${schema1_pins}" "${tmpdir}/schema1.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "schema-1 final rejection"
assert_contains "${stderr}" "malformed pins metadata row" "schema-1 final rejection stderr"
for generated_at_case in "2026-05-26 18:44:37Z|not a valid ISO 8601 UTC timestamp" "2026-13-26T18:44:37Z|not parseable ISO 8601 UTC"; do
  bad_value="${generated_at_case%%|*}"
  expected_error="${generated_at_case#*|}"
  sed "s/2026-05-26T18:44:37Z/${bad_value}/" "${pins}" > "${tmpdir}/bad-generated-at.txt"
  run_regen "${tmpdir}/bad-generated-at.txt" "${tmpdir}/bad-generated-at.sh" "test-tag" "${stderr}"
  assert_nonzero "${run_status}" "bad generated_at ${bad_value}"
  assert_contains "${stderr}" "${expected_error}" "bad generated_at stderr"
done
awk '$0 !~ /^component\|icu\|/' "${pins}" > "${tmpdir}/missing-icu.txt"
run_regen "${tmpdir}/missing-icu.txt" "${tmpdir}/missing-icu.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "missing component row"
assert_contains "${stderr}" "component rows" "missing component row stderr"
unreleased_pins="${tmpdir}/unreleased-pins.txt"
sed 's/test-tag/unreleased/g; s/2026-05-26T18:44:37Z/unreleased/' "${pins}" > "${unreleased_pins}"
run_regen "${unreleased_pins}" "${tmpdir}/unreleased.sh" "unreleased" "${stderr}"
assert_status "${run_status}" 0 "unreleased sentinel accepted with unreleased release_tag"
bad_unreleased_pins="${tmpdir}/bad-unreleased-pins.txt"
sed 's/2026-05-26T18:44:37Z/unreleased/' "${pins}" > "${bad_unreleased_pins}"
run_regen "${bad_unreleased_pins}" "${tmpdir}/bad-unreleased.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "unreleased sentinel rejected for published tag"
assert_contains "${stderr}" "generated_at=unreleased requires release_tag=unreleased" "unreleased sentinel stderr"
cat "${pins}" "${schema1_pins}" > "${tmpdir}/schema2-plus-schema1.txt"
run_regen "${tmpdir}/schema2-plus-schema1.txt" "${tmpdir}/schema2-plus-schema1.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "schema2 plus schema1 concatenation"
assert_contains "${stderr}" "pins manifest must contain exactly one version row" "schema2 plus schema1 stderr"
cat > "${tmpdir}/schema1-metadata-schema2-components.txt" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|test-tag|generated_at|test-tag
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${tmpdir}/schema1-metadata-schema2-components.txt" "${tmpdir}/schema1-metadata-schema2-components.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "schema1 metadata with schema2 component rows"
assert_contains "${stderr}" "malformed pins metadata row" "schema1 metadata with component rows stderr"
component_negative_cases=(
  "duplicate|s/^component|icu|69.1$/component|mimalloc|3.3.2/"
  "reordered|s/^component|sqlite|3.53.1$/component|icu|69.1/; s/^component|icu|69.1$/component|sqlite|3.53.1/"
  "trailing-delimiter|s/^component|sqlite|3.53.1$/component|sqlite|3.53.1|/"
  "empty-value|s/^component|icu|69.1$/component|icu|/"
  "plex-full-uri|s#^component|plex|1.42.2\$#component|plex|lscr.io/linuxserver/plex:1.42.2#"
  "emby-full-uri|s#^component|emby|version-4.9.3.0\$#component|emby|lscr.io/linuxserver/emby:version-4.9.3.0#"
)
for case_spec in "${component_negative_cases[@]}"; do
  case_name="${case_spec%%|*}"
  sed_script="${case_spec#*|}"
  sed "${sed_script}" "${pins}" > "${tmpdir}/component-${case_name}.txt"
  run_regen "${tmpdir}/component-${case_name}.txt" "${tmpdir}/component-${case_name}.sh" "test-tag" "${stderr}"
  assert_nonzero "${run_status}" "component negative ${case_name}"
  assert_contains "${stderr}" "component rows" "component negative ${case_name} stderr"
done
sed '/^version|2|/a\
# interleaved component-block comment' "${pins}" > "${tmpdir}/component-interleaved-comment.txt"
run_regen "${tmpdir}/component-interleaved-comment.txt" "${tmpdir}/component-interleaved-comment.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "component block interleaved comment"
assert_contains "${stderr}" "component block" "component block interleaved comment stderr"

tmpl_no_helpers="${tmpdir}/tmpl-no-helpers.sh"
grep -vF "__INJECTED_HELPERS__" "${repo_root}/scripts/update-sqlite-library.sh.template" > "${tmpl_no_helpers}"
run_regen "${pins}" "${tmpdir}/missing-helpers.sh" "test-tag" "${stderr}" "${tmpl_no_helpers}"
assert_nonzero "${run_status}" "missing helpers placeholder"
assert_contains "${stderr}" "FATAL: template missing or has duplicate __INJECTED_HELPERS__ placeholder" "missing helpers placeholder stderr"

placeholder_pins="${tmpdir}/placeholder-pins.txt"
cat > "${placeholder_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=2
version|2|managed_window|5|release_tag|test-tag|generated_at|2026-05-26T18:44:37Z
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
pre|1|plex|linux-x86_64-v2|__SQLITE_LIBRARY_PINS_BLOCK__|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${placeholder_pins}" "${tmpdir}/placeholder.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "pins placeholder collision"
assert_contains "${stderr}" "FATAL: pins manifest contains placeholder token" "pins placeholder collision stderr"

helper_placeholder_pins="${tmpdir}/helper-placeholder-pins.txt"
cat > "${helper_placeholder_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=2
version|2|managed_window|5|release_tag|test-tag|generated_at|2026-05-26T18:44:37Z
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
pre|1|plex|linux-x86_64-v2|__INJECTED_HELPERS__|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${helper_placeholder_pins}" "${tmpdir}/helper-placeholder.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "helper placeholder collision"
assert_contains "${stderr}" "FATAL: pins manifest contains placeholder token" "helper placeholder collision stderr"

eof_pins="${tmpdir}/eof-pins.txt"
cat > "${eof_pins}" <<EOF
# SQLITE_LIBRARY_PINS schema=2
version|2|managed_window|5|release_tag|test-tag|generated_at|2026-05-26T18:44:37Z
component|sqlite|3.53.1
component|mimalloc|3.3.2
component|icu|69.1
component|plex|1.42.2
component|emby|version-4.9.3.0
PINS_EOF
pre|1|plex|linux-x86_64-v2|lscr.io/linuxserver/plex:test|lscr.io/linuxserver/plex@sha256:test|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|${pre_sha}
post|1|plex|linux-x86_64-v2|release|test-tag|/usr/lib/plexmediaserver/lib/libsqlite3.so|sqlite-test-tag-library-plex-linux-x86_64-v2.so|${post_sha}
EOF
run_regen "${eof_pins}" "${tmpdir}/eof.sh" "test-tag" "${stderr}"
assert_nonzero "${run_status}" "PINS_EOF collision"
assert_contains "${stderr}" "FATAL: pins manifest contains literal PINS_EOF line" "PINS_EOF collision stderr"

run_regen "${unreleased_pins}" "${tmpdir}/unreleased-rendered.sh" "unreleased" "${stderr}"
assert_status "${run_status}" 0 "unreleased artifact regen"
bash -n "${tmpdir}/unreleased-rendered.sh"
assert_contains "${tmpdir}/unreleased-rendered.sh" 'SQLITE_LIBRARY_PINS_RELEASE_TAG="unreleased"' "unreleased artifact release tag"
assert_contains "${tmpdir}/unreleased-rendered.sh" 'SQLITE_LIBRARY_PINS_GENERATED_AT="unreleased"' "unreleased artifact generated_at"
assert_not_contains "${tmpdir}/unreleased-rendered.sh" "__SQLITE_LIBRARY_PINS_BLOCK__" "unreleased artifact pins placeholder"
assert_not_contains "${tmpdir}/unreleased-rendered.sh" "__INJECTED_HELPERS__" "unreleased artifact helpers placeholder"

rendered2="${tmpdir}/rendered-atomic.sh"
rendered2_first="${tmpdir}/rendered-atomic-first.sh"
atomic_pins="${tmpdir}/atomic-pins.txt"
write_happy_pins "${atomic_pins}" "atomic-tag" "${pre_sha}" "${post_sha}"
run_regen "${atomic_pins}" "${rendered2}" "atomic-tag" "${stderr}"
assert_status "${run_status}" 0 "S6_atomic_write first render"
if [[ ! -s "${rendered2}" ]]; then
  fatal "S6_atomic_write first render: expected non-empty ${rendered2}"
fi
if [[ ! -x "${rendered2}" ]]; then
  fatal "S6_atomic_write first render: expected executable ${rendered2}"
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
if [[ ! -x "${rendered2}" ]]; then
  fatal "S6_atomic_write second render: expected executable ${rendered2}"
fi
if compgen -G "${rendered2}.tmp.*" >/dev/null; then
  fatal "S6_atomic_write second render: leaked tmp file for ${rendered2}"
fi
diff "${rendered2_first}" "${rendered2}"

echo "regen deploy pins tests passed"
