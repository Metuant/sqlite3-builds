#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-plex-plexpatch-phase.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-plex-plexpatch-phase.XXXXXX)"
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
  grep -aFq "$needle" "$path" || {
    echo "FATAL: $context: expected to find [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  }
}

assert_not_contains() {
  needle="$1"
  path="$2"
  context="$3"
  if grep -aFq "$needle" "$path"; then
    echo "FATAL: $context: did not expect to find [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  fi
}

assert_window_hex() {
  path=$1
  offset=$2
  expected=$3
  message=$4
  actual="$(dd if="$path" bs=1 skip="$offset" count=16 2>/dev/null | od -An -tx1 -v | tr -d ' \n')"
  assert_eq "$expected" "$actual" "$message"
}

assert_source_id() {
  path=$1
  expected=$2
  message=$3
  actual="$(dd if="$path" bs=1 skip=32 count=84 2>/dev/null)"
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

write_pms_file() {
  path=$1
  pool_hex=$2
  source_id=$3
  write_hex_file "$path" "$pool_hex"
  printf '0123456789abcdef%s' "$source_id" >> "$path"
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
real_dd="$(command -v dd)"
cat > "$tmp_root/bin/dd" <<'EOF_DD'
#!/usr/bin/env bash
output=""
for arg in "$@"; do
  case "$arg" in
    of=*) output="${arg#of=}" ;;
  esac
done
if [ -n "${FAIL_DD_OF_PREFIX:-}" ]; then
  case "$output" in
    "${FAIL_DD_OF_PREFIX}"*) exit 1 ;;
  esac
fi
exec "$REAL_DD" "$@"
EOF_DD
chmod +x "$tmp_root/bin/uname" "$tmp_root/bin/dd"
export REAL_DD="$real_dd"

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
  cp lsio-mods/shared/cont-init-fragments/plex-patch.sh "$lib_root/plex-patch.sh"

  pms_orig="00112233445566778899aabbccddeeff"
  pms_patched="ff112233445566778899aabbccddeeff"
  scanner_orig="102132435465768798a9bacbdcedfe0f"
  scanner_patched="ef2132435465768798a9bacbdcedfe0f"
  source_id_old="$(awk -F '\t' '$2 == "plex" { print $5 }' pins/library-compat-groups.tsv)"
  # shellcheck source=pins/versions.env
  . pins/versions.env
  source_id_new="${SQLITE_SOURCE_ID//%20/ }"
  write_pms_file "$pms_path" "$pms_orig" "$source_id_old"
  write_pms_file "$case_root/pms-patched" "$pms_patched" "$source_id_old"
  write_pms_file "$case_root/pms-source-id-patched" "$pms_patched" "$source_id_new"
  write_hex_file "$scanner_path" "$scanner_orig"
  write_hex_file "$case_root/scanner-patched" "$scanner_patched"
  printf 'sqlite-current\n' > "$target_path"

  artifact_sha="$(sha_file "$target_path")"
  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  pms_source_id_patched_sha="$(sha_file "$case_root/pms-source-id-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-patch|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-patch|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-patch|linux-arm64|plex_pms:source-id-patched|$pms_path|$pms_source_id_patched_sha
detect|1|plex|plex-patch|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_sha
detect|1|plex|plex-patch|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
artifact|1|plex|plex-patch|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|$target_path|$artifact_sha
pool-site|1|plex|plex-patch|linux-arm64|$pms_path|pms-site|0|0|$pms_orig|$pms_patched
pool-site|1|plex|plex-patch|linux-arm64|$scanner_path|scanner-site|0|0|$scanner_orig|$scanner_patched
EOF_PINS

  mkdir -p "$fixture/opt/sqlite3-lsio-mod/pins"
  cp pins/library-compat-groups.tsv "$fixture/opt/sqlite3-lsio-mod/pins/library-compat-groups.tsv"
  cp pins/versions.env "$fixture/opt/sqlite3-lsio-mod/pins/versions.env"

  script_copy="$case_root/run"
  cp lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-plexpatch/run "$script_copy"
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
  phase_log="$case_root/${1:-phase.log}"
  expected_rc="${2:-0}"
  set +e
  PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  assert_eq "$expected_rc" "$rc" "$case_name plexpatch exit status"
}

run_data_driven_detector_path() {
  setup_case data_driven_detector_path
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_patched" "pms detector path was not patched"
  assert_window_hex "$scanner_path" 0 "$scanner_patched" "scanner detector path was not patched"
  assert_source_id "$pms_path" "$source_id_new" "PMS source id was not patched"
  assert_contains "event=patched binary=$pms_path site=pms-site" "$phase_log" "pms patch log"
  assert_contains "event=patched binary=$scanner_path site=scanner-site" "$phase_log" "scanner patch log"
  assert_contains "event=source-id-patched binary=$pms_path" "$phase_log" "source-id patch log"
}

run_pool_patched_input() {
  setup_case pool_patched_input
  cp "$pms_path" "$pms_path.bundled.bak"
  write_pms_file "$pms_path" "$pms_patched" "$source_id_old"
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_patched" "pool-patched PMS bytes changed"
  assert_contains "event=source-id-patched binary=$pms_path" "$phase_log" "pool-patched source-id log"
  assert_source_id "$pms_path" "$source_id_new" "pool-patched input source id was not patched"
}

run_not_current_reason() {
  setup_case not_current_reason
  printf 'sqlite-not-current\n' > "$target_path"
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_orig" "pms changed when sqlite target was not current"
  assert_contains 'event=current-artifact-mismatch' "$phase_log" "plexpatch selected_artifact_is_current warning"
  assert_contains 'event=skip-sqlite-not-current' "$phase_log" "plexpatch skip log"
}

run_ambiguous_source_id_pin() {
  setup_case ambiguous_source_id_pin
  printf 'SQLITE_SOURCE_ID=2000-01-01%%2000:00:00%%20%s\n' \
    '0000000000000000000000000000000000000000000000000000000000000000' \
    >> "$fixture/opt/sqlite3-lsio-mod/pins/versions.env"
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_orig" "PMS changed with ambiguous source-id pin"
  assert_source_id "$pms_path" "$source_id_old" "PMS source id changed with ambiguous source-id pin"
  assert_contains 'event=missing-source-id-pin' "$phase_log" "ambiguous source-id pin warning"
}

run_invalid_source_id_pin() {
  setup_case invalid_source_id_pin
  printf 'SQLITE_SOURCE_ID=invalid\n' > "$fixture/opt/sqlite3-lsio-mod/pins/versions.env"
  run_phase
  assert_window_hex "$pms_path" 0 "$pms_orig" "PMS changed with invalid source-id pin"
  assert_source_id "$pms_path" "$source_id_old" "PMS source id changed with invalid source-id pin"
  assert_contains 'event=invalid-source-id-pin' "$phase_log" "invalid source-id pin warning"
}

run_repeated_scanner_recovery() {
  setup_case repeated_scanner_recovery
  export FAIL_DD_OF_PREFIX="$scanner_path.sqlite3-mod."
  run_phase phase-run1.log
  unset FAIL_DD_OF_PREFIX

  assert_window_hex "$pms_path" 0 "$pms_patched" "run 1 PMS pool site was not patched"
  assert_source_id "$pms_path" "$source_id_new" "run 1 PMS source id was not patched"
  assert_window_hex "$scanner_path" 0 "$scanner_orig" "run 1 Scanner changed after forced write failure"
  assert_contains "plex_patch event=source-id-patched binary=$pms_path" "$case_root/phase-run1.log" "run 1 PMS success log"
  assert_contains "pool_patch event=write-failed binary=$scanner_path" "$case_root/phase-run1.log" "run 1 Scanner failure log"

  run_phase phase-run2.log
  assert_window_hex "$pms_path" 0 "$pms_patched" "run 2 PMS pool site changed"
  assert_source_id "$pms_path" "$source_id_new" "run 2 PMS source id changed"
  assert_window_hex "$scanner_path" 0 "$scanner_patched" "run 2 Scanner recovery did not run through selection"
  assert_contains "plex_patch event=already-patched binary=$pms_path" "$case_root/phase-run2.log" "run 2 PMS idempotence log"
  assert_contains "pool_patch event=patched binary=$scanner_path site=scanner-site" "$case_root/phase-run2.log" "run 2 Scanner recovery log"
}

run_pms_failure_is_terminal() {
  setup_case pms_failure_is_terminal
  export FAIL_DD_OF_PREFIX="$pms_path.sqlite3-mod."
  run_phase phase.log 1
  unset FAIL_DD_OF_PREFIX

  assert_window_hex "$pms_path" 0 "$pms_orig" "PMS changed after forced write failure"
  assert_source_id "$pms_path" "$source_id_old" "PMS source id changed after forced write failure"
  assert_window_hex "$scanner_path" 0 "$scanner_patched" "Scanner did not proceed after PMS failure"
  assert_contains "plex_patch event=write-failed binary=$pms_path" "$phase_log" "PMS failure detail log"
  assert_contains "mod=plex phase=04-plex-patch event=failed step=pms-patch" "$phase_log" "PMS terminal failure log"
  assert_not_contains "mod=plex phase=04-plex-patch event=complete" "$phase_log" "PMS failure success marker"
}

wanted_case="${1:-}"
ran=0
for name in data_driven_detector_path pool_patched_input not_current_reason ambiguous_source_id_pin invalid_source_id_pin repeated_scanner_recovery pms_failure_is_terminal; do
  if case_selected "$wanted_case" "$name"; then
    "run_${name}"
    ran=1
  fi
done

[ "$ran" -eq 1 ] || fail "no matching Plex patch phase case selected"
printf 'Plex patch phase tests passed\n'
