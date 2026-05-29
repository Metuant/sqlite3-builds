#!/usr/bin/env bash
sqlite3_mod_component="sqlite3-lsio-mod"
sqlite3_mod_log_dir="/tmp/sqlite3-lsio-mod"
sqlite3_mod_log_file="${sqlite3_mod_log_dir}/sqlite3-lsio-mod.log"

sqlite3_mod_init_logging() {
  if mkdir -p "$sqlite3_mod_log_dir" 2>/dev/null; then
    : >> "$sqlite3_mod_log_file" 2>/dev/null || sqlite3_mod_log_file=""
  else
    sqlite3_mod_log_file=""
  fi
}

sqlite3_mod_log() {
  level=$1
  shift
  line="level=${level} component=${sqlite3_mod_component} $*"
  printf '%s\n' "$line"
  if [ -n "${sqlite3_mod_log_file:-}" ]; then
    printf '%s\n' "$line" >> "$sqlite3_mod_log_file" 2>/dev/null || true
  fi
}

log_info() { sqlite3_mod_log info "$@"; }
log_warn() { sqlite3_mod_log warn "$@"; }
log_fatal() { sqlite3_mod_log fatal "$@"; exit 1; }
