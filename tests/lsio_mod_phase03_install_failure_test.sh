#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-phase03-install.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-phase03-install.XXXXXX)"
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

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

case_selected() {
  wanted="${1:-}"
  case_mod="$2"
  [ -z "$wanted" ] || [ "$wanted" = "$case_mod" ]
}

mkdir -p "$tmp_root/bin"
cat > "$tmp_root/bin/uname" <<'EOF_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
EOF_UNAME
chmod +x "$tmp_root/bin/uname"

setup_lib_root() {
  lib_root="$1"
  mkdir -p "$lib_root"
  cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
  cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
  cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
  cp lsio-mods/shared/cont-init-fragments/manifest-parser.sh "$lib_root/manifest-parser.sh"
  cp lsio-mods/shared/cont-init-fragments/selector.sh "$lib_root/selector.sh"
  cp lsio-mods/shared/cont-init-fragments/atomic-write.sh "$lib_root/atomic-write.sh"
  cp lsio-mods/shared/cont-init-fragments/swap.sh "$lib_root/swap.sh"
}

prepare_plex_case() {
  case_root="$tmp_root/plex"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  artifact_dir="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64/icu69"
  runtime_root="$fixture/usr/lib/plexmediaserver"
  runtime_lib_dir="$runtime_root/lib"
  target_path="$runtime_lib_dir/libsqlite3.so"
  pms_path="$runtime_root/Plex Media Server"
  scanner_path="$runtime_root/Plex Media Scanner"
  mkdir -p "$artifact_dir" "$runtime_lib_dir"
  setup_lib_root "$lib_root"

  printf 'sqlite-corrupt-artifact\n' > "$artifact_dir/libsqlite3.so"
  printf 'sqlite-current-expected\n' > "$case_root/current-expected"
  printf 'sqlite-bundled-target\n' > "$target_path"
  printf 'untrusted backup\n' > "$target_path.bundled.bak"
  printf 'plex pms pristine\n' > "$pms_path"
  printf 'plex pms patched\n' > "$case_root/pms-patched"
  printf 'plex scanner pristine\n' > "$scanner_path"
  printf 'plex scanner patched\n' > "$case_root/scanner-patched"
  for so in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
    printf 'icu-runtime-%s\n' "$so" > "$runtime_lib_dir/$so"
  done

  current_sha="$(sha_file "$case_root/current-expected")"
  baseline_sha="$(sha_file "$target_path")"
  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"
  icu_uc_sha="$(sha_file "$runtime_lib_dir/libicuucplex.so.69")"
  icu_i18n_sha="$(sha_file "$runtime_lib_dir/libicui18nplex.so.69")"
  icu_data_sha="$(sha_file "$runtime_lib_dir/libicudataplex.so.69")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-install-fail|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-install-fail|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-install-fail|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_sha
detect|1|plex|plex-install-fail|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
artifact|1|plex|plex-install-fail|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|$target_path|$current_sha
pre|2|plex|plex-install-fail|linux-arm64|target_sqlite|lscr.io/linuxserver/plex:fixture|sha256:fixture|$target_path|$baseline_sha
pre|2|plex|plex-install-fail|linux-arm64|plex_icu_linked:libicuucplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|$runtime_lib_dir/libicuucplex.so.69|$icu_uc_sha
pre|2|plex|plex-install-fail|linux-arm64|plex_icu_linked:libicui18nplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|$runtime_lib_dir/libicui18nplex.so.69|$icu_i18n_sha
pre|2|plex|plex-install-fail|linux-arm64|plex_icu_linked:libicudataplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|$runtime_lib_dir/libicudataplex.so.69|$icu_data_sha
EOF_PINS

  script_copy="$case_root/run"
  cp lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-swap/run "$script_copy"
  python3 - "$script_copy" "$fixture" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
fixture = sys.argv[2]
text = script_path.read_text()
text = text.replace("/opt/sqlite3-lsio-mod", f"{fixture}/opt/sqlite3-lsio-mod")
text = text.replace("/usr/lib/plexmediaserver/lib/", f"{fixture}/usr/lib/plexmediaserver/lib/")
script_path.write_text(text)
PY
}

prepare_emby_case() {
  case_root="$tmp_root/emby"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  artifact_dir="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64/generic"
  emby_root="$fixture/app/emby"
  runtime_lib_dir="$emby_root/lib"
  target_path="$runtime_lib_dir/libsqlite3.so.3.49.2"
  deps_path="$emby_root/EmbyServer.deps.json"
  dll_path="$emby_root/EmbyServer.dll"
  mkdir -p "$artifact_dir" "$runtime_lib_dir"
  setup_lib_root "$lib_root"

  printf 'sqlite-corrupt-artifact\n' > "$artifact_dir/libsqlite3.so"
  printf 'sqlite-current-expected\n' > "$case_root/current-expected"
  printf 'sqlite-bundled-target\n' > "$target_path"
  printf 'untrusted backup\n' > "$target_path.bundled.bak"
  printf '{"runtimeTarget":{"name":"fixture"}}\n' > "$deps_path"
  printf 'fixture emby dll\n' > "$dll_path"

  current_sha="$(sha_file "$case_root/current-expected")"
  baseline_sha="$(sha_file "$target_path")"
  deps_sha="$(sha_file "$deps_path")"
  dll_sha="$(sha_file "$dll_path")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|emby|emby-install-fail|linux-arm64|emby_deps|$deps_path|$deps_sha
detect|1|emby|emby-install-fail|linux-arm64|emby_dll|$dll_path|$dll_sha
artifact|1|emby|emby-install-fail|linux-arm64|generic|artifacts/linux-arm64/generic/libsqlite3.so|$target_path|$current_sha
pre|2|emby|emby-install-fail|linux-arm64|target_sqlite|lscr.io/linuxserver/emby:fixture|-|$target_path|$baseline_sha
EOF_PINS

  script_copy="$case_root/run"
  cp lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-swap/run "$script_copy"
  python3 - "$script_copy" "$fixture" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
fixture = sys.argv[2]
text = script_path.read_text()
text = text.replace("/opt/sqlite3-lsio-mod", f"{fixture}/opt/sqlite3-lsio-mod")
script_path.write_text(text)
PY
}

run_case() {
  mod="$1"
  "prepare_${mod}_case"
  chmod +x "$script_copy"
  before_sha="$(sha_file "$target_path")"
  phase_log="$case_root/phase.log"
  set +e
  PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  after_sha="$(sha_file "$target_path")"
  assert_eq "0" "$rc" "$mod phase03 install failure exit status"
  assert_eq "$before_sha" "$after_sha" "$mod phase03 install failure preserved target"
  assert_contains 'event=temp-sha-mismatch' "$phase_log" "$mod phase03 install failure reason"
  assert_contains 'event=install-failed-target-preserved' "$phase_log" "$mod phase03 preserved-target log"
  assert_contains 'event=current-artifact-mismatch' "$phase_log" "$mod phase03 selected_artifact_is_current warning"
}

wanted_mod="${1:-}"
ran=0
for case_mod in plex emby; do
  if case_selected "$wanted_mod" "$case_mod"; then
    run_case "$case_mod"
    ran=1
  fi
done

[ "$ran" -eq 1 ] || fail "no matching phase03 install failure case selected"
printf 'phase03 install failure tests passed\n'
