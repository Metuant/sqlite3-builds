#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/arch.sh

uname() {
  if [ "$#" -eq 1 ] && [ "$1" = "-m" ]; then
    printf 'x86_64\n'
    return 0
  fi
  command uname "$@"
}

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/resolve-arch-test.XXXXXX" 2>/dev/null || mktemp -d /tmp/resolve-arch-test.XXXXXX)"
cleanup() {
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

write_cpuinfo() {
  path="$1"
  flags="$2"
  cat > "$path" <<EOF_CPUINFO
processor   : 0
vendor_id   : GenuineIntel
cpu family  : 6
model       : 140
model name  : fixture
flags       : $flags
EOF_CPUINFO
}

run_resolve_arch() {
  fixture="$1"
  stdout_path="$2"
  stderr_path="$3"
  set +e
  resolve_arch "$fixture" >"$stdout_path" 2>"$stderr_path"
  rc=$?
  set -e
  return "$rc"
}

v2_flags="cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3"
v3_extra_flags="avx avx2 bmi1 bmi2 f16c fma abm movbe xsave"

v3_fixture="$tmp_root/v3.cpuinfo"
write_cpuinfo "$v3_fixture" "$v2_flags $v3_extra_flags"
v3_stdout="$tmp_root/v3.stdout"
v3_stderr="$tmp_root/v3.stderr"
if run_resolve_arch "$v3_fixture" "$v3_stdout" "$v3_stderr"; then
  :
else
  rc=$?
  stderr_text="$(cat "$v3_stderr")"
  fail "v3 fixture exit status: expected [0], actual [$rc], stderr [$stderr_text]"
fi
v3_actual="$(cat "$v3_stdout")"
assert_eq "linux-x86_64-v3" "$v3_actual" "v3 fixture result"

v2_fixture="$tmp_root/v2.cpuinfo"
write_cpuinfo "$v2_fixture" "cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3 avx bmi1 bmi2 f16c fma abm movbe xsave"
v2_stdout="$tmp_root/v2.stdout"
v2_stderr="$tmp_root/v2.stderr"
if run_resolve_arch "$v2_fixture" "$v2_stdout" "$v2_stderr"; then
  :
else
  rc=$?
  stderr_text="$(cat "$v2_stderr")"
  fail "v2 fixture exit status: expected [0], actual [$rc], stderr [$stderr_text]"
fi
v2_actual="$(cat "$v2_stdout")"
assert_eq "linux-x86_64-v2" "$v2_actual" "v2 fixture result"

unsupported_fixture="$tmp_root/unsupported.cpuinfo"
write_cpuinfo "$unsupported_fixture" "cx16 lahf_lm popcnt pni sse4_1 ssse3"
unsupported_stdout="$tmp_root/unsupported.stdout"
unsupported_stderr="$tmp_root/unsupported.stderr"
unsupported_rc=0
if run_resolve_arch "$unsupported_fixture" "$unsupported_stdout" "$unsupported_stderr"; then
  unsupported_actual="$(cat "$unsupported_stdout")"
  fail "unsupported fixture exit status: expected non-zero, actual [0], stdout [$unsupported_actual]"
else
  unsupported_rc=$?
fi
unsupported_actual="$(cat "$unsupported_stdout")"
assert_eq "" "$unsupported_actual" "unsupported fixture stdout"
[ "$unsupported_rc" -ne 0 ] || fail "unsupported fixture exit status: expected non-zero, actual [$unsupported_rc]"

printf 'resolve_arch tests passed\n'
