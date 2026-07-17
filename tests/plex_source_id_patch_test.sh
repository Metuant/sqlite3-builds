#!/usr/bin/env bash
set -euo pipefail

. ./lsio-mods/shared/cont-init-fragments/atomic-write.sh
. ./lsio-mods/shared/cont-init-fragments/plex-patch.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-plex-source-id.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-plex-source-id.XXXXXX)"
cleanup() {
  chmod -R u+w "$tmp" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT

fail() {
  printf 'FATAL: %s\n' "$*" >&2
  exit 1
}

assert_eq() {
  local expected=$1 actual=$2 message=$3
  [ "$actual" = "$expected" ] || fail "$message: expected [$expected], actual [$actual]"
}

assert_contains() {
  local needle=$1 path=$2 message=$3
  grep -Fq "$needle" "$path" || fail "$message: missing [$needle] in $path"
}

assert_not_contains() {
  local needle=$1 path=$2 message=$3
  if grep -Fq "$needle" "$path"; then
    fail "$message: unexpected [$needle] in $path"
  fi
}

sha_file() {
  sha256sum "$1" | awk '{print $1}'
}

write_hex_file() {
  local path=$1 hex=$2
  printf '%s' "$hex" | xxd -r -p > "$path"
}

write_pristine() {
  local path=$1 source_id=$2
  write_hex_file "$path" "$pool_original_hex"
  printf '0123456789abcdef%s' "$source_id" >> "$path"
}

write_pool_patched() {
  local path=$1 source_id=$2
  write_hex_file "$path" "$pool_patched_hex"
  printf '0123456789abcdef%s' "$source_id" >> "$path"
}

assert_source_count() {
  local path=$1 source_id=$2 expected=$3 message=$4 actual
  actual="$(grep -aoF "$source_id" "$path" | awk 'END { print NR + 0 }')"
  assert_eq "$expected" "$actual" "$message"
}

run_patch() {
  local expected_rc=$1 log=$2
  shift 2
  set +e
  plex_patch_apply_pms "$@" > "$log" 2>&1
  local rc=$?
  set -e
  assert_eq "$expected_rc" "$rc" "Plex patch exit status"
}

old_id="$(awk -F '\t' '
  NR == 1 { for (i = 1; i <= NF; i++) if ($i == "sqlite_source_id_guard") guard_col = i; next }
  $2 == "plex" { value = $guard_col; count++ }
  END { if (guard_col > 0 && count == 1) print value; else exit 1 }
' pins/library-compat-groups.tsv)"
# shellcheck source=pins/versions.env
. pins/versions.env
new_id="${SQLITE_SOURCE_ID//%20/ }"
assert_eq 84 "${#old_id}" "OLD source id length"
assert_eq 84 "${#new_id}" "NEW source id length"

pool_original_hex='be140000004c89ff41b800000000506a'
pool_patched_hex='be100000004c89ff41b800000000506a'
pool_site="pms-site|0|1|$pool_original_hex|$pool_patched_hex"
bad_sha='ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff'

pristine="$tmp/pristine/Plex Media Server"
mkdir -p "${pristine%/*}"
write_pristine "$pristine" "$old_id"
chmod 0640 "$pristine"
pristine_owner="$(sqlite3_mod_stat_owner "$pristine")"
pristine_sha="$(sha_file "$pristine")"
pool_post_fixture="$tmp/pool-post-fixture"
write_pool_patched "$pool_post_fixture" "$old_id"
pool_post_sha="$(sha_file "$pool_post_fixture")"
source_id_patched_fixture="$tmp/source-id-patched-fixture"
write_pool_patched "$source_id_patched_fixture" "$new_id"
source_id_patched_sha="$(sha_file "$source_id_patched_fixture")"
for icu in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
  printf 'icu-%s\n' "$icu" > "${pristine%/*}/$icu"
done
icu_before="$(sha256sum "${pristine%/*}"/libicu*plex.so.69)"
run_patch 0 "$tmp/pristine.log" "$pristine" "$pristine_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_source_count "$pristine" "$new_id" 1 "pristine input source id"
assert_eq "$pool_patched_hex" "$(dd if="$pristine" bs=1 count=16 2>/dev/null | od -An -tx1 -v | tr -d ' \n')" "pristine input pool site"
assert_eq 640 "$(sqlite3_mod_stat_mode "$pristine")" "mode preservation"
assert_eq "$pristine_owner" "$(sqlite3_mod_stat_owner "$pristine")" "owner preservation"
assert_eq "$pristine_sha" "$(sha_file "$pristine.bundled.bak")" "single pristine backup"
assert_eq "$icu_before" "$(sha256sum "${pristine%/*}"/libicu*plex.so.69)" "Plex ICU files must remain unchanged"

pool_post="$tmp/pool-post/Plex Media Server"
mkdir -p "${pool_post%/*}"
write_pristine "$pool_post.bundled.bak" "$old_id"
write_pool_patched "$pool_post" "$old_id"
pool_post_pre_sha="$(sha_file "$pool_post.bundled.bak")"
pool_post_current_sha="$(sha_file "$pool_post")"
run_patch 0 "$tmp/pool-post.log" "$pool_post" "$pool_post_pre_sha" "$pool_post_current_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_source_count "$pool_post" "$new_id" 1 "pool-patched input source id"
assert_eq "$pool_post_pre_sha" "$(sha_file "$pool_post.bundled.bak")" "pool-patched input pristine backup"

idempotent_sha="$(sha_file "$pool_post")"
run_patch 0 "$tmp/idempotent.log" "$pool_post" "$bad_sha" "$bad_sha" "$idempotent_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$idempotent_sha" "$(sha_file "$pool_post")" "already-patched input changed"
assert_contains 'event=already-patched' "$tmp/idempotent.log" "already-patched log"

tampered_final="$tmp/tampered-final"
write_pool_patched "$tampered_final" "$new_id"
printf 'tampered' >> "$tampered_final"
tampered_final_sha="$(sha_file "$tampered_final")"
run_patch 1 "$tmp/tampered-final.log" "$tampered_final" "$bad_sha" "$bad_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$tampered_final_sha" "$(sha_file "$tampered_final")" "tampered already-patched input changed"
assert_contains 'event=post-sha-mismatch' "$tmp/tampered-final.log" "tampered already-patched SHA log"
assert_not_contains 'event=already-patched' "$tmp/tampered-final.log" "tampered already-patched success log"

zero_old="$tmp/zero-old"
write_pristine "$zero_old" "${old_id/a/b}"
zero_sha="$(sha_file "$zero_old")"
run_patch 1 "$tmp/zero-old.log" "$zero_old" "$zero_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$zero_sha" "$(sha_file "$zero_old")" "zero OLD-match input changed"
assert_contains 'event=source-id-match-count' "$tmp/zero-old.log" "zero OLD-match log"
assert_contains 'count=0' "$tmp/zero-old.log" "zero OLD-match count"

multi_old="$tmp/multi-old"
write_pristine "$multi_old" "$old_id"
printf 'x%s' "$old_id" >> "$multi_old"
multi_sha="$(sha_file "$multi_old")"
run_patch 1 "$tmp/multi-old.log" "$multi_old" "$multi_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$multi_sha" "$(sha_file "$multi_old")" "multiple OLD-match input changed"
assert_contains 'count=2' "$tmp/multi-old.log" "multiple OLD-match count"

wrong_pre="$tmp/wrong-pre"
write_pristine "$wrong_pre" "$old_id"
wrong_pre_sha="$(sha_file "$wrong_pre")"
run_patch 1 "$tmp/wrong-pre.log" "$wrong_pre" "$bad_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$wrong_pre_sha" "$(sha_file "$wrong_pre")" "wrong pre-SHA input changed"
assert_contains 'event=pre-sha-mismatch' "$tmp/wrong-pre.log" "wrong pre-SHA log"

wrong_post="$tmp/wrong-post"
write_pristine "$wrong_post.bundled.bak" "$old_id"
write_pool_patched "$wrong_post" "$old_id"
wrong_post_sha="$(sha_file "$wrong_post")"
wrong_post_pre_sha="$(sha_file "$wrong_post.bundled.bak")"
run_patch 1 "$tmp/wrong-post.log" "$wrong_post" "$wrong_post_pre_sha" "$bad_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$wrong_post_sha" "$(sha_file "$wrong_post")" "wrong post-SHA input changed"
assert_contains 'event=post-sha-mismatch' "$tmp/wrong-post.log" "wrong post-SHA log"

wrong_final="$tmp/wrong-final"
write_pristine "$wrong_final" "$old_id"
wrong_final_sha="$(sha_file "$wrong_final")"
run_patch 1 "$tmp/wrong-final.log" "$wrong_final" "$wrong_final_sha" "$pool_post_sha" "$bad_sha" "$old_id" "$new_id" "$pool_site"
assert_eq "$wrong_final_sha" "$(sha_file "$wrong_final")" "wrong final-SHA input changed"
assert_contains 'event=post-sha-mismatch' "$tmp/wrong-final.log" "wrong final-SHA log"

fake_bin="$tmp/fake-bin"
mkdir -p "$fake_bin"
real_mktemp="$(command -v mktemp)"
real_dd="$(command -v dd)"
real_mv="$(command -v mv)"
cat > "$fake_bin/mktemp" <<'EOF_MKTEMP'
#!/usr/bin/env bash
for arg in "$@"; do
  case "$arg" in
    "${FAIL_MKTEMP_PREFIX:-}"*) [ -n "${FAIL_MKTEMP_PREFIX:-}" ] && exit 1 ;;
  esac
done
exec "$REAL_MKTEMP" "$@"
EOF_MKTEMP
cat > "$fake_bin/dd" <<'EOF_DD'
#!/usr/bin/env bash
input=""
output=""
count=""
for arg in "$@"; do
  case "$arg" in
    if=*) input=${arg#if=} ;;
    of=*) output=${arg#of=} ;;
    count=*) count=${arg#count=} ;;
  esac
done
if [ "$count" = "84" ] && [ -n "${FAIL_SOURCE_WRITE_PREFIX:-}" ]; then
  case "$output" in "${FAIL_SOURCE_WRITE_PREFIX}"*) exit 1 ;; esac
fi
if [ "$count" = "84" ] && [ -n "${FAIL_SOURCE_READ_PREFIX:-}" ]; then
  case "$input" in "${FAIL_SOURCE_READ_PREFIX}"*) printf 'readback-mismatch'; exit 0 ;; esac
fi
exec "$REAL_DD" "$@"
EOF_DD
cat > "$fake_bin/mv" <<'EOF_MV'
#!/usr/bin/env bash
last=""
for arg in "$@"; do last=$arg; done
if [ -n "${MV_COUNT_FILE:-}" ] && [ "$last" = "${MV_COUNT_DEST:-}" ]; then
  printf '1\n' >> "$MV_COUNT_FILE"
fi
if [ -n "${FAIL_MV_DEST:-}" ] && [ "$last" = "$FAIL_MV_DEST" ]; then
  exit 1
fi
exec "$REAL_MV" "$@"
EOF_MV
chmod +x "$fake_bin/mktemp" "$fake_bin/dd" "$fake_bin/mv"

old_path=$PATH
export REAL_MKTEMP="$real_mktemp" REAL_DD="$real_dd" REAL_MV="$real_mv"
PATH="$fake_bin:$PATH"
hash -r

temp_fail="$tmp/temp-fail"
write_pristine "$temp_fail" "$old_id"
temp_fail_sha="$(sha_file "$temp_fail")"
export FAIL_MKTEMP_PREFIX="$temp_fail.sqlite3-mod."
run_patch 1 "$tmp/temp-fail.log" "$temp_fail" "$temp_fail_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
unset FAIL_MKTEMP_PREFIX
assert_eq "$temp_fail_sha" "$(sha_file "$temp_fail")" "temp failure changed target"
assert_contains 'reason=mktemp' "$tmp/temp-fail.log" "temp failure log"

write_fail="$tmp/write-fail"
write_pristine "$write_fail" "$old_id"
write_fail_sha="$(sha_file "$write_fail")"
export FAIL_SOURCE_WRITE_PREFIX="$write_fail.sqlite3-mod."
run_patch 1 "$tmp/write-fail.log" "$write_fail" "$write_fail_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
unset FAIL_SOURCE_WRITE_PREFIX
assert_eq "$write_fail_sha" "$(sha_file "$write_fail")" "source-id write failure changed target"
assert_contains 'event=source-id-write-failed' "$tmp/write-fail.log" "source-id write failure log"

read_fail="$tmp/read-fail"
write_pristine "$read_fail" "$old_id"
read_fail_sha="$(sha_file "$read_fail")"
export FAIL_SOURCE_READ_PREFIX="$read_fail.sqlite3-mod."
run_patch 1 "$tmp/read-fail.log" "$read_fail" "$read_fail_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
unset FAIL_SOURCE_READ_PREFIX
assert_eq "$read_fail_sha" "$(sha_file "$read_fail")" "source-id readback failure changed target"
assert_contains 'event=source-id-readback-failed' "$tmp/read-fail.log" "source-id readback failure log"

rename_fail="$tmp/rename-fail"
write_pristine "$rename_fail" "$old_id"
rename_fail_sha="$(sha_file "$rename_fail")"
export FAIL_MV_DEST="$rename_fail"
run_patch 1 "$tmp/rename-fail.log" "$rename_fail" "$rename_fail_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
unset FAIL_MV_DEST
assert_eq "$rename_fail_sha" "$(sha_file "$rename_fail")" "rename failure changed target"
assert_contains 'reason=mv' "$tmp/rename-fail.log" "rename failure log"

single_replace="$tmp/single-replace"
write_pristine "$single_replace" "$old_id"
single_replace_sha="$(sha_file "$single_replace")"
mv_count_file="$tmp/mv-count"
: > "$mv_count_file"
export MV_COUNT_FILE="$mv_count_file" MV_COUNT_DEST="$single_replace"
run_patch 0 "$tmp/single-replace.log" "$single_replace" "$single_replace_sha" "$pool_post_sha" "$source_id_patched_sha" "$old_id" "$new_id" "$pool_site"
unset MV_COUNT_FILE MV_COUNT_DEST
assert_eq 1 "$(awk 'END { print NR + 0 }' "$mv_count_file")" "PMS atomic replace count"

PATH=$old_path
hash -r
unset REAL_MKTEMP REAL_DD REAL_MV

printf 'Plex source-id patch tests passed\n'
