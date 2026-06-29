#!/usr/bin/env bash

retry() {
  local attempt=1 max_attempts=3 backoff=5 rc=0

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
    echo "WARN: command failed (attempt ${attempt}/${max_attempts}), retrying in ${backoff}s: $*" >&2
    sleep "${backoff}"
    attempt=$((attempt + 1))
    backoff=$((backoff * 2))
  done
}
