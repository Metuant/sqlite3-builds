#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-preflight-root.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-preflight-root.XXXXXX)"
cleanup() {
  chmod -R u+w "$tmp_root" 2>/dev/null || true
  rm -rf "$tmp_root"
}
trap cleanup EXIT

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

assert_contains() {
  needle="$1"
  path="$2"
  context="$3"
  grep -Fq "$needle" "$path" || {
    echo "FATAL: $context: expected to find [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  }
}

assert_not_contains() {
  needle="$1"
  path="$2"
  context="$3"
  if grep -Fq "$needle" "$path"; then
    echo "FATAL: $context: did not expect [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  fi
}

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

write_file() {
  path="$1"
  text="$2"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' "$text" > "$path"
}

setup_lib_root() {
  lib_root="$1"
  mkdir -p "$lib_root"
  cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
  cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
  cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
  cp lsio-mods/shared/cont-init-fragments/manifest-parser.sh "$lib_root/manifest-parser.sh"
  cp lsio-mods/shared/cont-init-fragments/selector.sh "$lib_root/selector.sh"
}

write_script_copy() {
  mod="$1"
  case_root="$2"
  fixture="$3"
  script_copy="$case_root/run"
  sed "s|/opt/sqlite3-lsio-mod|$fixture/opt/sqlite3-lsio-mod|g" \
    "lsio-mods/$mod/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/run" > "$script_copy"
  chmod +x "$script_copy"
}

prepare_plex_case() {
  case_root="$tmp_root/plex"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  artifact="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64/icu69/libsqlite3.so"
  runtime_root="$case_root/runtime"
  pms_path="$runtime_root/Plex Media Server"
  scanner_path="$runtime_root/Plex Media Scanner"
  mkdir -p "$fixture/opt/sqlite3-lsio-mod" "$runtime_root"
  setup_lib_root "$lib_root"
  write_file "$artifact" "plex replacement sqlite"
  write_file "$pms_path" "plex pms pristine"
  write_file "$case_root/pms-patched" "plex pms patched"
  write_file "$case_root/pms-source-id-patched" "plex pms source-id patched"
  write_file "$scanner_path" "plex scanner pristine"
  write_file "$case_root/scanner-patched" "plex scanner patched"
  artifact_sha="$(sha_file "$artifact")"
  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  pms_source_id_patched_sha="$(sha_file "$case_root/pms-source-id-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"
  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PLEX_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-root-test|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-root-test|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-root-test|linux-arm64|plex_pms:source-id-patched|$pms_path|$pms_source_id_patched_sha
detect|1|plex|plex-root-test|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_sha
detect|1|plex|plex-root-test|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
artifact|1|plex|plex-root-test|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|$artifact_sha
pre|2|plex|plex-root-test|linux-arm64|target_sqlite|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libsqlite3.so|$pms_sha
pre|2|plex|plex-root-test|linux-arm64|plex_icu_linked:libicuucplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicuucplex.so.69|$pms_sha
pre|2|plex|plex-root-test|linux-arm64|plex_icu_linked:libicui18nplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicui18nplex.so.69|$scanner_sha
pre|2|plex|plex-root-test|linux-arm64|plex_icu_linked:libicudataplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicudataplex.so.69|$pms_patched_sha
pool-site|1|plex|plex-root-test|linux-arm64|$pms_path|pms-site|0|0|00112233445566778899aabbccddeeff|ff112233445566778899aabbccddeeff
pool-site|1|plex|plex-root-test|linux-arm64|$scanner_path|scanner-site|8|15|11112222333344445555666677778888|11112222333344ff5555666677778888
EOF_PLEX_PINS
  write_script_copy plex "$case_root" "$fixture"
}

prepare_emby_case() {
  case_root="$tmp_root/emby"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  artifact="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64/generic/libsqlite3.so"
  runtime_root="$case_root/runtime"
  deps_path="$runtime_root/EmbyServer.deps.json"
  dll_path="$runtime_root/EmbyServer.dll"
  mkdir -p "$fixture/opt/sqlite3-lsio-mod" "$runtime_root"
  setup_lib_root "$lib_root"
  write_file "$artifact" "emby replacement sqlite"
  write_file "$deps_path" "emby deps"
  write_file "$dll_path" "emby dll"
  write_file "$case_root/emby-target" "emby bundled sqlite"
  artifact_sha="$(sha_file "$artifact")"
  deps_sha="$(sha_file "$deps_path")"
  dll_sha="$(sha_file "$dll_path")"
  target_sha="$(sha_file "$case_root/emby-target")"
  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_EMBY_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|emby|emby-root-test|linux-arm64|emby_deps|$deps_path|$deps_sha
detect|1|emby|emby-root-test|linux-arm64|emby_dll|$dll_path|$dll_sha
artifact|1|emby|emby-root-test|linux-arm64|generic|artifacts/linux-arm64/generic/libsqlite3.so|/app/emby/lib/libsqlite3.so.3.49.2|$artifact_sha
pre|2|emby|emby-root-test|linux-arm64|target_sqlite|lscr.io/linuxserver/emby:fixture|-|/app/emby/lib/libsqlite3.so.3.49.2|$target_sha
EOF_EMBY_PINS
  write_script_copy emby "$case_root" "$fixture"
}

run_preflight_case() {
  mod="$1"
  "prepare_${mod}_case"
  phase_log="$case_root/preflight.log"
  ambient="$case_root/not-mod-root"
  mkdir -p "$ambient"
  set +e
  (
    cd "$ambient"
    PATH="$tmp_root/bin:$PATH" "$bash_path" "$script_copy" > "$phase_log" 2>&1
  )
  rc=$?
  set -e
  assert_eq "0" "$rc" "$mod preflight staged artifact root exit status"
  assert_contains "event=missing-target" "$phase_log" "$mod preflight reached post-validation target check"
  assert_not_contains "event=malformed-baked-pins" "$phase_log" "$mod preflight must not reject staged manifest"
  assert_not_contains "reason=missing-artifact-file" "$phase_log" "$mod preflight must resolve artifacts from manifest root"
}

run_missing_command_case() {
  mod="$1"
  "prepare_${mod}_case"
  phase_log="$case_root/missing-command.log"
  empty_path="$case_root/empty-path"
  mkdir -p "$empty_path"
  set +e
  PATH="$empty_path" "$bash_path" "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  assert_eq "0" "$rc" "$mod preflight missing command exit status"
  assert_contains "event=missing-command command=awk" "$phase_log" "$mod preflight missing command warning"
  assert_not_contains "event=malformed-baked-pins" "$phase_log" "$mod preflight missing command must not hard-fail validation"
}

mkdir -p "$tmp_root/bin"
cat > "$tmp_root/bin/uname" <<'EOF_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
EOF_UNAME
chmod +x "$tmp_root/bin/uname"
bash_path="$(command -v bash)"
unset sqlite3_mod_root sqlite3_mod_artifact_root

for mod in plex emby; do
  run_preflight_case "$mod"
  run_missing_command_case "$mod"
done

printf 'preflight artifact root tests passed\n'
