#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-emby-config.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-emby-config.XXXXXX)"
cleanup() {
  chmod -R u+w "$tmp_root" 2>/dev/null || true
  rm -rf "$tmp_root"
}
trap cleanup EXIT

fail() {
  echo "FATAL: $*" >&2
  exit 1
}

assert_eq() {
  expected="$1"
  actual="$2"
  context="$3"
  [ "$expected" = "$actual" ] || fail "$context: expected [$expected], actual [$actual]"
}

assert_contains() {
  needle="$1"
  path="$2"
  context="$3"
  grep -Fq "$needle" "$path" || {
    echo "FATAL: $context: expected to find [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  }
}

assert_not_contains() {
  needle="$1"
  path="$2"
  context="$3"
  if grep -Fq "$needle" "$path"; then
    echo "FATAL: $context: did not expect to find [$needle] in $path" >&2
    cat "$path" >&2
    exit 1
  fi
}

grep_count() {
  pattern="$1"
  path="$2"
  grep -cE "$pattern" "$path" || true
}

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

tag_value() {
  tag="$1"
  path="$2"
  sed -nE -e "s|^[[:space:]]*<${tag}>([^<]*)</${tag}>[[:space:]]*$|\1|" -e "t found" -e "b" -e ":found" -e "p" -e "q" "$path"
}

assert_tag_value() {
  tag="$1"
  expected="$2"
  actual="$(tag_value "$tag" "$config")"
  assert_eq "$expected" "$actual" "tag $tag value"
}

assert_valid_wrapper() {
  path="$1"
  assert_eq "1" "$(grep_count '^[[:space:]]*<ServerConfiguration>[[:space:]]*$' "$path")" "opening XML wrapper count"
  assert_eq "1" "$(grep_count '^[[:space:]]*</ServerConfiguration>[[:space:]]*$' "$path")" "closing XML wrapper count"
}

assert_no_temp_leftovers() {
  if find "$config_dir" -maxdepth 1 -name 'system.xml.sqlite3-mod.*' -print -quit | grep -q .; then
    find "$config_dir" -maxdepth 1 -name 'system.xml.sqlite3-mod.*' -print >&2
    fail "config-adjacent temp file remains in $config_dir"
  fi
  if find "$case_tmpdir" -mindepth 1 -print -quit | grep -q .; then
    find "$case_tmpdir" -mindepth 1 -print >&2
    fail "temp file remains under TMPDIR $case_tmpdir"
  fi
}

mkdir -p "$tmp_root/bin"
cat > "$tmp_root/bin/uname" <<'EOF_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
EOF_UNAME
chmod +x "$tmp_root/bin/uname"

setup_case() {
  case_name="$1"
  case_root="$tmp_root/$case_name"
  fixture="$case_root/fixture"
  lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
  runtime_lib_dir="$fixture/app/emby/lib"
  config_dir="$fixture/config/config"
  case_tmpdir="$case_root/tmpdir"
  mkdir -p "$lib_root" "$runtime_lib_dir" "$config_dir" "$case_tmpdir"

  cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
  cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
  cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
  cp lsio-mods/shared/cont-init-fragments/atomic-write.sh "$lib_root/atomic-write.sh"
  . "$lib_root/atomic-write.sh"

  cat > "$runtime_lib_dir/libsqlite3.so.3.49.2" <<'EOF_SQLITE'
sqlite-current
EOF_SQLITE
  current_sha="$(sha_file "$runtime_lib_dir/libsqlite3.so.3.49.2")"
  target_path="$fixture/app/emby/lib/libsqlite3.so.3.49.2"

  cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
version|2|release_tag|2026.05.27-r3
current|1|emby|linux-arm64|sqlite-fixture-library-linux-arm64.so|${target_path}|${current_sha}
EOF_PINS

  script_copy="$case_root/init-mod-sqlite3-config-run"
  cp lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-config/run "$script_copy"
  python3 - "$script_copy" "$fixture" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
fixture = sys.argv[2]
text = script_path.read_text()
text = text.replace("/opt/sqlite3-lsio-mod", f"{fixture}/opt/sqlite3-lsio-mod")
text = text.replace("/app/emby/lib/", f"{fixture}/app/emby/lib/")
text = text.replace("/config/", f"{fixture}/config/")
script_path.write_text(text)
PY
  chmod +x "$script_copy"
}

run_phase() {
  phase_log="$case_root/phase.log"
  set +e
  PATH="$tmp_root/bin:$PATH" TMPDIR="$case_tmpdir" bash "$script_copy" > "$phase_log" 2>&1
  rc=$?
  set -e
}

write_all_values() {
  library="$1"
  auth="$2"
  other="$3"
  mmio="$4"
  cat > "$config" <<EOF_XML
<ServerConfiguration>
  <MaxLibraryDbConnections>${library}</MaxLibraryDbConnections>
  <MaxAuthDbConnections>${auth}</MaxAuthDbConnections>
  <MaxOtherDbConnections>${other}</MaxOtherDbConnections>
  <EnableSqLiteMmio>${mmio}</EnableSqLiteMmio>
</ServerConfiguration>
EOF_XML
}

write_all_database_values() {
  library="$1"
  auth="$2"
  other="$3"
  mmio="$4"
  cat > "$config" <<EOF_XML
<ServerConfiguration>
  <MaxLibraryDatabaseConnections>${library}</MaxLibraryDatabaseConnections>
  <MaxAuthDatabaseConnections>${auth}</MaxAuthDatabaseConnections>
  <MaxOtherDatabaseConnections>${other}</MaxOtherDatabaseConnections>
  <EnableSqLiteMmio>${mmio}</EnableSqLiteMmio>
</ServerConfiguration>
EOF_XML
}

config=""
setup_case values_differ
config="$config_dir/system.xml"
write_all_values 1 1 1 false
chmod 0640 "$config"
run_phase
assert_eq "0" "$rc" "values-differ exit status"
assert_eq "640" "$(sqlite3_mod_stat_mode "$config")" "values-differ config mode"
assert_tag_value MaxLibraryDbConnections 16
assert_tag_value MaxAuthDbConnections 4
assert_tag_value MaxOtherDbConnections 2
assert_tag_value EnableSqLiteMmio true
assert_eq "4" "$(grep_count 'event=config-updated' "$phase_log")" "values-differ config-updated count"
assert_no_temp_leftovers

setup_case database_values_differ
config="$config_dir/system.xml"
write_all_database_values 1 1 1 false
run_phase
assert_eq "0" "$rc" "database-values-differ exit status"
assert_tag_value MaxLibraryDatabaseConnections 16
assert_tag_value MaxAuthDatabaseConnections 4
assert_tag_value MaxOtherDatabaseConnections 2
assert_tag_value EnableSqLiteMmio true
assert_eq "0" "$(grep_count '^[[:space:]]*<MaxLibraryDbConnections>[^<]*</MaxLibraryDbConnections>[[:space:]]*$' "$config")" "database-values-differ library Db creation count"
assert_eq "0" "$(grep_count '^[[:space:]]*<MaxAuthDbConnections>[^<]*</MaxAuthDbConnections>[[:space:]]*$' "$config")" "database-values-differ auth Db creation count"
assert_eq "0" "$(grep_count '^[[:space:]]*<MaxOtherDbConnections>[^<]*</MaxOtherDbConnections>[[:space:]]*$' "$config")" "database-values-differ other Db creation count"
assert_contains 'event=config-updated tag=MaxLibraryDatabaseConnections old=1 new=16' "$phase_log" "database-values-differ library log"
assert_contains 'event=config-updated tag=MaxAuthDatabaseConnections old=1 new=4' "$phase_log" "database-values-differ auth log"
assert_contains 'event=config-updated tag=MaxOtherDatabaseConnections old=1 new=2' "$phase_log" "database-values-differ other log"
assert_eq "4" "$(grep_count 'event=config-updated' "$phase_log")" "database-values-differ config-updated count"
assert_no_temp_leftovers

setup_case already_set
config="$config_dir/system.xml"
write_all_values 16 4 2 true
before_sha="$(sha_file "$config")"
run_phase
after_sha="$(sha_file "$config")"
assert_eq "0" "$rc" "already-set exit status"
assert_eq "$before_sha" "$after_sha" "already-set config sha"
assert_eq "4" "$(grep_count 'event=config-already-set' "$phase_log")" "already-set count"
assert_eq "0" "$(grep_count 'event=config-updated' "$phase_log")" "already-set updated count"
assert_no_temp_leftovers

setup_case swap_not_current
config="$config_dir/system.xml"
write_all_values 1 1 1 false
printf 'sqlite-not-current\n' > "$runtime_lib_dir/libsqlite3.so.3.49.2"
before_sha="$(sha_file "$config")"
run_phase
after_sha="$(sha_file "$config")"
assert_eq "0" "$rc" "swap-not-current exit status"
assert_eq "$before_sha" "$after_sha" "swap-not-current config sha"
assert_contains 'event=skip-sqlite-not-current' "$phase_log" "swap-not-current log"
assert_eq "0" "$(grep_count 'event=config-updated' "$phase_log")" "swap-not-current updated count"
assert_no_temp_leftovers

setup_case missing_config
config="$config_dir/system.xml"
run_phase
assert_eq "0" "$rc" "missing-config exit status"
assert_contains 'event=missing-config' "$phase_log" "missing-config log"
assert_no_temp_leftovers

setup_case disable_async_untouched
config="$config_dir/system.xml"
cat > "$config" <<'EOF_XML'
<ServerConfiguration>
  <MaxLibraryDbConnections>1</MaxLibraryDbConnections>
  <MaxAuthDbConnections>1</MaxAuthDbConnections>
  <MaxOtherDbConnections>1</MaxOtherDbConnections>
  <EnableSqLiteMmio>false</EnableSqLiteMmio>
  <DisableAsyncIO>false</DisableAsyncIO>
</ServerConfiguration>
EOF_XML
run_phase
assert_eq "0" "$rc" "DisableAsyncIO exit status"
assert_eq "false" "$(tag_value DisableAsyncIO "$config")" "DisableAsyncIO value"
assert_no_temp_leftovers

setup_case missing_element
config="$config_dir/system.xml"
cat > "$config" <<'EOF_XML'
<ServerConfiguration>
  <MaxLibraryDbConnections>1</MaxLibraryDbConnections>
  <MaxOtherDbConnections>1</MaxOtherDbConnections>
  <EnableSqLiteMmio>false</EnableSqLiteMmio>
  <DisableAsyncIO>false</DisableAsyncIO>
</ServerConfiguration>
EOF_XML
run_phase
assert_eq "0" "$rc" "missing-element exit status"
assert_contains 'event=missing-element tag=MaxAuthDbConnections' "$phase_log" "missing-element log"
assert_eq "3" "$(grep_count 'event=config-updated' "$phase_log")" "missing-element updated count"
assert_eq "0" "$(grep_count '^[[:space:]]*<MaxAuthDbConnections>[^<]*</MaxAuthDbConnections>[[:space:]]*$' "$config")" "missing element creation count"
assert_valid_wrapper "$config"
assert_no_temp_leftovers

setup_case missing_connection_pairs
config="$config_dir/system.xml"
cat > "$config" <<'EOF_XML'
<ServerConfiguration>
  <EnableSqLiteMmio>false</EnableSqLiteMmio>
  <DisableAsyncIO>false</DisableAsyncIO>
</ServerConfiguration>
EOF_XML
run_phase
assert_eq "0" "$rc" "missing-connection-pairs exit status"
assert_eq "3" "$(grep_count 'event=missing-element tag=Max.*Connections' "$phase_log")" "missing-connection-pairs warning count"
assert_eq "1" "$(grep_count 'event=missing-element tag=MaxLibraryDbConnections' "$phase_log")" "missing-connection-pairs library warning count"
assert_eq "1" "$(grep_count 'event=missing-element tag=MaxAuthDbConnections' "$phase_log")" "missing-connection-pairs auth warning count"
assert_eq "1" "$(grep_count 'event=missing-element tag=MaxOtherDbConnections' "$phase_log")" "missing-connection-pairs other warning count"
assert_eq "1" "$(grep_count 'event=config-updated tag=EnableSqLiteMmio' "$phase_log")" "missing-connection-pairs mmio update count"
assert_eq "0" "$(grep_count 'event=config-updated tag=Max.*Connections' "$phase_log")" "missing-connection-pairs connection update count"
assert_eq "false" "$(tag_value DisableAsyncIO "$config")" "missing-connection-pairs DisableAsyncIO value"
assert_valid_wrapper "$config"
assert_no_temp_leftovers

setup_case write_failure
config="$config_dir/system.xml"
write_all_values 1 1 1 false
before_sha="$(sha_file "$config")"
chmod 0555 "$config_dir"
run_phase
chmod 0755 "$config_dir"
after_sha="$(sha_file "$config")"
assert_eq "0" "$rc" "write-failure exit status"
assert_eq "$before_sha" "$after_sha" "write-failure config sha"
assert_contains 'event=config-write-failed tag=MaxLibraryDbConnections' "$phase_log" "write-failure first tag log"
assert_contains 'event=config-write-failed tag=EnableSqLiteMmio' "$phase_log" "write-failure later tag log"
assert_valid_wrapper "$config"
assert_no_temp_leftovers

setup_case commented_duplicate_guard
config="$config_dir/system.xml"
cat > "$config" <<'EOF_XML'
<ServerConfiguration>
  <!-- <MaxLibraryDbConnections>16</MaxLibraryDbConnections> -->
  <MaxLibraryDbConnections>1</MaxLibraryDbConnections>
  <MaxAuthDbConnections>1</MaxAuthDbConnections>
  <MaxAuthDbConnections>3</MaxAuthDbConnections>
  <MaxOtherDbConnections>1</MaxOtherDbConnections>
  <EnableSqLiteMmio>false</EnableSqLiteMmio>
</ServerConfiguration>
EOF_XML
run_phase
assert_eq "0" "$rc" "commented/duplicate exit status"
assert_tag_value MaxLibraryDbConnections 16
assert_contains '<!-- <MaxLibraryDbConnections>16</MaxLibraryDbConnections> -->' "$config" "comment preserved"
assert_contains 'event=ambiguous-element tag=MaxAuthDbConnections count=2' "$phase_log" "ambiguous duplicate log"
assert_eq "1" "$(grep_count '^[[:space:]]*<MaxAuthDbConnections>1</MaxAuthDbConnections>[[:space:]]*$' "$config")" "first duplicate auth value"
assert_eq "1" "$(grep_count '^[[:space:]]*<MaxAuthDbConnections>3</MaxAuthDbConnections>[[:space:]]*$' "$config")" "second duplicate auth value"
assert_tag_value MaxOtherDbConnections 2
assert_tag_value EnableSqLiteMmio true
assert_no_temp_leftovers

setup_case paired_form_ambiguous_guard
config="$config_dir/system.xml"
cat > "$config" <<'EOF_XML'
<ServerConfiguration>
  <MaxLibraryDbConnections>1</MaxLibraryDbConnections>
  <MaxLibraryDatabaseConnections>3</MaxLibraryDatabaseConnections>
  <MaxAuthDbConnections>1</MaxAuthDbConnections>
  <MaxOtherDatabaseConnections>1</MaxOtherDatabaseConnections>
  <EnableSqLiteMmio>false</EnableSqLiteMmio>
</ServerConfiguration>
EOF_XML
run_phase
assert_eq "0" "$rc" "paired-form ambiguous exit status"
assert_contains 'event=ambiguous-element tag=MaxLibraryDbConnections|MaxLibraryDatabaseConnections count=2' "$phase_log" "paired-form ambiguous log"
assert_eq "1" "$(tag_value MaxLibraryDbConnections "$config")" "paired-form ambiguous Db value"
assert_eq "3" "$(tag_value MaxLibraryDatabaseConnections "$config")" "paired-form ambiguous Database value"
assert_tag_value MaxAuthDbConnections 4
assert_tag_value MaxOtherDatabaseConnections 2
assert_tag_value EnableSqLiteMmio true
assert_no_temp_leftovers

printf 'emby config phase test passed\n'
