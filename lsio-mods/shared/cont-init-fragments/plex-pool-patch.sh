#!/usr/bin/env bash
# Sourced shared Plex pool-patch core. Callers pass every path, SHA, and site.

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

plex_pool_patch_populate_binary_tmp() {
  local tmp=$1
  local bin=$2
  local _expected_pre_sha=$3
  local site label offset write_seek original_hex patched_hex write_index write_hex_index write_hex_byte observed_hex
  shift 3

  if ! cp -f "$bin" "$tmp"; then
    plex_pool_patch_failure_event="write-failed"
    plex_pool_patch_failure_site="copy"
    return 1
  fi

  for site in "$@"; do
    IFS='|' read -r label offset write_seek original_hex patched_hex <<EOF_SITE
$site
EOF_SITE
    write_index=$((write_seek - offset))
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
    if ! printf '%b' "\\x${write_hex_byte}" | dd of="$tmp" bs=1 seek="$write_seek" count=1 conv=notrunc 2>/dev/null; then
      plex_pool_patch_failure_event="write-failed"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
    observed_hex="$(plex_pool_patch_read_hex "$tmp" "$offset")"
    if [ "$observed_hex" != "$patched_hex" ]; then
      plex_pool_patch_failure_event="verify-failed"
      plex_pool_patch_failure_site="$label"
      return 1
    fi
  done
  return 0
}

plex_pool_patch_apply_binary() {
  local bin=$1
  local expected_pre_sha=$2
  local all_pre all_post site label offset write_seek original_hex patched_hex observed_hex bak current_sha
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
    observed_hex="$(plex_pool_patch_read_hex "$bin" "$offset")"
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
