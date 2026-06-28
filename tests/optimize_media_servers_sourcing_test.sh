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

assert_not_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) fail "$message" "not contains [$needle]" "$haystack" ;;
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

cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
printf 'sqlite3 called\n' > "${OPTIMIZE_SOURCE_PROBE_DIR}/sqlite3-called"
exit 77
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"

cat > "$tmp/bin/sqlite-ok" <<'EOF_SQLITE_OK'
#!/usr/bin/env bash
printf 'sqlite-ok called\n' >> "${OPTIMIZE_SOURCE_PROBE_DIR}/sqlite-ok-called"
printf '1\n'
exit 0
EOF_SQLITE_OK
chmod +x "$tmp/bin/sqlite-ok"

cat > "$tmp/probe.sh" <<'EOF_PROBE'
#!/usr/bin/env bash
set +e
set +x

PATH="${1}:$PATH"
export PATH
export OPTIMIZE_SOURCE_PROBE_DIR="${2}"

. ./scripts/optimize_media_servers.sh

assert_unset() {
  local name
  name="$1"
  [ -z "${!name+x}" ] || {
    printf 'unexpected-var-set:%s=%s\n' "$name" "${!name}"
    exit 40
  }
}

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
declare -F load_optimize_config >/dev/null || {
  printf 'missing-config-loader\n'
  exit 29
}
declare -F print_usage >/dev/null || {
  printf 'missing-usage-helper\n'
  exit 30
}

declare -p PLEX_INSTANCES >/dev/null 2>&1 && {
  printf 'plex-instances-set-by-source\n'
  exit 31
}
declare -p EMBY_INSTANCES >/dev/null 2>&1 && {
  printf 'emby-instances-set-by-source\n'
  exit 32
}
assert_unset PLEX_BINARY
assert_unset GENERIC_SQLITE_BINARY
assert_unset BACKUP_PATH
assert_unset PLEX_OPTIMIZE_API
assert_unset PLEX_PROCESS_BLOB_DB
assert_unset STATS_BANDWIDTH_RETAIN_DAYS

[ "${_PAGE_SIZE:-}" = "16384" ] || {
  printf 'bad-page-size:%s\n' "${_PAGE_SIZE:-unset}"
  exit 15
}
[ "${_PLEX_DB:-}" = "com.plexapp.plugins.library.db" ] || {
  printf 'bad-plex-db:%s\n' "${_PLEX_DB:-unset}"
  exit 33
}
[ "${_PLEX_BLOB_DB:-}" = "com.plexapp.plugins.library.blobs.db" ] || {
  printf 'bad-plex-blob-db:%s\n' "${_PLEX_BLOB_DB:-unset}"
  exit 34
}
[ "${_EMBY_DB:-}" = "library.db" ] || {
  printf 'bad-emby-db:%s\n' "${_EMBY_DB:-unset}"
  exit 35
}
[ "${#_EMBY_INDEXES[@]}" -eq 1 ] || {
  printf 'bad-emby-index-count:%s\n' "${#_EMBY_INDEXES[@]}"
  exit 36
}
[ "${#_PLEX_INDEXES[@]}" -eq 2 ] || {
  printf 'bad-plex-index-count:%s\n' "${#_PLEX_INDEXES[@]}"
  exit 37
}
[ "${#_PLEX_STAT4_LEADER_INDEXES[@]}" -eq 2 ] || {
  printf 'bad-plex-stat4-leader-count:%s\n' "${#_PLEX_STAT4_LEADER_INDEXES[@]}"
  exit 38
}
[ "${SQLITE3_DISABLE_AUTOPRAGMA:-}" = "1" ] || {
  printf 'bad-autopragma:%s\n' "${SQLITE3_DISABLE_AUTOPRAGMA:-unset}"
  exit 17
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

missing_conf="$tmp/missing/optimize_media_servers.conf"
rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$missing_conf"
  bash scripts/optimize_media_servers.sh
) >"$tmp/missing-conf.out" 2>"$tmp/missing-conf.err"
rc=$?
set -e
assert_eq 1 "$rc" "missing config rc"
missing_err="$(cat "$tmp/missing-conf.err")"
assert_contains "$missing_err" "ERROR: required config file not found: $missing_conf" "missing config path diagnostic"
assert_contains "$missing_err" "see --help" "missing config help pointer"
[ ! -e "$tmp/docker-called" ] || fail "missing config should not run docker" "no docker marker" "docker marker exists"
[ ! -e "$tmp/sqlite3-called" ] || fail "missing config should not run sqlite3" "no sqlite marker" "sqlite marker exists"
[ ! -e "$tmp/sqlite-ok-called" ] || fail "missing config should not run configured sqlite" "no sqlite-ok marker" "sqlite-ok marker exists"

rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$missing_conf"
  bash scripts/optimize_media_servers.sh --bogus
) >"$tmp/bogus.out" 2>"$tmp/bogus.err"
rc=$?
set -e
assert_eq 2 "$rc" "unknown argument rc"
bogus_err="$(cat "$tmp/bogus.err")"
assert_contains "$bogus_err" "Usage:" "unknown argument usage"
assert_contains "$bogus_err" "--help" "unknown argument help pointer"
assert_not_contains "$bogus_err" "required config file not found" "unknown argument before config lookup"
[ ! -e "$tmp/docker-called" ] || fail "unknown argument should not run docker" "no docker marker" "docker marker exists"
[ ! -e "$tmp/sqlite3-called" ] || fail "unknown argument should not run sqlite3" "no sqlite marker" "sqlite marker exists"
[ ! -e "$tmp/sqlite-ok-called" ] || fail "unknown argument should not run configured sqlite" "no sqlite-ok marker" "sqlite-ok marker exists"

bad_conf="$tmp/failing.conf"
cat > "$bad_conf" <<EOF_BAD_CONF
PLEX_INSTANCES=("sqlite3-builds-bad-plex")
EMBY_INSTANCES=("sqlite3-builds-bad-emby")
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$tmp/backups"
PLEX_OPTIMIZE_API=0
PLEX_PROCESS_BLOB_DB=0
STATS_BANDWIDTH_RETAIN_DAYS=90
return 42
EOF_BAD_CONF
rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$bad_conf"
  bash scripts/optimize_media_servers.sh
) >"$tmp/failing-conf.out" 2>"$tmp/failing-conf.err"
rc=$?
set -e
assert_eq 42 "$rc" "failing config rc"
failing_err="$(cat "$tmp/failing-conf.err")"
assert_contains "$failing_err" "ERROR: failed to source config file: $bad_conf" "failing config diagnostic"
assert_contains "$failing_err" "see --help" "failing config help pointer"
[ ! -e "$tmp/docker-called" ] || fail "failing config should not run docker" "no docker marker" "docker marker exists"
[ ! -e "$tmp/sqlite3-called" ] || fail "failing config should not run sqlite3" "no sqlite marker" "sqlite marker exists"
[ ! -e "$tmp/sqlite-ok-called" ] || fail "failing config should not run configured sqlite" "no sqlite-ok marker" "sqlite-ok marker exists"

early_fail_conf="$tmp/early-failing.conf"
cat > "$early_fail_conf" <<EOF_EARLY_FAIL_CONF
false
PLEX_INSTANCES=("sqlite3-builds-late-plex")
EMBY_INSTANCES=("sqlite3-builds-late-emby")
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$tmp/backups"
PLEX_OPTIMIZE_API=0
PLEX_PROCESS_BLOB_DB=0
STATS_BANDWIDTH_RETAIN_DAYS=90
EOF_EARLY_FAIL_CONF
rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$early_fail_conf"
  bash scripts/optimize_media_servers.sh
) >"$tmp/early-failing-conf.out" 2>"$tmp/early-failing-conf.err"
rc=$?
set -e
assert_eq 1 "$rc" "early failing config rc"
early_failing_err="$(cat "$tmp/early-failing-conf.err")"
assert_contains "$early_failing_err" "ERROR: failed to source config file: $early_fail_conf" "early failing config diagnostic"
assert_contains "$early_failing_err" "see --help" "early failing config help pointer"
[ ! -e "$tmp/docker-called" ] || fail "early failing config should not run docker" "no docker marker" "docker marker exists"
[ ! -e "$tmp/sqlite3-called" ] || fail "early failing config should not run sqlite3" "no sqlite marker" "sqlite marker exists"
[ ! -e "$tmp/sqlite-ok-called" ] || fail "early failing config should not run configured sqlite" "no sqlite-ok marker" "sqlite-ok marker exists"

cat > "$tmp/loader-restore-probe.sh" <<'EOF_LOADER_RESTORE'
#!/usr/bin/env bash
set +e
set +x
. ./scripts/optimize_media_servers.sh
set -ex
if load_optimize_config "$1"; then
  set +x
  printf 'loader-unexpected-success\n'
  exit 41
else
  loader_rc=$?
fi
flags="$-"
set +x
case "$flags" in
  *e*) ;;
  *) printf 'loader-errexit-not-restored:%s\n' "$flags"; exit 42 ;;
esac
case "$flags" in
  *x*) ;;
  *) printf 'loader-xtrace-not-restored:%s\n' "$flags"; exit 43 ;;
esac
printf 'loader-restore-ok:%s:%s\n' "$loader_rc" "$flags"
EOF_LOADER_RESTORE

cat > "$tmp/loader-fails.conf" <<'EOF_LOADER_CONF'
set +e
set +x
return 42
EOF_LOADER_CONF
set +e
bash "$tmp/loader-restore-probe.sh" "$tmp/loader-fails.conf" >"$tmp/loader-restore.out" 2>"$tmp/loader-restore.err"
rc=$?
set -e
assert_eq 0 "$rc" "loader restore probe rc"
assert_contains "$(cat "$tmp/loader-restore.out")" "loader-restore-ok:42:" "loader restore marker"
assert_contains "$(cat "$tmp/loader-restore.err")" "ERROR: failed to source config file: $tmp/loader-fails.conf" "loader restore failure diagnostic"

good_conf="$tmp/good.conf"
cat > "$good_conf" <<EOF_GOOD_CONF
PLEX_INSTANCES=("sqlite3-builds-missing-plex")
EMBY_INSTANCES=("sqlite3-builds-missing-emby")
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$tmp/backups"
PLEX_OPTIMIZE_API=0
PLEX_PROCESS_BLOB_DB=0
STATS_BANDWIDTH_RETAIN_DAYS=90
EOF_GOOD_CONF
rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$good_conf"
  bash scripts/optimize_media_servers.sh
) >"$tmp/good-conf.out" 2>"$tmp/good-conf.err"
rc=$?
set -e
assert_eq 0 "$rc" "configured missing-instance rc"
good_out="$(cat "$tmp/good-conf.out")"
assert_contains "$good_out" "Skipped Missing Plex Instance: sqlite3-builds-missing-plex" "configured Plex instance value"
assert_contains "$good_out" "Skipped Missing Emby Instance: sqlite3-builds-missing-emby" "configured Emby instance value"
assert_not_contains "$good_out" "Plex optimize summary:" "PLEX_OPTIMIZE_API=0 skips summary"
[ ! -e "$tmp/docker-called" ] || fail "missing configured instances should not run docker" "no docker marker" "docker marker exists"
[ -e "$tmp/sqlite-ok-called" ] || fail "configured run should preflight configured sqlite" "sqlite-ok marker" "missing"

empty_conf="$tmp/empty.conf"
cat > "$empty_conf" <<EOF_EMPTY_CONF
PLEX_INSTANCES=()
EMBY_INSTANCES=()
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$tmp/backups"
PLEX_OPTIMIZE_API=0
PLEX_PROCESS_BLOB_DB=0
STATS_BANDWIDTH_RETAIN_DAYS=90
EOF_EMPTY_CONF
rm -f "$tmp/docker-called" "$tmp/sqlite3-called" "$tmp/sqlite-ok-called"
set +e
(
  PATH="$tmp/bin:$PATH"
  export PATH
  export OPTIMIZE_SOURCE_PROBE_DIR="$tmp"
  export OPTIMIZE_MEDIA_SERVERS_CONF="$empty_conf"
  bash scripts/optimize_media_servers.sh
) >"$tmp/empty-conf.out" 2>"$tmp/empty-conf.err"
rc=$?
set -e
assert_eq 0 "$rc" "empty configured instances rc"
assert_eq "" "$(cat "$tmp/empty-conf.out")" "empty configured instances stdout"
empty_err="$(cat "$tmp/empty-conf.err")"
assert_contains "$empty_err" "WARNING: no Plex or Emby instances configured; nothing to do" "empty configured instances warning"
[ ! -e "$tmp/docker-called" ] || fail "empty configured instances should not run docker" "no docker marker" "docker marker exists"
[ ! -e "$tmp/sqlite3-called" ] || fail "empty configured instances should not run sqlite3" "no sqlite marker" "sqlite marker exists"
[ ! -e "$tmp/sqlite-ok-called" ] || fail "empty configured instances should not run configured sqlite" "no sqlite-ok marker" "sqlite-ok marker exists"

printf 'optimize_media_servers sourcing tests passed\n'
