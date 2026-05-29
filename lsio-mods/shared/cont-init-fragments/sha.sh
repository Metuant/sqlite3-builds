#!/usr/bin/env bash
require_cmd() {
  command -v "$1" >/dev/null 2>&1 || return 1
}

require_all_or_warn() {
  for cmd in "$@"; do
    if ! require_cmd "$cmd"; then
      log_warn "phase=${phase_name:-unknown} event=missing-command command=${cmd}"
      return 1
    fi
  done
}

sha256_of() {
  sha256sum "$1" | awk '{print $1}'
}
