#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage:
  stage-lsio-mod.sh --mod plex|emby --output-dir DIR --baked-pins FILE \
    --artifact ARCH:SOURCE_PATH [--artifact ...]
USAGE
}

mod=""
output_dir=""
baked_pins=""
artifacts=()

require_value() {
  local flag=$1
  if [ "$#" -lt 2 ] || [ -z "${2:-}" ] || [[ "${2:-}" == --* ]]; then
    echo "FATAL: missing value for $flag" >&2
    exit 2
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --mod) require_value "$1" "${2:-}"; mod="$2"; shift 2 ;;
    --output-dir) require_value "$1" "${2:-}"; output_dir="$2"; shift 2 ;;
    --baked-pins) require_value "$1" "${2:-}"; baked_pins="$2"; shift 2 ;;
    --artifact) require_value "$1" "${2:-}"; artifacts+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
case "$mod" in plex|emby) ;; *) usage; exit 2 ;; esac
[ -n "$output_dir" ] || { echo "FATAL: missing --output-dir" >&2; exit 2; }
[ -f "$baked_pins" ] || { echo "FATAL: missing --baked-pins file" >&2; exit 2; }

mkdir -p "$output_dir"
cp -R "lsio-mods/${mod}/." "$output_dir/"
mkdir -p "$output_dir/root-fs/opt/sqlite3-lsio-mod/lib"
cp lsio-mods/shared/cont-init-fragments/*.sh "$output_dir/root-fs/opt/sqlite3-lsio-mod/lib/"
cp "$baked_pins" "$output_dir/root-fs/opt/sqlite3-lsio-mod/baked-pins.txt"

for artifact in "${artifacts[@]}"; do
  IFS=':' read -r arch source_path <<EOF_ART
$artifact
EOF_ART
  [ -n "$arch" ] && [ -f "$source_path" ] || { echo "FATAL: malformed or missing artifact: $artifact" >&2; exit 1; }
  mkdir -p "$output_dir/root-fs/opt/sqlite3-lsio-mod/artifacts/${arch}"
  cp "$source_path" "$output_dir/root-fs/opt/sqlite3-lsio-mod/artifacts/${arch}/libsqlite3.so"
  chmod 0644 "$output_dir/root-fs/opt/sqlite3-lsio-mod/artifacts/${arch}/libsqlite3.so"
done

find "$output_dir/root-fs/etc/s6-overlay/s6-rc.d" -path '*/run' -type f -exec chmod 0755 {} +
find "$output_dir/root-fs/opt/sqlite3-lsio-mod/lib" -type f -exec chmod 0644 {} +
printf '%s\n' "$output_dir"
