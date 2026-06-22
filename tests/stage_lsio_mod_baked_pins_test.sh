#!/usr/bin/env bash
set -euo pipefail

# shellcheck disable=SC1091
# shellcheck source=lsio-mods/shared/cont-init-fragments/logging.sh
. ./lsio-mods/shared/cont-init-fragments/logging.sh
# shellcheck disable=SC1091
# shellcheck source=lsio-mods/shared/cont-init-fragments/sha.sh
. ./lsio-mods/shared/cont-init-fragments/sha.sh
# shellcheck disable=SC1091
# shellcheck source=lsio-mods/shared/cont-init-fragments/manifest-parser.sh
. ./lsio-mods/shared/cont-init-fragments/manifest-parser.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-stage.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-stage.XXXXXX)"

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

write_artifact() {
  local path content
  path="$1"
  content="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' "$content" > "$path"
  sha256_of "$path"
}

assert_valid_staged_manifest() {
  local staged name stdout stderr rc
  staged="$1"
  name="$2"
  stdout="$tmp/${name}.validator.stdout"
  stderr="$tmp/${name}.validator.stderr"
  export baked_pins="$staged/root-fs/opt/sqlite3-lsio-mod/baked-pins.txt"
  export sqlite3_mod_root="$staged/root-fs/opt/sqlite3-lsio-mod"
  export sqlite3_mod_artifact_root=""

  set +e
  validate_baked_pins_schema >"$stdout" 2>"$stderr"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    fail "$name validator rejection: rc=[$rc] stdout=[$(cat "$stdout")] stderr=[$(cat "$stderr")]"
  fi
}

assert_staged_artifact() {
  local staged arch compat source expected actual relpath legacy_path
  staged="$1"
  arch="$2"
  compat="$3"
  source="$4"
  relpath="root-fs/opt/sqlite3-lsio-mod/artifacts/${arch}/${compat}/libsqlite3.so"
  legacy_path="$staged/root-fs/opt/sqlite3-lsio-mod/artifacts/${arch}/libsqlite3.so"

  [ -f "$staged/$relpath" ] || fail "missing staged artifact: $relpath"
  expected="$(sha256_of "$source")"
  actual="$(sha256_of "$staged/$relpath")"
  [ "$actual" = "$expected" ] || fail "staged artifact SHA mismatch for $relpath: expected [$expected], actual [$actual]"
  [ ! -e "$legacy_path" ] || fail "unexpected schema-v2 staged artifact path: $legacy_path"
}

render_common_args=(
  --release-tag 2026.05.28-r1
  --generated-at 2026-05-28T00:00:00Z
)

pre_fragment="$tmp/pre-fragment.txt"
pool_baselines="$tmp/pool-baselines.txt"
: > "$pre_fragment"
: > "$pool_baselines"

plex_v2="$tmp/src/plex/linux-x86_64-v2/libsqlite3.so"
plex_v3="$tmp/src/plex/linux-x86_64-v3/libsqlite3.so"
plex_arm="$tmp/src/plex/linux-arm64/libsqlite3.so"
emby_v2="$tmp/src/emby/linux-x86_64-v2/libsqlite3.so"
emby_v3="$tmp/src/emby/linux-x86_64-v3/libsqlite3.so"
emby_arm="$tmp/src/emby/linux-arm64/libsqlite3.so"
emby_generic2_v2="$tmp/src/emby-generic2/linux-x86_64-v2/libsqlite3.so"
emby_generic2_v3="$tmp/src/emby-generic2/linux-x86_64-v3/libsqlite3.so"
emby_generic2_arm="$tmp/src/emby-generic2/linux-arm64/libsqlite3.so"

plex_v2_sha="$(write_artifact "$plex_v2" "plex stage replacement linux-x86_64-v2")"
plex_v3_sha="$(write_artifact "$plex_v3" "plex stage replacement linux-x86_64-v3")"
plex_arm_sha="$(write_artifact "$plex_arm" "plex stage replacement linux-arm64")"
emby_v2_sha="$(write_artifact "$emby_v2" "emby stage replacement linux-x86_64-v2")"
emby_v3_sha="$(write_artifact "$emby_v3" "emby stage replacement linux-x86_64-v3")"
emby_arm_sha="$(write_artifact "$emby_arm" "emby stage replacement linux-arm64")"
emby_generic2_v2_sha="$(write_artifact "$emby_generic2_v2" "emby generic2 stage replacement linux-x86_64-v2")"
emby_generic2_v3_sha="$(write_artifact "$emby_generic2_v3" "emby generic2 stage replacement linux-x86_64-v3")"
emby_generic2_arm_sha="$(write_artifact "$emby_generic2_arm" "emby generic2 stage replacement linux-arm64")"

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

plex_manifest="$tmp/baked-pins-plex.txt"
plex_staged="$tmp/staged-plex"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --pool-baselines "$pool_baselines" \
  --artifact "linux-x86_64-v2:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v2.so:$plex_v2:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-x86_64-v3:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-x86_64-v3.so:$plex_v3:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --artifact "linux-arm64:icu69:sqlite-2026.05.28-r1-library-plex-icu69-linux-arm64.so:$plex_arm:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$plex_manifest"
bash tools/lsio-mod/stage-lsio-mod.sh \
  --mod plex \
  --output-dir "$plex_staged" \
  --baked-pins "$plex_manifest" \
  --artifact "linux-x86_64-v2:icu69:$plex_v2" \
  --artifact "linux-x86_64-v3:icu69:$plex_v3" \
  --artifact "linux-arm64:icu69:$plex_arm" > "$tmp/stage-plex.out"
assert_valid_staged_manifest "$plex_staged" "plex-stage"
assert_staged_artifact "$plex_staged" linux-x86_64-v2 icu69 "$plex_v2"
assert_staged_artifact "$plex_staged" linux-x86_64-v3 icu69 "$plex_v3"
assert_staged_artifact "$plex_staged" linux-arm64 icu69 "$plex_arm"

emby_manifest="$tmp/baked-pins-emby.txt"
emby_staged="$tmp/staged-emby"
bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  "${render_common_args[@]}" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --artifact "linux-x86_64-v2:generic:sqlite-2026.05.28-r1-library-generic-linux-x86_64-v2.so:$emby_v2:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-x86_64-v3:generic:sqlite-2026.05.28-r1-library-generic-linux-x86_64-v3.so:$emby_v3:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-arm64:generic:sqlite-2026.05.28-r1-library-generic-linux-arm64.so:$emby_arm:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-x86_64-v2:generic2:sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v2.so:$emby_generic2_v2:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-x86_64-v3:generic2:sqlite-2026.05.28-r1-library-generic2-linux-x86_64-v3.so:$emby_generic2_v3:/app/emby/lib/libsqlite3.so.3.49.2" \
  --artifact "linux-arm64:generic2:sqlite-2026.05.28-r1-library-generic2-linux-arm64.so:$emby_generic2_arm:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$emby_manifest"
bash tools/lsio-mod/stage-lsio-mod.sh \
  --mod emby \
  --output-dir "$emby_staged" \
  --baked-pins "$emby_manifest" \
  --artifact "linux-x86_64-v2:generic:$emby_v2" \
  --artifact "linux-x86_64-v3:generic:$emby_v3" \
  --artifact "linux-arm64:generic:$emby_arm" \
  --artifact "linux-x86_64-v2:generic2:$emby_generic2_v2" \
  --artifact "linux-x86_64-v3:generic2:$emby_generic2_v3" \
  --artifact "linux-arm64:generic2:$emby_generic2_arm" > "$tmp/stage-emby.out"
assert_valid_staged_manifest "$emby_staged" "emby-stage"
assert_staged_artifact "$emby_staged" linux-x86_64-v2 generic "$emby_v2"
assert_staged_artifact "$emby_staged" linux-x86_64-v3 generic "$emby_v3"
assert_staged_artifact "$emby_staged" linux-arm64 generic "$emby_arm"
assert_staged_artifact "$emby_staged" linux-x86_64-v2 generic2 "$emby_generic2_v2"
assert_staged_artifact "$emby_staged" linux-x86_64-v3 generic2 "$emby_generic2_v3"
assert_staged_artifact "$emby_staged" linux-arm64 generic2 "$emby_generic2_arm"

printf 'stage lsio mod baked pins tests passed\n'
