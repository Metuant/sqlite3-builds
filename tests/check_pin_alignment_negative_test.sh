#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/check-pin-alignment-negative.XXXXXX" 2>/dev/null || mktemp -d /tmp/check-pin-alignment-negative.XXXXXX)"

cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

stage_scratch() {
  scratch="$1"
  mkdir -p \
    "$scratch/tests" \
    "$scratch/pins" \
    "$scratch/.github/workflows" \
    "$scratch/build" \
    "$scratch/docker-cli" \
    "$scratch/docker-library" \
    "$scratch/docker-build-base" \
    "$scratch/src" \
    "$scratch/tools/ci" \
    "$scratch/tools/lsio-mod" \
    "$scratch/tools" \
    "$scratch/scripts"

  cp "$repo_root/tests/check_pin_alignment.sh" "$scratch/tests/check_pin_alignment.sh"
  cp "$repo_root/pins/versions.env" "$scratch/pins/versions.env"
  cp "$repo_root/pins/library-compat-groups.tsv" "$scratch/pins/library-compat-groups.tsv"
  cp "$repo_root/pins/runtime-support.tsv" "$scratch/pins/runtime-support.tsv"
  cp "$repo_root/.github/workflows/sqlite-build.yml" "$scratch/.github/workflows/sqlite-build.yml"
  cp "$repo_root/.github/workflows/base.yml" "$scratch/.github/workflows/base.yml"
  cp "$repo_root/build/Build.sh" "$scratch/build/Build.sh"
  cp "$repo_root/build/build_static_sqlite.sh" "$scratch/build/build_static_sqlite.sh"
  cp "$repo_root/build/base_image_ref.sh" "$scratch/build/base_image_ref.sh"
  cp "$repo_root/build/expected-sqlite-config-count.txt" "$scratch/build/expected-sqlite-config-count.txt"
  cp "$repo_root/build/expected-sqlite-dbconfig-count.txt" "$scratch/build/expected-sqlite-dbconfig-count.txt"
  cp "$repo_root/docker-cli/Dockerfile" "$scratch/docker-cli/Dockerfile"
  cp "$repo_root/docker-library/Dockerfile" "$scratch/docker-library/Dockerfile"
  cp "$repo_root/docker-build-base/Dockerfile" "$scratch/docker-build-base/Dockerfile"
  cp "$repo_root/docker-build-base/ubuntu-toolchain-r-test.asc" "$scratch/docker-build-base/ubuntu-toolchain-r-test.asc"
  cp "$repo_root/src/auto_extension.c" "$scratch/src/auto_extension.c"
  cp "$repo_root/src/emby_fts_rewrite.c" "$scratch/src/emby_fts_rewrite.c"
  cp "$repo_root/src/plex_fts_rewrite.c" "$scratch/src/plex_fts_rewrite.c"
  cp "$repo_root/tools/ci/mod-bake-smoke.sh" "$scratch/tools/ci/mod-bake-smoke.sh"
  cp "$repo_root/tools/lsio-mod/render-lsio-mod-baked-pins.sh" "$scratch/tools/lsio-mod/render-lsio-mod-baked-pins.sh"
  cp "$repo_root/scripts/optimize_media_servers.sh" "$scratch/scripts/optimize_media_servers.sh"
}

assert_rejected() {
  name="$1"
  injected_path="$2"
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"

  case "$injected_path" in
    pins/versions.env)
      printf '\nPLEX_IMAGE_TAG=foo\n' >> "$scratch/$injected_path"
      ;;
    scripts/reintroduced-pin.sh)
      printf '%s\n' 'PLEX_IMAGE_TAG=foo' > "$scratch/$injected_path"
      ;;
    docker-build-base/reintroduced-pin.sh)
      printf '%s\n' 'PLEX_IMAGE_TAG=foo' > "$scratch/$injected_path"
      ;;
    *)
      fail "unsupported injected path: $injected_path"
      ;;
  esac

  set +e
  output="$(cd "$scratch" && bash tests/check_pin_alignment.sh 2>&1)"
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    printf '%s\n' "$output" >&2
    fail "$name: check_pin_alignment.sh accepted injected retired scalar"
  fi

  if ! printf '%s\n' "$output" | grep -Fq "$injected_path:"; then
    printf '%s\n' "$output" >&2
    fail "$name: missing injected path in guard output"
  fi

  if ! printf '%s\n' "$output" | grep -Fq 'FATAL: retired scalar pin reappeared'; then
    printf '%s\n' "$output" >&2
    fail "$name: missing retired-scalar fatal message"
  fi
}

assert_fails_with() {
  name="$1"
  expected_text="$2"
  scratch="$tmp_root/$name"
  stage_scratch "$scratch"

  case "$name" in
    missing-base-ref-script-input)
      grep -Fv 'printf '\''%s'\'' "${CMAKE_SHA256_AARCH64}"' \
        "$scratch/build/base_image_ref.sh" > "$scratch/build/base_image_ref.sh.tmp"
      mv "$scratch/build/base_image_ref.sh.tmp" "$scratch/build/base_image_ref.sh"
      ;;
    inline-generic-base)
      awk '
        $0 == "FROM ${BASE_IMAGE} AS base-generic" {
          print "FROM ubuntu:18.04 AS base-generic"
          print "RUN add-apt-repository ppa:ubuntu-toolchain-r/test"
          next
        }
        { print }
      ' "$scratch/docker-library/Dockerfile" > "$scratch/docker-library/Dockerfile.tmp"
      mv "$scratch/docker-library/Dockerfile.tmp" "$scratch/docker-library/Dockerfile"
      ;;
    *)
      fail "unsupported failure case: $name"
      ;;
  esac

  set +e
  output="$(cd "$scratch" && bash tests/check_pin_alignment.sh 2>&1)"
  status=$?
  set -e

  if [ "$status" -eq 0 ]; then
    printf '%s\n' "$output" >&2
    fail "$name: check_pin_alignment.sh accepted invalid fixture"
  fi

  if ! printf '%s\n' "$output" | grep -Fq "$expected_text"; then
    printf '%s\n' "$output" >&2
    fail "$name: missing expected failure text: $expected_text"
  fi
}

assert_rejected pins-source pins/versions.env
assert_rejected scripts-source scripts/reintroduced-pin.sh
assert_rejected retired-scalar-under-base-context docker-build-base/reintroduced-pin.sh
assert_fails_with missing-base-ref-script-input CMAKE_SHA256_AARCH64
assert_fails_with inline-generic-base BASE_IMAGE

printf 'negative pin-alignment checks passed\n'
