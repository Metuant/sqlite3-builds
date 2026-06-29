#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH='' cd -- "${script_dir}/.." && pwd)"

# shellcheck disable=SC1091
. "${repo_root}/pins/versions.env"

: "${BASEIMAGE_UBUNTU:?missing pin BASEIMAGE_UBUNTU}"
: "${CMAKE_VERSION:?missing pin CMAKE_VERSION}"
: "${CMAKE_SHA256_X86_64:?missing pin CMAKE_SHA256_X86_64}"
: "${CMAKE_SHA256_AARCH64:?missing pin CMAKE_SHA256_AARCH64}"

hash_input() {
  cat "${repo_root}/docker-build-base/Dockerfile"
  cat "${repo_root}/docker-build-base/ubuntu-toolchain-r-test.asc"
  printf '%s' "${BASEIMAGE_UBUNTU}"
  printf '%s' "${CMAKE_VERSION}"
  printf '%s' "${CMAKE_SHA256_X86_64}"
  printf '%s' "${CMAKE_SHA256_AARCH64}"
}

sha256_stream() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 -r | awk '{print $1}'
  else
    echo "FATAL: no SHA-256 tool found; need sha256sum, shasum, or openssl" >&2
    return 1
  fi
}

digest="$(hash_input | sha256_stream | tr '[:upper:]' '[:lower:]')"
if ! printf '%s\n' "${digest}" | grep -Eq '^[0-9a-f]{64}$'; then
  echo "FATAL: unexpected SHA-256 digest: ${digest}" >&2
  exit 1
fi

base_tag="$(printf '%s' "${digest}" | cut -c 1-16)"
printf 'BASE_REF=ghcr.io/darthshadow/sqlite3-build-base:src-%s\n' "${base_tag}"
