#!/usr/bin/env bash
install_sqlite_artifact() {
  src=$1
  target=$2
  expected_sha=$3
  tmp="${target}.sqlite3-mod.$$"
  if ! cp -f "$src" "$tmp"; then
    log_warn "phase=${phase_name} event=temp-copy-failed target_path=${target}"
    return 1
  fi
  tmp_sha="$(sha256_of "$tmp")"
  if [ "$tmp_sha" != "$expected_sha" ]; then
    rm -f "$tmp" 2>/dev/null || true
    log_warn "phase=${phase_name} event=temp-sha-mismatch target_path=${target} expected=${expected_sha} actual=${tmp_sha}"
    return 1
  fi
  if ! mv -f "$tmp" "$target"; then
    rm -f "$tmp" 2>/dev/null || true
    log_warn "phase=${phase_name} event=mv-failed target_path=${target}"
    return 1
  fi
  installed_sha="$(sha256_of "$target")"
  if [ "$installed_sha" != "$expected_sha" ]; then
    log_warn "phase=${phase_name} event=installed-sha-mismatch target_path=${target} expected=${expected_sha} actual=${installed_sha}"
    return 1
  fi
  return 0
}
