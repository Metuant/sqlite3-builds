#!/usr/bin/env sh
set -eu
for tool in dd od tr printf; do
  command -v "${tool}" >/dev/null 2>&1 || { echo "FATAL: missing required tool ${tool}" >&2; exit 1; }
done
patch_site() {
  bin=$1
  offset_label=$2
  offset_dec=$3
  write_seek_dec=$4
  original_hex=$5
  patched_hex=$6
  if [ ! -f "${bin}" ]; then
    echo "FATAL: missing ${bin}" >&2
    exit 1
  fi
  observed_hex="$(
    dd if="${bin}" bs=1 skip="${offset_dec}" count=16 2>/dev/null |
      od -An -tx1 -v |
      tr -d ' \n'
  )"
  if [ "${observed_hex}" = "${patched_hex}" ]; then
    echo "already patched: ${bin} @ ${offset_label}"
    return 0
  fi
  if [ "${observed_hex}" != "${original_hex}" ]; then
    echo "UNEXPECTED bytes at ${bin} @ ${offset_label}: ${observed_hex} (expected ${original_hex})" >&2
    exit 1
  fi
  if ! printf '\020' | dd of="${bin}" bs=1 seek="${write_seek_dec}" count=1 conv=notrunc 2>/dev/null; then
    echo "FATAL: dd write failed at ${bin} @ ${offset_label}" >&2
    exit 1
  fi
  echo "patched: ${bin} @ ${offset_label} 20 -> 16"
}
patch_site \
  "/usr/lib/plexmediaserver/Plex Media Server" \
  "0xae4a17" \
  "11422231" \
  "11422232" \
  "be140000004c89ff41b800000000506a" \
  "be100000004c89ff41b800000000506a"
patch_site \
  "/usr/lib/plexmediaserver/Plex Media Scanner" \
  "0x36f06d" \
  "3600493" \
  "3600494" \
  "be140000004c89ff41b800000000506a" \
  "be100000004c89ff41b800000000506a"
patch_site \
  "/usr/lib/plexmediaserver/Plex Media Scanner" \
  "0x38b37b" \
  "3715963" \
  "3715964" \
  "be140000004889df41b80000000068d0" \
  "be100000004889df41b80000000068d0"
