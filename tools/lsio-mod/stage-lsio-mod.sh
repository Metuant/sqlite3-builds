#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage:
  stage-lsio-mod.sh --mod plex|emby --output-dir DIR --baked-pins FILE \
    --artifact ARCH:COMPAT_GROUP:SOURCE_PATH [--artifact ...]
USAGE
}

mod=""
output_dir=""
baked_pins=""
artifacts=()
declare -A artifact_source staged_artifact_sha

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
  IFS=':' read -r arch compat_group source_path extra <<EOF_ART
$artifact
EOF_ART
  [ -z "${extra:-}" ] || { echo "FATAL: malformed or missing artifact: $artifact" >&2; exit 1; }
  [ -n "$arch" ] && [ -n "$compat_group" ] && [ -f "$source_path" ] || { echo "FATAL: malformed or missing artifact: $artifact" >&2; exit 1; }
  [ -z "${artifact_source[$compat_group|$arch]:-}" ] || { echo "FATAL: duplicate artifact for compat_group=$compat_group arch=$arch" >&2; exit 1; }
  artifact_source[$compat_group|$arch]="$source_path"
done

while IFS= read -r line || [ -n "$line" ]; do
  line="${line%$'\r'}"
  case "$line" in
    ""|\#*) continue ;;
  esac

  IFS='|' read -r kind version row_mod _server_id arch compat_group relpath _target_path expected_sha extra <<EOF_ROW
$line
EOF_ROW
  [ "$kind" = "artifact" ] || continue
  [ "$row_mod" = "$mod" ] || continue
  [ -z "${extra:-}" ] || { echo "FATAL: malformed artifact row in baked pins: $line" >&2; exit 1; }
  [ "$version" = "1" ] || { echo "FATAL: unsupported artifact row version: $version" >&2; exit 1; }
  [ -n "$arch" ] && [ -n "$compat_group" ] && [ -n "$relpath" ] && [ -n "$expected_sha" ] || { echo "FATAL: malformed artifact row in baked pins: $line" >&2; exit 1; }
  IFS='/' read -r rel_root rel_arch rel_compat rel_file rel_extra <<EOF_RELPATH
$relpath
EOF_RELPATH
  [ "$rel_root" = "artifacts" ] && [ "$rel_arch" = "$arch" ] && [ "$rel_compat" = "$compat_group" ] && [ "$rel_file" = "libsqlite3.so" ] && [ -z "${rel_extra:-}" ] || {
    echo "FATAL: invalid artifact relpath in baked pins: $relpath" >&2
    exit 1
  }
  case "$relpath" in
    *..*) echo "FATAL: unsafe artifact relpath in baked pins: $relpath" >&2; exit 1 ;;
  esac
  source_path="${artifact_source[$compat_group|$arch]:-}"
  [ -n "$source_path" ] || { echo "FATAL: missing --artifact for manifest compat_group=$compat_group arch=$arch" >&2; exit 1; }
  actual_sha="$(sha256sum "$source_path" | awk '{print $1}')"
  [ "$actual_sha" = "$expected_sha" ] || { echo "FATAL: artifact SHA mismatch for $source_path: expected $expected_sha got $actual_sha" >&2; exit 1; }
  if [ -n "${staged_artifact_sha[$relpath]:-}" ]; then
    [ "${staged_artifact_sha[$relpath]}" = "$expected_sha" ] || { echo "FATAL: conflicting artifact rows for relpath: $relpath" >&2; exit 1; }
    continue
  fi
  artifact_dest="$output_dir/root-fs/opt/sqlite3-lsio-mod/$relpath"
  artifact_dest_dir="$(dirname "$artifact_dest")"
  mkdir -p "$artifact_dest_dir"
  cp "$source_path" "$artifact_dest"
  chmod 0644 "$artifact_dest"
  staged_artifact_sha[$relpath]="$expected_sha"
done < "$baked_pins"

find "$output_dir/root-fs/etc/s6-overlay/s6-rc.d" -path '*/run' -type f -exec chmod 0755 {} +
find "$output_dir/root-fs/opt/sqlite3-lsio-mod/lib" -type f -exec chmod 0644 {} +
printf '%s\n' "$output_dir"
