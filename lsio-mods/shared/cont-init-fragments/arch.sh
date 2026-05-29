#!/usr/bin/env bash
resolve_arch() {
  machine="$(uname -m)"
  case "$machine" in
    x86_64)
      flags=""
      if [ -r /proc/cpuinfo ]; then
        flags="$(awk -F: '/^flags[[:space:]]*:/{print $2; exit}' /proc/cpuinfo)"
      fi
      has_all_flags() {
        for f in "$@"; do
          printf ' %s ' "$flags" | grep -q " $f " || return 1
        done
      }
      if has_all_flags cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3 avx avx2 bmi1 bmi2 f16c fma lzcnt movbe osxsave; then
        printf 'linux-x86_64-v3\n'
      elif has_all_flags cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3; then
        printf 'linux-x86_64-v2\n'
      else
        return 1
      fi
      ;;
    aarch64|arm64) printf 'linux-arm64\n' ;;
    *) return 1 ;;
  esac
}
