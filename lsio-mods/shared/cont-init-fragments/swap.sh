#!/usr/bin/env bash
# phase_name is supplied by each s6 run script before sourcing this fragment.
# shellcheck disable=SC2154
sqlite3_mod_copy_artifact() {
  local tmp=$1
  local _target=$2
  local src=$3
  local expected_sha=$4
  local target_path=$5
  local tmp_sha
  if ! cp -f "$src" "$tmp"; then
    sqlite3_mod_install_failure_event="temp-copy-failed"
    return 1
  fi
  tmp_sha="$(sha256_of "$tmp")"
  if [ "$tmp_sha" != "$expected_sha" ]; then
    sqlite3_mod_install_failure_event="temp-sha-mismatch"
    sqlite3_mod_install_failure_actual="$tmp_sha"
    sqlite3_mod_install_failure_target="$target_path"
    return 1
  fi
  return 0
}

install_sqlite_artifact() {
  local src=$1
  local target=$2
  local expected_sha=$3
  local installed_sha
  sqlite3_mod_install_failure_event=""
  sqlite3_mod_install_failure_actual=""
  # Retained as install-failure context for sourced callers.
  # shellcheck disable=SC2034
  sqlite3_mod_install_failure_target="$target"
  if ! sqlite3_mod_atomic_replace "$target" sqlite3_mod_copy_artifact "$src" "$expected_sha" "$target"; then
    case "${sqlite3_mod_install_failure_event:-}" in
      temp-copy-failed) log_warn "phase=${phase_name} event=temp-copy-failed target_path=${target}" ;;
      temp-sha-mismatch) log_warn "phase=${phase_name} event=temp-sha-mismatch target_path=${target} expected=${expected_sha} actual=${sqlite3_mod_install_failure_actual}" ;;
      *) log_warn "phase=${phase_name} event=mv-failed target_path=${target}" ;;
    esac
    return 1
  fi
  installed_sha="$(sha256_of "$target")"
  if [ "$installed_sha" != "$expected_sha" ]; then
    log_warn "phase=${phase_name} event=installed-sha-mismatch target_path=${target} expected=${expected_sha} actual=${installed_sha}"
    return 1
  fi
  return 0
}
