#!/bin/sh
# shellcheck disable=SC2016
set -eu

unset CDPATH
repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "${repo_root}"

fail() {
  printf 'FATAL: %s\n' "$1" >&2
  exit 1
}

require_line() {
  file="$1"
  line="$2"
  grep -Fxq -- "$line" "$file" || fail "$file missing exact line: $line"
}

reject_pattern() {
  file="$1"
  pattern="$2"
  if grep -Eq -- "$pattern" "$file"; then
    fail "$file contains rejected pattern: $pattern"
  fi
}

require_pattern() {
  file="$1"
  pattern="$2"
  label="$3"
  grep -Eq -- "$pattern" "$file" || fail "$label missing from $file"
}

legacy_scalar_keys() {
  printf '%s_%s_%s\n' PLEX IMAGE TAG
  printf '%s_%s_%s\n' EMBY IMAGE TAG
  printf '%s_%s\n' ICU VERSION
  printf '%s_%s\n' ICU SHA512
}

reject_legacy_scalar_keys() {
  key_list="$(legacy_scalar_keys)"
  set -- pins .github/workflows build docker-library tools scripts
  for key in $key_list; do
    matches="$(grep -R -n -- "$key" "$@" 2>/dev/null || true)"
    if [ -n "$matches" ]; then
      printf '%s\n' "$matches" >&2
      fail "retired scalar pin reappeared"
    fi
  done
}

pin_file="pins/versions.env"
[ -r "$pin_file" ] || fail "$pin_file not readable"
# shellcheck source=pins/versions.env
. "$pin_file"
reject_legacy_scalar_keys

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

assert_wrapper_compat_default() {
  key="$1"
  compat_group="$2"
  field="$3"
  needle="${key}=\"\${${key}-\$(compat_group_pin ${compat_group} ${field})}\""
  grep -Fq "$needle" build/build_static_sqlite.sh || \
    fail "build/build_static_sqlite.sh does not default $key from pins/library-compat-groups.tsv"
}

compat_group_pin() {
  compat_group="$1"
  field="$2"
  awk -F '\t' -v want_group="$compat_group" -v want_field="$field" '
    NR == 1 {
      for (i = 1; i <= NF; i++) {
        if ($i == want_field) {
          field_index = i
        }
      }
      if (field_index == 0) {
        printf "FATAL: missing compat group field: %s\n", want_field > "/dev/stderr"
        exit 2
      }
      next
    }
    $0 ~ /^[[:space:]]*($|#)/ { next }
    $1 == want_group {
      if (value != "") {
        printf "FATAL: duplicate compat group: %s\n", want_group > "/dev/stderr"
        exit 2
      }
      value = $field_index
    }
    END {
      if (value == "") {
        printf "FATAL: missing compat group: %s\n", want_group > "/dev/stderr"
        exit 1
      }
      print value
    }
  ' pins/library-compat-groups.tsv
}

for key in \
  SQLITE_AMALG_URL \
  SQLITE_AMALG_SHA3_256 \
  SQLITE_SRC_URL \
  SQLITE_SRC_SHA3_256 \
  MIMALLOC_VERSION \
  MIMALLOC_URL \
  MIMALLOC_SHA512
do
  assert_wrapper_pin_default "$key"
done

assert_wrapper_compat_default ICU_SOURCE_VERSION icu69 icu_source_version
assert_wrapper_compat_default ICU_SOURCE_SHA512 icu69 icu_source_sha512

require_line build/build_static_sqlite.sh 'pins_file="${script_dir}/../pins/versions.env"'
require_line build/build_static_sqlite.sh 'compat_groups_file="${script_dir}/../pins/library-compat-groups.tsv"'
require_line .github/workflows/sqlite-build.yml '        run: grep -E '\''^[A-Za-z_][A-Za-z0-9_]*='\'' pins/versions.env >> "$GITHUB_ENV"'

load_step_count="$(grep -Fc "run: grep -E '^[A-Za-z_][A-Za-z0-9_]*=' pins/versions.env >> \"\$GITHUB_ENV\"" .github/workflows/sqlite-build.yml)"
assert_eq '.github/workflows/sqlite-build.yml Load version pins step count' '3' "$load_step_count"

top_env_pins="$(awk '
  /^env:/ { in_env=1; next }
  /^jobs:/ { in_env=0 }
  in_env && /^[[:space:]]+[A-Z_]+:/ { print }
' .github/workflows/sqlite-build.yml | grep -E 'SQLITE_|MIMALLOC_|ICU_|(PLEX|EMBY)_.*TAG' || true)"
[ -z "$top_env_pins" ] || fail ".github/workflows/sqlite-build.yml keeps pin keys in top-level env: $top_env_pins"

assert_eq 'build/Build.sh MIMALLOC_VERSION' "$MIMALLOC_VERSION" "$(extract_build_sh_default MIMALLOC_VERSION)"
assert_eq 'build/Build.sh MIMALLOC_URL' "$MIMALLOC_URL" "$(extract_build_sh_default MIMALLOC_URL)"
assert_eq 'build/Build.sh MIMALLOC_SHA512' "$MIMALLOC_SHA512" "$(extract_build_sh_default MIMALLOC_SHA512)"

plex_icu_source_version="$(compat_group_pin icu69 icu_source_version)"
plex_icu_source_sha512="$(compat_group_pin icu69 icu_source_sha512)"
generic_icu_source_version="$(compat_group_pin generic icu_source_version)"
generic_icu_source_sha512="$(compat_group_pin generic icu_source_sha512)"
[ "$plex_icu_source_version" != "-" ] || fail "icu69 missing ICU source version"
[ "$plex_icu_source_sha512" != "-" ] || fail "icu69 missing ICU source SHA512"
[ "$generic_icu_source_version" = "-" ] || fail "generic compat group must not carry ICU source version"
[ "$generic_icu_source_sha512" = "-" ] || fail "generic compat group must not carry ICU source SHA512"

for key in ICU_SOURCE_VERSION ICU_SOURCE_SHA512 MIMALLOC_VERSION MIMALLOC_URL MIMALLOC_SHA512; do
  require_line docker-library/Dockerfile "ARG $key"
  reject_pattern docker-library/Dockerfile "^ARG ${key}="
done
reject_pattern .github/workflows/sqlite-build.yml '--build-arg ICU_(VERSION|SHA512)='
reject_pattern .github/workflows/sqlite-build.yml 'env\.(PLEX|EMBY)_.*TAG'
reject_pattern .github/workflows/sqlite-build.yml '\$(PLEX|EMBY)_.*TAG'
reject_pattern tools/ci/mod-bake-smoke.sh '(PLEX|EMBY)_.*TAG'
require_pattern tools/lsio-mod/render-lsio-mod-baked-pins.sh "printf '# baked-pins schema=3\\\\n'" 'schema-v3 render comment'
require_pattern tools/lsio-mod/render-lsio-mod-baked-pins.sh "printf 'meta[|]3[|]release_tag[|]%s[|]generated_at[|]%s\\\\n'" 'schema-v3 meta render'

awk -F '\t' '
  function die(message) {
    printf "FATAL: %s\n", message > "/dev/stderr"
    exit 1
  }
  function nonempty(label, value) {
    if (value == "") {
      die(label " is empty")
    }
  }
  function valid_mod(value) {
    return value == "plex" || value == "emby"
  }
  FILENAME == "pins/library-compat-groups.tsv" {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "compat_group") {
      next
    }
    if (NF != 9) {
      die("malformed compat group row at line " FNR)
    }
    for (i = 1; i <= 9; i++) {
      nonempty("compat group field " i " at line " FNR, $i)
    }
    if (!valid_mod($2)) {
      die("invalid compat group mod at line " FNR ": " $2)
    }
    if ($1 in compat_mod) {
      die("duplicate compat_group: " $1)
    }
    if ($2 in mod_group) {
      die("duplicate compat group for mod: " $2)
    }
    if ($2 == "plex") {
      if ($6 == "-" || $7 == "-") {
        die("Plex compat group missing ICU source fields: " $1)
      }
      if ($8 != "libicuucplex.so.69;libicui18nplex.so.69;libicudataplex.so.69") {
        die("Plex compat group linked ICU sonames drift: " $1)
      }
    }
    if ($2 == "emby" && ($6 != "-" || $7 != "-" || $8 != "-")) {
      die("Emby compat group must not carry ICU fields: " $1)
    }
    compat_mod[$1] = $2
    compat_smoke[$1] = $9
    mod_group[$2] = $1
    next
  }
  FILENAME == "pins/runtime-support.tsv" {
    if ($0 ~ /^[[:space:]]*($|#)/ || $1 == "mod") {
      next
    }
    if (NF != 7) {
      die("malformed runtime support row at line " FNR)
    }
    for (i = 1; i <= 7; i++) {
      nonempty("runtime support field " i " at line " FNR, $i)
    }
    if (!valid_mod($1)) {
      die("invalid runtime support mod at line " FNR ": " $1)
    }
    if (!($4 in compat_mod)) {
      die("runtime support references unknown compat_group at line " FNR ": " $4)
    }
    if (compat_mod[$4] != $1) {
      die("runtime support compat_group/mod mismatch at line " FNR ": " $4)
    }
    if ($5 == "supported") {
      if ($2 in server_compat) {
        die("duplicate supported server_id: " $2)
      }
      server_compat[$2] = $4
      supported_by_mod[$1]++
      supported_by_group[$4]++
    }
    next
  }
  END {
    for (compat in compat_mod) {
      if (!(compat in supported_by_group)) {
        die("compat_group has no supported runtime-support row: " compat)
      }
      smoke = compat_smoke[compat]
      if (!(smoke in server_compat)) {
        die("compat_group smoke_server_id does not resolve to a supported row: " compat " -> " smoke)
      }
      if (server_compat[smoke] != compat) {
        die("compat_group smoke_server_id uses another compat_group: " compat " -> " smoke)
      }
    }
    if (supported_by_mod["plex"] == 0) {
      die("missing supported Plex runtime-support row")
    }
    if (supported_by_mod["emby"] == 0) {
      die("missing supported Emby runtime-support row")
    }
  }
' pins/library-compat-groups.tsv pins/runtime-support.tsv

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

printf 'pins aligned: sqlite=%s mimalloc=%s icu_source=%s bases=%s,%s counts=%s/%s\n' \
  "$SQLITE_VERSION_DOTTED" \
  "$MIMALLOC_VERSION" \
  "$plex_icu_source_version" \
  "$BASEIMAGE_UBUNTU" \
  "$BASEIMAGE_ALPINE" \
  "$SQLITE_EXPECTED_CONFIG_COUNT" \
  "$SQLITE_EXPECTED_DBCONFIG_COUNT"
