#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-sourcing.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-sourcing.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  local message expected actual
  message="$1"
  expected="$2"
  actual="$3"
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$message" "$expected" "$actual" >&2
  exit 1
}

assert_eq() {
  local expected actual message
  expected="$1"
  actual="$2"
  message="$3"
  [ "$actual" = "$expected" ] || fail "$message" "$expected" "$actual"
}

assert_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) fail "$message" "contains [$needle]" "$haystack" ;;
  esac
}

set +e
bash -n scripts/optimize_media_servers.sh >"$tmp/bash-n.out" 2>"$tmp/bash-n.err"
rc=$?
set -e
assert_eq 0 "$rc" "bash -n scripts/optimize_media_servers.sh rc"
assert_eq "" "$(cat "$tmp/bash-n.err")" "bash -n stderr"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/docker" <<'EOF_DOCKER'
#!/usr/bin/env bash
printf 'docker called\n' > "${OPTIMIZE_SOURCE_PROBE_DIR}/docker-called"
exit 77
EOF_DOCKER
chmod +x "$tmp/bin/docker"

cat > "$tmp/probe.sh" <<'EOF_PROBE'
#!/usr/bin/env bash
set +e
set +x

PATH="${1}:$PATH"
export PATH
export OPTIMIZE_SOURCE_PROBE_DIR="${2}"

. ./scripts/optimize_media_servers.sh

declare -F discover_fts_tables >/dev/null || {
  printf 'missing-discover-helper\n'
  exit 10
}
declare -F run_fts_integrity_gate >/dev/null || {
  printf 'missing-integrity-helper\n'
  exit 11
}
declare -F run_foreign_key_check_warn >/dev/null || {
  printf 'missing-fk-helper\n'
  exit 12
}
declare -F run_post_swap_fts_maintenance >/dev/null || {
  printf 'missing-post-swap-helper\n'
  exit 13
}
declare -F build_emby_optimize_sql >/dev/null || {
  printf 'missing-emby-optimize-helper\n'
  exit 21
}
declare -F build_plex_optimize_sql >/dev/null || {
  printf 'missing-plex-optimize-helper\n'
  exit 22
}
declare -F plex_stat4_preflight >/dev/null || {
  printf 'missing-plex-stat4-preflight-helper\n'
  exit 23
}
declare -F discover_plex_stat4_analyze_targets >/dev/null || {
  printf 'missing-plex-stat4-discovery-helper\n'
  exit 24
}
declare -F run_plex_stat4_analyze >/dev/null || {
  printf 'missing-plex-stat4-run-helper\n'
  exit 25
}
declare -F try_deflate_plex_statistics_bandwidth >/dev/null || {
  printf 'missing-deflate-helper\n'
  exit 14
}

[ "${PAGE_SIZE:-}" = "16384" ] || {
  printf 'bad-page-size:%s\n' "${PAGE_SIZE:-unset}"
  exit 15
}
[ "${STATS_BANDWIDTH_RETAIN_DAYS:-}" = "90" ] || {
  printf 'bad-retain-days:%s\n' "${STATS_BANDWIDTH_RETAIN_DAYS:-unset}"
  exit 16
}
[ "${PLEX_OPTIMIZE_API:-}" = "0" ] || {
  printf 'bad-plex-optimize-api:%s\n' "${PLEX_OPTIMIZE_API:-unset}"
  exit 28
}
[ "${SQLITE3_DISABLE_AUTOPRAGMA:-}" = "1" ] || {
  printf 'bad-autopragma:%s\n' "${SQLITE3_DISABLE_AUTOPRAGMA:-unset}"
  exit 17
}
[ "${GENERIC_SQLITE_BINARY:-}" = "${HOME}/bin/sqlite3" ] || {
  printf 'bad-generic-sqlite-binary:%s\n' "${GENERIC_SQLITE_BINARY:-unset}"
  exit 26
}
old_binary_name="EMBY""_BINARY"
[ -z "${!old_binary_name+x}" ] || {
  printf 'emby-binary-alias-present:%s\n' "${!old_binary_name}"
  exit 27
}

case "$-" in
  *e*)
    printf 'errexit-enabled:%s\n' "$-"
    exit 18
    ;;
esac
case "$-" in
  *x*)
    printf 'xtrace-enabled:%s\n' "$-"
    exit 19
    ;;
esac

false
printf 'after-false\n'

if [ -e "${OPTIMIZE_SOURCE_PROBE_DIR}/docker-called" ]; then
  printf 'driver-command-ran\n'
  exit 20
fi

printf 'source-ok\n'
EOF_PROBE

set +e
bash "$tmp/probe.sh" "$tmp/bin" "$tmp" >"$tmp/probe.out" 2>"$tmp/probe.err"
rc=$?
set -e

probe_out="$(cat "$tmp/probe.out")"
probe_err="$(cat "$tmp/probe.err")"
assert_eq 0 "$rc" "source probe rc"
assert_contains "$probe_out" "after-false" "source probe continued after false"
assert_contains "$probe_out" "source-ok" "source probe completion marker"
assert_eq "" "$probe_err" "source probe stderr"
[ ! -e "$tmp/docker-called" ] || fail "source should not run driver commands" "no docker marker" "docker marker exists"

printf 'optimize_media_servers sourcing tests passed\n'
