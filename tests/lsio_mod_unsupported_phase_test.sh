#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-unsupported-phase.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-unsupported-phase.XXXXXX)"
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

write_hex_file() {
  path=$1
  hex=$2
  printf '%s' "$hex" | xxd -r -p > "$path"
}

case_selected() {
  wanted_mod="${1:-}"
  wanted_phase="${2:-}"
  case_mod="$3"
  case_phase="$4"
  { [ -z "$wanted_mod" ] || [ "$wanted_mod" = "$case_mod" ]; } &&
    { [ -z "$wanted_phase" ] || [ "$wanted_phase" = "$case_phase" ]; }
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
  cp lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh "$lib_root/plex-pool-patch.sh"
}

prepare_plex_case() {
  case_root="$tmp_root/plex-$phase"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  runtime_root="$fixture/plex"
  pms_path="$runtime_root/Plex Media Server"
  scanner_path="$runtime_root/Plex Media Scanner"
  mkdir -p "$runtime_root" "$fixture/opt/sqlite3-lsio-mod"
  setup_lib_root "$lib_root"

  pms_orig="00112233445566778899aabbccddeeff"
  pms_patched="ff112233445566778899aabbccddeeff"
  scanner_orig="102132435465768798a9bacbdcedfe0f"
  scanner_patched="ef2132435465768798a9bacbdcedfe0f"
  write_hex_file "$pms_path" "$pms_orig"
  write_hex_file "$case_root/pms-patched" "$pms_patched"
  write_hex_file "$scanner_path" "$scanner_orig"
  write_hex_file "$case_root/scanner-patched" "$scanner_patched"

  pms_sha="$(sha_file "$pms_path")"
  pms_patched_sha="$(sha_file "$case_root/pms-patched")"
  scanner_sha="$(sha_file "$scanner_path")"
  scanner_patched_sha="$(sha_file "$case_root/scanner-patched")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-unsupported|linux-arm64|plex_pms:pristine|$pms_path|$pms_sha
detect|1|plex|plex-unsupported|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-unsupported|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_sha
detect|1|plex|plex-unsupported|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
pre|2|plex|plex-unsupported|linux-arm64|plex_icu_linked:libicuucplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicuucplex.so.69|$pms_sha
pre|2|plex|plex-unsupported|linux-arm64|plex_icu_linked:libicui18nplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicui18nplex.so.69|$scanner_sha
pre|2|plex|plex-unsupported|linux-arm64|plex_icu_linked:libicudataplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|/usr/lib/plexmediaserver/lib/libicudataplex.so.69|$pms_patched_sha
pool-site|1|plex|plex-unsupported|linux-arm64|$pms_path|pms-site|0|0|$pms_orig|$pms_patched
pool-site|1|plex|plex-unsupported|linux-arm64|$scanner_path|scanner-site|0|0|$scanner_orig|$scanner_patched
unsupported|1|plex|plex-unsupported|linux-arm64|icu69|local-offline-fixture
EOF_PINS

  script_copy="$case_root/run"
  cp "lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-$phase/run" "$script_copy"
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

prepare_emby_case() {
  case_root="$tmp_root/emby-$phase"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  emby_root="$fixture/emby"
  deps_path="$emby_root/EmbyServer.deps.json"
  dll_path="$emby_root/EmbyServer.dll"
  mkdir -p "$emby_root" "$fixture/opt/sqlite3-lsio-mod"
  setup_lib_root "$lib_root"

  printf '{"runtimeTarget":{"name":"fixture"}}\n' > "$deps_path"
  printf 'fixture emby dll\n' > "$dll_path"
  deps_sha="$(sha_file "$deps_path")"
  dll_sha="$(sha_file "$dll_path")"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|emby|emby-unsupported|linux-arm64|emby_deps|$deps_path|$deps_sha
detect|1|emby|emby-unsupported|linux-arm64|emby_dll|$dll_path|$dll_sha
unsupported|1|emby|emby-unsupported|linux-arm64|generic|local-offline-fixture
EOF_PINS

  script_copy="$case_root/run"
  cp "lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-$phase/run" "$script_copy"
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
  phase="$2"
  "prepare_${mod}_case"
  chmod +x "$script_copy"
  phase_log="$case_root/phase.log"
  set +e
  PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
  assert_eq "0" "$rc" "$mod $phase unsupported exit status"
  assert_contains "mod=${mod} phase=" "$phase_log" "$mod $phase log has mod"
  assert_contains "event=unsupported-server" "$phase_log" "$mod $phase unsupported log"
  case "$mod" in
    plex) assert_contains "compat_group=icu69 reason=local-offline-fixture" "$phase_log" "$mod $phase unsupported reason" ;;
    emby) assert_contains "compat_group=generic reason=local-offline-fixture" "$phase_log" "$mod $phase unsupported reason" ;;
  esac
  if grep -Fq 'event=missing-artifact-row' "$phase_log"; then
    cat "$phase_log" >&2
    fail "$mod $phase must not fatal or warn missing-artifact-row for unsupported tuple"
  fi
}

wanted_mod="${1:-}"
wanted_phase="${2:-}"
ran=0
for case_mod in plex emby; do
  case "$case_mod" in
    plex) case_phases="preflight verify swap poolpatch" ;;
    emby) case_phases="preflight verify swap config" ;;
    *) fail "unsupported test mod: $case_mod" ;;
  esac
  for case_phase in $case_phases; do
    if case_selected "$wanted_mod" "$wanted_phase" "$case_mod" "$case_phase"; then
      run_case "$case_mod" "$case_phase"
      ran=1
    fi
  done
done

[ "$ran" -eq 1 ] || fail "no matching unsupported phase case selected"
printf 'unsupported phase tests passed\n'
