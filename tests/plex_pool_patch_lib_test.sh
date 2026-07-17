#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/atomic-write.sh
. ./lsio-mods/shared/cont-init-fragments/plex-patch.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-plex-pool.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-plex-pool.XXXXXX)"
cleanup() {
  chmod -R u+w "$tmp" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT

assert_eq() {
  expected=$1
  actual=$2
  message=$3
  [ "$actual" = "$expected" ] || {
    printf 'FATAL: %s: expected %s got %s\n' "$message" "$expected" "$actual" >&2
    exit 1
  }
}

assert_file_hex() {
  path=$1
  expected=$2
  message=$3
  actual="$(dd if="$path" bs=1 skip=0 count=16 2>/dev/null | od -An -tx1 -v | tr -d ' \n')"
  assert_eq "$expected" "$actual" "$message"
}

assert_window_hex() {
  path=$1
  offset=$2
  expected=$3
  message=$4
  actual="$(dd if="$path" bs=1 skip="$offset" count=16 2>/dev/null | od -An -tx1 -v | tr -d ' \n')"
  assert_eq "$expected" "$actual" "$message"
}

write_hex_file() {
  path=$1
  hex=$2
  printf '%s' "$hex" | xxd -r -p > "$path"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

run_patch() {
  log=$1
  shift
  set +e
  plex_pool_patch_apply_binary "$@" > "$log" 2>&1
  rc=$?
  set -e
  assert_eq 0 "$rc" "pool patch returned non-zero"
}

orig_hex="be140000004c89ff41b800000000506a"
patched_hex="be100000004c89ff41b800000000506a"
unknown_hex="ffffffffffffffffffffffffffffffff"
site="0x0|0|1|$orig_hex|$patched_hex"

bin="$tmp/pms"
write_hex_file "$bin" "$orig_hex"
chmod 0640 "$bin"
pre_sha="$(sha256_file "$bin")"
run_patch "$tmp/original.log" "$bin" "$pre_sha" "$site"
assert_file_hex "$bin" "$patched_hex" "original state was not patched"
assert_eq "640" "$(sqlite3_mod_stat_mode "$bin")" "patched binary mode was not preserved"
[ -f "$bin.bundled.bak" ] || { echo "FATAL: backup not created" >&2; exit 1; }
grep -Fq 'event=patched' "$tmp/original.log" || { echo "FATAL: patch log missing" >&2; exit 1; }

run_patch "$tmp/already.log" "$bin" "$pre_sha" "$site"
assert_file_hex "$bin" "$patched_hex" "already-patched state changed"
grep -Fq 'event=already-patched' "$tmp/already.log" || { echo "FATAL: already-patched log missing" >&2; exit 1; }

derived="$tmp/derived"
derived_orig="be140000004c89ff41b800000000506a"
derived_patched="be110000004c89ff41b800000000506a"
derived_site="0x0|0|1|$derived_orig|$derived_patched"
write_hex_file "$derived" "$derived_orig"
derived_sha="$(sha256_file "$derived")"
run_patch "$tmp/derived.log" "$derived" "$derived_sha" "$derived_site"
assert_file_hex "$derived" "$derived_patched" "derived write byte was not used"

arm64="$tmp/arm64"
arm64_orig="81028052e00313aae4031f2ae7031f2a"
arm64_patched="01028052e00313aae4031f2ae7031f2a"
arm64_site="0x0|0|0|$arm64_orig|$arm64_patched"
write_hex_file "$arm64" "$arm64_orig"
arm64_sha="$(sha256_file "$arm64")"
run_patch "$tmp/arm64.log" "$arm64" "$arm64_sha" "$arm64_site"
assert_file_hex "$arm64" "$arm64_patched" "arm64 write_index 0 site was not patched"

unknown_with_verified_backup="$tmp/unknown-verified"
write_hex_file "$unknown_with_verified_backup" "$unknown_hex"
cp "$bin.bundled.bak" "$unknown_with_verified_backup.bundled.bak"
run_patch "$tmp/unknown-verified.log" "$unknown_with_verified_backup" "$pre_sha" "$site"
assert_file_hex "$unknown_with_verified_backup" "$unknown_hex" "unknown target with verified backup was modified"
grep -Fq 'event=unknown-target-state' "$tmp/unknown-verified.log" || { echo "FATAL: unknown-state log missing" >&2; exit 1; }

unknown_without_backup="$tmp/unknown-no-backup"
write_hex_file "$unknown_without_backup" "$unknown_hex"
run_patch "$tmp/unknown-no-backup.log" "$unknown_without_backup" "$pre_sha" "$site"
assert_file_hex "$unknown_without_backup" "$unknown_hex" "unknown target without backup was modified"
grep -Fq 'event=unknown-target-state' "$tmp/unknown-no-backup.log" || { echo "FATAL: unknown no-backup log missing" >&2; exit 1; }

bad_backup="$tmp/bad-backup"
write_hex_file "$bad_backup" "$orig_hex"
write_hex_file "$bad_backup.bundled.bak" "$unknown_hex"
run_patch "$tmp/bad-backup.log" "$bad_backup" "$pre_sha" "$site"
assert_file_hex "$bad_backup" "$orig_hex" "target changed when existing backup SHA was bad"
grep -Fq 'event=bad-backup-sha' "$tmp/bad-backup.log" || { echo "FATAL: bad-backup-sha log missing" >&2; exit 1; }

missing="$tmp/missing"
run_patch "$tmp/missing.log" "$missing" "$pre_sha" "$site"
grep -Fq 'event=missing-binary' "$tmp/missing.log" || { echo "FATAL: missing-binary log missing" >&2; exit 1; }

no_sites="$tmp/no-sites"
write_hex_file "$no_sites" "$orig_hex"
run_patch "$tmp/no-sites.log" "$no_sites" "$pre_sha"
grep -Fq 'event=no-sites' "$tmp/no-sites.log" || { echo "FATAL: no-sites log missing" >&2; exit 1; }

pre_sha_mismatch="$tmp/pre-sha-mismatch"
write_hex_file "$pre_sha_mismatch" "$orig_hex"
run_patch "$tmp/pre-sha-mismatch.log" "$pre_sha_mismatch" "$unknown_hex" "$site"
assert_file_hex "$pre_sha_mismatch" "$orig_hex" "pre-sha-mismatch changed target"
grep -Fq 'event=pre-sha-mismatch' "$tmp/pre-sha-mismatch.log" || { echo "FATAL: pre-sha-mismatch log missing" >&2; exit 1; }

invalid_site="$tmp/invalid-site"
write_hex_file "$invalid_site" "$orig_hex"
invalid_site_sha="$(sha256_file "$invalid_site")"
invalid_site_row="bad-site|0|16|$orig_hex|$patched_hex"
run_patch "$tmp/invalid-site.log" "$invalid_site" "$invalid_site_sha" "$invalid_site_row"
assert_file_hex "$invalid_site" "$orig_hex" "invalid-site changed target"
grep -Fq 'event=invalid-site' "$tmp/invalid-site.log" || { echo "FATAL: invalid-site log missing" >&2; exit 1; }

leading_zero="$tmp/leading-zero"
leading_zero_orig="00112233445566778899aabbccddeeff"
leading_zero_patched="0011223344aa66778899aabbccddeeff"
leading_zero_padding="000000000000000000000000000000"
write_hex_file "$leading_zero" "${leading_zero_padding}${leading_zero_orig}"
leading_zero_sha="$(sha256_file "$leading_zero")"
leading_zero_site="leading-zero|015|020|$leading_zero_orig|$leading_zero_patched"
run_patch "$tmp/leading-zero.log" "$leading_zero" "$leading_zero_sha" "$leading_zero_site"
assert_window_hex "$leading_zero" 15 "$leading_zero_patched" "leading-zero decimal offset site was not patched"
grep -Fq 'event=patched' "$tmp/leading-zero.log" || { echo "FATAL: leading-zero patch log missing" >&2; exit 1; }

fake_bin="$tmp/fake-bin"
mkdir -p "$fake_bin"
real_cp="$(command -v cp)"
cat > "$fake_bin/cp" <<'EOF_CP'
#!/usr/bin/env bash
last=""
for arg in "$@"; do last=$arg; done
if [ -n "${FAIL_CP_DEST:-}" ] && [ "$last" = "$FAIL_CP_DEST" ]; then
  exit 1
fi
exec "$REAL_CP" "$@"
EOF_CP
chmod +x "$fake_bin/cp"

backup_fail="$tmp/backup-fail"
write_hex_file "$backup_fail" "$orig_hex"
export REAL_CP="$real_cp"
export FAIL_CP_DEST="$backup_fail.bundled.bak"
old_path=$PATH
PATH="$fake_bin:$PATH"
hash -r
run_patch "$tmp/backup-fail.log" "$backup_fail" "$pre_sha" "$site"
PATH=$old_path
hash -r
unset REAL_CP FAIL_CP_DEST
assert_file_hex "$backup_fail" "$orig_hex" "target changed after backup create failure"
grep -Fq 'event=backup-create-failed' "$tmp/backup-fail.log" || { echo "FATAL: backup-create-failed log missing" >&2; exit 1; }

real_dd="$(command -v dd)"
cat > "$fake_bin/dd" <<'EOF_DD'
#!/usr/bin/env bash
out=""
for arg in "$@"; do
  case "$arg" in
    of=*) out=${arg#of=} ;;
  esac
done
if [ -n "${FAIL_DD_OF:-}" ] && [ "$out" = "$FAIL_DD_OF" ]; then
  case "${FAIL_DD_MODE:-}" in
    fail-after-corrupt)
      printf '\377' | "$REAL_DD" of="$out" bs=1 seek=1 count=1 conv=notrunc 2>/dev/null
      exit 1
      ;;
    wrong-byte-success)
      printf '\377' | "$REAL_DD" of="$out" bs=1 seek=1 count=1 conv=notrunc 2>/dev/null
      exit 0
      ;;
  esac
fi
case "$out" in
  "${FAIL_DD_OF_PREFIX:-}"*)
    [ -n "${FAIL_DD_OF_PREFIX:-}" ] || exec "$REAL_DD" "$@"
  case "${FAIL_DD_MODE:-}" in
    fail-after-corrupt)
      printf '\377' | "$REAL_DD" of="$out" bs=1 seek=1 count=1 conv=notrunc 2>/dev/null
      exit 1
      ;;
    wrong-byte-success)
      printf '\377' | "$REAL_DD" of="$out" bs=1 seek=1 count=1 conv=notrunc 2>/dev/null
      exit 0
      ;;
  esac
  ;;
esac
exec "$REAL_DD" "$@"
EOF_DD
chmod +x "$fake_bin/dd"

write_fail="$tmp/write-fail"
write_hex_file "$write_fail" "$orig_hex"
export REAL_DD="$real_dd"
export REAL_CP="$real_cp"
export FAIL_DD_OF_PREFIX="$write_fail.sqlite3-mod."
export FAIL_DD_MODE="fail-after-corrupt"
PATH="$fake_bin:$PATH"
hash -r
run_patch "$tmp/write-fail.log" "$write_fail" "$pre_sha" "$site"
PATH=$old_path
hash -r
unset REAL_DD REAL_CP FAIL_DD_OF_PREFIX FAIL_DD_MODE
assert_file_hex "$write_fail" "$orig_hex" "write failure did not restore bundled backup"
grep -Fq 'event=write-failed' "$tmp/write-fail.log" || { echo "FATAL: write-failed log missing" >&2; exit 1; }

verify_fail="$tmp/verify-fail"
write_hex_file "$verify_fail" "$orig_hex"
export REAL_DD="$real_dd"
export REAL_CP="$real_cp"
export FAIL_DD_OF_PREFIX="$verify_fail.sqlite3-mod."
export FAIL_DD_MODE="wrong-byte-success"
PATH="$fake_bin:$PATH"
hash -r
run_patch "$tmp/verify-fail.log" "$verify_fail" "$pre_sha" "$site"
PATH=$old_path
hash -r
unset REAL_DD REAL_CP FAIL_DD_OF_PREFIX FAIL_DD_MODE
assert_file_hex "$verify_fail" "$orig_hex" "verify failure did not restore bundled backup"
grep -Fq 'event=verify-failed' "$tmp/verify-fail.log" || { echo "FATAL: verify-failed log missing" >&2; exit 1; }

printf 'plex pool patch lib tests passed\n'
