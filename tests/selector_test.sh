#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/logging.sh
. ./lsio-mods/shared/cont-init-fragments/sha.sh
. ./lsio-mods/shared/cont-init-fragments/selector.sh
. ./lsio-mods/shared/cont-init-fragments/manifest-parser.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/selector-test.XXXXXX" 2>/dev/null || mktemp -d /tmp/selector-test.XXXXXX)"
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
manifest="$tmp_root/selector.manifest"

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

assert_contains() {
  local needle path context
  needle="$1"
  path="$2"
  context="$3"
  grep -Fq "$needle" "$path" || fail "$context: expected [$path] to contain [$needle], actual [$(cat "$path")]"
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
  write_file "$sha_root/emby-deps-alt" "emby alternate deps detector"
  write_file "$sha_root/emby-dll-alt" "emby alternate dll detector"
  write_file "$sha_root/wrong-detector" "wrong detector content"
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
  emby_deps_alt_sha="$(sha256_of "$sha_root/emby-deps-alt")"
  emby_dll_alt_sha="$(sha256_of "$sha_root/emby-dll-alt")"
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

reset_live_files() {
  rm -f "$plex_pms_path" "$plex_scanner_path" "$emby_deps_path" "$emby_dll_path" "$emby_deps_alt_path" "$emby_dll_alt_path"
}

set_plex_live_pristine() {
  copy_file "$sha_root/plex-pms-pristine" "$plex_pms_path"
  copy_file "$sha_root/plex-scanner-pristine" "$plex_scanner_path"
}

set_plex_live_patched() {
  copy_file "$sha_root/plex-pms-patched" "$plex_pms_path"
  copy_file "$sha_root/plex-scanner-patched" "$plex_scanner_path"
}

set_plex_live_mixed() {
  copy_file "$sha_root/plex-pms-pristine" "$plex_pms_path"
  copy_file "$sha_root/plex-scanner-patched" "$plex_scanner_path"
}

set_emby_live_primary() {
  copy_file "$sha_root/emby-deps" "$emby_deps_path"
  copy_file "$sha_root/emby-dll" "$emby_dll_path"
}

set_emby_live_mismatched() {
  copy_file "$sha_root/wrong-detector" "$emby_deps_path"
  copy_file "$sha_root/wrong-detector" "$emby_dll_path"
}

append_emby_alt_server() {
  local target_manifest
  target_manifest="$1"
  emby_deps_alt_path="$live_root/emby-alt/EmbyServer.deps.json"
  emby_dll_alt_path="$live_root/emby-alt/EmbyServer.dll"
  cat >> "$target_manifest" <<EOF_ALT
detect|1|emby|emby-4.9.5|$arch|emby_deps|$emby_deps_alt_path|$emby_deps_alt_sha
detect|1|emby|emby-4.9.5|$arch|emby_dll|$emby_dll_alt_path|$emby_dll_alt_sha
artifact|1|emby|emby-4.9.5|$arch|generic|artifacts/$arch/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$emby_artifact_sha
pre|2|emby|emby-4.9.5|$arch|target_sqlite|lscr.io/linuxserver/emby:version-4.9.5.0|-|/app/emby/lib/libsqlite3.so.3.49.2|$emby_target_sha
EOF_ALT
}

append_emby_cross_product_server() {
  local target_manifest
  target_manifest="$1"
  cat >> "$target_manifest" <<EOF_CROSS
detect|1|emby|emby-cross|$arch|emby_deps|$emby_deps_path|$emby_deps_alt_sha
detect|1|emby|emby-cross|$arch|emby_dll|$emby_dll_path|$emby_dll_alt_sha
artifact|1|emby|emby-cross|$arch|generic|artifacts/$arch/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$emby_artifact_sha
pre|2|emby|emby-cross|$arch|target_sqlite|lscr.io/linuxserver/emby:version-cross|-|/app/emby/lib/libsqlite3.so.3.49.2|$emby_target_sha
EOF_CROSS
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

capture_current_artifact() {
  local name mod target_manifest server_id
  name="$1"
  mod="$2"
  target_manifest="$3"
  server_id="$4"
  current_stdout="$tmp_root/${name}.stdout"
  current_stderr="$tmp_root/${name}.stderr"
  # shellcheck disable=SC2034
  sqlite3_mod_root="$mod_root"
  # shellcheck disable=SC2034
  sqlite3_mod_artifact_root="$artifact_root"
  set +e
  selected_artifact_is_current "$mod" "$arch" "$target_manifest" "$server_id" >"$current_stdout" 2>"$current_stderr"
  current_rc=$?
  set -e
}

assert_selected() {
  local name mod target_manifest expected
  name="$1"
  mod="$2"
  target_manifest="$3"
  expected="$4"
  capture_selector "$name" "$mod" "$target_manifest"
  assert_eq "0" "$selector_rc" "$name exit status"
  assert_eq "$expected" "$(cat "$selector_stdout")" "$name selected server"
}

assert_artifact_current() {
  local name mod target_manifest server_id
  name="$1"
  mod="$2"
  target_manifest="$3"
  server_id="$4"
  capture_current_artifact "$name" "$mod" "$target_manifest" "$server_id"
  assert_eq "0" "$current_rc" "$name current-artifact exit status"
}

assert_artifact_not_current() {
  local name mod target_manifest server_id expected_stderr
  name="$1"
  mod="$2"
  target_manifest="$3"
  server_id="$4"
  expected_stderr="$5"
  capture_current_artifact "$name" "$mod" "$target_manifest" "$server_id"
  assert_eq "1" "$current_rc" "$name current-artifact exit status"
  assert_eq "" "$(cat "$current_stdout")" "$name current-artifact stdout"
  assert_eq "$expected_stderr" "$(cat "$current_stderr")" "$name current-artifact stderr"
}

assert_no_selection_warn() {
  local name mod target_manifest expected_text
  name="$1"
  mod="$2"
  target_manifest="$3"
  expected_text="$4"
  capture_selector "$name" "$mod" "$target_manifest"
  assert_eq "0" "$selector_rc" "$name exit status"
  assert_eq "" "$(cat "$selector_stdout")" "$name selected server"
  assert_contains "level=warn" "$selector_stderr" "$name warning level"
  if [ -n "$expected_text" ]; then
    assert_contains "$expected_text" "$selector_stderr" "$name warning text"
  fi
}

setup_fixture_files
render_manifest "$manifest"
emby_deps_alt_path="$live_root/emby-alt/EmbyServer.deps.json"
emby_dll_alt_path="$live_root/emby-alt/EmbyServer.dll"

reset_live_files
set_emby_live_primary
assert_selected "emby-exact-match" "emby" "$manifest" "emby-4.9.3"

reset_live_files
set_plex_live_pristine
assert_selected "plex-pristine-match" "plex" "$manifest" "plex-1.43.2"

reset_live_files
set_plex_live_patched
assert_selected "plex-known-patched-match" "plex" "$manifest" "plex-1.43.2"

reset_live_files
set_plex_live_mixed
assert_selected "plex-mixed-pristine-patched-match" "plex" "$manifest" "plex-1.43.2"

ambiguous_manifest="$tmp_root/ambiguous.manifest"
cp "$manifest" "$ambiguous_manifest"
append_emby_alt_server "$ambiguous_manifest"
reset_live_files
set_emby_live_primary
copy_file "$sha_root/emby-deps-alt" "$emby_deps_alt_path"
copy_file "$sha_root/emby-dll-alt" "$emby_dll_alt_path"
assert_no_selection_warn "ambiguous-complete-match" "emby" "$ambiguous_manifest" "ambiguous"

partial_manifest="$manifest"
reset_live_files
copy_file "$sha_root/emby-deps" "$emby_deps_path"
assert_no_selection_warn "partial-detector-match" "emby" "$partial_manifest" "partial"

plex_path_conflict_manifest="$tmp_root/plex-path-conflict.manifest"
plex_pms_conflict_path="$live_root/plex-conflict/Plex Media Server"
awk 'BEGIN { FS=OFS="|" } $1=="detect" && $3=="plex" && $6=="plex_pms:patched" { $7=conflict_path } { print }' conflict_path="$plex_pms_conflict_path" "$manifest" > "$plex_path_conflict_manifest"
reset_live_files
set_plex_live_pristine
capture_selector "plex-divergent-role-paths" "plex" "$plex_path_conflict_manifest"
assert_eq "0" "$selector_rc" "plex divergent role paths exit status"
assert_eq "" "$(cat "$selector_stdout")" "plex divergent role paths selected server"
assert_eq "level=warn component=sqlite3-lsio-mod mod=plex phase=selector event=partial-detector-match arch=$arch" "$(cat "$selector_stderr")" "plex divergent role paths stderr"

reset_live_files
set_emby_live_mismatched
assert_no_selection_warn "present-but-mismatched-detectors" "emby" "$manifest" "mismatch"

reset_live_files
assert_no_selection_warn "zero-detectors" "emby" "$manifest" "zero-detectors"

current_manifest="$tmp_root/current-artifact.manifest"
current_target="$live_root/current/emby/libsqlite3.so.3.49.2"
copy_file "$artifact_root/$arch/generic/libsqlite3.so" "$current_target"
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8=target } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $9=target } { print }' target="$current_target" "$manifest" > "$current_manifest"
reset_live_files
set_emby_live_primary
assert_selected "emby-current-artifact-select" "emby" "$current_manifest" "emby-4.9.3"
assert_artifact_current "emby-current-artifact" "emby" "$current_manifest" "emby-4.9.3"

missing_artifact_row_manifest="$tmp_root/current-missing-artifact-row.manifest"
awk 'BEGIN { FS=OFS="|" } !($1=="artifact" && $3=="emby") { print }' "$current_manifest" > "$missing_artifact_row_manifest"
assert_artifact_not_current \
  "current-missing-artifact-row" \
  "emby" \
  "$missing_artifact_row_manifest" \
  "emby-4.9.3" \
  "level=warn component=sqlite3-lsio-mod mod=emby phase=selector event=missing-artifact-row arch=$arch server_id=emby-4.9.3"

assert_artifact_not_current \
  "current-empty-server-id" \
  "emby" \
  "$current_manifest" \
  "" \
  "level=warn component=sqlite3-lsio-mod mod=emby phase=selector event=missing-artifact-row arch=$arch server_id="

missing_target_manifest="$tmp_root/current-missing-target.manifest"
missing_target="$live_root/current/missing/libsqlite3.so.3.49.2"
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8=target } { print }' target="$missing_target" "$manifest" > "$missing_target_manifest"
assert_artifact_not_current \
  "current-missing-target" \
  "emby" \
  "$missing_target_manifest" \
  "emby-4.9.3" \
  "level=warn component=sqlite3-lsio-mod mod=emby phase=selector event=missing-target arch=$arch server_id=emby-4.9.3 target_path=$missing_target"

sha_failed_manifest="$tmp_root/current-target-sha-failed.manifest"
sha_failed_target="$live_root/current/unreadable/libsqlite3.so.3.49.2"
write_file "$sha_failed_target" "unreadable current artifact"
chmod 000 "$sha_failed_target"
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8=target } { print }' target="$sha_failed_target" "$manifest" > "$sha_failed_manifest"
assert_artifact_not_current \
  "current-target-sha-failed" \
  "emby" \
  "$sha_failed_manifest" \
  "emby-4.9.3" \
  "level=warn component=sqlite3-lsio-mod mod=emby phase=selector event=target-sha-failed arch=$arch server_id=emby-4.9.3 target_path=$sha_failed_target"

mismatch_manifest="$tmp_root/current-artifact-mismatch.manifest"
mismatch_target="$live_root/current/mismatch/libsqlite3.so.3.49.2"
write_file "$mismatch_target" "mismatched current artifact"
mismatch_actual_sha="$(sha256_of "$mismatch_target")"
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8=target } { print }' target="$mismatch_target" "$manifest" > "$mismatch_manifest"
assert_artifact_not_current \
  "current-artifact-mismatch" \
  "emby" \
  "$mismatch_manifest" \
  "emby-4.9.3" \
  "level=warn component=sqlite3-lsio-mod mod=emby phase=selector event=current-artifact-mismatch arch=$arch server_id=emby-4.9.3 target_path=$mismatch_target expected=$emby_artifact_sha actual=$mismatch_actual_sha"

exact_with_partial_manifest="$tmp_root/exact-with-partial.manifest"
cp "$manifest" "$exact_with_partial_manifest"
append_emby_alt_server "$exact_with_partial_manifest"
reset_live_files
set_emby_live_primary
copy_file "$sha_root/emby-deps-alt" "$emby_deps_alt_path"
assert_selected "exact-match-ignores-other-partial" "emby" "$exact_with_partial_manifest" "emby-4.9.3"

cross_product_manifest="$tmp_root/cross-product.manifest"
cp "$manifest" "$cross_product_manifest"
append_emby_cross_product_server "$cross_product_manifest"
reset_live_files
copy_file "$sha_root/emby-deps" "$emby_deps_path"
copy_file "$sha_root/emby-dll-alt" "$emby_dll_path"
assert_no_selection_warn "cross-product-no-complete-match" "emby" "$cross_product_manifest" ""

artifact_mismatch_manifest="$tmp_root/artifact-mismatch.manifest"
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $9="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" } { print }' "$manifest" > "$artifact_mismatch_manifest"
reset_live_files
set_emby_live_primary
assert_selected "detectors-select-with-artifact-sha-mismatch" "emby" "$artifact_mismatch_manifest" "emby-4.9.3"

printf 'selector tests passed\n'
