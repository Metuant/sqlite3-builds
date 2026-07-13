#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-plex-api.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-plex-api.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fixture_prefs="$repo_root/tests/fixtures/plex-preferences/Preferences.xml"
fixture_token="$(sed -n 's/.*PlexOnlineToken="\([^"]*\)".*/\1/p' "$fixture_prefs")"

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

count_log_occurrences() {
  local needle file
  needle="$1"
  file="$2"
  awk -v needle="$needle" 'index($0, needle) { count++ } END { print count + 0 }' "$file"
}

safe_instance_name() {
  local prefix name safe
  prefix="$1"
  name="$2"
  safe="$(printf '%s' "$name" | tr -c 'A-Za-z0-9_-' '-')"
  printf '%s-%s\n' "$prefix" "$safe"
}

first_line_of() {
  local file needle
  file="$1"
  needle="$2"
  awk -v needle="$needle" 'index($0, needle) { print NR; exit }' "$file"
}

assert_line_lt() {
  local lhs rhs message
  lhs="$1"
  rhs="$2"
  message="$3"
  case "$lhs" in
    ''|*[!0-9]*) fail "$message" "lhs line number present" "$lhs" ;;
  esac
  case "$rhs" in
    ''|*[!0-9]*) fail "$message" "rhs line number present" "$rhs" ;;
  esac
  [ "$lhs" -lt "$rhs" ] || fail "$message" "$lhs < $rhs" "$lhs >= $rhs"
}

assert_file_nonempty() {
  local file message
  file="$1"
  message="$2"
  [ -s "$file" ] || fail "$message" "non-empty file" "$(cat "$file" 2>/dev/null || true)"
}

assert_case_artifacts_redacted() {
  local case_dir name file
  case_dir="$1"
  name="$2"

  while IFS= read -r file
  do
    case "$file" in
      */Preferences.xml)
        continue
        ;;
    esac
    if grep -F "$fixture_token" "$file" >/dev/null; then
      fail "$name temp artifact redaction" "no token in $file" "token found"
    fi
  done < <(find "$case_dir" -type f)
}

map_test_opt_path() {
  local path prefix
  path="$1"
  if [ -n "${OPTIMIZE_API_INSTANCE_ROOT:-}" ] && [ -n "${OPTIMIZE_API_INSTANCE:-}" ]; then
    prefix="/opt/${OPTIMIZE_API_INSTANCE}"
    case "$path" in
      "$prefix"|"$prefix"/*)
        printf '%s%s\n' "$OPTIMIZE_API_INSTANCE_ROOT" "${path#"$prefix"}"
        return 0
        ;;
    esac
  fi
  printf '%s\n' "$path"
}

function [ {
  if builtin [ "$#" -eq 3 ] && builtin [ "$1" = "-d" ] && builtin [ "$3" = "]" ]; then
    builtin [ -d "$(map_test_opt_path "$2")" ]
    return
  fi
  if builtin [ "$#" -eq 4 ] && builtin [ "$1" = "!" ] && builtin [ "$2" = "-d" ] && builtin [ "$4" = "]" ]; then
    builtin [ ! -d "$(map_test_opt_path "$3")" ]
    return
  fi
  builtin [ "$@"
}

mkdir -p "$tmp/bin"

cat > "$tmp/bin/sqlite-ok" <<'EOF_SQLITE'
#!/usr/bin/env bash
exit 0
EOF_SQLITE
chmod +x "$tmp/bin/sqlite-ok"

cat > "$tmp/bin/docker" <<'EOF_DOCKER'
#!/usr/bin/env bash
cmd="${1:-}"
shift || true
printf '%s %s\n' "$cmd" "$*" >> "$OPTIMIZE_API_DOCKER_ARGV_LOG"

emit_running_instance() {
  local arg filter_next name_regex
  filter_next=0
  name_regex=""

  for arg in "$@"; do
    if [ "$filter_next" = "1" ]; then
      case "$arg" in
        name=*) name_regex="${arg#name=}" ;;
      esac
      filter_next=0
      continue
    fi
    if [ "$arg" = "--filter" ]; then
      filter_next=1
    fi
  done

  if [ -z "$name_regex" ] || [[ "$OPTIMIZE_API_INSTANCE" =~ $name_regex ]]; then
    printf '%s\n' "$OPTIMIZE_API_INSTANCE"
  fi
}

case "$cmd" in
  stop)
    printf '%s %s\n' "$cmd" "${1:-}" >> "$OPTIMIZE_API_LIFECYCLE_LOG"
    case "$OPTIMIZE_API_DOCKER_SCENARIO" in
      stop-command-fails)
        exit 70
        ;;
    esac
    exit 0
    ;;
  start)
    printf '%s %s\n' "$cmd" "${1:-}" >> "$OPTIMIZE_API_LIFECYCLE_LOG"
    case "$OPTIMIZE_API_DOCKER_SCENARIO" in
      start-command-fails)
        exit 71
        ;;
    esac
    exit 0
    ;;
  inspect)
    case "$OPTIMIZE_API_DOCKER_SCENARIO" in
      inspect-fails)
        exit 72
        ;;
    esac
    printf '172.18.0.23\n'
    exit 0
    ;;
  ps)
    count_file="$OPTIMIZE_API_CASE_DIR/ps-count"
    if [ -f "$count_file" ]; then
      count="$(cat "$count_file")"
    else
      count=0
    fi
    count=$((count + 1))
    printf '%s\n' "$count" > "$count_file"

    case "$OPTIMIZE_API_DOCKER_SCENARIO:$count" in
      fail-to-stop:1)
        emit_running_instance "$@"
        ;;
      fail-to-start:1)
        :
        ;;
      fail-to-start:2)
        :
        ;;
      *:1)
        :
        ;;
      *)
        emit_running_instance "$@"
        ;;
    esac
    exit 0
    ;;
  *)
    printf 'unexpected docker command: %s\n' "$cmd" >&2
    exit 64
    ;;
esac
EOF_DOCKER
chmod +x "$tmp/bin/docker"

cat > "$tmp/bin/curl" <<'EOF_CURL'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$OPTIMIZE_API_CURL_ARGV_LOG"
ps -p "$$" -o command= >> "$OPTIMIZE_API_CURL_PROCESS_LOG" 2>/dev/null || printf '%s %s\n' "$0" "$*" >> "$OPTIMIZE_API_CURL_PROCESS_LOG"

config="$(cat)"
expected_token="$(sed -n 's/.*PlexOnlineToken="\([^"]*\)".*/\1/p' "$OPTIMIZE_API_PREFS_FIXTURE")"

case "$*" in
  *"$expected_token"*)
    printf 'token leaked through curl argv\n' >&2
    exit 90
    ;;
esac

method="$(printf '%s\n' "$config" | sed -n 's/^request = "\(.*\)"$/\1/p')"
url="$(printf '%s\n' "$config" | sed -n 's/^url = "\(.*\)"$/\1/p')"
max_time="$(printf '%s\n' "$config" | sed -n 's/^max-time = \([0-9][0-9]*\)$/\1/p')"

case "$url" in
  *"$expected_token"*)
    printf 'token leaked through request URL\n' >&2
    exit 91
    ;;
esac

case "$url" in
  *'/identity')
    :
    ;;
  *)
    case "$config" in
      *"X-Plex-Token: $expected_token"*) ;;
      *)
        printf 'missing token header\n' >&2
        exit 92
        ;;
    esac
    ;;
esac

printf '%s %s max-time=%s\n' "$method" "$url" "$max_time" >> "$OPTIMIZE_API_REQUEST_LOG"

endpoint="other"
case "$url" in
  *'/identity') endpoint="identity" ;;
  *'/activities') endpoint="activities" ;;
  *'/library/optimize?async=1') endpoint="put" ;;
esac

count_file="$OPTIMIZE_API_CASE_DIR/curl-${endpoint}-count"
if [ -f "$count_file" ]; then
  count="$(cat "$count_file")"
else
  count=0
fi
count=$((count + 1))
printf '%s\n' "$count" > "$count_file"

marker='__PLEX_OPTIMIZE_HTTP_STATUS__:'
activity='<MediaContainer><Activity type="general.db.optimize" title="Optimizing database" progress="40" /></MediaContainer>'
idle='<MediaContainer size="0"></MediaContainer>'

advance_fake_time() {
  local seconds now
  seconds="$1"
  [ -n "${OPTIMIZE_API_FAKE_TIME_FILE:-}" ] || return 0
  now="$(cat "$OPTIMIZE_API_FAKE_TIME_FILE" 2>/dev/null || printf '0')"
  printf '%s\n' $((now + seconds)) > "$OPTIMIZE_API_FAKE_TIME_FILE"
}

case "$endpoint:$OPTIMIZE_API_HTTP_SCENARIO:$count" in
  identity:readiness-wait:1)
    advance_fake_time 55
    printf '%s503' "$marker"
    ;;
  identity:readiness-wait:2)
    printf '%s200' "$marker"
    ;;
  identity:identity-not-ready:*)
    printf '%s503' "$marker"
    ;;
  identity:*:*)
    printf '%s200' "$marker"
    ;;
  activities:completed:1|activities:readiness-wait:1|activities:accepted-never-started:*|activities:started-unconfirmed:1)
    printf '%s%s200' "$idle" "$marker"
    ;;
  activities:completed:2|activities:readiness-wait:2|activities:started-unconfirmed:*)
    printf '%s%s200' "$activity" "$marker"
    ;;
  activities:completed:3|activities:readiness-wait:3)
    printf '%s%s200' "$idle" "$marker"
    ;;
  activities:completed:4|activities:readiness-wait:4)
    printf '%s%s200' "$idle" "$marker"
    ;;
  activities:already-running:1)
    printf '%s%s200' "$activity" "$marker"
    ;;
  activities:activities-preflight-unreachable:1)
    exit 94
    ;;
  activities:activities-preflight-http:1)
    printf '%s503' "$marker"
    ;;
  activities:trigger-request-failed:1|activities:trigger-http:1)
    printf '%s%s200' "$idle" "$marker"
    ;;
  activities:deadline-shrink:1)
    printf '%s%s200' "$idle" "$marker"
    ;;
  activities:deadline-shrink:2)
    advance_fake_time 291
    printf '%s%s200' "$activity" "$marker"
    ;;
  activities:deadline-shrink:3|activities:deadline-shrink:4)
    printf '%s%s200' "$idle" "$marker"
    ;;
  put:trigger-request-failed:1)
    exit 95
    ;;
  put:trigger-http:1)
    printf '%s500' "$marker"
    ;;
  put:*:1)
    printf '%s%s200' "$idle" "$marker"
    ;;
  *)
    printf 'unexpected request for %s endpoint %s count %s\n' "$OPTIMIZE_API_HTTP_SCENARIO" "$endpoint" "$count" >&2
    exit 93
    ;;
esac
EOF_CURL
chmod +x "$tmp/bin/curl"

prepare_plex_instance() {
  local case_dir instance_name instance_path plex_root copy_prefs layout
  case_dir="$1"
  instance_name="$2"
  copy_prefs="$3"
  layout="${4:-normal}"
  instance_path="$case_dir/opt/$instance_name"
  plex_root="$instance_path/Library/Application Support/Plex Media Server"
  if [ "$layout" != "missing-databases" ]; then
    mkdir -p "$plex_root/Plug-in Support/Databases"
  else
    mkdir -p "$plex_root"
  fi
  mkdir -p "$plex_root/Cache/PhotoTranscoder" "$plex_root/Crash Reports" "$plex_root/Codecs" "$plex_root/Plug-in Support/Caches"
  if [ "$copy_prefs" = "1" ]; then
    cp "$fixture_prefs" "$plex_root/Preferences.xml"
  fi
}

prepare_emby_instance() {
  local case_dir instance_name instance_path layout
  case_dir="$1"
  instance_name="$2"
  layout="${3:-normal}"
  instance_path="$case_dir/opt/$instance_name"
  if [ "$layout" != "missing-data" ]; then
    mkdir -p "$instance_path/data"
  else
    mkdir -p "$instance_path"
  fi
}

run_plex_case() {
  local name http_scenario docker_scenario flag prefs expected_rc layout maintenance_scenario case_dir instance instance_root rc out err
  name="$1"
  http_scenario="$2"
  docker_scenario="$3"
  flag="$4"
  prefs="$5"
  expected_rc="$6"
  layout="${7:-normal}"
  maintenance_scenario="${8:-ok}"

  case_dir="$tmp/$name"
  mkdir -p "$case_dir"
  : > "$case_dir/docker-argv.log"
  : > "$case_dir/curl-argv.log"
  : > "$case_dir/curl-process.log"
  : > "$case_dir/requests.log"
  : > "$case_dir/lifecycle.log"
  : > "$case_dir/maintenance.log"
  printf '0\n' > "$case_dir/fake-time"
  instance="$(safe_instance_name plex "$name")"
  instance_root="$case_dir/opt/$instance"
  prepare_plex_instance "$case_dir" "$instance" "$prefs" "$layout"

  set +e
  (
    PATH="$tmp/bin:$PATH"
    export PATH
    export OPTIMIZE_API_CASE_DIR="$case_dir"
    export OPTIMIZE_API_DOCKER_ARGV_LOG="$case_dir/docker-argv.log"
    export OPTIMIZE_API_CURL_ARGV_LOG="$case_dir/curl-argv.log"
    export OPTIMIZE_API_CURL_PROCESS_LOG="$case_dir/curl-process.log"
    export OPTIMIZE_API_REQUEST_LOG="$case_dir/requests.log"
    export OPTIMIZE_API_LIFECYCLE_LOG="$case_dir/lifecycle.log"
    export OPTIMIZE_API_DOCKER_SCENARIO="$docker_scenario"
    export OPTIMIZE_API_HTTP_SCENARIO="$http_scenario"
    export OPTIMIZE_API_PREFS_FIXTURE="$fixture_prefs"
    export OPTIMIZE_API_INSTANCE="$instance"
    export OPTIMIZE_API_INSTANCE_ROOT="$instance_root"
    export OPTIMIZE_API_FAKE_TIME_FILE="$case_dir/fake-time"
    export OPTIMIZE_API_MAINTENANCE_SCENARIO="$maintenance_scenario"

    . ./scripts/optimize_media_servers.sh
    plex_stat4_preflight() { return 1; }
    optimize_plex_db() {
      printf 'plex-db %s|%s|%s|%s\n' "$1" "${2:-}" "${3:-}" "${4:-}" >> "$OPTIMIZE_API_CASE_DIR/maintenance.log"
      if [ "$OPTIMIZE_API_MAINTENANCE_SCENARIO" = "exit-fail" ]; then
        exit 42
      fi
    }
    plex_preferences_file() {
      map_test_opt_path "/opt/$1/Library/Application Support/Plex Media Server/Preferences.xml"
    }
    run_plex_maintenance_safely() {
      local blob_pre_swap_hook
      (
        set -e
        optimize_plex_db "${_PLEX_DB}" "SELECT 1 FROM versioned_metadata_items LIMIT 1;" "try_deflate_plex_statistics_bandwidth"
        if [ "${PLEX_PROCESS_BLOB_DB:-0}" = "1" ]; then
          blob_pre_swap_hook=""
          if [ "${PLEX_TRIM_FINISHED_SEASON_BLOBS:-0}" = "1" ]; then
            blob_pre_swap_hook="try_trim_plex_finished_season_blobs"
          fi
          optimize_plex_db "${_PLEX_BLOB_DB}" "" "${blob_pre_swap_hook}" ""
        fi
      )
    }
    plex_optimize_now() { cat "$OPTIMIZE_API_FAKE_TIME_FILE"; }
    plex_optimize_sleep() {
      local seconds now
      seconds="${1:-0}"
      now="$(cat "$OPTIMIZE_API_FAKE_TIME_FILE")"
      printf '%s\n' $((now + seconds)) > "$OPTIMIZE_API_FAKE_TIME_FILE"
    }

    cat > "$case_dir/optimize.conf" <<EOF_CONF
PLEX_INSTANCES=("$instance")
EMBY_INSTANCES=()
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$case_dir/backups"
PLEX_OPTIMIZE_API=$flag
PLEX_PROCESS_BLOB_DB=0
PLEX_TRIM_FINISHED_SEASON_BLOBS=0
STATS_BANDWIDTH_RETAIN_DAYS=90
EOF_CONF
    export OPTIMIZE_MEDIA_SERVERS_CONF="$case_dir/optimize.conf"

    main
    main_rc=$?
    assert_eq "$tmp/bin/sqlite-ok" "$PLEX_BINARY" "$name config PLEX_BINARY"
    assert_eq "$tmp/bin/sqlite-ok" "$GENERIC_SQLITE_BINARY" "$name config GENERIC_SQLITE_BINARY"
    assert_eq "$case_dir/backups" "$BACKUP_PATH" "$name config BACKUP_PATH"
    assert_eq "$flag" "$PLEX_OPTIMIZE_API" "$name config PLEX_OPTIMIZE_API"
    assert_eq "0" "$PLEX_PROCESS_BLOB_DB" "$name config PLEX_PROCESS_BLOB_DB"
    assert_eq "0" "$PLEX_TRIM_FINISHED_SEASON_BLOBS" "$name config PLEX_TRIM_FINISHED_SEASON_BLOBS"
    assert_eq "90" "$STATS_BANDWIDTH_RETAIN_DAYS" "$name config STATS_BANDWIDTH_RETAIN_DAYS"
    assert_eq "$instance" "${PLEX_INSTANCES[*]}" "$name config PLEX_INSTANCES"
    assert_eq "" "${EMBY_INSTANCES[*]}" "$name config EMBY_INSTANCES"
    exit "$main_rc"
  ) >"$case_dir/out" 2>"$case_dir/err"
  rc=$?
  set -e

  assert_eq "$expected_rc" "$rc" "$name rc"
  out="$(cat "$case_dir/out")"
  err="$(cat "$case_dir/err")"
  assert_not_contains "$out" "$fixture_token" "$name stdout redaction"
  assert_not_contains "$err" "$fixture_token" "$name stderr redaction"
  assert_not_contains "$(cat "$case_dir/docker-argv.log")" "$fixture_token" "$name docker argv redaction"
  assert_not_contains "$(cat "$case_dir/curl-argv.log")" "$fixture_token" "$name curl argv redaction"
  if [ -s "$case_dir/curl-argv.log" ]; then
    assert_file_nonempty "$case_dir/curl-process.log" "$name process-list proof captured"
    assert_not_contains "$(cat "$case_dir/curl-process.log")" "$fixture_token" "$name process-list redaction"
  fi
  assert_not_contains "$(cat "$case_dir/requests.log")" "$fixture_token" "$name request URL redaction"
  assert_case_artifacts_redacted "$case_dir" "$name"
}

run_emby_case() {
  local name docker_scenario expected_rc layout maintenance_scenario case_dir instance instance_root rc
  name="$1"
  docker_scenario="$2"
  expected_rc="$3"
  layout="${4:-normal}"
  maintenance_scenario="${5:-ok}"

  case_dir="$tmp/$name"
  mkdir -p "$case_dir"
  : > "$case_dir/docker-argv.log"
  : > "$case_dir/curl-argv.log"
  : > "$case_dir/requests.log"
  : > "$case_dir/lifecycle.log"
  : > "$case_dir/maintenance.log"
  instance="$(safe_instance_name emby "$name")"
  instance_root="$case_dir/opt/$instance"
  prepare_emby_instance "$case_dir" "$instance" "$layout"

  set +e
  (
    PATH="$tmp/bin:$PATH"
    export PATH
    export OPTIMIZE_API_CASE_DIR="$case_dir"
    export OPTIMIZE_API_DOCKER_ARGV_LOG="$case_dir/docker-argv.log"
    export OPTIMIZE_API_CURL_ARGV_LOG="$case_dir/curl-argv.log"
    export OPTIMIZE_API_REQUEST_LOG="$case_dir/requests.log"
    export OPTIMIZE_API_LIFECYCLE_LOG="$case_dir/lifecycle.log"
    export OPTIMIZE_API_DOCKER_SCENARIO="$docker_scenario"
    export OPTIMIZE_API_HTTP_SCENARIO="completed"
    export OPTIMIZE_API_PREFS_FIXTURE="$fixture_prefs"
    export OPTIMIZE_API_INSTANCE="$instance"
    export OPTIMIZE_API_INSTANCE_ROOT="$instance_root"
    export OPTIMIZE_API_MAINTENANCE_SCENARIO="$maintenance_scenario"

    . ./scripts/optimize_media_servers.sh
    rebuild_db_vacuum_into() {
      printf 'emby-db\n' >> "$OPTIMIZE_API_CASE_DIR/maintenance.log"
      if [ "$OPTIMIZE_API_MAINTENANCE_SCENARIO" = "exit-fail" ]; then
        exit 42
      fi
    }
    run_emby_maintenance_safely() {
      (
        set -e
        rebuild_db_vacuum_into
      )
    }

    cat > "$case_dir/optimize.conf" <<EOF_CONF
PLEX_INSTANCES=()
EMBY_INSTANCES=("$instance")
PLEX_BINARY="$tmp/bin/sqlite-ok"
GENERIC_SQLITE_BINARY="$tmp/bin/sqlite-ok"
BACKUP_PATH="$case_dir/backups"
PLEX_OPTIMIZE_API=1
PLEX_PROCESS_BLOB_DB=0
PLEX_TRIM_FINISHED_SEASON_BLOBS=0
STATS_BANDWIDTH_RETAIN_DAYS=90
EOF_CONF
    export OPTIMIZE_MEDIA_SERVERS_CONF="$case_dir/optimize.conf"

    main
    main_rc=$?
    assert_eq "$tmp/bin/sqlite-ok" "$PLEX_BINARY" "$name config PLEX_BINARY"
    assert_eq "$tmp/bin/sqlite-ok" "$GENERIC_SQLITE_BINARY" "$name config GENERIC_SQLITE_BINARY"
    assert_eq "$case_dir/backups" "$BACKUP_PATH" "$name config BACKUP_PATH"
    assert_eq "1" "$PLEX_OPTIMIZE_API" "$name config PLEX_OPTIMIZE_API"
    assert_eq "0" "$PLEX_PROCESS_BLOB_DB" "$name config PLEX_PROCESS_BLOB_DB"
    assert_eq "0" "$PLEX_TRIM_FINISHED_SEASON_BLOBS" "$name config PLEX_TRIM_FINISHED_SEASON_BLOBS"
    assert_eq "90" "$STATS_BANDWIDTH_RETAIN_DAYS" "$name config STATS_BANDWIDTH_RETAIN_DAYS"
    assert_eq "" "${PLEX_INSTANCES[*]}" "$name config PLEX_INSTANCES"
    assert_eq "$instance" "${EMBY_INSTANCES[*]}" "$name config EMBY_INSTANCES"
    exit "$main_rc"
  ) >"$case_dir/out" 2>"$case_dir/err"
  rc=$?
  set -e

  assert_eq "$expected_rc" "$rc" "$name rc"
  assert_not_contains "$(cat "$case_dir/requests.log")" "library/optimize" "$name Emby has no optimize API"
}

run_plex_case flag-off completed ok 0 1 0
assert_eq 0 "$(count_log_occurrences 'identity' "$tmp/flag-off/requests.log")" "flag off skips identity wait"
assert_eq 0 "$(count_log_occurrences 'library/optimize?async=1' "$tmp/flag-off/requests.log")" "flag off skips optimize PUT"
assert_contains "$(cat "$tmp/flag-off/lifecycle.log")" "stop plex-flag-off" "flag off stops Plex"
assert_contains "$(cat "$tmp/flag-off/lifecycle.log")" "start plex-flag-off" "flag off starts Plex"

run_plex_case completed-readiness readiness-wait ok 1 1 0
assert_contains "$(cat "$tmp/completed-readiness/out")" "Plex optimize completed:" "completed terminal state"
assert_contains "$(cat "$tmp/completed-readiness/out")" "Plex optimize summary: accepted=0 already-running=0 completed=1 skipped=0 warned=0" "completed summary"
assert_eq 2 "$(count_log_occurrences 'GET http://172.18.0.23:32400/identity' "$tmp/completed-readiness/requests.log")" "readiness wait retries identity"
assert_contains "$(cat "$tmp/completed-readiness/requests.log")" "GET http://172.18.0.23:32400/identity max-time=3" "readiness request max-time fits deadline"
identity_line="$(first_line_of "$tmp/completed-readiness/requests.log" 'GET http://172.18.0.23:32400/identity')"
activities_line="$(first_line_of "$tmp/completed-readiness/requests.log" 'GET http://172.18.0.23:32400/activities')"
assert_line_lt "$identity_line" "$activities_line" "identity wait before activities preflight"
assert_eq 1 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/completed-readiness/requests.log")" "completed PUT once"
assert_eq 4 "$(count_log_occurrences 'GET http://172.18.0.23:32400/activities' "$tmp/completed-readiness/requests.log")" "completed waits for debounced absent activity"

run_plex_case already-running already-running ok 1 1 0
assert_contains "$(cat "$tmp/already-running/out")" "Plex optimize already running:" "already-running terminal state"
assert_contains "$(cat "$tmp/already-running/out")" "Plex optimize summary: accepted=0 already-running=1 completed=0 skipped=0 warned=0" "already-running summary"
assert_eq 0 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/already-running/requests.log")" "already-running suppresses PUT"

run_plex_case accepted-never-started accepted-never-started ok 1 1 0
assert_contains "$(cat "$tmp/accepted-never-started/out")" "Plex optimize accepted but did not start:" "accepted-never-started terminal state"
assert_contains "$(cat "$tmp/accepted-never-started/out")" "Plex optimize summary: accepted=1 already-running=0 completed=0 skipped=0 warned=1" "accepted-never-started summary"
assert_eq 1 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/accepted-never-started/requests.log")" "accepted-never-started PUT once"
assert_eq 4 "$(count_log_occurrences 'GET http://172.18.0.23:32400/activities' "$tmp/accepted-never-started/requests.log")" "accepted-never-started requires three startup polls after preflight"

run_plex_case deadline-shrink deadline-shrink ok 1 1 0
assert_contains "$(cat "$tmp/deadline-shrink/out")" "Plex optimize completed:" "deadline-shrink terminal state"
assert_contains "$(cat "$tmp/deadline-shrink/requests.log")" "GET http://172.18.0.23:32400/activities max-time=5" "activities request max-time fits remaining deadline"

run_plex_case started-unconfirmed started-unconfirmed ok 1 1 0
assert_contains "$(cat "$tmp/started-unconfirmed/out")" "Plex optimize started but completion unconfirmed:" "started-unconfirmed terminal state"
assert_contains "$(cat "$tmp/started-unconfirmed/out")" "Plex optimize summary: accepted=1 already-running=0 completed=0 skipped=0 warned=1" "started-unconfirmed summary"
assert_eq 1 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/started-unconfirmed/requests.log")" "started-unconfirmed PUT once"

run_plex_case missing-token completed ok 1 0 1
assert_contains "$(cat "$tmp/missing-token/err")" "WARNING: Plex optimize token missing" "missing token warning"
assert_contains "$(cat "$tmp/missing-token/out")" "Plex optimize summary: accepted=0 already-running=0 completed=0 skipped=1 warned=1" "all no-op nonzero summary"
assert_eq 0 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/missing-token/requests.log")" "missing token no PUT"

run_plex_case container-ip-unavailable completed inspect-fails 1 1 1
assert_contains "$(cat "$tmp/container-ip-unavailable/err")" "WARNING: Plex optimize container IP unavailable" "container IP warning"
assert_eq 0 "$(count_log_occurrences 'identity' "$tmp/container-ip-unavailable/requests.log")" "container IP skip makes no identity request"

run_plex_case identity-not-ready identity-not-ready ok 1 1 1
assert_contains "$(cat "$tmp/identity-not-ready/err")" "WARNING: Plex optimize identity not ready" "identity warning"
assert_eq 0 "$(count_log_occurrences 'activities' "$tmp/identity-not-ready/requests.log")" "identity skip makes no activities request"
assert_eq 0 "$(count_log_occurrences 'library/optimize?async=1' "$tmp/identity-not-ready/requests.log")" "identity skip makes no PUT"

run_plex_case activities-preflight-unreachable activities-preflight-unreachable ok 1 1 1
assert_contains "$(cat "$tmp/activities-preflight-unreachable/err")" "WARNING: Plex optimize activities preflight unreachable" "activities preflight unreachable warning"
assert_eq 0 "$(count_log_occurrences 'library/optimize?async=1' "$tmp/activities-preflight-unreachable/requests.log")" "activities preflight unreachable no PUT"

run_plex_case activities-preflight-http activities-preflight-http ok 1 1 1
assert_contains "$(cat "$tmp/activities-preflight-http/err")" "WARNING: Plex optimize activities preflight HTTP 503" "activities preflight status warning"
assert_eq 0 "$(count_log_occurrences 'library/optimize?async=1' "$tmp/activities-preflight-http/requests.log")" "activities preflight HTTP no PUT"

run_plex_case trigger-request-failed trigger-request-failed ok 1 1 1
assert_contains "$(cat "$tmp/trigger-request-failed/err")" "WARNING: Plex optimize trigger request failed" "trigger request warning"
assert_eq 1 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/trigger-request-failed/requests.log")" "trigger request failed PUT once"

run_plex_case trigger-http trigger-http ok 1 1 1
assert_contains "$(cat "$tmp/trigger-http/err")" "WARNING: Plex optimize trigger HTTP 500" "trigger HTTP warning"
assert_eq 1 "$(count_log_occurrences 'PUT http://172.18.0.23:32400/library/optimize?async=1' "$tmp/trigger-http/requests.log")" "trigger HTTP PUT once"

run_plex_case missing-databases completed ok 0 1 0 missing-databases
assert_contains "$(cat "$tmp/missing-databases/out")" "Skipped Missing Plex Instance:" "missing Plex databases skip"
assert_not_contains "$(cat "$tmp/missing-databases/lifecycle.log")" "stop " "missing Plex databases does not stop"
assert_not_contains "$(cat "$tmp/missing-databases/lifecycle.log")" "start " "missing Plex databases does not start"

run_plex_case stop-command-fails completed stop-command-fails 0 1 1
assert_contains "$(cat "$tmp/stop-command-fails/err")" "WARNING: docker stop failed for Plex" "stop failure warning"
assert_not_contains "$(cat "$tmp/stop-command-fails/lifecycle.log")" "start " "stop failure does not start"

run_plex_case start-command-fails completed start-command-fails 0 1 1
assert_contains "$(cat "$tmp/start-command-fails/err")" "ERROR: docker start failed for Plex" "start command failure labeled"

run_plex_case maintenance-exits completed ok 0 1 1 normal exit-fail
assert_contains "$(cat "$tmp/maintenance-exits/lifecycle.log")" "start plex-maintenance-exits" "maintenance exit still restarts Plex"
assert_contains "$(cat "$tmp/maintenance-exits/err")" "WARNING: Plex maintenance failed" "maintenance exit warning"

run_plex_case fail-to-stop completed fail-to-stop 0 1 1
assert_contains "$(cat "$tmp/fail-to-stop/err")" "planned-downtime gate failed" "fail-to-stop abort"
assert_not_contains "$(cat "$tmp/fail-to-stop/lifecycle.log")" "start " "fail-to-stop does not start"

run_plex_case fail-to-start completed fail-to-start 0 1 1
assert_contains "$(cat "$tmp/fail-to-start/err")" "failed to reach running after start" "fail-to-start error"
assert_contains "$(cat "$tmp/fail-to-start/lifecycle.log")" "start plex-fail-to-start" "fail-to-start attempted start"

run_emby_case emby-lifecycle ok 0
assert_contains "$(cat "$tmp/emby-lifecycle/lifecycle.log")" "stop emby-emby-lifecycle" "Emby stops"
assert_contains "$(cat "$tmp/emby-lifecycle/lifecycle.log")" "start emby-emby-lifecycle" "Emby starts"
assert_eq "" "$(cat "$tmp/emby-lifecycle/requests.log")" "Emby makes no HTTP requests"

run_emby_case emby-missing-data ok 0 missing-data
assert_contains "$(cat "$tmp/emby-missing-data/out")" "Skipped Missing Emby Instance:" "missing Emby data skip"
assert_not_contains "$(cat "$tmp/emby-missing-data/lifecycle.log")" "stop " "missing Emby data does not stop"
assert_not_contains "$(cat "$tmp/emby-missing-data/lifecycle.log")" "start " "missing Emby data does not start"

run_emby_case emby-maintenance-exits ok 1 normal exit-fail
assert_contains "$(cat "$tmp/emby-maintenance-exits/lifecycle.log")" "start emby-emby-maintenance-exits" "maintenance exit still restarts Emby"
assert_contains "$(cat "$tmp/emby-maintenance-exits/err")" "WARNING: Emby maintenance failed" "Emby maintenance exit warning"

run_emby_case emby-start-command-fails start-command-fails 1
assert_contains "$(cat "$tmp/emby-start-command-fails/err")" "ERROR: docker start failed for Emby" "Emby start command failure labeled"

run_emby_case emby-fail-to-start fail-to-start 1
assert_contains "$(cat "$tmp/emby-fail-to-start/err")" "failed to reach running after start" "Emby fail-to-start error"

printf 'optimize_media_servers Plex API tests passed\n'
