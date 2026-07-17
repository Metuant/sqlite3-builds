#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/logging.sh
. ./lsio-mods/shared/cont-init-fragments/sha.sh
. ./lsio-mods/shared/cont-init-fragments/manifest-parser.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/manifest-parser-test.XXXXXX" 2>/dev/null || mktemp -d /tmp/manifest-parser-test.XXXXXX)"
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
valid_manifest="$tmp_root/valid.manifest"

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_eq() {
  expected="$1"
  actual="$2"
  context="$3"
  [ "$expected" = "$actual" ] || fail "$context: expected [$expected], actual [$actual]"
}

write_file() {
  path="$1"
  text="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' "$text" > "$path"
}

setup_fixture_files() {
  plex_pms_path="$live_root/plex/Plex Media Server"
  plex_scanner_path="$live_root/plex/Plex Media Scanner"
  emby_deps_path="$live_root/emby/EmbyServer.deps.json"
  emby_dll_path="$live_root/emby/EmbyServer.dll"

  write_file "$sha_root/plex-pms-pristine" "plex pms pristine detector"
  write_file "$sha_root/plex-pms-patched" "plex pms known patched detector"
  write_file "$sha_root/plex-pms-source-id-patched" "plex pms source-id patched detector"
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
  plex_pms_source_id_patched_sha="$(sha256_of "$sha_root/plex-pms-source-id-patched")"
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
  output="$1"
  sed \
    -e "s|@ARCH@|$arch|g" \
    -e "s|@PLEX_PMS_PATH@|$plex_pms_path|g" \
    -e "s|@PLEX_SCANNER_PATH@|$plex_scanner_path|g" \
    -e "s|@EMBY_DEPS_PATH@|$emby_deps_path|g" \
    -e "s|@EMBY_DLL_PATH@|$emby_dll_path|g" \
    -e "s|@PLEX_PMS_PRISTINE_SHA@|$plex_pms_pristine_sha|g" \
    -e "s|@PLEX_PMS_PATCHED_SHA@|$plex_pms_patched_sha|g" \
    -e "s|@PLEX_PMS_SOURCE_ID_PATCHED_SHA@|$plex_pms_source_id_patched_sha|g" \
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

normalize_valid_pool_site_contexts() {
  manifest="$1"
  awk 'BEGIN { FS=OFS="|" }
    $1=="pool-site" && $6==pms { $10="00112233445566778899aabbccddeeff"; $11="ff112233445566778899aabbccddeeff" }
    $1=="pool-site" && $6==scanner { $10="11112222333344445555666677778888"; $11="11112222333344aa5555666677778888" }
    { print }' pms="$plex_pms_path" scanner="$plex_scanner_path" "$manifest" > "$manifest.tmp"
  mv "$manifest.tmp" "$manifest"
}

capture_validator() {
  manifest="$1"
  name="$2"
  validator_stdout="$tmp_root/${name}.stdout"
  validator_stderr="$tmp_root/${name}.stderr"
  # shellcheck disable=SC2034
  baked_pins="$manifest"
  # shellcheck disable=SC2034
  sqlite3_mod_root="$mod_root"
  # shellcheck disable=SC2034
  sqlite3_mod_artifact_root="$artifact_root"
  set +e
  validate_baked_pins_schema >"$validator_stdout" 2>"$validator_stderr"
  validator_rc=$?
  set -e
}

assert_valid() {
  manifest="$1"
  context="$2"
  capture_validator "$manifest" "$context"
  if [ "$validator_rc" -ne 0 ]; then
    fail "$context rejected valid manifest: rc=[$validator_rc] stdout=[$(cat "$validator_stdout")] stderr=[$(cat "$validator_stderr")]"
  fi
}

assert_invalid() {
  manifest="$1"
  context="$2"
  capture_validator "$manifest" "$context"
  if [ "$validator_rc" -eq 0 ]; then
    fail "$context accepted invalid manifest"
  fi
}

assert_invalid_reason() {
  manifest="$1"
  context="$2"
  expected="$3"
  capture_validator "$manifest" "$context"
  if [ "$validator_rc" -eq 0 ]; then
    fail "$context accepted invalid manifest"
  fi
  grep -Fq -- "$expected" "$validator_stderr" || {
    fail "$context reason mismatch: expected [$expected], stderr=[$(cat "$validator_stderr")]"
  }
}

copy_bad() {
  name="$1"
  bad="$tmp_root/${name}.manifest"
  cp "$valid_manifest" "$bad"
}

setup_fixture_files
render_manifest "$valid_manifest"
normalize_valid_pool_site_contexts "$valid_manifest"

assert_valid "$valid_manifest" "valid-v3"
assert_eq "$plex_pms_path|$plex_pms_pristine_sha" \
  "$(manifest_parser_selected_pristine_detector_row plex "$arch" "$valid_manifest" plex-1.43.2 plex_pms)" \
  "selected pristine Plex PMS detector"
assert_eq "$plex_pms_path|$plex_pms_patched_sha" \
  "$(manifest_parser_selected_patched_detector_row plex "$arch" "$valid_manifest" plex-1.43.2 plex_pms)" \
  "selected patched Plex PMS detector"
if manifest_parser_selected_patched_detector_row emby "$arch" "$valid_manifest" emby-4.9.3 emby_dll >/dev/null; then
  fail "patched detector selector accepted an Emby role"
fi

copy_bad pre-image-digest-opaque
awk 'BEGIN { FS=OFS="|" } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $8="not-a-digest" } { print }' "$valid_manifest" > "$bad"
assert_valid "$bad" "pre-image-digest-opaque"

unsupported_local_manifest="$tmp_root/unsupported-local.manifest"
awk 'BEGIN { FS=OFS="|" } !($1=="artifact" && $3=="emby") { print } END { print "unsupported|1|emby|emby-4.9.3|" arch "|generic|local-offline-missing-artifact" }' arch="$arch" "$valid_manifest" > "$unsupported_local_manifest"
assert_valid "$unsupported_local_manifest" "unsupported-local-replaces-artifact"

assert_invalid "$tmp_root/missing.manifest" "missing-manifest"

copy_bad unknown-kind
printf '%s\n' 'bogus|1|emby' >> "$bad"
assert_invalid "$bad" "unknown-row-kind"

copy_bad missing-meta
awk -F'|' '$1!="meta" { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-meta-row"

copy_bad duplicate-meta
printf '%s\n' 'meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z' >> "$bad"
assert_invalid "$bad" "duplicate-meta-row"

copy_bad meta-arity
printf '%s\n' 'meta|3|release_tag|2026.05.28-r1|generated_at' >> "$bad"
assert_invalid "$bad" "meta-arity"

copy_bad detect-arity
printf '%s\n' 'detect|1|emby|emby-bad' >> "$bad"
assert_invalid "$bad" "detect-arity"

copy_bad artifact-arity
printf '%s\n' 'artifact|1|emby|emby-bad' >> "$bad"
assert_invalid "$bad" "artifact-arity"

copy_bad pre-arity
printf '%s\n' 'pre|2|emby|emby-bad' >> "$bad"
assert_invalid "$bad" "pre-arity"

copy_bad pool-site-arity
printf '%s\n' 'pool-site|1|plex|plex-bad' >> "$bad"
assert_invalid "$bad" "pool-site-arity"

copy_bad unsupported-arity
printf '%s\n' 'unsupported|1|emby|emby-bad' >> "$bad"
assert_invalid "$bad" "unsupported-arity"

copy_bad unsupported-artifact-mutual-exclusion
printf '%s\n' "unsupported|1|emby|emby-4.9.3|$arch|generic|local-offline-missing-artifact" >> "$bad"
assert_invalid "$bad" "unsupported-artifact-mutual-exclusion"

copy_bad empty-field
awk 'BEGIN { FS=OFS="|" } $1=="detect" && $3=="emby" && $6=="emby_deps" { $7="" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "empty-field"

copy_bad sentinel-outside-pre-image-digest
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $7="-" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "sentinel-outside-pre-image-digest"

copy_bad invalid-sha
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $9="not-a-sha" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "invalid-sha"

copy_bad invalid-hex-context
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $10="00112233445566778899aabbccddee" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid "$bad" "invalid-hex-context"

copy_bad duplicate-row-key
awk -F'|' '$1=="detect" && $3=="emby" && $6=="emby_deps" { print; exit }' "$valid_manifest" >> "$bad"
assert_invalid "$bad" "duplicate-row-key"

copy_bad duplicate-pool-site-row-key
awk -F'|' '$1=="pool-site" && $3=="plex" { print; exit }' "$valid_manifest" >> "$bad"
assert_invalid_reason "$bad" "duplicate-pool-site-row-key" "reason=duplicate-row-key"

copy_bad emby-detector-count
awk 'BEGIN { FS=OFS="|" } !($1=="detect" && $3=="emby" && $6=="emby_dll") { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "emby-detector-count"

copy_bad plex-detector-requirement
awk 'BEGIN { FS=OFS="|" } !($1=="detect" && $3=="plex" && $6=="plex_pms:source-id-patched") { print }' "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "plex-detector-requirement" "reason=plex-dual-detector-requirement"

copy_bad plex-detector-path-conflict
awk 'BEGIN { FS=OFS="|" } $1=="detect" && $3=="plex" && $6=="plex_pms:patched" { $7=$7 ".patched" } { print }' "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "plex-detector-path-conflict" "reason=plex-detector-path-conflict"

copy_bad plex-source-id-detector-path-conflict
awk 'BEGIN { FS=OFS="|" } $1=="detect" && $3=="plex" && $6=="plex_pms:source-id-patched" { $7=$7 ".source-id-patched" } { print }' "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "plex-source-id-detector-path-conflict" "reason=plex-detector-path-conflict"

copy_bad duplicate-canonical-detector-set
awk 'BEGIN { FS=OFS="|" } $1!="meta" && $3=="plex" { $4="plex-duplicate"; print }' "$valid_manifest" >> "$bad"
assert_invalid "$bad" "duplicate-canonical-detector-set"

copy_bad missing-artifact-row
awk 'BEGIN { FS=OFS="|" } !($1=="artifact" && $3=="emby") { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-artifact-row"

copy_bad missing-artifact-file
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $7="artifacts/linux-x86_64-v3/generic/missing.so" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-artifact-file"

copy_bad artifact-sha-mismatch
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $9="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "artifact-sha-mismatch"

copy_bad missing-pre-target-sqlite
awk 'BEGIN { FS=OFS="|" } !($1=="pre" && $3=="emby" && $6=="target_sqlite") { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-pre-target-sqlite"

copy_bad plex-artifact-target-allowlist
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="plex" { $8="/tmp/libsqlite3.so" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "plex-artifact-target-allowlist"

copy_bad plex-pre-target-allowlist
awk 'BEGIN { FS=OFS="|" } $1=="pre" && $3=="plex" && $6=="target_sqlite" { $9="/tmp/libsqlite3.so" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "plex-pre-target-allowlist"

copy_bad emby-target-allowlist
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8="/usr/lib/jellyfin/bin/libe_sqlite3.so" } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $9="/usr/lib/jellyfin/bin/libe_sqlite3.so" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "emby-target-allowlist"

copy_bad emby-major-alias-target
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8="/app/emby/lib/libsqlite3.so.3" } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $9="/app/emby/lib/libsqlite3.so.3" } { print }' "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "emby-major-alias-target" "reason=emby-target-allowlist"

copy_bad emby-zero-alias-target
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8="/app/emby/lib/libsqlite3.so.0" } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $9="/app/emby/lib/libsqlite3.so.0" } { print }' "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "emby-zero-alias-target" "reason=emby-target-allowlist"

copy_bad emby-alias-symlink
awk 'BEGIN { FS=OFS="|" } $1=="artifact" && $3=="emby" { $8="/app/emby/lib/libsqlite3.so" } $1=="pre" && $3=="emby" && $6=="target_sqlite" { $9="/app/emby/lib/libsqlite3.so" } { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "emby-alias-symlink"

copy_bad missing-plex-icu-pre
awk 'BEGIN { FS=OFS="|" } !($1=="pre" && $3=="plex" && $6=="plex_icu_linked:libicuucplex.so.69") { print }' "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-plex-icu-pre"

copy_bad missing-plex-pool-site
awk 'BEGIN { FS=OFS="|" } !($1=="pool-site" && $6==p) { print }' p="$plex_scanner_path" "$valid_manifest" > "$bad"
assert_invalid "$bad" "missing-plex-pool-site"

copy_bad pool-site-not-pristine-detector
printf '%s\n' "pool-site|1|plex|plex-1.43.2|$arch|/tmp/not-a-pristine-detector|pool-open-flags|0|0|00112233445566778899aabbccddeeff|ffeeddccbbaa99887766554433221100" >> "$bad"
assert_invalid "$bad" "pool-site-not-pristine-detector"

copy_bad pool-site-write-seek-out-of-range
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $8="0"; $9="16" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid "$bad" "pool-site-write-seek-out-of-range"

copy_bad pool-site-write-seek-negative
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $8="15"; $9="0" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid "$bad" "pool-site-write-seek-negative"

copy_bad pool-site-noop-context
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $11=$10 } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "pool-site-noop-context" "reason=pool-site-byte-diff"

copy_bad pool-site-multibyte-context
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $10="00112233445566778899aabbccddeeff"; $11="ffeeddccbbaa99887766554433221100" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "pool-site-multibyte-context" "reason=pool-site-byte-diff"

copy_bad pool-site-wrong-byte-context
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $8="0"; $9="1"; $10="00112233445566778899aabbccddeeff"; $11="ff112233445566778899aabbccddeeff" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "pool-site-wrong-byte-context" "reason=pool-site-byte-diff"

copy_bad pool-site-leading-zero-offset
awk 'BEGIN { FS=OFS="|" } $1=="pool-site" && $6==p { $8="010"; $9="020"; $10="000102030405060708090a0b0c0d0e0f"; $11="00010203040506070809ff0b0c0d0e0f" } { print }' p="$plex_pms_path" "$valid_manifest" > "$bad"
assert_invalid_reason "$bad" "pool-site-leading-zero-offset" "reason=pool-site-offset-leading-zero"

copy_bad legacy-current-row
printf '%s\n' 'current|1|emby|linux-x86_64-v3|libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' >> "$bad"
assert_invalid "$bad" "legacy-current-row"

copy_bad legacy-pool-pre-row
printf '%s\n' 'pool-pre|1|plex|linux-x86_64-v3|/usr/lib/plexmediaserver/Plex Media Server|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' >> "$bad"
assert_invalid "$bad" "legacy-pool-pre-row"

copy_bad legacy-version-v2-row
printf '%s\n' 'version|2|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z' >> "$bad"
assert_invalid "$bad" "legacy-version-v2-row"

copy_bad non-v2-version-row
printf '%s\n' 'version|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z' >> "$bad"
assert_invalid_reason "$bad" "non-v2-version-row" "reason=unknown-row-kind"

copy_bad legacy-managed-window-row
printf '%s\n' 'managed_window|1|plex|linux-x86_64-v3|1.43.2' >> "$bad"
assert_invalid "$bad" "legacy-managed-window-row"

printf 'manifest parser tests passed\n'
