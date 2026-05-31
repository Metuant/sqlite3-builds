#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"

actual_config="$(grep -cE '^[[:space:]]*\{ SQLITE_CONFIG_[A-Z0-9_]+,' "${repo_root}/src/observability.c")"
expected_config="$(tr -d '[:space:]' < "${repo_root}/build/expected-sqlite-config-count.txt")"

if [[ "${actual_config}" != "${expected_config}" ]]; then
  echo "FATAL: observability SQLITE_CONFIG_ decode-table count mismatch: actual=${actual_config}, expected=${expected_config}; regenerate build/expected-sqlite-config-count.txt after auditing src/observability.c" >&2
  exit 1
fi

actual_dbconfig="$(grep -cE '^[[:space:]]*\{ SQLITE_DBCONFIG_[A-Z0-9_]+,' "${repo_root}/src/observability.c")"
expected_dbconfig="$(tr -d '[:space:]' < "${repo_root}/build/expected-sqlite-dbconfig-count.txt")"

if [[ "${actual_dbconfig}" != "${expected_dbconfig}" ]]; then
  echo "FATAL: observability SQLITE_DBCONFIG_ decode-table count mismatch: actual=${actual_dbconfig}, expected=${expected_dbconfig}; regenerate build/expected-sqlite-dbconfig-count.txt after auditing src/observability.c" >&2
  exit 1
fi

echo "observability decode-table counts OK (config=${actual_config}, dbconfig=${actual_dbconfig})"
