#!/usr/bin/env bash

retry() {
  local attempt=1 max_attempts=6 backoff=10 delay=0 rc=0

  while true; do
    if "$@"; then
      return 0
    else
      rc=$?
    fi
    if [ "${attempt}" -ge "${max_attempts}" ]; then
      echo "FATAL: command failed after ${attempt} attempts: $*" >&2
      return "${rc}"
    fi
    delay=$((backoff / 2 + RANDOM % (backoff - backoff / 2 + 1)))
    echo "WARN: command failed (attempt ${attempt}/${max_attempts}), retrying in ${delay}s: $*" >&2
    sleep "${delay}"
    attempt=$((attempt + 1))
    backoff=$((backoff * 2))
    if [ "${backoff}" -gt 60 ]; then
      backoff=60
    fi
  done
}
