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
  actual="$2"
  [ -z "$wanted" ] || [ "$wanted" = "$actual" ]
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

  cat >> "$lib_root/swap.sh" <<'EOF_FAILURE_INJECTION'

# Test-only fault injection appended to the staged fixture copy.
sha256_of() {
  local path=$1
  local actual
  actual="$(sha256sum "$path" | awk '{print $1}')"
  if [ "$path" = "${PHASE03_TEST_TARGET_PATH:-}" ] &&
     [ "$actual" = "${PHASE03_TEST_CURRENT_SHA:-}" ] &&
     [ ! -f "${PHASE03_TEST_INSTALL_MISMATCH_MARKER:-}" ]; then
    case "${PHASE03_TEST_FAILURE_MODE:-}" in
      post-rename-mismatch|restore-copy-failure|restore-readback-mismatch)
        : > "$PHASE03_TEST_INSTALL_MISMATCH_MARKER"
        printf '%064d\n' 0
        return 0
        ;;
    esac
  fi
  if [ "${PHASE03_TEST_FAILURE_MODE:-}" = "restore-readback-mismatch" ] &&
     [ "$path" = "${PHASE03_TEST_TARGET_PATH:-}" ] &&
     [ "$actual" = "${PHASE03_TEST_BASELINE_SHA:-}" ] &&
     [ -f "${PHASE03_TEST_INSTALL_MISMATCH_MARKER:-}" ]; then
    : > "$PHASE03_TEST_RESTORE_READBACK_MARKER"
    printf '%064d\n' 0
    return 0
  fi
  printf '%s\n' "$actual"
}

cp() {
  local src=${1:-}
  local dest=${2:-}
  if [ "$src" = "-f" ]; then
    src=${2:-}
    dest=${3:-}
  fi
  if [ "${PHASE03_TEST_FAILURE_MODE:-}" = "restore-copy-failure" ] &&
     [ "$src" = "${PHASE03_TEST_BAK_PATH:-}" ]; then
    case "$dest" in
      "${PHASE03_TEST_TARGET_PATH}".sqlite3-mod.*)
        printf 'partial restore bytes\n' > "$dest"
        : > "$PHASE03_TEST_RESTORE_COPY_MARKER"
        return 1
        ;;
    esac
  fi
  command cp "$@"
}
EOF_FAILURE_INJECTION
}

prepare_plex_case() {
  scenario="$1"
  case_root="$tmp_root/plex-$scenario"
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

  printf 'sqlite-current-expected\n' > "$case_root/current-expected"
  if [ "$scenario" = "temp-sha-mismatch" ]; then
    printf 'sqlite-corrupt-artifact\n' > "$artifact_dir/libsqlite3.so"
  else
    cp "$case_root/current-expected" "$artifact_dir/libsqlite3.so"
  fi
  printf 'sqlite-bundled-target\n' > "$target_path"
  if [ "$scenario" = "temp-sha-mismatch" ]; then
    printf 'untrusted backup\n' > "$target_path.bundled.bak"
  else
    cp "$target_path" "$target_path.bundled.bak"
  fi
  printf 'plex pms pristine\n' > "$pms_path"
  printf 'plex pms patched\n' > "$case_root/pms-patched"
  printf 'plex pms source-id patched\n' > "$case_root/pms-source-id-patched"
  printf 'plex scanner pristine\n' > "$scanner_path"
  printf 'plex scanner patched\n' > "$case_root/scanner-patched"
  for so in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
    printf 'icu-runtime-%s\n' "$so" > "$runtime_lib_dir/$so"
  done

  current_sha="$(sha_file "$case_root/current-expected")"
  baseline_sha="$(sha_file "$target_path")"
  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  pms_source_id_patched_sha="$(sha_file "$case_root/pms-source-id-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"
  icu_uc_sha="$(sha_file "$runtime_lib_dir/libicuucplex.so.69")"
  icu_i18n_sha="$(sha_file "$runtime_lib_dir/libicui18nplex.so.69")"
  icu_data_sha="$(sha_file "$runtime_lib_dir/libicudataplex.so.69")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-install-fail|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-install-fail|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-install-fail|linux-arm64|plex_pms:source-id-patched|$pms_path|$pms_source_id_patched_sha
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
  scenario="$1"
  case_root="$tmp_root/emby-$scenario"
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

  printf 'sqlite-current-expected\n' > "$case_root/current-expected"
  if [ "$scenario" = "temp-sha-mismatch" ]; then
    printf 'sqlite-corrupt-artifact\n' > "$artifact_dir/libsqlite3.so"
  else
    cp "$case_root/current-expected" "$artifact_dir/libsqlite3.so"
  fi
  printf 'sqlite-bundled-target\n' > "$target_path"
  if [ "$scenario" = "temp-sha-mismatch" ]; then
    printf 'untrusted backup\n' > "$target_path.bundled.bak"
  else
    cp "$target_path" "$target_path.bundled.bak"
  fi
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
  scenario="$2"
  "prepare_${mod}_case" "$scenario"
  chmod +x "$script_copy"
  before_sha="$(sha_file "$target_path")"
  phase_log="$case_root/phase.log"
  install_mismatch_marker="$case_root/install-mismatch-entered"
  restore_copy_marker="$case_root/restore-copy-entered"
  restore_readback_marker="$case_root/restore-readback-entered"
  set +e
  PHASE03_TEST_FAILURE_MODE="$scenario" \
  PHASE03_TEST_TARGET_PATH="$target_path" \
  PHASE03_TEST_BAK_PATH="$target_path.bundled.bak" \
  PHASE03_TEST_CURRENT_SHA="$current_sha" \
  PHASE03_TEST_BASELINE_SHA="$baseline_sha" \
  PHASE03_TEST_INSTALL_MISMATCH_MARKER="$install_mismatch_marker" \
  PHASE03_TEST_RESTORE_COPY_MARKER="$restore_copy_marker" \
  PHASE03_TEST_RESTORE_READBACK_MARKER="$restore_readback_marker" \
  PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  after_sha="$(sha_file "$target_path")"
  assert_contains 'event=current-artifact-mismatch' "$phase_log" "$mod phase03 selected_artifact_is_current warning"
  case "$scenario" in
    temp-sha-mismatch)
      assert_eq "0" "$rc" "$mod phase03 temp SHA mismatch exit status"
      assert_eq "$before_sha" "$after_sha" "$mod phase03 temp SHA mismatch preserved target"
      assert_contains 'event=temp-sha-mismatch' "$phase_log" "$mod phase03 temp SHA mismatch reason"
      assert_contains 'event=install-failed-target-preserved' "$phase_log" "$mod phase03 preserved-target log"
      ;;
    post-rename-mismatch)
      assert_eq "0" "$rc" "$mod phase03 post-rename mismatch exit status"
      assert_eq "$baseline_sha" "$after_sha" "$mod phase03 post-rename mismatch restored baseline"
      [ -f "$install_mismatch_marker" ] || fail "$mod phase03 post-rename mismatch did not reach installed readback"
      assert_contains 'event=installed-sha-mismatch' "$phase_log" "$mod phase03 post-rename mismatch reason"
      assert_contains 'event=install-failed-baseline-restored' "$phase_log" "$mod phase03 post-rename recovery result"
      ;;
    restore-copy-failure)
      assert_eq "0" "$rc" "$mod phase03 restore copy failure exit status"
      assert_eq "$current_sha" "$after_sha" "$mod phase03 restore copy failure preserved verified artifact"
      [ -f "$install_mismatch_marker" ] || fail "$mod phase03 restore copy failure did not reach installed readback"
      [ -f "$restore_copy_marker" ] || fail "$mod phase03 restore copy failure did not reach restore temp copy"
      assert_contains 'event=restore-temp-copy-failed' "$phase_log" "$mod phase03 restore copy failure reason"
      assert_contains 'event=install-failed-target-current' "$phase_log" "$mod phase03 restore copy failure final classification"
      ;;
    restore-readback-mismatch)
      [ "$rc" -ne 0 ] || fail "$mod phase03 restore readback mismatch reported success"
      assert_eq "$baseline_sha" "$after_sha" "$mod phase03 restore readback mismatch published baseline bytes atomically"
      [ -f "$install_mismatch_marker" ] || fail "$mod phase03 restore readback mismatch did not reach installed readback"
      [ -f "$restore_readback_marker" ] || fail "$mod phase03 restore readback mismatch did not reach restored readback"
      assert_contains 'event=restored-sha-mismatch' "$phase_log" "$mod phase03 restore readback mismatch reason"
      assert_contains 'event=install-recovery-unsafe' "$phase_log" "$mod phase03 restore readback mismatch hard exit"
      ;;
    *)
      fail "unknown phase03 install failure scenario: $scenario"
      ;;
  esac
  if find "${target_path%/*}" -maxdepth 1 -name "${target_path##*/}.sqlite3-mod.*" -print -quit | grep -q .; then
    fail "$mod phase03 $scenario leaked a temporary replacement file"
  fi
}

wanted_mod="${1:-}"
wanted_scenario="${2:-}"
ran=0
for case_mod in plex emby; do
  for case_scenario in temp-sha-mismatch post-rename-mismatch restore-copy-failure restore-readback-mismatch; do
    if case_selected "$wanted_mod" "$case_mod" && case_selected "$wanted_scenario" "$case_scenario"; then
      run_case "$case_mod" "$case_scenario"
      ran=1
    fi
  done
done

[ "$ran" -eq 1 ] || fail "no matching phase03 install failure case selected"
printf 'phase03 install failure tests passed\n'
