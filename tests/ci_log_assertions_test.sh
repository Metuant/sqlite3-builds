#!/usr/bin/env bash
set -euo pipefail

. ./tools/ci/lib/assertions.sh

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

run_expect_fail() {
  context=$1
  shift
  set +e
  "$@" >/tmp/ci-negative.out 2>/tmp/ci-negative.err
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    cat /tmp/ci-negative.out >&2
    cat /tmp/ci-negative.err >&2
    fail "$context: expected non-zero exit"
  fi
}

fixture() {
  printf 'tests/fixtures/%s\n' "$1"
}

pass_log="$(cat "$(fixture ci-log-pass.txt)")"
missing_marker_log="$(cat "$(fixture ci-log-missing-marker.txt)")"
unexpected_marker_log="$(cat "$(fixture ci-log-unexpected-marker.txt)")"
bad_signal_log="$(cat "$(fixture ci-log-bad-signal.txt)")"
open_missing_log="$(cat "$(fixture ci-log-open-missing.txt)")"
config_rc21_log="$(cat "$(fixture ci-log-config-rc21.txt)")"
wrong_version_log="$(cat "$(fixture ci-log-emby-version-wrong.txt)")"
compiler_missing_line_log="$(cat "$(fixture ci-log-compiler-missing-line.txt)")"
compiler_missing_flag_log="$(cat "$(fixture ci-log-compiler-missing-flag.txt)")"

cursor_slice="$(slice_log_after_cursor 2 < "$(fixture ci-log-cursor-same-second.txt)")"
assert_marker_present "$cursor_slice" 'new-run Sqlite version: 3\.53\.1' "cursor slice missing new run"
assert_marker_absent "$cursor_slice" 'old-run' "cursor slice retained old run"

assert_marker_present "$pass_log" '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "marker should be present"
run_expect_fail "assert_marker_present missing marker" assert_marker_present "$missing_marker_log" '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "missing marker"

assert_marker_absent "$missing_marker_log" 'slow_query' "marker should be absent"
run_expect_fail "assert_marker_absent unexpected marker" assert_marker_absent "$unexpected_marker_log" '\[sqlite3-builds-obs\]' "unexpected marker"

bad_signal_re='\bSQLITE_(ERROR|INTERNAL|NOMEM|IOERR|CORRUPT|FULL|CANTOPEN|PROTOCOL|MISMATCH|MISUSE|NOTADB|FORMAT|AUTH)\b|database disk image is malformed|Cannot create table|no such function|no such collation|Failed to open'
scan_bad_signal "$pass_log" "$bad_signal_re" "bad signal should be absent"
run_expect_fail "scan_bad_signal detects bad signal" scan_bad_signal "$bad_signal_log" "$bad_signal_re" "bad signal expected"

open_count="$(assert_open_marker_count "$pass_log" "Emby")"
assert_eq "1" "$open_count" "open marker count"
run_expect_fail "assert_open_marker_count missing open marker" assert_open_marker_count "$open_missing_log" "Emby"

assert_no_startup_config_rc21 "$pass_log" "Emby"
run_expect_fail "assert_no_startup_config_rc21 detects rc21" assert_no_startup_config_rc21 "$config_rc21_log" "Emby"

assert_emby_sqlite_version "Sqlite version: 3.53.1" "3.53.1"
run_expect_fail "assert_emby_sqlite_version wrong version" assert_emby_sqlite_version "$wrong_version_log" "3.53.1"

assert_emby_compile_options "$pass_log" >/tmp/ci-compile.out
run_expect_fail "assert_emby_compile_options missing line" assert_emby_compile_options "$compiler_missing_line_log"
run_expect_fail "assert_emby_compile_options missing flag" assert_emby_compile_options "$compiler_missing_flag_log"

printf 'ci log assertions tests passed\n'
