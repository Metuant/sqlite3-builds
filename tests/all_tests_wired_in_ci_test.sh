#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

# Scope: tests/*_test.sh only. The .c smokes and check_*.sh scripts run through build/smoke steps.
# ALLOWLIST entries are test basenames intentionally not wired into CI.
# Format: "name_test.sh" # reason: <why CI must skip it>
ALLOWLIST=()

is_allowlisted() {
  local name="$1"
  local allowed

  for allowed in "${ALLOWLIST[@]}"; do
    [ "$name" = "$allowed" ] && return 0
  done

  return 1
}

missing=()

for test_path in tests/*_test.sh; do
  name="$(basename "$test_path")"

  if is_allowlisted "$name"; then
    continue
  fi

  if ! grep -Fq -- "bash tests/$name" .github/workflows/sqlite-build.yml; then
    missing+=("$name")
  fi
done

if [ "${#missing[@]}" -gt 0 ]; then
  printf 'Missing CI wiring for tests:\n' >&2
  printf '  %s\n' "${missing[@]}" >&2
  fail "some tests/*_test.sh are not wired into CI"
fi

printf 'all tests/*_test.sh are wired into CI\n'
