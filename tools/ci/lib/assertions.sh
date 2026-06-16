#!/usr/bin/env bash

# String-based helpers (return on failure; caller handles diagnostics)

slice_log_after_cursor() {
  cursor=$1
  start_line=$((cursor + 1))
  tail -n +"$start_line"
}

assert_marker_present() {
  log_text=$1
  marker_re=$2
  message=$3
  if ! grep -E "$marker_re" <<<"$log_text" >/dev/null; then
    echo "$message" >&2
    return 1
  fi
}

assert_marker_absent() {
  log_text=$1
  marker_re=$2
  message=$3
  if grep -E "$marker_re" <<<"$log_text" >/dev/null; then
    echo "$message" >&2
    return 1
  fi
}

scan_bad_signal() {
  log_text=$1
  bad_signal_re=$2
  message=$3
  if printf "%s\n" "$log_text" | grep -Ev '^\[sqlite3-builds-obs\]' | grep -E "$bad_signal_re"; then
    echo "$message" >&2
    return 1
  fi
}

assert_open_marker_count() {
  log_text=$1
  label=$2
  open_marker_count="$(printf "%s\n" "$log_text" | grep -Ec '\[sqlite3-builds-obs\].* sqlite3_open(_v2|16)? ' || true)"
  if [ "$open_marker_count" -lt 1 ]; then
    echo "FATAL: ${label} observability positive-mode marker count check failed: open=${open_marker_count}" >&2
    return 1
  fi
  printf "%s\n" "$open_marker_count"
}

assert_no_startup_config_rc21() {
  log_text=$1
  label=$2
  if printf "%s\n" "$log_text" | grep -E '\[sqlite3-builds-obs\] .* sqlite3_config op=SQLITE_CONFIG_[A-Z0-9_]+ rc=21' >/dev/null; then
    echo "FATAL: ${label} logs contain startup sqlite3_config rc=21 after lazy auto-extension registration" >&2
    return 1
  fi
}

assert_emby_sqlite_version() {
  sqlite_version_line=$1
  expected_version=$2
  observed_version="$(printf "%s\n" "$sqlite_version_line" | sed -nE 's/.*Sqlite version:[[:space:]]*([^[:space:]]+).*/\1/p')"
  if [ "$observed_version" != "$expected_version" ]; then
    echo "FATAL: Emby logged Sqlite version $observed_version, expected $expected_version from SQLITE_VERSION_DOTTED=${expected_version}" >&2
    return 1
  fi
}

assert_emby_compile_options() {
  log_text=$1
  compile_options="$(printf "%s\n" "$log_text" | sed -n 's/.*Sqlite compiler options:[[:space:]]*//p' | tail -n 1)"
  printf "Emby Sqlite compiler options: %s\n" "$compile_options"
  if [ -z "$compile_options" ]; then
    echo "FATAL: Emby did not log Sqlite compiler options" >&2
    return 1
  fi

  compile_options_csv="$(printf "%s" "$compile_options" | tr -d '[:space:]')"
  compile_options_wrapped=",${compile_options_csv},"
  required_flags=(
    "DQS=1"
    "DEFAULT_WORKER_THREADS=8"
    "MAX_WORKER_THREADS=8"
    "THREADSAFE=2"
    "DEFAULT_WAL_AUTOCHECKPOINT=16000"
    "DEFAULT_JOURNAL_SIZE_LIMIT=67108864"
    "SORTER_PMASZ=8192"
    "DEFAULT_CACHE_SIZE=-1048576"
    "DEFAULT_MMAP_SIZE=34359738368"
    "MAX_MMAP_SIZE=1099511627776"
    "ENABLE_FTS3_TOKENIZER"
    "ENABLE_SETLK_TIMEOUT"
    "ENABLE_NULL_TRIM"
    "ENABLE_PERCENTILE"
    "ENABLE_FTS5"
    "ENABLE_RTREE"
  )
  missing_flags=()
  for flag in "${required_flags[@]}"; do
    if [[ "$compile_options_wrapped" != *",$flag,"* ]]; then
      missing_flags+=("$flag")
    fi
  done
  if [ "${#missing_flags[@]}" -gt 0 ]; then
    echo "FATAL: Emby Sqlite compiler options missing required flag(s): ${missing_flags[*]}" >&2
    return 1
  fi
}

# File-based helpers (exit on failure; cat the file for CI diagnostics)

require_grep() {
  local needle="$1" file="$2" message="$3"
  if grep -Fq "$needle" "$file"; then
    return 0
  fi
  cat "$file"
  echo "$message" >&2
  exit 1
}

require_egrep() {
  local pattern="$1" file="$2" message="$3"
  if grep -Eq "$pattern" "$file"; then
    return 0
  fi
  cat "$file"
  echo "$message" >&2
  exit 1
}

reject_grep() {
  local needle="$1" file="$2" message="$3"
  if grep -Fq "$needle" "$file"; then
    cat "$file"
    echo "$message" >&2
    exit 1
  fi
}

fixed_count() {
  local needle="$1" file="$2"
  awk -v needle="$needle" 'index($0, needle) { count++ } END { print count + 0 }' "$file"
}

assert_component_present() {
  local slice="$1" run_label="$2"
  if grep -Fq 'component=sqlite3-lsio-mod' "$slice"; then
    return 0
  fi
  cat "$slice"
  if [ "$run_label" = "run 2" ]; then
    echo "FATAL: init-mod SQLite phase chain did not re-run on restart (3-run lifecycle assumption violated)" >&2
  else
    echo "FATAL: LSIO mod markers missing from ${run_label} log slice" >&2
  fi
  exit 1
}

assert_no_bad_signal() {
  local slice="$1" run_label="$2" bad_signal_re="$3"
  if grep -Ev '^\[sqlite3-builds-obs\]' "$slice" | grep -E "$bad_signal_re"; then
    echo "FATAL: LSIO mod smoke ${run_label} logs contain SQLite/runtime bad signal" >&2
    exit 1
  fi
}

assert_common_run() {
  local slice="$1" run_label="$2" expected_arch_re="$3" bad_signal_re="$4"
  assert_component_present "$slice" "$run_label"
  require_egrep "$expected_arch_re" "$slice" "FATAL: ${run_label} log slice missing expected architecture marker"
  assert_no_bad_signal "$slice" "$run_label" "$bad_signal_re"
}

assert_swap_run() {
  local slice="$1" run_label="$2" expected="$3"
  if [ "$expected" = "installed" ]; then
    require_grep 'event=installed' "$slice" "FATAL: ${run_label} missing first-apply install marker"
  else
    require_grep 'event=skip-already-current' "$slice" "FATAL: ${run_label} missing already-current swap marker"
    reject_grep 'event=installed' "$slice" "FATAL: ${run_label} unexpectedly reinstalled SQLite instead of skipping already-current"
  fi
}

assert_runtime_load() {
  local slice="$1" run_label="$2" matrix_mod="$3" sqlite_version_dotted="$4"
  local open_marker_count
  if [ "$matrix_mod" = "emby" ]; then
    local sqlite_version_line observed_version expected_version
    sqlite_version_line="$(grep 'Sqlite version:' "$slice" | tail -n 1 || true)"
    if [ -z "$sqlite_version_line" ]; then
      cat "$slice"
      echo "FATAL: ${run_label} Emby did not log Sqlite version after app readiness" >&2
      exit 1
    fi
    observed_version="$(printf "%s\n" "$sqlite_version_line" | sed -nE 's/.*Sqlite version:[[:space:]]*([^[:space:]]+).*/\1/p')"
    expected_version="${sqlite_version_dotted}"
    if [ "$observed_version" != "$expected_version" ]; then
      cat "$slice"
      echo "FATAL: ${run_label} Emby logged Sqlite version ${observed_version}, expected ${expected_version}" >&2
      exit 1
    fi
    require_grep 'MAX_MMAP_SIZE=1099511627776' "$slice" "FATAL: ${run_label} Emby logs missing tuned MAX_MMAP_SIZE compile option"
  fi

  require_egrep '\[sqlite3-builds-obs\].*SQLITE_TRACE_STMT' "$slice" "FATAL: ${run_label} ${matrix_mod} logs missing SQLITE_TRACE_STMT observability marker"
  open_marker_count="$(grep -Ec '\[sqlite3-builds-obs\].* sqlite3_open(_v2|16)? ' "$slice" || true)"
  if [ "$open_marker_count" -lt 1 ]; then
    cat "$slice"
    echo "FATAL: ${run_label} ${matrix_mod} observability open marker count check failed: open=${open_marker_count}" >&2
    exit 1
  fi
  if grep -E '\[sqlite3-builds-obs\] .* sqlite3_config op=SQLITE_CONFIG_[A-Z0-9_]+ rc=21' "$slice" >/dev/null; then
    cat "$slice"
    echo "FATAL: ${run_label} ${matrix_mod} logs contain startup sqlite3_config rc=21 after lazy auto-extension registration" >&2
    exit 1
  fi
}

assert_plex_run() {
  local slice="$1" run_label="$2" expected_swap="$3" matrix_arch_suffix="$4" expected_arch_re="$5" bad_signal_re="$6"
  assert_common_run "$slice" "$run_label" "$expected_arch_re" "$bad_signal_re"
  assert_swap_run "$slice" "$run_label" "$expected_swap"
  case "$matrix_arch_suffix" in amd64|arm64) ;; *) echo "FATAL: unsupported Plex arch suffix: $matrix_arch_suffix" >&2; exit 1 ;; esac
  if [ "$expected_swap" = "installed" ]; then
    require_grep 'pool_patch event=patched' "$slice" "FATAL: ${run_label} missing Plex pool patch apply marker"
  else
    require_grep 'pool_patch event=already-patched' "$slice" "FATAL: ${run_label} missing Plex pool already-patched marker"
    reject_grep 'pool_patch event=patched' "$slice" "FATAL: ${run_label} unexpectedly re-applied Plex pool patch"
  fi
}

emby_tag_forms() {
  case "$1" in
    MaxLibraryDbConnections) printf '%s\n%s\n' MaxLibraryDbConnections MaxLibraryDatabaseConnections ;;
    MaxAuthDbConnections) printf '%s\n%s\n' MaxAuthDbConnections MaxAuthDatabaseConnections ;;
    MaxOtherDbConnections) printf '%s\n%s\n' MaxOtherDbConnections MaxOtherDatabaseConnections ;;
    EnableSqLiteMmio) printf '%s\n' EnableSqLiteMmio ;;
    *)
      echo "FATAL: unsupported Emby config tag: $1" >&2
      exit 1
      ;;
  esac
}

assert_emby_tag_transition() {
  local slice="$1" tag="$2" run_label="$3"
  local updated_count already_count missing_count total actual_tag
  updated_count=0
  already_count=0
  while IFS= read -r actual_tag; do
    updated_count=$((updated_count + $(fixed_count "event=config-updated tag=${actual_tag}" "$slice")))
    already_count=$((already_count + $(fixed_count "event=config-already-set tag=${actual_tag}" "$slice")))
    reject_grep "event=config-write-failed tag=${actual_tag}" "$slice" "FATAL: ${run_label} Emby config write failed for ${actual_tag}"
  done < <(emby_tag_forms "$tag")
  missing_count="$(fixed_count "event=missing-element tag=${tag}" "$slice")"
  total=$((updated_count + already_count + missing_count))
  if [ "$total" -ne 1 ]; then
    cat "$slice"
    echo "FATAL: ${run_label} Emby tag ${tag} expected exactly one config-updated/config-already-set/missing-element marker, found ${total}" >&2
    exit 1
  fi
}

assert_emby_run3_idempotent() {
  local slice="$1"
  local tag actual_tag total
  for tag in MaxLibraryDbConnections MaxAuthDbConnections MaxOtherDbConnections EnableSqLiteMmio; do
    total="$(fixed_count "event=missing-element tag=${tag}" "$slice")"
    while IFS= read -r actual_tag; do
      reject_grep "event=config-updated tag=${actual_tag}" "$slice" "FATAL: run 3 Emby config updated ${actual_tag} instead of staying idempotent"
      reject_grep "event=config-write-failed tag=${actual_tag}" "$slice" "FATAL: run 3 Emby config write failed for ${actual_tag}"
      total=$((total + $(fixed_count "event=config-already-set tag=${actual_tag}" "$slice")))
    done < <(emby_tag_forms "$tag")
    if [ "$total" -ne 1 ]; then
      cat "$slice"
      echo "FATAL: run 3 Emby tag ${tag} expected exactly one config-already-set/missing-element marker, found ${total}" >&2
      exit 1
    fi
  done
}
