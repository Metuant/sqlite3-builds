#!/usr/bin/env bash
set -euo pipefail

script="tools/ci/skopeo-mod-image-oci-check.sh"

bash -n "$script"

expect_failure() {
  local expected="$1"
  shift
  local output
  if output="$("$@" 2>&1)"; then
    echo "FATAL: command unexpectedly succeeded: $*" >&2
    exit 1
  fi
  if ! grep -Fq "$expected" <<< "$output"; then
    printf 'FATAL: expected failure text not found: %s\noutput:\n%s\n' "$expected" "$output" >&2
    exit 1
  fi
}

expect_failure 'usage: skopeo-mod-image-oci-check.sh <mod> <publish-arch> <image-archive> <image-ref>' bash "$script"
expect_failure 'FATAL: unsupported mod: jellyfin' bash "$script" jellyfin amd64 image.tar ref
expect_failure 'FATAL: unsupported publish arch: x86' bash "$script" plex x86 image.tar ref
expect_failure 'FATAL: missing image archive: missing.tar' bash "$script" plex amd64 missing.tar ref

grep -Fq 'copy --format oci' "$script"
grep -Fq 'inspect --config' "$script"
grep -Fq 'inspect --raw' "$script"
grep -Fq 'baked_pins_text' "$script"
grep -Fq 'artifact_relpaths' "$script"
grep -Fq 'opt/sqlite3-lsio-mod/{relpath}' "$script"
if grep -Fq '{"plex": "icu69", "emby": "generic"}' "$script"; then
  echo "FATAL: skopeo check still hardcodes one compatibility group per mod" >&2
  exit 1
fi

printf 'skopeo mod image OCI static checks passed\n'
