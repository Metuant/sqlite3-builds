#!/usr/bin/env bash
# phase_name is supplied by each s6 run script before sourcing this fragment.
# shellcheck disable=SC2154
sqlite3_mod_copy_verified_bytes() {
  local tmp=$1
  local _target=$2
  local src=$3
  local expected_sha=$4
  local tmp_sha
  if ! cp -f "$src" "$tmp"; then
    sqlite3_mod_replace_failure_event="temp-copy-failed"
    return 1
  fi
  tmp_sha="$(sha256_of "$tmp")"
  if [ "$tmp_sha" != "$expected_sha" ]; then
    sqlite3_mod_replace_failure_event="temp-sha-mismatch"
    sqlite3_mod_replace_failure_actual="$tmp_sha"
    return 1
  fi
  return 0
}

sqlite3_mod_verified_replace() {
  local src=$1
  local target=$2
  local expected_sha=$3
  local actual_sha
  sqlite3_mod_replace_failure_event=""
  sqlite3_mod_replace_failure_actual=""
  if ! sqlite3_mod_atomic_replace "$target" sqlite3_mod_copy_verified_bytes "$src" "$expected_sha"; then
    return 1
  fi
  actual_sha="$(sha256_of "$target")"
  if [ "$actual_sha" != "$expected_sha" ]; then
    sqlite3_mod_replace_failure_event="readback-sha-mismatch"
    sqlite3_mod_replace_failure_actual="$actual_sha"
    return 1
  fi
  return 0
}

install_sqlite_artifact() {
  local src=$1
  local target=$2
  local expected_sha=$3
  if ! sqlite3_mod_verified_replace "$src" "$target" "$expected_sha"; then
    case "${sqlite3_mod_replace_failure_event:-}" in
      temp-copy-failed) log_warn "phase=${phase_name} event=temp-copy-failed target_path=${target}" ;;
      temp-sha-mismatch) log_warn "phase=${phase_name} event=temp-sha-mismatch target_path=${target} expected=${expected_sha} actual=${sqlite3_mod_replace_failure_actual}" ;;
      readback-sha-mismatch) log_warn "phase=${phase_name} event=installed-sha-mismatch target_path=${target} expected=${expected_sha} actual=${sqlite3_mod_replace_failure_actual}" ;;
      *) log_warn "phase=${phase_name} event=mv-failed target_path=${target}" ;;
    esac
    return 1
  fi
  return 0
}

restore_sqlite_baseline() {
  local src=$1
  local target=$2
  local expected_sha=$3
  if ! sqlite3_mod_verified_replace "$src" "$target" "$expected_sha"; then
    case "${sqlite3_mod_replace_failure_event:-}" in
      temp-copy-failed) log_warn "phase=${phase_name} event=restore-temp-copy-failed target_path=${target}" ;;
      temp-sha-mismatch) log_warn "phase=${phase_name} event=restore-temp-sha-mismatch target_path=${target} expected=${expected_sha} actual=${sqlite3_mod_replace_failure_actual}" ;;
      readback-sha-mismatch) log_warn "phase=${phase_name} event=restored-sha-mismatch target_path=${target} expected=${expected_sha} actual=${sqlite3_mod_replace_failure_actual}" ;;
      *) log_warn "phase=${phase_name} event=restore-mv-failed target_path=${target}" ;;
    esac
    return 1
  fi
  return 0
}

recover_failed_sqlite_install() {
  local mod=$1
  local bak=$2
  local target=$3
  local baseline_sha=$4
  local artifact_sha=$5
  local failure_reason final_sha

  failure_reason="no-trusted-backup"
  if [ -f "$bak" ] && [ "$(sha256_of "$bak")" = "$baseline_sha" ]; then
    if restore_sqlite_baseline "$bak" "$target" "$baseline_sha"; then
      log_warn "mod=${mod} phase=${phase_name} event=install-failed-baseline-restored target_path=${target} sha256=${baseline_sha}"
      return 0
    fi
    failure_reason="${sqlite3_mod_replace_failure_event:-restore-failed}"
  fi

  final_sha=""
  if [ -f "$target" ]; then
    final_sha="$(sha256_of "$target")"
  fi
  if [ "$final_sha" = "$baseline_sha" ]; then
    log_warn "mod=${mod} phase=${phase_name} event=install-failed-target-preserved target_path=${target} reason=${failure_reason} sha256=${final_sha}"
    return 0
  fi
  if [ "$final_sha" = "$artifact_sha" ]; then
    log_warn "mod=${mod} phase=${phase_name} event=install-failed-target-current target_path=${target} reason=${failure_reason} sha256=${final_sha}"
    return 0
  fi

  log_warn "mod=${mod} phase=${phase_name} event=install-recovery-unsafe target_path=${target} reason=${failure_reason} expected_artifact=${artifact_sha} expected_baseline=${baseline_sha} actual=${final_sha:-unavailable}"
  return 1
}
