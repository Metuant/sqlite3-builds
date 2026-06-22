#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-plex-poolpatch-phase.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-plex-poolpatch-phase.XXXXXX)"
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

assert_window_hex() {
  path=$1
  offset=$2
  expected=$3
  message=$4
  actual="$(dd if="$path" bs=1 skip="$offset" count=16 2>/dev/null | od -An -tx1 -v | tr -d ' \n')"
  assert_eq "$expected" "$actual" "$message"
}

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

write_hex_file() {
  path=$1
  hex=$2
  printf '%s' "$hex" | xxd -r -p > "$path"
}

case_selected() {
  wanted="${1:-}"
  case_name="$2"
  [ -z "$wanted" ] || [ "$wanted" = "$case_name" ]
}

mkdir -p "$tmp_root/bin"
cat > "$tmp_root/bin/uname" <<'EOF_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
EOF_UNAME
chmod +x "$tmp_root/bin/uname"

setup_case() {
  case_name="$1"
  case_root="$tmp_root/$case_name"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  runtime_lib_dir="$fixture/usr/lib/plexmediaserver/lib"
  target_path="$runtime_lib_dir/libsqlite3.so"
  detector_root="$fixture/detectors"
  pms_path="$detector_root/Custom PMS"
  scanner_path="$detector_root/Custom Scanner"
  mkdir -p "$lib_root" "$runtime_lib_dir" "$detector_root"

  cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
  cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
  cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
  cp lsio-mods/shared/cont-init-fragments/manifest-parser.sh "$lib_root/manifest-parser.sh"
  cp lsio-mods/shared/cont-init-fragments/selector.sh "$lib_root/selector.sh"
  cp lsio-mods/shared/cont-init-fragments/atomic-write.sh "$lib_root/atomic-write.sh"
  cp lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh "$lib_root/plex-pool-patch.sh"

  pms_orig="00112233445566778899aabbccddeeff"
  pms_patched="ff112233445566778899aabbccddeeff"
  scanner_orig="102132435465768798a9bacbdcedfe0f"
  scanner_patched="ef2132435465768798a9bacbdcedfe0f"
  write_hex_file "$pms_path" "$pms_orig"
  write_hex_file "$case_root/pms-patched" "$pms_patched"
  write_hex_file "$scanner_path" "$scanner_orig"
  write_hex_file "$case_root/scanner-patched" "$scanner_patched"
  printf 'sqlite-current\n' > "$target_path"

  artifact_sha="$(sha_file "$target_path")"
  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-poolpatch|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-poolpatch|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-poolpatch|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_sha
detect|1|plex|plex-poolpatch|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
artifact|1|plex|plex-poolpatch|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|$target_path|$artifact_sha
pool-site|1|plex|plex-poolpatch|linux-arm64|$pms_path|pms-site|0|0|$pms_orig|$pms_patched
pool-site|1|plex|plex-poolpatch|linux-arm64|$scanner_path|scanner-site|0|0|$scanner_orig|$scanner_patched
EOF_PINS

  script_copy="$case_root/run"
  cp lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-poolpatch/run "$script_copy"
  python3 - "$script_copy" "$fixture" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
fixture = sys.argv[2]
text = script_path.read_text()
text = text.replace("/opt/sqlite3-lsio-mod", f"{fixture}/opt/sqlite3-lsio-mod")
script_path.write_text(text)
PY
  chmod +x "$script_copy"
}

run_phase() {
  phase_log="$case_root/phase.log"
  set +e
  PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  assert_eq "0" "$rc" "$case_name poolpatch exit status"
}

run_data_driven_detector_path() {
  setup_case data_driven_detector_path
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_patched" "pms detector path was not patched"
  assert_window_hex "$scanner_path" 0 "$scanner_patched" "scanner detector path was not patched"
  assert_contains "event=patched binary=$pms_path site=pms-site" "$phase_log" "pms patch log"
  assert_contains "event=patched binary=$scanner_path site=scanner-site" "$phase_log" "scanner patch log"
}

run_not_current_reason() {
  setup_case not_current_reason
  printf 'sqlite-not-current\n' > "$target_path"
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_orig" "pms changed when sqlite target was not current"
  assert_contains 'event=current-artifact-mismatch' "$phase_log" "poolpatch selected_artifact_is_current warning"
  assert_contains 'event=skip-sqlite-not-current' "$phase_log" "poolpatch skip log"
}

wanted_case="${1:-}"
ran=0
for name in data_driven_detector_path not_current_reason; do
  if case_selected "$wanted_case" "$name"; then
    "run_${name}"
    ran=1
  fi
done

[ "$ran" -eq 1 ] || fail "no matching plex poolpatch phase case selected"
printf 'plex poolpatch phase tests passed\n'
