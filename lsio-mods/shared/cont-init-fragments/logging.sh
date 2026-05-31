#!/usr/bin/env bash
sqlite3_mod_component="sqlite3-lsio-mod"

sqlite3_mod_init_logging() {
  :
}

sqlite3_mod_log() {
  level=$1
  shift
  line="level=${level} component=${sqlite3_mod_component} $*"
  printf '%s\n' "$line"
}

log_info() { sqlite3_mod_log info "$@"; }
log_warn() { sqlite3_mod_log warn "$@"; }
log_fatal() { sqlite3_mod_log fatal "$@"; exit 1; }
