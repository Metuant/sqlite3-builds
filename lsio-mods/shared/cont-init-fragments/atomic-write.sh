#!/usr/bin/env bash

sqlite3_mod_stat_owner() {
  stat -c '%u:%g' "$1" 2>/dev/null || stat -f '%u:%g' "$1"
}

sqlite3_mod_stat_mode() {
  stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"
}

sqlite3_mod_atomic_replace() {
  local target=$1
  local populate_fn=$2
  local target_dir target_base tmp target_owner target_mode tmp_owner
  shift 2
  sqlite3_mod_atomic_replace_error=""

  target_dir="${target%/*}"
  target_base="${target##*/}"
  if [ "$target_dir" = "$target" ]; then target_dir="."; fi

  tmp=""
  if ! tmp="$(mktemp "${target_dir}/${target_base}.sqlite3-mod.XXXXXX")"; then
    sqlite3_mod_atomic_replace_error="mktemp"
    return 1
  fi

  if ! "$populate_fn" "$tmp" "$target" "$@"; then
    if [ -z "${sqlite3_mod_atomic_replace_error:-}" ]; then
      sqlite3_mod_atomic_replace_error="populate"
    fi
    rm -f "$tmp" 2>/dev/null || true
    return 1
  fi

  if [ -e "$target" ]; then
    target_owner="$(sqlite3_mod_stat_owner "$target")" || { sqlite3_mod_atomic_replace_error="metadata"; rm -f "$tmp" 2>/dev/null || true; return 1; }
    target_mode="$(sqlite3_mod_stat_mode "$target")" || { sqlite3_mod_atomic_replace_error="metadata"; rm -f "$tmp" 2>/dev/null || true; return 1; }
    tmp_owner="$(sqlite3_mod_stat_owner "$tmp")" || { sqlite3_mod_atomic_replace_error="metadata"; rm -f "$tmp" 2>/dev/null || true; return 1; }
    if [ "$tmp_owner" != "$target_owner" ]; then
      chown "$target_owner" "$tmp" || { sqlite3_mod_atomic_replace_error="metadata"; rm -f "$tmp" 2>/dev/null || true; return 1; }
    fi
    chmod "$target_mode" "$tmp" || { sqlite3_mod_atomic_replace_error="metadata"; rm -f "$tmp" 2>/dev/null || true; return 1; }
  fi

  mv -f "$tmp" "$target" || { sqlite3_mod_atomic_replace_error="mv"; rm -f "$tmp" 2>/dev/null || true; return 1; }
}
