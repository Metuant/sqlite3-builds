#!/bin/sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "${repo_root}"

fail() {
  printf 'FATAL: %s\n' "$1" >&2
  exit 1
}

require_line() {
  file="$1"
  line="$2"
  grep -Fxq "$line" "$file" || fail "$file missing exact line: $line"
}

reject_pattern() {
  file="$1"
  pattern="$2"
  if grep -Eq "$pattern" "$file"; then
    fail "$file contains rejected pattern: $pattern"
  fi
}

pin_file="pins/versions.env"
[ -r "$pin_file" ] || fail "$pin_file not readable"
. "$pin_file"

required_pins='
SQLITE_VERSION
SQLITE_VERSION_DOTTED
SQLITE_AMALG_URL
SQLITE_AMALG_SHA3_256
SQLITE_SRC_URL
SQLITE_SRC_SHA3_256
MIMALLOC_VERSION
MIMALLOC_URL
MIMALLOC_SHA512
ICU_VERSION
ICU_SHA512
PLEX_IMAGE_TAG
EMBY_IMAGE_TAG
SQLITE_EXPECTED_CONFIG_COUNT
SQLITE_EXPECTED_DBCONFIG_COUNT
BASEIMAGE_UBUNTU
BASEIMAGE_ALPINE
'

for key in $required_pins; do
  eval "value=\${${key}-}"
  [ -n "$value" ] || fail "$pin_file missing non-empty $key"
done

assert_eq() {
  label="$1"
  expected="$2"
  actual="$3"
  [ -n "$actual" ] || fail "$label missing"
  [ "$actual" = "$expected" ] || fail "$label drift: observed=$actual expected=$expected"
}

extract_build_sh_default() {
  key="$1"
  value="$(sed -n "s/^${key}=\"\${${key}:-\\([^\}]*\\)}\"$/\\1/p" build/Build.sh)"
  [ -n "$value" ] || fail "build/Build.sh missing ${key} default"
  printf '%s\n' "$value"
}

assert_wrapper_pin_default() {
  key="$1"
  needle="${key}=\"\${${key}-\$(pin_default ${key})}\""
  grep -Fq "$needle" build/build_static_sqlite.sh || \
    fail "build/build_static_sqlite.sh does not default $key from pins/versions.env"
}

for key in \
  SQLITE_AMALG_URL \
  SQLITE_AMALG_SHA3_256 \
  SQLITE_SRC_URL \
  SQLITE_SRC_SHA3_256 \
  MIMALLOC_VERSION \
  MIMALLOC_URL \
  MIMALLOC_SHA512 \
  ICU_VERSION \
  ICU_SHA512
do
  assert_wrapper_pin_default "$key"
done

require_line build/build_static_sqlite.sh 'pins_file="${script_dir}/../pins/versions.env"'
require_line .github/workflows/sqlite-build.yml '        run: grep -E '\''^[A-Za-z_][A-Za-z0-9_]*='\'' pins/versions.env >> "$GITHUB_ENV"'

load_step_count="$(grep -Fc "run: grep -E '^[A-Za-z_][A-Za-z0-9_]*=' pins/versions.env >> \"\$GITHUB_ENV\"" .github/workflows/sqlite-build.yml)"
assert_eq '.github/workflows/sqlite-build.yml Load version pins step count' '3' "$load_step_count"

top_env_pins="$(awk '
  /^env:/ { in_env=1; next }
  /^jobs:/ { in_env=0 }
  in_env && /^[[:space:]]+[A-Z_]+:/ { print }
' .github/workflows/sqlite-build.yml | grep -E 'SQLITE_|MIMALLOC_|ICU_|PLEX_IMAGE_TAG|EMBY_IMAGE_TAG' || true)"
[ -z "$top_env_pins" ] || fail ".github/workflows/sqlite-build.yml keeps pin keys in top-level env: $top_env_pins"

assert_eq 'build/Build.sh MIMALLOC_VERSION' "$MIMALLOC_VERSION" "$(extract_build_sh_default MIMALLOC_VERSION)"
assert_eq 'build/Build.sh MIMALLOC_URL' "$MIMALLOC_URL" "$(extract_build_sh_default MIMALLOC_URL)"
assert_eq 'build/Build.sh MIMALLOC_SHA512' "$MIMALLOC_SHA512" "$(extract_build_sh_default MIMALLOC_SHA512)"

for key in ICU_VERSION ICU_SHA512 MIMALLOC_VERSION MIMALLOC_URL MIMALLOC_SHA512; do
  require_line docker-library/Dockerfile "ARG $key"
  reject_pattern docker-library/Dockerfile "^ARG ${key}="
done

require_line docker-cli/Dockerfile "FROM ${BASEIMAGE_ALPINE}"
require_line docker-cli/Dockerfile 'ENTRYPOINT []'
require_line docker-cli/Dockerfile 'CMD ["/bin/sh"]'
require_line docker-library/Dockerfile "FROM ${BASEIMAGE_UBUNTU} AS base-generic"
require_line docker-library/Dockerfile "FROM ${BASEIMAGE_ALPINE} AS base-plex"
require_line docker-library/Dockerfile 'ENTRYPOINT []'
require_line docker-library/Dockerfile 'CMD ["/bin/sh"]'
require_line docker-library/Dockerfile 'ARG SQLITE_AMALG_URL'
require_line docker-library/Dockerfile 'ARG SQLITE_AMALG_SHA3_256'
require_line docker-library/Dockerfile 'ARG SQLITE_SRC_URL=""'
require_line docker-library/Dockerfile 'ARG SQLITE_SRC_SHA3_256=""'
require_line docker-cli/Dockerfile 'ARG SQLITE_AMALG_URL'
require_line docker-cli/Dockerfile 'ARG SQLITE_AMALG_SHA3_256'

config_count="$(tr -d '[:space:]' < build/expected-sqlite-config-count.txt)"
dbconfig_count="$(tr -d '[:space:]' < build/expected-sqlite-dbconfig-count.txt)"
assert_eq 'build/expected-sqlite-config-count.txt' "$SQLITE_EXPECTED_CONFIG_COUNT" "$config_count"
assert_eq 'build/expected-sqlite-dbconfig-count.txt' "$SQLITE_EXPECTED_DBCONFIG_COUNT" "$dbconfig_count"

printf 'pins aligned: sqlite=%s mimalloc=%s icu=%s bases=%s,%s counts=%s/%s\n' \
  "$SQLITE_VERSION_DOTTED" \
  "$MIMALLOC_VERSION" \
  "$ICU_VERSION" \
  "$BASEIMAGE_UBUNTU" \
  "$BASEIMAGE_ALPINE" \
  "$SQLITE_EXPECTED_CONFIG_COUNT" \
  "$SQLITE_EXPECTED_DBCONFIG_COUNT"
