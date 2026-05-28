#!/bin/sh
set -eu

expected_url_for_version() {
  printf 'https://github.com/microsoft/mimalloc/archive/refs/tags/v%s.tar.gz\n' "$1"
}

fail() {
  printf 'FATAL: %s\n' "$1" >&2
  exit 1
}

extract_build_sh() {
  key="$1"
  sed -n "s/^${key}=\"\${${key}:-\\([^\}]*\\)}\".*/\\1/p" build/Build.sh
}

extract_wrapper() {
  key="$1"
  sed -n "s/^${key}='\\([^']*\\)'.*/\\1/p" build/build_static_sqlite.sh
}

extract_dockerfile() {
  key="$1"
  sed -n "s/^ARG ${key}=\\(.*\\)$/\\1/p" docker-library/Dockerfile
}

extract_workflow() {
  key="$1"
  sed -n "s/^[[:space:]]*${key}: \"\\([^\"]*\\)\"[[:space:]]*$/\\1/p" .github/workflows/sqlite-build.yml
}

require_dockerfile_text() {
  needle="$1"
  grep -Fq "$needle" docker-library/Dockerfile || fail "ICU URL drift: missing ${needle}"
}

check_site() {
  site="$1"
  version="$2"
  url="$3"
  sha="$4"

  [ -n "$version" ] || fail "$site missing MIMALLOC_VERSION"
  [ -n "$url" ] || fail "$site missing MIMALLOC_URL"
  [ -n "$sha" ] || fail "$site missing MIMALLOC_SHA512"

  expected_url="$(expected_url_for_version "$version")"
  if [ "$url" != "$expected_url" ]; then
    fail "$site MIMALLOC_URL mismatch: observed=$url expected=$expected_url"
  fi
}

build_version="$(extract_build_sh MIMALLOC_VERSION)"
build_url="$(extract_build_sh MIMALLOC_URL)"
build_sha="$(extract_build_sh MIMALLOC_SHA512)"
wrapper_version="$(extract_wrapper MIMALLOC_VERSION)"
wrapper_url="$(extract_wrapper MIMALLOC_URL)"
wrapper_sha="$(extract_wrapper MIMALLOC_SHA512)"
docker_version="$(extract_dockerfile MIMALLOC_VERSION)"
docker_url="$(extract_dockerfile MIMALLOC_URL)"
docker_sha="$(extract_dockerfile MIMALLOC_SHA512)"
workflow_version="$(extract_workflow MIMALLOC_VERSION)"
workflow_url="$(extract_workflow MIMALLOC_URL)"
workflow_sha="$(extract_workflow MIMALLOC_SHA512)"

check_site "build/Build.sh" "$build_version" "$build_url" "$build_sha"
check_site "build/build_static_sqlite.sh" "$wrapper_version" "$wrapper_url" "$wrapper_sha"
check_site "docker-library/Dockerfile" "$docker_version" "$docker_url" "$docker_sha"
check_site ".github/workflows/sqlite-build.yml" "$workflow_version" "$workflow_url" "$workflow_sha"

expected_version="$build_version"
expected_url="$build_url"
expected_sha="$build_sha"

for row in \
  "build/build_static_sqlite.sh|$wrapper_version|$wrapper_url|$wrapper_sha" \
  "docker-library/Dockerfile|$docker_version|$docker_url|$docker_sha" \
  ".github/workflows/sqlite-build.yml|$workflow_version|$workflow_url|$workflow_sha"
do
  site="${row%%|*}"
  rest="${row#*|}"
  version="${rest%%|*}"
  rest="${rest#*|}"
  url="${rest%%|*}"
  sha="${rest#*|}"

  [ "$version" = "$expected_version" ] || fail "$site MIMALLOC_VERSION drift: observed=$version expected=$expected_version"
  [ "$url" = "$expected_url" ] || fail "$site MIMALLOC_URL drift: observed=$url expected=$expected_url"
  [ "$sha" = "$expected_sha" ] || fail "$site MIMALLOC_SHA512 drift: observed=$sha expected=$expected_sha"
done

dockerfile_icu="$(extract_dockerfile ICU_VERSION)"
workflow_icu="$(extract_workflow ICU_VERSION)"
[ -n "$dockerfile_icu" ] || fail "docker-library/Dockerfile missing ICU_VERSION ARG default"
[ -n "$workflow_icu" ] || fail ".github/workflows/sqlite-build.yml missing ICU_VERSION env"
[ "$dockerfile_icu" = "$workflow_icu" ] || \
  fail "ICU_VERSION drift: Dockerfile=$dockerfile_icu workflow=$workflow_icu"

require_dockerfile_text 'icu_release_version="$(printf '\''%s'\'' "${ICU_VERSION}" | tr '\''.'\'' '\''-'\'')"'
require_dockerfile_text 'icu_archive_version="$(printf '\''%s'\'' "${ICU_VERSION}" | tr '\''.'\'' '\''_'\'')"'
require_dockerfile_text 'icu_archive="/tmp/icu4c-${icu_archive_version}-src.tgz"'
require_dockerfile_text 'icu_url="https://github.com/unicode-org/icu/releases/download/release-${icu_release_version}/icu4c-${icu_archive_version}-src.tgz"'

printf 'pins aligned: mimalloc=%s icu=%s\n' "$expected_version" "$workflow_icu"
