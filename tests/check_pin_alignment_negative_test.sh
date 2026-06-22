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
    "$scratch/docker-library" \
    "$scratch/tools" \
    "$scratch/scripts"

  cp "$repo_root/tests/check_pin_alignment.sh" "$scratch/tests/check_pin_alignment.sh"
  cp "$repo_root/pins/versions.env" "$scratch/pins/versions.env"
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

assert_rejected pins-source pins/versions.env
assert_rejected scripts-source scripts/reintroduced-pin.sh

printf 'negative pin-alignment checks passed\n'
