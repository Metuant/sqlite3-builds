#!/usr/bin/env bash
# Sourced shared Plex binary-patch core. Callers pass every path, SHA, site, and
# source-id value.

plex_pool_patch_sha256_of() {
  sha256sum "$1" | awk '{print $1}'
}

plex_pool_patch_read_hex() {
  local bin=$1
  local offset=$2
  dd if="$bin" bs=1 skip="$offset" count=16 2>/dev/null |
    od -An -tx1 -v |
    tr -d ' \n'
}

plex_pool_patch_verify_backup() {
  local bin=$1
  local bak=$2
  local expected_sha=$3
  local actual_sha
  if [ ! -f "$bak" ]; then
    printf 'warn pool_patch event=missing-backup binary=%s\n' "$bin" >&2
    return 1
  fi
  actual_sha="$(plex_pool_patch_sha256_of "$bak")"
  if [ "$actual_sha" != "$expected_sha" ]; then
    printf 'warn pool_patch event=bad-backup-sha binary=%s expected=%s actual=%s\n' "$bin" "$expected_sha" "$actual_sha" >&2
    return 1
  fi
}

plex_pool_patch_apply_sites_to_tmp() {
  local tmp=$1
  local site label offset write_seek original_hex patched_hex offset_num write_seek_num write_index write_hex_index write_hex_byte observed_hex
  shift

  for site in "$@"; do
    IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
    offset_num=$((10#$offset))
    write_seek_num=$((10#$write_seek))
    write_index=$((write_seek_num - offset_num))
    if [ "$write_index" -lt 0 ]; then
      plex_pool_patch_failure_event="invalid-site"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
    write_hex_index=$((write_index * 2))
    write_hex_byte="${patched_hex:$write_hex_index:2}"
    if [ "${#write_hex_byte}" -ne 2 ]; then
      plex_pool_patch_failure_event="invalid-site"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
    if ! printf '%b' "\\x${write_hex_byte}" | dd of="$tmp" bs=1 seek="$write_seek_num" count=1 conv=notrunc 2>/dev/null; then
      plex_pool_patch_failure_event="write-failed"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
    observed_hex="$(plex_pool_patch_read_hex "$tmp" "$offset_num")"
    if [ "$observed_hex" != "$patched_hex" ]; then
      plex_pool_patch_failure_event="verify-failed"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
  done
  return 0
}

plex_pool_patch_populate_binary_tmp() {
  local tmp=$1
  local bin=$2
  local _expected_pre_sha=$3
  shift 3

  if ! cp -f "$bin" "$tmp"; then
    plex_pool_patch_failure_event="write-failed"
    plex_pool_patch_failure_site="copy"
    return 1
  fi
  plex_pool_patch_apply_sites_to_tmp "$tmp" "$@"
}

plex_pool_patch_apply_binary() {
  local bin=$1
  local expected_pre_sha=$2
  local all_pre all_post site label offset write_seek original_hex patched_hex offset_num observed_hex bak current_sha
  shift 2

  if [ ! -f "$bin" ]; then
    printf 'warn pool_patch event=missing-binary binary=%s\n' "$bin" >&2
    return 0
  fi
  if [ "$#" -eq 0 ]; then
    printf 'warn pool_patch event=no-sites binary=%s\n' "$bin" >&2
    return 0
  fi

  all_pre=1
  all_post=1
  for site in "$@"; do
    IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
    offset_num=$((10#$offset))
    observed_hex="$(plex_pool_patch_read_hex "$bin" "$offset_num")"
    if [ "$observed_hex" != "$original_hex" ]; then
      all_pre=0
    fi
    if [ "$observed_hex" != "$patched_hex" ]; then
      all_post=0
    fi
  done

  if [ "$all_post" -eq 1 ]; then
    printf 'info pool_patch event=already-patched binary=%s\n' "$bin"
    return 0
  fi

  # Why: a verified .bundled.bak stays immutable so recovery keeps a known-good bundled binary.
  bak="${bin}.bundled.bak"
  # Why: unknown bytes skip instead of restoring so we do not downgrade a legitimately updated binary.
  if [ "$all_pre" -ne 1 ]; then
    printf 'warn pool_patch event=unknown-target-state binary=%s\n' "$bin" >&2
    return 0
  fi

  current_sha="$(plex_pool_patch_sha256_of "$bin")"
  if [ "$current_sha" != "$expected_pre_sha" ]; then
    printf 'warn pool_patch event=pre-sha-mismatch binary=%s expected=%s actual=%s\n' "$bin" "$expected_pre_sha" "$current_sha" >&2
    return 0
  fi

  if [ ! -f "$bak" ]; then
    if ! cp "$bin" "$bak"; then
      printf 'warn pool_patch event=backup-create-failed binary=%s\n' "$bin" >&2
      return 0
    fi
  elif ! plex_pool_patch_verify_backup "$bin" "$bak" "$expected_pre_sha"; then
    printf 'warn pool_patch event=existing-backup-untrusted binary=%s\n' "$bin" >&2
    return 0
  fi

  plex_pool_patch_failure_event=""
  plex_pool_patch_failure_site=""
  if ! sqlite3_mod_atomic_replace "$bin" plex_pool_patch_populate_binary_tmp "$expected_pre_sha" "$@"; then
    case "${plex_pool_patch_failure_event:-}" in
      invalid-site) printf 'warn pool_patch event=invalid-site binary=%s site=%s\n' "$bin" "$plex_pool_patch_failure_site" >&2 ;;
      verify-failed) printf 'warn pool_patch event=verify-failed binary=%s site=%s\n' "$bin" "$plex_pool_patch_failure_site" >&2 ;;
      *) printf 'warn pool_patch event=write-failed binary=%s site=%s\n' "$bin" "${plex_pool_patch_failure_site:-atomic}" >&2 ;;
    esac
    return 0
  fi

  for site in "$@"; do
    IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
    printf 'info pool_patch event=patched binary=%s site=%s\n' "$bin" "$label"
  done
  return 0
}

plex_patch_source_id_matches() {
  local bin=$1
  local source_id=$2
  LC_ALL=C grep -aobF "$source_id" "$bin" 2>/dev/null || true
}

plex_patch_match_count() {
  awk 'NF { count++ } END { print count + 0 }'
}

plex_patch_populate_pms_tmp() {
  local tmp=$1
  local bin=$2
  local source_id_offset=$3
  local source_id_new=$4
  local observed_source_id
  shift 4

  if ! cp -f "$bin" "$tmp"; then
    plex_patch_failure_event="write-failed"
    plex_patch_failure_site="copy"
    return 1
  fi
  if ! plex_pool_patch_apply_sites_to_tmp "$tmp" "$@"; then
    plex_patch_failure_event="${plex_pool_patch_failure_event:-write-failed}"
    plex_patch_failure_site="${plex_pool_patch_failure_site:-pool-site}"
    return 1
  fi
  if ! printf '%s' "$source_id_new" | dd of="$tmp" bs=1 seek="$source_id_offset" count=84 conv=notrunc 2>/dev/null; then
    plex_patch_failure_event="source-id-write-failed"
    plex_patch_failure_site="source-id"
    return 1
  fi
  if ! observed_source_id="$(dd if="$tmp" bs=1 skip="$source_id_offset" count=84 2>/dev/null)"; then
    plex_patch_failure_event="source-id-readback-failed"
    plex_patch_failure_site="source-id"
    return 1
  fi
  if [ "$observed_source_id" != "$source_id_new" ]; then
    plex_patch_failure_event="source-id-readback-failed"
    plex_patch_failure_site="source-id"
    return 1
  fi
}

plex_patch_populate_verified_pms_tmp() {
  local tmp=$1
  local bin=$2
  local source_id_offset=$3
  local source_id_new=$4
  local expected_source_id_patched_sha=$5
  local observed_sha
  shift 5

  if ! plex_patch_populate_pms_tmp "$tmp" "$bin" "$source_id_offset" "$source_id_new" "$@"; then
    return 1
  fi
  observed_sha="$(plex_pool_patch_sha256_of "$tmp")"
  if [ "$observed_sha" != "$expected_source_id_patched_sha" ]; then
    plex_patch_failure_event="post-sha-mismatch"
    plex_patch_failure_expected_sha="$expected_source_id_patched_sha"
    plex_patch_failure_actual_sha="$observed_sha"
    return 1
  fi
}

plex_patch_apply_pms() {
  local bin=$1
  local expected_pre_sha=$2
  local expected_pool_post_sha=$3
  local expected_source_id_patched_sha=$4
  local source_id_old=$5
  local source_id_new=$6
  local all_pre all_post site label offset write_seek original_hex patched_hex offset_num observed_hex
  local new_matches new_count old_matches old_count source_id_offset current_sha expected_current_sha bak pool_was_pre
  shift 6

  if [ ! -f "$bin" ]; then
    printf 'warn plex_patch event=missing-binary binary=%s\n' "$bin" >&2
    return 1
  fi
  if [ "$#" -eq 0 ]; then
    printf 'warn plex_patch event=no-sites binary=%s\n' "$bin" >&2
    return 1
  fi
  if [ "${#source_id_old}" -ne 84 ] || [ "${#source_id_new}" -ne 84 ]; then
    printf 'warn plex_patch event=invalid-source-id-length binary=%s old_length=%s new_length=%s\n' "$bin" "${#source_id_old}" "${#source_id_new}" >&2
    return 1
  fi

  all_pre=1
  all_post=1
  for site in "$@"; do
    IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
    offset_num=$((10#$offset))
    observed_hex="$(plex_pool_patch_read_hex "$bin" "$offset_num")"
    if [ "$observed_hex" != "$original_hex" ]; then
      all_pre=0
    fi
    if [ "$observed_hex" != "$patched_hex" ]; then
      all_post=0
    fi
  done

  new_matches="$(plex_patch_source_id_matches "$bin" "$source_id_new")"
  new_count="$(printf '%s\n' "$new_matches" | plex_patch_match_count)"
  if [ "$all_post" -eq 1 ] && [ "$new_count" -eq 1 ]; then
    old_matches="$(plex_patch_source_id_matches "$bin" "$source_id_old")"
    old_count="$(printf '%s\n' "$old_matches" | plex_patch_match_count)"
    if [ "$old_count" -eq 0 ]; then
      current_sha="$(plex_pool_patch_sha256_of "$bin")"
      if [ "$current_sha" != "$expected_source_id_patched_sha" ]; then
        printf 'warn plex_patch event=post-sha-mismatch binary=%s expected=%s actual=%s\n' "$bin" "$expected_source_id_patched_sha" "$current_sha" >&2
        return 1
      fi
      printf 'info plex_patch event=already-patched binary=%s\n' "$bin"
      return 0
    fi
    printf 'warn plex_patch event=conflicting-source-id-state binary=%s old_count=%s new_count=%s\n' "$bin" "$old_count" "$new_count" >&2
    return 1
  fi
  if [ "$new_count" -ne 0 ]; then
    printf 'warn plex_patch event=unknown-source-id-state binary=%s new_count=%s\n' "$bin" "$new_count" >&2
    return 1
  fi

  old_matches="$(plex_patch_source_id_matches "$bin" "$source_id_old")"
  old_count="$(printf '%s\n' "$old_matches" | plex_patch_match_count)"
  if [ "$old_count" -ne 1 ]; then
    printf 'warn plex_patch event=source-id-match-count binary=%s count=%s\n' "$bin" "$old_count" >&2
    return 1
  fi
  source_id_offset="$(printf '%s\n' "$old_matches" | awk -F: 'NF { print $1; exit }')"
  case "$source_id_offset" in
    ''|*[!0-9]*)
      printf 'warn plex_patch event=invalid-source-id-offset binary=%s\n' "$bin" >&2
      return 1
      ;;
  esac

  if [ "$all_pre" -eq 1 ]; then
    expected_current_sha="$expected_pre_sha"
    pool_was_pre=1
  elif [ "$all_post" -eq 1 ]; then
    expected_current_sha="$expected_pool_post_sha"
    pool_was_pre=0
  else
    printf 'warn plex_patch event=unknown-target-state binary=%s\n' "$bin" >&2
    return 1
  fi

  current_sha="$(plex_pool_patch_sha256_of "$bin")"
  if [ "$current_sha" != "$expected_current_sha" ]; then
    if [ "$pool_was_pre" -eq 1 ]; then
      printf 'warn plex_patch event=pre-sha-mismatch binary=%s expected=%s actual=%s\n' "$bin" "$expected_current_sha" "$current_sha" >&2
    else
      printf 'warn plex_patch event=post-sha-mismatch binary=%s expected=%s actual=%s\n' "$bin" "$expected_current_sha" "$current_sha" >&2
    fi
    return 1
  fi

  bak="${bin}.bundled.bak"
  if [ ! -f "$bak" ]; then
    if [ "$pool_was_pre" -ne 1 ]; then
      printf 'warn plex_patch event=missing-pristine-backup binary=%s\n' "$bin" >&2
      return 1
    fi
    if ! cp "$bin" "$bak"; then
      printf 'warn plex_patch event=backup-create-failed binary=%s\n' "$bin" >&2
      return 1
    fi
  elif ! plex_pool_patch_verify_backup "$bin" "$bak" "$expected_pre_sha"; then
    printf 'warn plex_patch event=existing-backup-untrusted binary=%s\n' "$bin" >&2
    return 1
  fi

  plex_patch_failure_event=""
  plex_patch_failure_site=""
  plex_patch_failure_expected_sha=""
  plex_patch_failure_actual_sha=""
  plex_pool_patch_failure_event=""
  plex_pool_patch_failure_site=""
  if ! sqlite3_mod_atomic_replace "$bin" plex_patch_populate_verified_pms_tmp "$source_id_offset" "$source_id_new" "$expected_source_id_patched_sha" "$@"; then
    case "${plex_patch_failure_event:-}" in
      invalid-site) printf 'warn plex_patch event=invalid-site binary=%s site=%s\n' "$bin" "$plex_patch_failure_site" >&2 ;;
      post-sha-mismatch) printf 'warn plex_patch event=post-sha-mismatch binary=%s expected=%s actual=%s\n' "$bin" "$plex_patch_failure_expected_sha" "$plex_patch_failure_actual_sha" >&2 ;;
      source-id-write-failed) printf 'warn plex_patch event=source-id-write-failed binary=%s\n' "$bin" >&2 ;;
      source-id-readback-failed) printf 'warn plex_patch event=source-id-readback-failed binary=%s\n' "$bin" >&2 ;;
      verify-failed) printf 'warn plex_patch event=verify-failed binary=%s site=%s\n' "$bin" "$plex_patch_failure_site" >&2 ;;
      write-failed) printf 'warn plex_patch event=write-failed binary=%s site=%s\n' "$bin" "${plex_patch_failure_site:-atomic}" >&2 ;;
      *) printf 'warn plex_patch event=atomic-replace-failed binary=%s reason=%s\n' "$bin" "${sqlite3_mod_atomic_replace_error:-unknown}" >&2 ;;
    esac
    return 1
  fi

  if [ "$pool_was_pre" -eq 1 ]; then
    for site in "$@"; do
      IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
      printf 'info pool_patch event=patched binary=%s site=%s\n' "$bin" "$label"
    done
  fi
  printf 'info plex_patch event=source-id-patched binary=%s offset=%s\n' "$bin" "$source_id_offset"
  return 0
}
