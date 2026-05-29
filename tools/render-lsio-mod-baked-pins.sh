#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage:
  render-lsio-mod-baked-pins.sh --mod plex|emby --release-tag TAG --generated-at ISO \
    --sha256sums SHA256SUMS --pre-fragment FILE --pool-baselines FILE --output FILE \
    --artifact ARCH:NAME:PATH:TARGET_PATH [--artifact ...]
USAGE
}

mod=""
release_tag=""
generated_at=""
sha256sums=""
pool_baselines=""
output=""
pre_fragments=()
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
    --release-tag) require_value "$1" "${2:-}"; release_tag="$2"; shift 2 ;;
    --generated-at) require_value "$1" "${2:-}"; generated_at="$2"; shift 2 ;;
    --sha256sums) require_value "$1" "${2:-}"; sha256sums="$2"; shift 2 ;;
    --pre-fragment) require_value "$1" "${2:-}"; pre_fragments+=("$2"); shift 2 ;;
    --pool-baselines) require_value "$1" "${2:-}"; pool_baselines="$2"; shift 2 ;;
    --output) require_value "$1" "${2:-}"; output="$2"; shift 2 ;;
    --artifact) require_value "$1" "${2:-}"; artifacts+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

case "$mod" in plex|emby) ;; *) usage; exit 2 ;; esac
[ -n "$release_tag" ] || { echo "FATAL: missing --release-tag" >&2; exit 2; }
[ -n "$generated_at" ] || { echo "FATAL: missing --generated-at" >&2; exit 2; }
[ -f "$sha256sums" ] || { echo "FATAL: missing --sha256sums file" >&2; exit 2; }
[ -n "$output" ] || { echo "FATAL: missing --output" >&2; exit 2; }
[ "${#pre_fragments[@]}" -gt 0 ] || { echo "FATAL: at least one --pre-fragment is required" >&2; exit 2; }

lookup_sha() {
  local name=$1 sha
  sha="$(awk -v name="$name" '$2 == name { print $1; found=1; exit } END { if (!found) exit 1 }' "$sha256sums")" || {
    echo "FATAL: missing SHA256SUMS entry for $name" >&2
    exit 1
  }
  [[ "$sha" =~ ^[0-9A-Fa-f]{64}$ ]] || {
    echo "FATAL: invalid SHA256SUMS entry for $name: $sha" >&2
    exit 1
  }
  printf '%s\n' "$sha"
}

tmp="${output}.tmp.$$"
trap 'rm -f "$tmp"' EXIT

{
  printf '# baked-pins schema=2\n'
  printf 'version|2|release_tag|%s|generated_at|%s\n' "$release_tag" "$generated_at"

  for fragment in "${pre_fragments[@]}"; do
    [ -f "$fragment" ] || { echo "FATAL: missing pre fragment: $fragment" >&2; exit 1; }
    awk -F'|' -v mod="$mod" '
      /^#/ || NF == 0 { next }
      $1 != "pre" { next }
      {
        if (NF != 9) {
          printf "FATAL: malformed pre row: %s\n", $0 > "/dev/stderr"; exit 1
        }
        for (i = 1; i <= NF; i++) {
          if ($i == "") {
            printf "FATAL: empty field in pre row: %s\n", $0 > "/dev/stderr"; exit 1
          }
        }
        if ($2 != "1") {
          printf "FATAL: unsupported pre row version: %s\n", $0 > "/dev/stderr"; exit 1
        }
        if ($8 != "runtime") {
          printf "FATAL: unsupported pre row source kind: %s\n", $0 > "/dev/stderr"; exit 1
        }
        if ($9 !~ /^[0-9A-Fa-f]{64}$/) {
          printf "FATAL: invalid pre row SHA: %s\n", $0 > "/dev/stderr"; exit 1
        }
        if ($3 == mod) { print }
      }
    ' "$fragment"
  done

  seen_arches=""
  for artifact in "${artifacts[@]}"; do
    IFS=':' read -r arch name path target_path <<EOF_ART
$artifact
EOF_ART
    [ -n "$arch" ] && [ -n "$name" ] && [ -n "$path" ] && [ -n "$target_path" ] || {
      echo "FATAL: malformed --artifact: $artifact" >&2
      exit 1
    }
    seen_arches="${seen_arches} ${arch}"
    if [ ! -f "$path" ]; then
      # CI prechecks artifact paths; this row keeps local/offline renders schema-complete.
      printf 'unsupported|%s|missing-artifact:%s\n' "$arch" "$name"
      continue
    fi
    sha="$(lookup_sha "$name")"
    actual="$(sha256sum "$path" | awk '{print $1}')"
    if [ "$actual" != "$sha" ]; then
      echo "FATAL: artifact SHA mismatch for $path: expected $sha got $actual" >&2
      exit 1
    fi
    printf 'current|1|%s|%s|%s|%s|%s\n' "$mod" "$arch" "$name" "$target_path" "$sha"
  done

  if [ "$mod" = "plex" ]; then
    [ -f "$pool_baselines" ] || { echo "FATAL: missing --pool-baselines file" >&2; exit 2; }
    awk -F'|' '
      /^#/ || NF == 0 { next }
      NF != 4 { printf "FATAL: malformed pool baseline row: %s\n", $0 > "/dev/stderr"; exit 1 }
      $1 == "plex" {
        if ($4 !~ /^[0-9A-Fa-f]{64}$/) {
          printf "FATAL: invalid pool baseline SHA: %s\n", $0 > "/dev/stderr"; exit 1
        }
        printf "pool-pre|1|%s|%s|%s|%s\n", $1, $2, $3, $4
      }
    ' "$pool_baselines"
  fi
} > "$tmp"

mv -f "$tmp" "$output"
trap - EXIT
