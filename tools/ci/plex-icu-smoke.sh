#!/usr/bin/env bash
set -euo pipefail

plex_img=$1
arch_suffix=$2
icu_version=$3
artifact_dir=$4

: "Plex ICU smoke arch ${arch_suffix}"
# WHY: The Plex image pin fixes the ICU runtime ABI checked below.
EXPECTED_ICU_MAJOR="${icu_version%%.*}"
ARTIFACT_DIR_ABS="$(realpath "$artifact_dir")"
PLEX_ICU_SOS=$(docker run --rm --entrypoint /bin/ls "$plex_img" \
  /usr/lib/plexmediaserver/lib/ \
  | grep '^libicu.*plex\.so\.[0-9]\+$' || true)
if [ -z "$PLEX_ICU_SOS" ]; then
  echo "FATAL: no Plex-renamed ICU SOs found in $plex_img"; exit 1
fi
PLEX_ICU_MAJOR_OFFENDERS=()
while IFS= read -r so; do
  so_major="$(printf "%s\n" "$so" | sed -n 's/.*\.so\.\([0-9]\+\)$/\1/p')"
  if [ "$so_major" != "$EXPECTED_ICU_MAJOR" ]; then
    PLEX_ICU_MAJOR_OFFENDERS+=("$so=$so_major")
  fi
done <<< "$PLEX_ICU_SOS"
if [ "${#PLEX_ICU_MAJOR_OFFENDERS[@]}" -gt 0 ]; then
  PLEX_ICU_MAJOR="${PLEX_ICU_MAJOR_OFFENDERS[*]}"
  echo "FATAL: Plex image $plex_img bundles ICU $PLEX_ICU_MAJOR;"
  echo "       build pinned to ICU $EXPECTED_ICU_MAJOR."
  echo "       Refresh the ICU pin, Plex build steps, and EXPECTED_ICU_MAJOR before re-running CI."
  exit 1
fi
if [ ! -f "$ARTIFACT_DIR_ABS/libsqlite3.so" ]; then
  echo "FATAL: missing extracted Plex library at $ARTIFACT_DIR_ABS/libsqlite3.so"
  exit 1
fi
if [ ! -f "$ARTIFACT_DIR_ABS/icu_smoke" ]; then
  echo "FATAL: missing extracted Plex ICU smoke binary at $ARTIFACT_DIR_ABS/icu_smoke"
  exit 1
fi

# WHY: Invoking Plex's bundled musl loader directly proves the smoke
# traverses the runtime closure used by the deployed Plex library.
docker run --rm \
  --mount "type=bind,src=${ARTIFACT_DIR_ABS}/libsqlite3.so,dst=/usr/lib/plexmediaserver/lib/libsqlite3.so,readonly" \
  --mount "type=bind,src=${ARTIFACT_DIR_ABS}/icu_smoke,dst=/tmp/icu_smoke,readonly" \
  --entrypoint /bin/bash \
  "$plex_img" \
  -c 'set -euo pipefail
      shopt -s nullglob
      plex_loaders=(/usr/lib/plexmediaserver/lib/ld-musl-*.so.1)
      if [ "${#plex_loaders[@]}" -eq 0 ]; then
        echo "FATAL: no Plex musl loader found"
        exit 1
      fi
      plex_loader="${plex_loaders[0]}"
      printf "Using Plex musl loader: %s\n" "$plex_loader"
      smoke_list="$("$plex_loader" --library-path /usr/lib/plexmediaserver/lib --list /tmp/icu_smoke)"
      printf "%s\n" "$smoke_list"
      printf "%s\n" "$smoke_list" | awk '"'"'/libsqlite3\.so/ && /\/usr\/lib\/plexmediaserver\/lib\/libsqlite3\.so/ { found=1 } END { exit !found }'"'"' || {
        echo "FATAL: icu_smoke did not resolve libsqlite3.so through /usr/lib/plexmediaserver/lib/"
        exit 1
      }
      for soname in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
        printf "%s\n" "$smoke_list" | awk -v s="$soname" '"'"'$0 ~ s && $0 ~ "/usr/lib/plexmediaserver/lib/" s { found=1 } END { exit !found }'"'"' || {
          echo "FATAL: icu_smoke did not resolve $soname through /usr/lib/plexmediaserver/lib/"
          exit 1
        }
      done
      "$plex_loader" --library-path /usr/lib/plexmediaserver/lib /tmp/icu_smoke' || {
  echo "FATAL: Plex ICU smoke failed; dumping musl loader lists"
  docker run --rm \
    --mount "type=bind,src=${ARTIFACT_DIR_ABS}/libsqlite3.so,dst=/usr/lib/plexmediaserver/lib/libsqlite3.so,readonly" \
    --mount "type=bind,src=${ARTIFACT_DIR_ABS}/icu_smoke,dst=/tmp/icu_smoke,readonly" \
    --entrypoint /bin/bash \
    "$plex_img" \
    -c 'set -euo pipefail
        shopt -s nullglob
        plex_loaders=(/usr/lib/plexmediaserver/lib/ld-musl-*.so.1)
        if [ "${#plex_loaders[@]}" -eq 0 ]; then
          echo "FATAL: no Plex musl loader found"
          exit 1
        fi
        plex_loader="${plex_loaders[0]}"
        printf "Using Plex musl loader: %s\n" "$plex_loader"
        "$plex_loader" --library-path /usr/lib/plexmediaserver/lib --list /usr/lib/plexmediaserver/lib/libsqlite3.so || true
        "$plex_loader" --library-path /usr/lib/plexmediaserver/lib --list /tmp/icu_smoke || true' || true
  exit 1
}
