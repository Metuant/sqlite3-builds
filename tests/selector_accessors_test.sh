#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/logging.sh
. ./lsio-mods/shared/cont-init-fragments/sha.sh
. ./lsio-mods/shared/cont-init-fragments/selector.sh
. ./lsio-mods/shared/cont-init-fragments/manifest-parser.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/selector-accessors-test.XXXXXX" 2>/dev/null || mktemp -d /tmp/selector-accessors-test.XXXXXX)"
cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

template="tests/fixtures/manifest-v3.template"
arch="linux-x86_64-v3"
staged_root="$tmp_root/staged"
mod_root="$staged_root/opt/sqlite3-lsio-mod"
artifact_root="$mod_root/artifacts"
live_root="$tmp_root/live"
sha_root="$tmp_root/sha-source"
manifest="$tmp_root/selector-accessors.manifest"

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_eq() {
  local expected actual context
  expected="$1"
  actual="$2"
  context="$3"
  [ "$expected" = "$actual" ] || fail "$context: expected [$expected], actual [$actual]"
}

write_file() {
  local path text
  path="$1"
  text="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' "$text" > "$path"
}

copy_file() {
  local source target
  source="$1"
  target="$2"
  mkdir -p "$(dirname "$target")"
  cp "$source" "$target"
}

setup_fixture_files() {
  plex_pms_path="$live_root/plex/Plex Media Server"
  plex_scanner_path="$live_root/plex/Plex Media Scanner"
  emby_deps_path="$live_root/emby/EmbyServer.deps.json"
  emby_dll_path="$live_root/emby/EmbyServer.dll"

  write_file "$sha_root/plex-pms-pristine" "plex pms pristine detector"
  write_file "$sha_root/plex-pms-patched" "plex pms known patched detector"
  write_file "$sha_root/plex-scanner-pristine" "plex scanner pristine detector"
  write_file "$sha_root/plex-scanner-patched" "plex scanner known patched detector"
  write_file "$sha_root/emby-deps" "emby deps detector"
  write_file "$sha_root/emby-dll" "emby dll detector"
  write_file "$sha_root/plex-target" "plex bundled sqlite"
  write_file "$sha_root/emby-target" "emby bundled sqlite"
  write_file "$sha_root/plex-icu-uc" "plex icu uc"
  write_file "$sha_root/plex-icu-i18n" "plex icu i18n"
  write_file "$sha_root/plex-icu-data" "plex icu data"
  write_file "$artifact_root/$arch/icu69/libsqlite3.so" "plex replacement sqlite"
  write_file "$artifact_root/$arch/generic/libsqlite3.so" "emby replacement sqlite"

  plex_pms_pristine_sha="$(sha256_of "$sha_root/plex-pms-pristine")"
  plex_pms_patched_sha="$(sha256_of "$sha_root/plex-pms-patched")"
  plex_scanner_pristine_sha="$(sha256_of "$sha_root/plex-scanner-pristine")"
  plex_scanner_patched_sha="$(sha256_of "$sha_root/plex-scanner-patched")"
  emby_deps_sha="$(sha256_of "$sha_root/emby-deps")"
  emby_dll_sha="$(sha256_of "$sha_root/emby-dll")"
  plex_target_sha="$(sha256_of "$sha_root/plex-target")"
  emby_target_sha="$(sha256_of "$sha_root/emby-target")"
  plex_icu_uc_sha="$(sha256_of "$sha_root/plex-icu-uc")"
  plex_icu_i18n_sha="$(sha256_of "$sha_root/plex-icu-i18n")"
  plex_icu_data_sha="$(sha256_of "$sha_root/plex-icu-data")"
  plex_artifact_sha="$(sha256_of "$artifact_root/$arch/icu69/libsqlite3.so")"
  emby_artifact_sha="$(sha256_of "$artifact_root/$arch/generic/libsqlite3.so")"
}

render_manifest() {
  local output
  output="$1"
  sed \
    -e "s|@ARCH@|$arch|g" \
    -e "s|@PLEX_PMS_PATH@|$plex_pms_path|g" \
    -e "s|@PLEX_SCANNER_PATH@|$plex_scanner_path|g" \
    -e "s|@EMBY_DEPS_PATH@|$emby_deps_path|g" \
    -e "s|@EMBY_DLL_PATH@|$emby_dll_path|g" \
    -e "s|@PLEX_PMS_PRISTINE_SHA@|$plex_pms_pristine_sha|g" \
    -e "s|@PLEX_PMS_PATCHED_SHA@|$plex_pms_patched_sha|g" \
    -e "s|@PLEX_SCANNER_PRISTINE_SHA@|$plex_scanner_pristine_sha|g" \
    -e "s|@PLEX_SCANNER_PATCHED_SHA@|$plex_scanner_patched_sha|g" \
    -e "s|@EMBY_DEPS_SHA@|$emby_deps_sha|g" \
    -e "s|@EMBY_DLL_SHA@|$emby_dll_sha|g" \
    -e "s|@PLEX_TARGET_SHA@|$plex_target_sha|g" \
    -e "s|@EMBY_TARGET_SHA@|$emby_target_sha|g" \
    -e "s|@PLEX_ICU_UC_SHA@|$plex_icu_uc_sha|g" \
    -e "s|@PLEX_ICU_I18N_SHA@|$plex_icu_i18n_sha|g" \
    -e "s|@PLEX_ICU_DATA_SHA@|$plex_icu_data_sha|g" \
    -e "s|@PLEX_ARTIFACT_SHA@|$plex_artifact_sha|g" \
    -e "s|@EMBY_ARTIFACT_SHA@|$emby_artifact_sha|g" \
    "$template" > "$output"
}

set_plex_live_pristine() {
  copy_file "$sha_root/plex-pms-pristine" "$plex_pms_path"
  copy_file "$sha_root/plex-scanner-pristine" "$plex_scanner_path"
}

set_emby_live_primary() {
  copy_file "$sha_root/emby-deps" "$emby_deps_path"
  copy_file "$sha_root/emby-dll" "$emby_dll_path"
}

validate_manifest() {
  local target_manifest
  target_manifest="$1"
  # shellcheck disable=SC2034
  sqlite3_mod_root="$mod_root"
  # shellcheck disable=SC2034
  sqlite3_mod_artifact_root="$artifact_root"
  validate_baked_pins_schema "$target_manifest"
}

capture_selector() {
  local name mod target_manifest
  name="$1"
  mod="$2"
  target_manifest="$3"
  selector_stdout="$tmp_root/${name}.stdout"
  selector_stderr="$tmp_root/${name}.stderr"
  # shellcheck disable=SC2034
  sqlite3_mod_root="$mod_root"
  # shellcheck disable=SC2034
  sqlite3_mod_artifact_root="$artifact_root"
  set +e
  select_supported_server "$mod" "$arch" "$target_manifest" >"$selector_stdout" 2>"$selector_stderr"
  selector_rc=$?
  set -e
}

select_required_server() {
  local name mod target_manifest expected selected
  name="$1"
  mod="$2"
  target_manifest="$3"
  expected="$4"
  capture_selector "$name" "$mod" "$target_manifest"
  assert_eq "0" "$selector_rc" "$name selector exit status"
  selected="$(cat "$selector_stdout")"
  assert_eq "$expected" "$selected" "$name selected server"
  printf '%s\n' "$selected"
}

capture_accessor() {
  local name
  name="$1"
  shift
  accessor_stdout="$tmp_root/${name}.stdout"
  accessor_stderr="$tmp_root/${name}.stderr"
  # shellcheck disable=SC2034
  sqlite3_mod_root="$mod_root"
  # shellcheck disable=SC2034
  sqlite3_mod_artifact_root="$artifact_root"
  set +e
  "$@" >"$accessor_stdout" 2>"$accessor_stderr"
  accessor_rc=$?
  set -e
}

assert_accessor_output() {
  local name expected actual_stderr
  name="$1"
  expected="$2"
  actual_stderr="$(cat "$accessor_stderr")"
  [ "$accessor_rc" = "0" ] || fail "$name exit status: expected [0], actual [$accessor_rc], stderr [$actual_stderr]"
  assert_eq "$expected" "$(cat "$accessor_stdout")" "$name stdout"
}

assert_accessor_missing() {
  local name actual_stderr
  name="$1"
  actual_stderr="$(cat "$accessor_stderr")"
  [ "$accessor_rc" = "1" ] || fail "$name exit status: expected [1], actual [$accessor_rc], stderr [$actual_stderr]"
  assert_eq "" "$(cat "$accessor_stdout")" "$name stdout"
}

assert_selector_uses_parser_accessors() {
  local selector_source
  selector_source="lsio-mods/shared/cont-init-fragments/selector.sh"
  if grep -Fq '[ "$kind" = "unsupported" ] || continue' "$selector_source"; then
    fail "selector.sh must delegate unsupported rows to manifest-parser.sh"
  fi
  if grep -Fq '[ "$kind" = "artifact" ] || continue' "$selector_source"; then
    fail "selector.sh must delegate artifact row access to manifest-parser.sh"
  fi
  grep -Fq 'manifest_parser_selected_unsupported_row "$@"' "$selector_source" || fail "selector.sh missing unsupported-row parser delegation"
  grep -Fq 'selected_artifact_row "$mod" "$arch" "$manifest" "$server_id"' "$selector_source" || fail "selected_artifact_is_current must use selected_artifact_row"
}

assert_selector_uses_parser_accessors
setup_fixture_files
render_manifest "$manifest"
validate_manifest "$manifest"

set_plex_live_pristine
plex_server_id="$(select_required_server "plex-select" "plex" "$manifest" "plex-1.43.2")"

capture_accessor "plex-artifact-row" selected_artifact_row "plex" "$arch" "$manifest" "$plex_server_id"
assert_accessor_output \
  "plex-artifact-row" \
  "icu69|artifacts/$arch/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$plex_artifact_sha"

capture_accessor "plex-target-sqlite-pre" selected_pre_sha256 "plex" "$arch" "$manifest" "$plex_server_id" "target_sqlite"
assert_accessor_output "plex-target-sqlite-pre" "$plex_target_sha"

capture_accessor "plex-icu-uc-pre" selected_pre_file_sha256_row "plex" "$arch" "$manifest" "$plex_server_id" "plex_icu_linked:libicuucplex.so.69"
assert_accessor_output "plex-icu-uc-pre" "/usr/lib/plexmediaserver/lib/libicuucplex.so.69|$plex_icu_uc_sha"

capture_accessor "plex-icu-i18n-pre" selected_pre_file_sha256_row "plex" "$arch" "$manifest" "$plex_server_id" "plex_icu_linked:libicui18nplex.so.69"
assert_accessor_output "plex-icu-i18n-pre" "/usr/lib/plexmediaserver/lib/libicui18nplex.so.69|$plex_icu_i18n_sha"

capture_accessor "plex-icu-data-pre" selected_pre_file_sha256_row "plex" "$arch" "$manifest" "$plex_server_id" "plex_icu_linked:libicudataplex.so.69"
assert_accessor_output "plex-icu-data-pre" "/usr/lib/plexmediaserver/lib/libicudataplex.so.69|$plex_icu_data_sha"

capture_accessor "plex-pms-pool-site" selected_pool_site_rows "plex" "$arch" "$manifest" "$plex_server_id" "$plex_pms_path"
assert_accessor_output "plex-pms-pool-site" "pool-open-flags|0|0|00112233445566778899aabbccddeeff|ff112233445566778899aabbccddeeff"

capture_accessor "plex-scanner-pool-site" selected_pool_site_rows "plex" "$arch" "$manifest" "$plex_server_id" "$plex_scanner_path"
assert_accessor_output "plex-scanner-pool-site" "pool-open-flags|8|15|11112222333344445555666677778888|11112222333344ff5555666677778888"

capture_accessor "plex-pms-pristine-detector" selected_pristine_detector_row "plex" "$arch" "$manifest" "$plex_server_id" "plex_pms"
assert_accessor_output "plex-pms-pristine-detector" "$plex_pms_path|$plex_pms_pristine_sha"

capture_accessor "plex-scanner-pristine-detector" selected_pristine_detector_row "plex" "$arch" "$manifest" "$plex_server_id" "plex_scanner"
assert_accessor_output "plex-scanner-pristine-detector" "$plex_scanner_path|$plex_scanner_pristine_sha"

set_emby_live_primary
emby_server_id="$(select_required_server "emby-select" "emby" "$manifest" "emby-4.9.3")"

capture_accessor "emby-artifact-row" selected_artifact_row "emby" "$arch" "$manifest" "$emby_server_id"
assert_accessor_output \
  "emby-artifact-row" \
  "generic|artifacts/$arch/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$emby_artifact_sha"

capture_accessor "emby-target-sqlite-pre" selected_pre_sha256 "emby" "$arch" "$manifest" "$emby_server_id" "target_sqlite"
assert_accessor_output "emby-target-sqlite-pre" "$emby_target_sha"

unsupported_manifest="$tmp_root/unsupported-local.manifest"
awk 'BEGIN { FS=OFS="|" } !($1=="artifact" && $3=="emby") { print } END { print "unsupported|1|emby|emby-4.9.3|" arch "|generic|local-offline-missing-artifact" }' arch="$arch" "$manifest" > "$unsupported_manifest"
validate_manifest "$unsupported_manifest"

capture_accessor "emby-unsupported-row" selected_unsupported_row "emby" "$arch" "$unsupported_manifest" "$emby_server_id"
assert_accessor_output "emby-unsupported-row" "generic|local-offline-missing-artifact"

capture_accessor "missing-unsupported-row" selected_unsupported_row "emby" "$arch" "$manifest" "$emby_server_id"
assert_accessor_missing "missing-unsupported-row"

capture_accessor "missing-unsupported-server" selected_unsupported_row "emby" "$arch" "$unsupported_manifest" "emby-not-selected"
assert_accessor_missing "missing-unsupported-server"

capture_accessor "missing-selected-artifact" selected_artifact_row "plex" "$arch" "$manifest" "plex-not-selected"
assert_accessor_missing "missing-selected-artifact"

printf 'selector accessor tests passed\n'
