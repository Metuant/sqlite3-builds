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

reject_line() {
  file="$1"
  line="$2"
  if grep -Fxq -- "$line" "$file"; then
    fail "$file contains rejected exact line: $line"
  fi
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
  set -- pins .github/workflows build docker-library docker-build-base tools scripts
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
CMAKE_VERSION
CMAKE_SHA256_X86_64
CMAKE_SHA256_AARCH64
UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT
SQLITE_EXPECTED_CONFIG_COUNT
SQLITE_EXPECTED_DBCONFIG_COUNT
BASEIMAGE_UBUNTU
BASEIMAGE_ALPINE
GENERIC_GLIBC_MAX
'

for key in $required_pins; do
  eval "value=\${${key}-}"
  [ -n "$value" ] || fail "$pin_file missing non-empty $key"
done

printf '%s\n' "$UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT" | grep -Eq '^[0-9A-F]{40}$' || \
  fail "$pin_file UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT must be 40 uppercase hex characters"

assert_eq() {
  label="$1"
  expected="$2"
  actual="$3"
  [ -n "$actual" ] || fail "$label missing"
  [ "$actual" = "$expected" ] || fail "$label drift: observed=$actual expected=$expected"
}

extract_sqlite_url_version_suffix() {
  label="$1"
  url="$2"
  basename="${url##*/}"
  suffix="${basename%.zip}"
  suffix="${suffix##*-}"
  case "$suffix" in
    ''|*[!0-9]*) fail "$label version suffix is not numeric: observed=$suffix expected=$SQLITE_VERSION" ;;
  esac
  printf '%s\n' "$suffix"
}

assert_eq 'SQLITE_AMALG_URL version suffix' "$SQLITE_VERSION" "$(extract_sqlite_url_version_suffix SQLITE_AMALG_URL "$SQLITE_AMALG_URL")"
assert_eq 'SQLITE_SRC_URL version suffix' "$SQLITE_VERSION" "$(extract_sqlite_url_version_suffix SQLITE_SRC_URL "$SQLITE_SRC_URL")"

extract_build_sh_default() {
  key="$1"
  value="$(sed -n "s/^${key}=\"\${${key}:-\\([^\}]*\\)}\"$/\\1/p" build/Build.sh)"
  [ -n "$value" ] || fail "build/Build.sh missing ${key} default"
  printf '%s\n' "$value"
}

extract_single_int() {
  file="$1"
  pattern="$2"
  label="$3"
  count="$(grep -Ec -- "$pattern" "$file" || true)"
  [ "$count" = "1" ] || fail "$label matched $count lines in $file"
  value="$(grep -E -- "$pattern" "$file" | sed -E "s/$pattern/\\1/")"
  [ -n "$value" ] || fail "$label missing integer in $file"
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

assert_wrapper_build_arg() {
  key="$1"
  needle="--build-arg ${key}=\"\${${key}}\""
  grep -Fq -- "$needle" build/build_static_sqlite.sh || \
    fail "build/build_static_sqlite.sh does not forward $key as a docker build arg"
}

assert_wrapper_library_build_arg_absent() {
  key="$1"
  if ! awk -v key="$key" '
    index($0, "DockerLibraryImage") && index($0, "build --rm --no-cache=true") {
      in_library_build = 1
    }
    in_library_build && index($0, "--build-arg " key "=") {
      print
      found = 1
    }
    in_library_build && index($0, "-f docker-library/Dockerfile .") {
      in_library_build = 0
    }
    END {
      exit found ? 1 : 0
    }
  ' build/build_static_sqlite.sh; then
    fail "build/build_static_sqlite.sh forwards $key to docker-library/Dockerfile"
  fi
}

require_workflow_step_no_pattern() {
  file="$1"
  step="$2"
  pattern="$3"
  label="$4"
  step_body="$(awk -v step="$step" '
    /^[[:space:]]+- name: / {
      if (in_step) {
        in_step = 0
      }
      if (index($0, "- name: " step) > 0) {
        in_step = 1
        found = 1
        next
      }
    }
    in_step {
      print
    }
    END {
      if (!found) {
        exit 2
      }
    }
  ' "$file")" || fail "$file missing workflow step: $step"
  if printf '%s\n' "$step_body" | grep -Eq -- "$pattern"; then
    fail "$label contains rejected pattern: $pattern"
  fi
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

reject_line build/build_static_sqlite.sh 'CMAKE_VERSION="${CMAKE_VERSION-$(pin_default CMAKE_VERSION)}"'
reject_line build/build_static_sqlite.sh 'CMAKE_SHA256_X86_64="${CMAKE_SHA256_X86_64-$(pin_default CMAKE_SHA256_X86_64)}"'
reject_line build/build_static_sqlite.sh 'CMAKE_SHA256_AARCH64="${CMAKE_SHA256_AARCH64-$(pin_default CMAKE_SHA256_AARCH64)}"'
for key in CMAKE_VERSION CMAKE_SHA256_X86_64 CMAKE_SHA256_AARCH64; do
  assert_wrapper_library_build_arg_absent "$key"
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

build_sorterref="$(extract_single_int \
  build/Build.sh \
  '^[[:space:]]*-DSQLITE_DEFAULT_SORTERREF_SIZE=([0-9]+)([[:space:]]*\\)?$' \
  'build/Build.sh SQLITE_DEFAULT_SORTERREF_SIZE')"
build_pmasz="$(extract_single_int \
  build/Build.sh \
  '^[[:space:]]*-DSQLITE_SORTER_PMASZ=([0-9]+)([[:space:]]*\\)?$' \
  'build/Build.sh SQLITE_SORTER_PMASZ')"
rt_sorterref="$(extract_single_int \
  src/auto_extension.c \
  '^[[:space:]]*int[[:space:]]+cfg_rc[[:space:]]*=[[:space:]]*sqlite3_config\(SQLITE_CONFIG_SORTERREF_SIZE,[[:space:]]*([0-9]+)\);$' \
  'src/auto_extension.c SQLITE_CONFIG_SORTERREF_SIZE')"
rt_pmasz="$(extract_single_int \
  src/auto_extension.c \
  '^[[:space:]]*int[[:space:]]+pmasz_rc[[:space:]]*=[[:space:]]*sqlite3_config\(SQLITE_CONFIG_PMASZ,[[:space:]]*([0-9]+)\);$' \
  'src/auto_extension.c SQLITE_CONFIG_PMASZ')"

assert_eq 'SORTERREF compile<->runtime alignment' "$build_sorterref" "$rt_sorterref"
assert_eq 'PMASZ compile<->runtime alignment' "$build_pmasz" "$rt_pmasz"

require_line scripts/optimize_media_servers.sh '        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type = 8;"'
require_line scripts/optimize_media_servers.sh '        "CREATE INDEX IF NOT EXISTS idx_dshadow_taggings_tag_id_metadata_item_id ON taggings (tag_id, metadata_item_id);"'
require_line scripts/optimize_media_servers.sh '        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_guid_nocase ON metadata_items (guid COLLATE NOCASE);"'
require_line scripts/optimize_media_servers.sh '        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_item_views_account_grandparent_guid ON metadata_item_views (account_id, grandparent_guid);"'
require_line scripts/optimize_media_servers.sh '        "idx_dshadow_taggings_tag_id_metadata_item_id"'
require_line scripts/optimize_media_servers.sh '        "idx_dshadow_metadata_items_guid_nocase"'
require_line scripts/optimize_media_servers.sh '        "idx_dshadow_metadata_item_views_account_grandparent_guid"'
require_line src/emby_fts_rewrite.c '#define EMBY_LATEST_INDEX_NAME "idx_dshadow_emby_latest_gk_dc"'
require_line src/emby_fts_rewrite.c "        \"AND name='\" EMBY_LATEST_INDEX_NAME \"' \""
require_line src/emby_fts_rewrite.c '    "WITH keys(gk) AS MATERIALIZED (SELECT DISTINCT coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey) FROM MediaItems INDEXED BY " EMBY_LATEST_INDEX_NAME " WHERE Type = 8), picked AS MATERIALIZED (SELECT K.gk, (SELECT A2.Id FROM MediaItems AS A2 WHERE A2.Type = 8 AND coalesce(A2.SeriesPresentationUniqueKey, A2.PresentationUniqueKey) IS K.gk AND EXISTS (SELECT 1 FROM AncestorIds2 AS X WHERE X.ItemId = A2.Id AND X.AncestorId IN (";'
require_line src/plex_fts_rewrite.c '#define PLEX_TAG_INDEX_NAME "idx_dshadow_taggings_tag_id_metadata_item_id"'
require_line src/plex_fts_rewrite.c '#define PLEX_ONDECK_INDEX_NAME "idx_dshadow_metadata_item_views_account_grandparent_guid"'

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
for key in CMAKE_VERSION CMAKE_SHA256_X86_64 CMAKE_SHA256_AARCH64; do
  require_line docker-build-base/Dockerfile "ARG $key"
  reject_pattern docker-build-base/Dockerfile "^ARG ${key}="
  reject_pattern docker-library/Dockerfile "^ARG ${key}$"
  reject_pattern docker-library/Dockerfile "$key"
done
require_line docker-build-base/Dockerfile 'ARG BASEIMAGE_UBUNTU'
require_line docker-build-base/Dockerfile 'FROM ${BASEIMAGE_UBUNTU}'
require_line docker-build-base/Dockerfile 'ARG UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT'
require_pattern docker-build-base/Dockerfile 'COPY ubuntu-toolchain-r-test[.]asc' 'vendored toolchain key copy'
require_pattern docker-build-base/Dockerfile 'https://ppa[.]launchpadcontent[.]net/ubuntu-toolchain-r/test/ubuntu' 'direct toolchain PPA URL'
require_pattern docker-build-base/Dockerfile 'bionic' 'bionic PPA codename'
require_pattern docker-build-base/Dockerfile 'openssl' 'OpenSSL package/smoke'
require_pattern docker-build-base/Dockerfile '345bd227155241c08ab64f6a3e34ec2b1b590f4b9c98087dfef0d7e70dbe578b' 'OpenSSL SHA3 smoke hash'
reject_pattern docker-build-base/Dockerfile 'add-apt-repository|apt-key|keyserver|launchpad[.]net/api|software-properties-common|apt-transport-https'

for key in BASEIMAGE_UBUNTU CMAKE_VERSION CMAKE_SHA256_X86_64 CMAKE_SHA256_AARCH64; do
  require_pattern .github/workflows/base.yml "\\$\\{${key}\\}" ".github/workflows/base.yml $key reference"
  require_line build/base_image_ref.sh ": \"\${${key}:?missing pin ${key}}\""
  require_line build/base_image_ref.sh "  printf '%s' \"\${${key}}\""
done
require_line build/base_image_ref.sh '  cat "${repo_root}/docker-build-base/Dockerfile"'
require_line build/base_image_ref.sh '  cat "${repo_root}/docker-build-base/ubuntu-toolchain-r-test.asc"'

require_line .github/workflows/base.yml '        run: grep -E '\''^[A-Za-z_][A-Za-z0-9_]*='\'' pins/versions.env >> "$GITHUB_ENV"'
require_pattern .github/workflows/base.yml 'build/base_image_ref[.]sh' 'base ref script call'
require_pattern .github/workflows/base.yml 'docker/setup-buildx-action@d7f5e7f509e45cec5c76c4d5afdd7de93d0b3df5' 'pinned setup-buildx action'
require_pattern .github/workflows/base.yml 'docker buildx imagetools inspect "\$\{base_ref\}"|docker buildx imagetools inspect "\$\{BASE_REF\}"' 'base image inspect'
require_pattern .github/workflows/base.yml 'provenance=false' 'base build provenance disable'
require_pattern .github/workflows/base.yml 'docker buildx imagetools create' 'base manifest publish'
require_pattern .github/workflows/base.yml 'ghcr[.]io/darthshadow/sqlite3-build-base@sha256:<digest>|ghcr[.]io/darthshadow/sqlite3-build-base@%s|ghcr\[.\]io/darthshadow/sqlite3-build-base@sha256:' 'resolved base digest output'
require_line .github/workflows/base.yml '            --build-arg CMAKE_VERSION="${CMAKE_VERSION}" \'
require_line .github/workflows/base.yml '            --build-arg CMAKE_SHA256_X86_64="${CMAKE_SHA256_X86_64}" \'
require_line .github/workflows/base.yml '            --build-arg CMAKE_SHA256_AARCH64="${CMAKE_SHA256_AARCH64}" \'
require_line .github/workflows/base.yml '            --build-arg UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT="${UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT}" \'

require_pattern .github/workflows/sqlite-build.yml 'docker/setup-buildx-action@d7f5e7f509e45cec5c76c4d5afdd7de93d0b3df5' 'main workflow setup-buildx action'
require_pattern .github/workflows/sqlite-build.yml 'docker buildx build --load' 'main workflow buildx local load'
require_pattern .github/workflows/sqlite-build.yml '--cache-from type=gha' 'main workflow GHA cache import'
require_pattern .github/workflows/sqlite-build.yml '--cache-to type=gha,mode=max' 'main workflow GHA cache export'
require_pattern .github/workflows/sqlite-build.yml '--build-arg BASE_IMAGE="\$\{BASE_IMAGE\}"' 'library BASE_IMAGE build arg'
require_pattern .github/workflows/sqlite-build.yml '--build-arg BASEIMAGE_ALPINE="\$\{BASEIMAGE_ALPINE\}"' 'library BASEIMAGE_ALPINE build arg'
reject_pattern .github/workflows/sqlite-build.yml 'sudo docker build|--platform'
for step in 'Build sqlite library' 'Build sqlite Plex library'; do
  require_workflow_step_no_pattern \
    .github/workflows/sqlite-build.yml \
    "$step" \
    '--build-arg CMAKE_(VERSION|SHA256_X86_64|SHA256_AARCH64)=' \
    "$step"
done
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
require_line docker-library/Dockerfile 'ARG BASE_IMAGE'
require_line docker-library/Dockerfile 'FROM ${BASE_IMAGE} AS base-generic'
require_line docker-library/Dockerfile 'ARG BASEIMAGE_ALPINE'
require_line docker-library/Dockerfile 'FROM ${BASEIMAGE_ALPINE} AS base-plex'
require_line docker-library/Dockerfile "      generic_glibc_max=\"${GENERIC_GLIBC_MAX}\"; \\"
require_line docker-library/Dockerfile 'ENTRYPOINT []'
require_line docker-library/Dockerfile 'CMD ["/bin/sh"]'
require_line docker-library/Dockerfile 'ARG SQLITE_AMALG_URL'
require_line docker-library/Dockerfile 'ARG SQLITE_AMALG_SHA3_256'
require_line docker-library/Dockerfile 'ARG SQLITE_SRC_URL=""'
require_line docker-library/Dockerfile 'ARG SQLITE_SRC_SHA3_256=""'
require_line docker-cli/Dockerfile 'ARG SQLITE_AMALG_URL'
require_line docker-cli/Dockerfile 'ARG SQLITE_AMALG_SHA3_256'
reject_pattern docker-library/Dockerfile 'add-apt-repository|ppa:ubuntu-toolchain-r/test|^FROM ubuntu:18[.]04|wget[[:space:]]+-O'

require_pattern build/build_static_sqlite.sh 'build/base_image_ref[.]sh' 'local wrapper base ref script call'
require_pattern build/build_static_sqlite.sh 'buildx build --load' 'local wrapper base buildx local load'
require_pattern build/build_static_sqlite.sh '--build-arg BASE_IMAGE="\$\{BASE_IMAGE\}"' 'local wrapper BASE_IMAGE build arg'
require_pattern build/build_static_sqlite.sh '--build-arg BASEIMAGE_ALPINE="\$\{BASEIMAGE_ALPINE\}"' 'local wrapper BASEIMAGE_ALPINE build arg'

config_count="$(tr -d '[:space:]' < build/expected-sqlite-config-count.txt)"
dbconfig_count="$(tr -d '[:space:]' < build/expected-sqlite-dbconfig-count.txt)"
assert_eq 'build/expected-sqlite-config-count.txt' "$SQLITE_EXPECTED_CONFIG_COUNT" "$config_count"
assert_eq 'build/expected-sqlite-dbconfig-count.txt' "$SQLITE_EXPECTED_DBCONFIG_COUNT" "$dbconfig_count"

printf 'pins aligned: sqlite=%s mimalloc=%s base_cmake=%s base_ref_script=%s toolchain_key=%s icu_source=%s bases=%s,%s generic_glibc_max=%s counts=%s/%s sorterref=%s pmasz=%s\n' \
  "$SQLITE_VERSION_DOTTED" \
  "$MIMALLOC_VERSION" \
  "$CMAKE_VERSION" \
  "build/base_image_ref.sh" \
  "$UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT" \
  "$plex_icu_source_version" \
  "$BASEIMAGE_UBUNTU" \
  "$BASEIMAGE_ALPINE" \
  "$GENERIC_GLIBC_MAX" \
  "$SQLITE_EXPECTED_CONFIG_COUNT" \
  "$SQLITE_EXPECTED_DBCONFIG_COUNT" \
  "$build_sorterref" \
  "$build_pmasz"
