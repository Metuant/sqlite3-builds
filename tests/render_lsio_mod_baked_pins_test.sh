#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/logging.sh
. ./lsio-mods/shared/cont-init-fragments/sha.sh
. ./lsio-mods/shared/cont-init-fragments/manifest-parser.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-render.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-render.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fixture_inputs="tests/fixtures/render-lsio-mod-baked-pins-inputs"
export RENDER_LSIO_MOD_RUNTIME_SUPPORT="${fixture_inputs}/runtime-support.tsv"
export RENDER_LSIO_MOD_COMPAT_GROUPS="${fixture_inputs}/library-compat-groups.tsv"
export RENDER_LSIO_MOD_RUNTIME_BASELINES="${fixture_inputs}/runtime-baselines.tsv"
export RENDER_LSIO_MOD_POOL_SITES="${fixture_inputs}/plex-pool-patch-sites.tsv"
export RENDER_LSIO_MOD_POOL_REVIEWS="${fixture_inputs}/plex-pool-patch-reviews.tsv"
export RENDER_LSIO_MOD_VERSIONS_ENV="${fixture_inputs}/versions.env"
export MANIFEST_PARSER_ENABLE_FIXTURE_COMPAT_GROUPS=1

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_contains() {
  local file expected
  file="$1"
  expected="$2"
  grep -Fxq "$expected" "$file" || fail "missing expected row: $expected"
}

assert_not_regex() {
  local file pattern
  file="$1"
  pattern="$2"
  if grep -Eq "$pattern" "$file"; then
    fail "unexpected row matching [$pattern] in $file"
  fi
}

assert_file_not_contains() {
  local file unexpected
  file="$1"
  unexpected="$2"
  if grep -Fq -- "$unexpected" "$file"; then
    fail "unexpected text [$unexpected] in $file"
  fi
}

assert_count() {
  local file pattern expected actual
  file="$1"
  pattern="$2"
  expected="$3"
  actual="$(awk -v pattern="$pattern" '$0 ~ pattern { count++ } END { print count + 0 }' "$file")"
  [ "$actual" = "$expected" ] || fail "count for [$pattern]: expected [$expected], actual [$actual]"
}

assert_valid_manifest() {
  local manifest name stdout stderr rc
  manifest="$1"
  name="$2"
  stdout="$tmp/${name}.validator.stdout"
  stderr="$tmp/${name}.validator.stderr"
  baked_pins="$manifest"
  sqlite3_mod_root="$tmp/mod-root"
  sqlite3_mod_artifact_root="$tmp/validator-artifacts"
  set +e
  validate_baked_pins_schema >"$stdout" 2>"$stderr"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "$name validator rejection: rc=[$rc] stdout=[$(cat "$stdout")] stderr=[$(cat "$stderr")]"
  fi
}

expect_failure() {
  local name expected
  name="$1"
  expected="$2"
  shift 2
  if "$@" >"$tmp/${name}.out" 2>"$tmp/${name}.err"; then
    fail "$name unexpectedly succeeded"
  fi
  grep -Fq -- "$expected" "$tmp/${name}.err" || {
    fail "$name error mismatch: expected [$expected], stderr=[$(cat "$tmp/${name}.err")]"
  }
}

write_artifact() {
  local path content
  path="$1"
  content="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' "$content" > "$path"
  sha256_of "$path"
}

stage_validator_artifact() {
  local arch compat source
  arch="$1"
  compat="$2"
  source="$3"
  mkdir -p "$tmp/validator-artifacts/$arch/$compat"
  cp "$source" "$tmp/validator-artifacts/$arch/$compat/libsqlite3.so"
}

render_common_args=(
  --release-tag 2026.05.28-r1
  --generated-at 2026-05-28T00:00:00Z
)

bash tools/lsio-mod/render-lsio-mod-baked-pins.sh --help >"$tmp/render-help.out" 2>"$tmp/render-help.err"
assert_file_not_contains "$tmp/render-help.err" "--pool-baselines"
assert_file_not_contains tools/ci/mod-bake-smoke.sh "--pool-baselines"

pre_fragment="$tmp/pre-fragment.txt"
cat > "$pre_fragment" <<'EOF_PRE'
pre|1|plex|linux-arm64|lscr.io/linuxserver/plex:fixture|sha256:fixture-digest|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|5234567890abcdef5234567890abcdef5234567890abcdef5234567890abcdef
EOF_PRE

plex_v2="$tmp/src/plex/linux-x86_64-v2/libsqlite3.so"
plex_v3="$tmp/src/plex/linux-x86_64-v3/libsqlite3.so"
plex_arm="$tmp/src/plex/linux-arm64/libsqlite3.so"
emby_v2="$tmp/src/emby/linux-x86_64-v2/libsqlite3.so"
emby_v3="$tmp/src/emby/linux-x86_64-v3/libsqlite3.so"
emby_arm="$tmp/src/emby/linux-arm64/libsqlite3.so"
emby_generic2_v2="$tmp/src/emby-generic2/linux-x86_64-v2/libsqlite3.so"
emby_generic2_v3="$tmp/src/emby-generic2/linux-x86_64-v3/libsqlite3.so"
emby_generic2_arm="$tmp/src/emby-generic2/linux-arm64/libsqlite3.so"

plex_v2_sha="$(write_artifact "$plex_v2" "plex replacement linux-x86_64-v2")"
plex_v3_sha="$(write_artifact "$plex_v3" "plex replacement linux-x86_64-v3")"
plex_arm_sha="$(write_artifact "$plex_arm" "plex replacement linux-arm64")"
emby_v2_sha="$(write_artifact "$emby_v2" "emby replacement linux-x86_64-v2")"
emby_v3_sha="$(write_artifact "$emby_v3" "emby replacement linux-x86_64-v3")"
emby_arm_sha="$(write_artifact "$emby_arm" "emby replacement linux-arm64")"
emby_generic2_v2_sha="$(write_artifact "$emby_generic2_v2" "emby generic2 replacement linux-x86_64-v2")"
emby_generic2_v3_sha="$(write_artifact "$emby_generic2_v3" "emby generic2 replacement linux-x86_64-v3")"
emby_generic2_arm_sha="$(write_artifact "$emby_generic2_arm" "emby generic2 replacement linux-arm64")"

sha256sums="$tmp/SHA256SUMS"
cat > "$sha256sums" <<EOF_SUMS
$plex_v2_sha  sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v2.so
$plex_v3_sha  sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v3.so
$plex_arm_sha  sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so
$emby_v2_sha  sqlite-2026.05.28-r1-library-generic-linux-x86_64-v2.so
$emby_v3_sha  sqlite-2026.05.28-r1-library-generic-linux-x86_64-v3.so
$emby_arm_sha  sqlite-2026.05.28-r1-library-generic-linux-arm64.so
$emby_generic2_v2_sha  sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v2.so
$emby_generic2_v3_sha  sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v3.so
$emby_generic2_arm_sha  sqlite-2026.05.28-r1-library-generic2-linux-arm64.so
EOF_SUMS

emby_artifact_args=(
  --artifact "linux-x86_64-v2:generic:sqlite-2026.05.28-r1-library-generic-linux-x86_64-v2.so:$emby_v2:/app/emby/lib/libsqlite3.so.3.49.2"
  --artifact "linux-x86_64-v3:generic:sqlite-2026.05.28-r1-library-generic-linux-x86_64-v3.so:$emby_v3:/app/emby/lib/libsqlite3.so.3.49.2"
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$emby_arm:/app/emby/lib/libsqlite3.so.3.49.2"
  --artifact "linux-x86_64-v2:generic2:sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v2.so:$emby_generic2_v2:/app/emby/lib/libsqlite3.so.3.49.2"
  --artifact "linux-x86_64-v3:generic2:sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v3.so:$emby_generic2_v3:/app/emby/lib/libsqlite3.so.3.49.2"
  --artifact "linux-arm64:generic2:sqlite-2026.05.28-r1-library-generic2-linux-arm64.so:$emby_generic2_arm:/app/emby/lib/libsqlite3.so.3.49.2"
)
stage_validator_artifact linux-x86_64-v2 icu69 "$plex_v2"
stage_validator_artifact linux-x86_64-v3 icu69 "$plex_v3"
stage_validator_artifact linux-arm64 icu69 "$plex_arm"
stage_validator_artifact linux-x86_64-v2 generic "$emby_v2"
stage_validator_artifact linux-x86_64-v3 generic "$emby_v3"
stage_validator_artifact linux-arm64 generic "$emby_arm"
stage_validator_artifact linux-x86_64-v2 generic2 "$emby_generic2_v2"
stage_validator_artifact linux-x86_64-v3 generic2 "$emby_generic2_v3"
stage_validator_artifact linux-arm64 generic2 "$emby_generic2_arm"

plex_manifest="$tmp/baked-pins-plex.txt"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v2.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-x86_64-v3:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v3.so:$plex_v3:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$plex_manifest"

assert_contains "$plex_manifest" '# baked-pins schema=3'
assert_contains "$plex_manifest" 'meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z'
assert_count "$plex_manifest" '^meta[|]3[|]' 1
assert_count "$plex_manifest" '^detect[|]1[|]plex[|]plex-fixture[|]' 12
assert_count "$plex_manifest" '^artifact[|]1[|]plex[|]plex-fixture[|]' 3
assert_count "$plex_manifest" '^pre[|]2[|]plex[|]plex-fixture[|].*[|]plex_icu_linked:' 9
assert_count "$plex_manifest" '^pool-site[|]1[|]plex[|]plex-fixture[|]' 6
assert_contains "$plex_manifest" "detect|1|plex|plex-fixture|linux-x86_64-v2|plex_pms:pristine|/usr/lib/plexmediaserver/Plex Media Server|1111111111111111111111111111111111111111111111111111111111111111"
assert_contains "$plex_manifest" "artifact|1|plex|plex-fixture|linux-x86_64-v2|icu69|artifacts/linux-x86_64-v2/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$plex_v2_sha"
assert_contains "$plex_manifest" "artifact|1|plex|plex-fixture|linux-x86_64-v3|icu69|artifacts/linux-x86_64-v3/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$plex_v3_sha"
assert_contains "$plex_manifest" "artifact|1|plex|plex-fixture|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$plex_arm_sha"
assert_contains "$plex_manifest" 'pre|2|plex|plex-fixture|linux-arm64|target_sqlite|lscr.io/linuxserver/plex:fixture|sha256:fixture-digest|/usr/lib/plexmediaserver/lib/libsqlite3.so|5234567890abcdef5234567890abcdef5234567890abcdef5234567890abcdef'
assert_contains "$plex_manifest" 'pre|2|plex|plex-fixture|linux-x86_64-v2|plex_icu_linked:libicuucplex.so.69|lscr.io/linuxserver/plex:fixture|-|/usr/lib/plexmediaserver/lib/libicuucplex.so.69|6666666666666666666666666666666666666666666666666666666666666666'
assert_contains "$plex_manifest" 'pool-site|1|plex|plex-fixture|linux-x86_64-v2|/usr/lib/plexmediaserver/Plex Media Server|pms-small-pool|10|11|be140000004c89ff41b800000000506a|be100000004c89ff41b800000000506a'
assert_contains "$plex_manifest" 'pool-site|1|plex|plex-fixture|linux-arm64|/usr/lib/plexmediaserver/Plex Media Scanner|scanner-small-pool|30|30|81028052e00313aae4031f2ae7031f2a|01028052e00313aae4031f2ae7031f2a'
assert_not_regex "$plex_manifest" '^(version[|]2[|]|current[|]|pre[|]1[|]|pool-pre[|]|managed_window[|])'
assert_not_regex "$plex_manifest" '[|]runtime[|]'
assert_valid_manifest "$plex_manifest" "plex-render"

hidden_pool_baselines_manifest="$tmp/baked-pins-hidden-pool-baselines.txt"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --pool-baselines "$tmp/missing-pool-baselines.txt" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$hidden_pool_baselines_manifest"
assert_valid_manifest "$hidden_pool_baselines_manifest" "plex-hidden-pool-baselines-render"

prefixed_compat_groups="$tmp/library-compat-groups-prefixed.tsv"
awk 'BEGIN { FS=OFS="\t" } $1=="icu69" { $4="library-plex-icu69" } { print }' "$fixture_inputs/library-compat-groups.tsv" > "$prefixed_compat_groups"
orig_compat_groups="$RENDER_LSIO_MOD_COMPAT_GROUPS"
RENDER_LSIO_MOD_COMPAT_GROUPS="$prefixed_compat_groups"
prefixed_manifest="$tmp/baked-pins-prefixed-stem.txt"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:icu69:ignored-plex-v2.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-x86_64-v3:icu69:ignored-plex-v3.so:$plex_v3:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-arm64:icu69:ignored-plex-arm.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$prefixed_manifest"
assert_contains "$prefixed_manifest" "artifact|1|plex|plex-fixture|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$plex_arm_sha"
assert_valid_manifest "$prefixed_manifest" "plex-prefixed-stem-render"
RENDER_LSIO_MOD_COMPAT_GROUPS="$orig_compat_groups"

legacy_plex_sha256sums="$tmp/SHA256SUMS.legacy-plex"
cat > "$legacy_plex_sha256sums" <<EOF_LEGACY_PLEX_SUMS
$plex_v2_sha  sqlite-2026.05.28-r1-library-plex-linux-x86_64-v2.so
$plex_v3_sha  sqlite-2026.05.28-r1-library-plex-linux-x86_64-v3.so
$plex_arm_sha  sqlite-2026.05.28-r1-library-plex-linux-arm64.so
EOF_LEGACY_PLEX_SUMS
expect_failure legacy-plex-name 'FATAL: missing SHA256SUMS entry for sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v2.so' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$legacy_plex_sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v2.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-x86_64-v3:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v3.so:$plex_v3:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/baked-pins-legacy-plex-name.txt"

emby_manifest="$tmp/baked-pins-emby.txt"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  "${emby_artifact_args[@]}" \
  --output "$emby_manifest"

assert_contains "$emby_manifest" '# baked-pins schema=3'
assert_contains "$emby_manifest" 'meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z'
assert_count "$emby_manifest" '^detect[|]1[|]emby[|]emby-fixture[|]' 6
assert_count "$emby_manifest" '^artifact[|]1[|]emby[|]emby-fixture[|]' 3
assert_count "$emby_manifest" '^pre[|]2[|]emby[|]emby-fixture[|]' 3
assert_count "$emby_manifest" '^artifact[|]1[|]emby[|]emby-fixture-alt-target[|]' 3
assert_count "$emby_manifest" '^artifact[|]1[|]emby[|]emby-fixture-generic2[|]' 3
assert_contains "$emby_manifest" "artifact|1|emby|emby-fixture|linux-x86_64-v3|generic|artifacts/linux-x86_64-v3/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$emby_v3_sha"
assert_contains "$emby_manifest" "artifact|1|emby|emby-fixture-alt-target|linux-arm64|generic|artifacts/linux-arm64/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.50.0|$emby_arm_sha"
assert_contains "$emby_manifest" "artifact|1|emby|emby-fixture-generic2|linux-arm64|generic2|artifacts/linux-arm64/generic2/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$emby_generic2_arm_sha"
assert_not_regex "$emby_manifest" '^(version[|]2[|]|current[|]|pre[|]1[|]|pool-pre[|]|managed_window[|]|pool-site[|])'
assert_valid_manifest "$emby_manifest" "emby-render"

legacy_emby_sha256sums="$tmp/SHA256SUMS.legacy-emby"
cat > "$legacy_emby_sha256sums" <<EOF_LEGACY_EMBY_SUMS
$emby_v2_sha  sqlite-2026.05.28-r1-library-linux-x86_64-v2.so
$emby_v3_sha  sqlite-2026.05.28-r1-library-linux-x86_64-v3.so
$emby_arm_sha  sqlite-2026.05.28-r1-library-linux-arm64.so
EOF_LEGACY_EMBY_SUMS
expect_failure legacy-emby-name 'FATAL: missing SHA256SUMS entry for sqlite-2026.05.28-r1-library-generic-linux-x86_64-v2.so' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$legacy_emby_sha256sums" \
  --pre-fragment "$pre_fragment" \
  "${emby_artifact_args[@]}" \
  --output "$tmp/baked-pins-legacy-emby-name.txt"

expect_failure missing-artifact-file 'FATAL: missing sqlite artifact:' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$tmp/missing-libsqlite3.so:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-arm64:generic2:sqlite-2026.05.28-r1-library-generic2-linux-arm64.so:$emby_generic2_arm:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/missing-artifact-file.txt"

expect_failure missing-compat-group-artifact 'FATAL: missing --artifact for compat_group=generic2 arch=linux-arm64' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$emby_arm:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/missing-compat-group-artifact.txt"

expect_failure missing-x86-peer 'FATAL: missing paired x86_64 artifact class for compat_group=generic: linux-x86_64-v3' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:generic:sqlite-2026.05.28-r1-library-generic-linux-x86_64-v2.so:$emby_v2:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/missing-x86-peer.txt"

tampered_plex_arm="$tmp/src/plex/linux-arm64/tampered-libsqlite3.so"
printf '%s\n' "tampered plex replacement linux-arm64" > "$tampered_plex_arm"
expect_failure artifact-sha-mismatch 'FATAL: artifact SHA mismatch' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$tampered_plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/artifact-sha-mismatch.txt"

one_review_pool_reviews="$tmp/plex-pool-patch-reviews-one-review.tsv"
awk 'BEGIN { FS=OFS="\t" } !($1=="plex-fixture" && $2=="linux-arm64" && $3=="/usr/lib/plexmediaserver/Plex Media Server" && $4=="pms-small-pool" && $5=="10" && $10=="Deriver B") { print }' "$fixture_inputs/plex-pool-patch-reviews.tsv" > "$one_review_pool_reviews"
orig_pool_reviews="$RENDER_LSIO_MOD_POOL_REVIEWS"
RENDER_LSIO_MOD_POOL_REVIEWS="$one_review_pool_reviews"
expect_failure pool-review-count 'FATAL: missing approved pool-site reviews for plex-fixture linux-arm64 /usr/lib/plexmediaserver/Plex Media Server pms-small-pool 10' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/pool-review-count.txt"
RENDER_LSIO_MOD_POOL_REVIEWS="$orig_pool_reviews"

mutated_content_pool_sites="$tmp/plex-pool-patch-sites-mutated-content.tsv"
awk 'BEGIN { FS=OFS="\t" } $1=="plex-fixture" && $2=="linux-arm64" && $3=="/usr/lib/plexmediaserver/Plex Media Server" && $5=="pms-small-pool" && $6=="10" { $9="02028052e00313aae4031f2ae7031f2a" } { print }' "$fixture_inputs/plex-pool-patch-sites.tsv" > "$mutated_content_pool_sites"
orig_pool_sites="$RENDER_LSIO_MOD_POOL_SITES"
RENDER_LSIO_MOD_POOL_SITES="$mutated_content_pool_sites"
expect_failure pool-review-content-mismatch 'FATAL: missing approved pool-site reviews for plex-fixture linux-arm64 /usr/lib/plexmediaserver/Plex Media Server pms-small-pool 10' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/pool-review-content-mismatch.txt"
RENDER_LSIO_MOD_POOL_SITES="$orig_pool_sites"

exact_site_pool_sites="$tmp/plex-pool-patch-sites-exact-site.tsv"
awk 'BEGIN { FS=OFS="\t" } { print } $1=="plex-fixture" && $2=="linux-arm64" && $3=="/usr/lib/plexmediaserver/Plex Media Scanner" {
  print $1,$2,$3,$4,"scanner-second-pool","31","31",$8,$9
}' "$fixture_inputs/plex-pool-patch-sites.tsv" > "$exact_site_pool_sites"
partial_exact_site_pool_reviews="$tmp/plex-pool-patch-reviews-exact-site-partial.tsv"
cp "$fixture_inputs/plex-pool-patch-reviews.tsv" "$partial_exact_site_pool_reviews"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  plex-fixture linux-arm64 "/usr/lib/plexmediaserver/Plex Media Scanner" scanner-second-pool 31 31 81028052e00313aae4031f2ae7031f2a 01028052e00313aae4031f2ae7031f2a fixture-review "Deriver A" approved \
  >> "$partial_exact_site_pool_reviews"
orig_pool_sites="$RENDER_LSIO_MOD_POOL_SITES"
RENDER_LSIO_MOD_POOL_SITES="$exact_site_pool_sites"
RENDER_LSIO_MOD_POOL_REVIEWS="$partial_exact_site_pool_reviews"
expect_failure pool-review-exact-site 'FATAL: missing approved pool-site reviews for plex-fixture linux-arm64 /usr/lib/plexmediaserver/Plex Media Scanner scanner-second-pool 31' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/pool-review-exact-site.txt"
complete_exact_site_pool_reviews="$tmp/plex-pool-patch-reviews-exact-site-complete.tsv"
cp "$partial_exact_site_pool_reviews" "$complete_exact_site_pool_reviews"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  plex-fixture linux-arm64 "/usr/lib/plexmediaserver/Plex Media Scanner" scanner-second-pool 31 31 81028052e00313aae4031f2ae7031f2a 01028052e00313aae4031f2ae7031f2a fixture-review "Deriver B" approved \
  >> "$complete_exact_site_pool_reviews"
RENDER_LSIO_MOD_POOL_REVIEWS="$complete_exact_site_pool_reviews"
exact_site_manifest="$tmp/baked-pins-exact-site.txt"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$exact_site_manifest"
assert_contains "$exact_site_manifest" 'pool-site|1|plex|plex-fixture|linux-arm64|/usr/lib/plexmediaserver/Plex Media Scanner|scanner-second-pool|31|31|81028052e00313aae4031f2ae7031f2a|01028052e00313aae4031f2ae7031f2a'
assert_valid_manifest "$exact_site_manifest" "plex-exact-site-render"
RENDER_LSIO_MOD_POOL_SITES="$orig_pool_sites"
RENDER_LSIO_MOD_POOL_REVIEWS="$orig_pool_reviews"

pool_baseline_mismatch="$tmp/plex-pool-patch-sites-baseline-mismatch.tsv"
awk 'BEGIN { FS=OFS="\t" } $1=="plex-fixture" && $2=="linux-arm64" && $3=="/usr/lib/plexmediaserver/Plex Media Server" { $4="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" } { print }' "$fixture_inputs/plex-pool-patch-sites.tsv" > "$pool_baseline_mismatch"
RENDER_LSIO_MOD_POOL_SITES="$pool_baseline_mismatch"
expect_failure pool-baseline-mismatch 'FATAL: pool-site baseline mismatch for plex-fixture linux-arm64 /usr/lib/plexmediaserver/Plex Media Server' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/pool-baseline-mismatch.txt"
RENDER_LSIO_MOD_POOL_SITES="$orig_pool_sites"

pool_noop="$tmp/plex-pool-patch-sites-noop.tsv"
awk 'BEGIN { FS=OFS="\t" } $1=="plex-fixture" && $2=="linux-arm64" && $3=="/usr/lib/plexmediaserver/Plex Media Server" { $9=$8 } { print }' "$fixture_inputs/plex-pool-patch-sites.tsv" > "$pool_noop"
RENDER_LSIO_MOD_POOL_SITES="$pool_noop"
expect_failure pool-site-noop 'FATAL: pool-site patched_hex must differ from original_hex at exactly write_seek-offset byte at line' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/pool-site-noop.txt"
RENDER_LSIO_MOD_POOL_SITES="$orig_pool_sites"

duplicate_support="$tmp/runtime-support-duplicate.tsv"
cat "$fixture_inputs/runtime-support.tsv" > "$duplicate_support"
printf '%s\n' 'plex	plex-fixture	lscr.io/linuxserver/plex:fixture	icu69	supported	fixture-review	duplicate fixture plex support row' >> "$duplicate_support"
orig_runtime_support="$RENDER_LSIO_MOD_RUNTIME_SUPPORT"
RENDER_LSIO_MOD_RUNTIME_SUPPORT="$duplicate_support"
expect_failure duplicate-server-id 'FATAL: duplicate supported server_id: plex-fixture' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/duplicate-server-id.txt"
RENDER_LSIO_MOD_RUNTIME_SUPPORT="$orig_runtime_support"

x86_alias_sha256sums="$tmp/SHA256SUMS.x86-alias"
cat > "$x86_alias_sha256sums" <<EOF_X86_ALIAS_SUMS
$plex_v2_sha  sqlite-shared-x86-alias.so
EOF_X86_ALIAS_SUMS
expect_failure x86-aliasing 'FATAL: x86_64 artifact classes alias the same artifact name' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$x86_alias_sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:icu69:sqlite-shared-x86-alias.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-x86_64-v3:icu69:sqlite-shared-x86-alias.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/x86-aliasing.txt"

emby_alias_baselines="$tmp/runtime-baselines-emby-alias.tsv"
awk 'BEGIN { FS=OFS="\t" } $1=="pre" && $2=="emby" && $3=="emby-fixture" && $4=="linux-arm64" && $5=="target_sqlite" { $8="/app/emby/lib/libsqlite3.so.3" } { print }' "$fixture_inputs/runtime-baselines.tsv" > "$emby_alias_baselines"
orig_runtime_baselines="$RENDER_LSIO_MOD_RUNTIME_BASELINES"
RENDER_LSIO_MOD_RUNTIME_BASELINES="$emby_alias_baselines"
expect_failure emby-alias-target 'FATAL: invalid Emby target_sqlite path for emby-fixture linux-arm64: /app/emby/lib/libsqlite3.so.3' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$emby_arm:/app/emby/lib/libsqlite3.so.3" \
  --output "$tmp/emby-alias-target.txt"
RENDER_LSIO_MOD_RUNTIME_BASELINES="$orig_runtime_baselines"

bad_pre="$tmp/bad-pre-fragment.txt"
cat > "$bad_pre" <<'EOF_BAD_PRE'
pre|1|emby|linux-arm64|lscr.io/linuxserver/emby:fixture|sha256:fixture-digest|/app/emby/lib/libsqlite3.so.3.49.2|runtime|not-a-sha
EOF_BAD_PRE
expect_failure bad-pre 'FATAL: invalid pre fragment SHA:' \
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$bad_pre" \
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$emby_arm:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/bad-pre-output.txt"

if bash tools/lsio-mod/render-lsio-mod-baked-pins.sh --mod >"$tmp/render-dangling.out" 2>"$tmp/render-dangling.err"; then
  fail "dangling renderer option was accepted"
fi

printf 'baked pins renderer tests passed\n'
