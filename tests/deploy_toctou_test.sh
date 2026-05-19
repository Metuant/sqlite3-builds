#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmpdir="$(mktemp -d /tmp/deploy-toctou-test.XXXXXX 2>/dev/null || { mkdir -p /tmp/deploy-toctou-test-$$; echo /tmp/deploy-toctou-test-$$; })"
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

real_sha256sum="$(command -v sha256sum)"
real_uname="$(command -v uname)"

escape_sed() {
  printf '%s' "$1" | sed 's/[|&]/\\&/g'
}

write_rendered_script() {
  local target_path=$1 pins_file=$2 output_file=$3
  local target_path_escaped
  target_path_escaped="$(escape_sed "${target_path}")"
  awk -v pins_file="${pins_file}" '
    BEGIN { replacing_pins = 0 }
    index($0, "SQLITE_LIBRARY_PINS=$(cat <<'\''PINS_EOF'\''") == 1 {
      print
      while ((getline line < pins_file) > 0) {
        print line
      }
      close(pins_file)
      replacing_pins = 1
      next
    }
    replacing_pins && /^PINS_EOF$/ {
      print
      replacing_pins = 0
      next
    }
    replacing_pins { next }
    { print }
  ' "${repo_root}/scripts/update-sqlite-library.sh" \
    | sed "s|/app/emby/lib/libsqlite3.so.3.49.2|${target_path_escaped}|g" \
    > "${output_file}"
  chmod +x "${output_file}"
}

write_fake_curl() {
  cat > "${tmpdir}/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

output=""
url=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o)
      output=$2
      shift 2
      ;;
    -w)
      shift 2
      ;;
    -f | -s | -S | -L | -fsSL)
      shift
      ;;
    --*)
      shift
      ;;
    *)
      url=$1
      shift
      ;;
  esac
done

[[ -n "${output}" ]] || exit 0
case "${url}" in
  */SHA256SUMS)
    cp "${RELEASE_FIXTURE_DIR}/SHA256SUMS" "${output}"
    ;;
  */sqlite-unreleased-library-linux-arm64.tar.gz)
    cp "${RELEASE_FIXTURE_DIR}/sqlite-unreleased-library-linux-arm64.tar.gz" "${output}"
    ;;
  *)
    echo "FATAL: unexpected curl url ${url}" >&2
    exit 1
    ;;
esac
EOF
  chmod +x "${tmpdir}/bin/curl"
}

write_fake_uname() {
  cat > "${tmpdir}/bin/uname" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "-m" ]]; then
  echo "arm64"
  exit 0
fi

exec "${REAL_UNAME}" "$@"
EOF
  chmod +x "${tmpdir}/bin/uname"
}

write_fake_sha256sum() {
  cat > "${tmpdir}/bin/sha256sum" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

path=$1
counter_file="${SHA_COUNTER_DIR}/$(printf '%s' "${path}" | tr '/ ' '__').count"
count=0
if [[ -f "${counter_file}" ]]; then
  count="$(cat "${counter_file}")"
fi
count=$((count + 1))
printf '%s' "${count}" > "${counter_file}"

if [[ "${path}" == "${TARGET_PATH}" ]]; then
  case "${SHA_MODE}" in
    stable)
      ;;
    change-on-second)
      if [[ "${count}" -eq 2 ]]; then
        printf 'changed concurrently\n' > "${TARGET_PATH}"
      fi
      ;;
    delete-on-second)
      if [[ "${count}" -eq 2 ]]; then
        rm -f "${TARGET_PATH}"
      fi
      ;;
    *)
      echo "FATAL: unexpected SHA_MODE ${SHA_MODE}" >&2
      exit 1
      ;;
  esac
fi

exec "${REAL_SHA256SUM}" "$@"
EOF
  chmod +x "${tmpdir}/bin/sha256sum"
}

run_case() {
  local mode=$1 script_path=$2 stdout_file=$3 stderr_file=$4
  rm -rf "${tmpdir}/sha-counts"
  mkdir -p "${tmpdir}/sha-counts"
  set +e
  PATH="${tmpdir}/bin:${PATH}" \
    REAL_SHA256SUM="${real_sha256sum}" \
    REAL_UNAME="${real_uname}" \
    RELEASE_FIXTURE_DIR="${tmpdir}/release" \
    SHA_COUNTER_DIR="${tmpdir}/sha-counts" \
    SHA_MODE="${mode}" \
    TARGET_PATH="${target_path}" \
    TAG="unreleased" \
    bash "${script_path}" emby >"${stdout_file}" 2>"${stderr_file}"
  local status=$?
  set -e
  run_status="${status}"
}

mkdir -p "${tmpdir}/bin" "${tmpdir}/release" "${tmpdir}/runtime"
write_fake_curl
write_fake_uname
write_fake_sha256sum

target_path="${tmpdir}/runtime/libsqlite3.so.3.49.2"
release_so="${tmpdir}/release/libsqlite3.so"
pins_file="${tmpdir}/SQLITE_LIBRARY_PINS"
rendered_script="${tmpdir}/update-sqlite-library.sh"
stdout="${tmpdir}/stdout"
stderr="${tmpdir}/stderr"

printf 'managed pre image\n' > "${target_path}"
printf 'new deployed library\n' > "${release_so}"
pre_sha="$("${real_sha256sum}" "${target_path}" | awk '{print $1}')"
release_so_sha="$("${real_sha256sum}" "${release_so}" | awk '{print $1}')"

cat > "${pins_file}" <<EOF
# SQLITE_LIBRARY_PINS schema=1
version|1|managed_window|5|release_tag|unreleased|generated_at|unreleased
pre|1|emby|linux-arm64|lscr.io/linuxserver/emby:test|lscr.io/linuxserver/emby@sha256:test|${target_path}|runtime|${pre_sha}
post|1|emby|linux-arm64|release|unreleased|${target_path}|sqlite-unreleased-library-linux-arm64.so|${release_so_sha}
EOF

archive_path="${tmpdir}/release/sqlite-unreleased-library-linux-arm64.tar.gz"
tar -C "${tmpdir}/release" -czf "${archive_path}" libsqlite3.so
archive_sha="$("${real_sha256sum}" "${archive_path}" | awk '{print $1}')"
cat > "${tmpdir}/release/SHA256SUMS" <<EOF
${archive_sha}  sqlite-unreleased-library-linux-arm64.tar.gz
${release_so_sha}  sqlite-unreleased-library-linux-arm64.so
EOF

write_rendered_script "${target_path}" "${pins_file}" "${rendered_script}"

printf 'managed pre image\n' > "${target_path}"
run_case "stable" "${rendered_script}" "${stdout}" "${stderr}"
if [[ "${run_status}" -ne 0 ]]; then
  echo "FATAL: stable SHA control path stderr" >&2
  cat "${stderr}" >&2
  exit 1
fi
actual_post_sha="$("${real_sha256sum}" "${target_path}" | awk '{print $1}')"
if [[ "${actual_post_sha}" != "${release_so_sha}" ]]; then
  fatal "stable SHA control path: expected deployed sha ${release_so_sha}, got ${actual_post_sha}"
fi

printf 'managed pre image\n' > "${target_path}"
run_case "change-on-second" "${rendered_script}" "${stdout}" "${stderr}"
assert_nonzero "${run_status}" "TOCTOU change detection"
assert_contains "${stderr}" "FATAL: TOCTOU:" "TOCTOU change detection stderr"

printf 'managed pre image\n' > "${target_path}"
run_case "delete-on-second" "${rendered_script}" "${stdout}" "${stderr}"
assert_nonzero "${run_status}" "target disappearance"
if [[ -f "${target_path}" ]]; then
  fatal "target disappearance: expected ${target_path} to be absent after simulated deletion"
fi

echo "deploy TOCTOU tests passed"
