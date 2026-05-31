#!/usr/bin/env bash
set -euo pipefail

. ./tools/ci/lib/assertions.sh

bad_signal_re='\bSQLITE_(ERROR|INTERNAL|NOMEM|IOERR|CORRUPT|FULL|CANTOPEN|PROTOCOL|MISMATCH|MISUSE|NOTADB|FORMAT|AUTH)\b|database disk image is malformed|Cannot create table|no such function|no such collation|Failed to open'

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_eq() {
  expected=$1
  actual=$2
  context=$3
  [ "$expected" = "$actual" ] || fail "$context: expected [$expected], actual [$actual]"
}

fixture() {
  printf 'tests/fixtures/%s\n' "$1"
}

run_expect_fail() {
  context=$1
  shift
  set +e
  bash -c '. ./tools/ci/lib/assertions.sh; "$@"' _ "$@" >/tmp/mod-bake-negative.out 2>/tmp/mod-bake-negative.err
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    cat /tmp/mod-bake-negative.out >&2
    cat /tmp/mod-bake-negative.err >&2
    fail "$context: expected non-zero exit"
  fi
}

emby_run2="$(fixture mod-bake-emby-run2-pass.txt)"
emby_run3="$(fixture mod-bake-emby-run3-pass.txt)"
plex_run1="$(fixture mod-bake-plex-amd64-run1-pass.txt)"

assert_eq "1" "$(fixed_count 'event=config-updated tag=MaxLibraryDbConnections' "$emby_run2")" "fixed_count"
require_grep 'event=skip-already-current' "$emby_run2" "skip marker should be present"
require_egrep 'arch=linux-x86_64-v(2|3)' "$emby_run2" "arch marker should match"
reject_grep 'event=installed' "$emby_run2" "installed marker should be absent"
assert_component_present "$emby_run2" "run 2"
assert_no_bad_signal "$emby_run2" "run 2" "$bad_signal_re"
assert_common_run "$emby_run2" "run 2" 'arch=linux-x86_64-v(2|3)' "$bad_signal_re"
assert_swap_run "$emby_run2" "run 2" skip
assert_runtime_load "$emby_run2" "run 2" emby "3.53.1"
assert_plex_run "$plex_run1" "run 1" installed amd64 'arch=linux-x86_64-v(2|3)' "$bad_signal_re"
assert_emby_tag_transition "$emby_run2" MaxLibraryDbConnections "run 2"
assert_emby_tag_transition "$emby_run2" MaxAuthDbConnections "run 2"
assert_emby_tag_transition "$emby_run2" MaxOtherDbConnections "run 2"
assert_emby_tag_transition "$emby_run2" EnableSqLiteMmio "run 2"
assert_emby_run3_idempotent "$emby_run3"

run_expect_fail "require_grep missing marker" require_grep missing "$(fixture mod-bake-require-missing.txt)" "missing marker"
run_expect_fail "require_egrep missing marker" require_egrep 'arch=linux-arm64' "$emby_run2" "missing arch"
run_expect_fail "reject_grep unexpected marker" reject_grep 'event=skip-already-current' "$emby_run2" "unexpected skip"
run_expect_fail "assert_component_present missing component" assert_component_present "$(fixture mod-bake-component-missing.txt)" "run 1"
run_expect_fail "assert_no_bad_signal detects signal" assert_no_bad_signal "$(fixture mod-bake-bad-signal.txt)" "run 1" "$bad_signal_re"
run_expect_fail "assert_common_run missing arch" assert_common_run "$emby_run2" "run 2" 'arch=linux-arm64' "$bad_signal_re"

run_expect_fail "assert_swap_run installed missing installed" assert_swap_run "$(fixture mod-bake-swap-missing-installed.txt)" "run 1" installed
run_expect_fail "assert_swap_run skip missing skip" assert_swap_run "$(fixture mod-bake-swap-missing-skip.txt)" "run 2" skip
run_expect_fail "assert_swap_run skip reinstalled" assert_swap_run "$(fixture mod-bake-swap-reinstalled.txt)" "run 2" skip

run_expect_fail "assert_runtime_load missing version" assert_runtime_load "$(fixture mod-bake-runtime-missing-version.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_runtime_load wrong version" assert_runtime_load "$(fixture mod-bake-runtime-wrong-version.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_runtime_load missing mmap" assert_runtime_load "$(fixture mod-bake-runtime-missing-mmap.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_runtime_load missing trace" assert_runtime_load "$(fixture mod-bake-runtime-missing-trace.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_runtime_load missing open" assert_runtime_load "$(fixture mod-bake-runtime-missing-open.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_runtime_load rc21" assert_runtime_load "$(fixture mod-bake-runtime-rc21.txt)" "run 1" emby "3.53.1"
run_expect_fail "assert_plex_run missing pool patch" assert_plex_run "$(fixture mod-bake-plex-missing-patched.txt)" "run 1" installed amd64 'arch=linux-x86_64-v(2|3)' "$bad_signal_re"

run_expect_fail "assert_emby_tag_transition zero markers" assert_emby_tag_transition "$(fixture mod-bake-emby-transition-zero.txt)" MaxLibraryDbConnections "run 2"
run_expect_fail "assert_emby_tag_transition two different markers" assert_emby_tag_transition "$(fixture mod-bake-emby-transition-two-different.txt)" MaxLibraryDbConnections "run 2"
run_expect_fail "assert_emby_tag_transition config write failed" assert_emby_tag_transition "$(fixture mod-bake-emby-transition-write-failed.txt)" MaxLibraryDbConnections "run 2"
run_expect_fail "assert_emby_tag_transition ambiguous element" assert_emby_tag_transition "$(fixture mod-bake-emby-transition-ambiguous-element.txt)" MaxLibraryDbConnections "run 2"
run_expect_fail "emby_tag_forms unsupported" emby_tag_forms UnsupportedTag

run_expect_fail "assert_emby_run3_idempotent updated" assert_emby_run3_idempotent "$(fixture mod-bake-emby-run3-updated.txt)"
run_expect_fail "assert_emby_run3_idempotent write failed" assert_emby_run3_idempotent "$(fixture mod-bake-emby-run3-write-failed.txt)"
run_expect_fail "assert_emby_run3_idempotent total break" assert_emby_run3_idempotent "$(fixture mod-bake-emby-run3-total-break.txt)"

printf 'mod bake assertions tests passed\n'
