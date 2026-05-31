#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-render.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-render.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

plex_artifact="$tmp/libsqlite3-plex.so"
emby_artifact="$tmp/libsqlite3-emby.so"
printf 'sqlite-plex-artifact\n' > "$plex_artifact"
printf 'sqlite-emby-artifact\n' > "$emby_artifact"
plex_artifact_sha="$(sha256sum "$plex_artifact" | awk '{print $1}')"
emby_artifact_sha="$(sha256sum "$emby_artifact" | awk '{print $1}')"
cat > "$tmp/SHA256SUMS" <<EOF_SUMS
$plex_artifact_sha  sqlite-2026.05.28-r1-library-plex-linux-x86_64-v3.so
$emby_artifact_sha  sqlite-2026.05.28-r1-library-linux-x86_64-v3.so
EOF_SUMS
cat > "$tmp/pre" <<'EOF_PRE'
pre|1|plex|linux-x86_64-v3|lscr.io/linuxserver/plex:1.42.2|sha256:image|/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|1111111111111111111111111111111111111111111111111111111111111111
pre|1|plex|linux-x86_64-v3|lscr.io/linuxserver/plex:1.42.2|sha256:image|/usr/lib/plexmediaserver/lib/libicuucplex.so.69|runtime|2222222222222222222222222222222222222222222222222222222222222222
pre|1|emby|linux-x86_64-v3|lscr.io/linuxserver/emby:version-4.9.3.0|sha256:image|/app/emby/lib/libsqlite3.so.3.49.2|runtime|3333333333333333333333333333333333333333333333333333333333333333
EOF_PRE
cat > "$tmp/pool" <<'EOF_POOL'
plex|linux-x86_64-v3|/usr/lib/plexmediaserver/Plex Media Server|4444444444444444444444444444444444444444444444444444444444444444
EOF_POOL

bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod plex \
  --release-tag 2026.05.28-r1 \
  --generated-at 2026-05-28T00:00:00Z \
  --sha256sums "$tmp/SHA256SUMS" \
  --pre-fragment "$tmp/pre" \
  --pool-baselines "$tmp/pool" \
  --artifact "linux-x86_64-v3:sqlite-2026.05.28-r1-library-plex-linux-x86_64-v3.so:$plex_artifact:/usr/lib/plexmediaserver/lib/libsqlite3.so" \
  --output "$tmp/baked-pins.txt"

grep -Fxq '# baked-pins schema=2' "$tmp/baked-pins.txt"
grep -Fxq 'version|2|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z' "$tmp/baked-pins.txt"
grep -Fq 'current|1|plex|linux-x86_64-v3|sqlite-2026.05.28-r1-library-plex-linux-x86_64-v3.so|/usr/lib/plexmediaserver/lib/libsqlite3.so|' "$tmp/baked-pins.txt"
grep -Fq 'pre|1|plex|linux-x86_64-v3|' "$tmp/baked-pins.txt"
grep -Fq 'libicuucplex.so.69' "$tmp/baked-pins.txt"
grep -Fq 'pool-pre|1|plex|linux-x86_64-v3|/usr/lib/plexmediaserver/Plex Media Server|' "$tmp/baked-pins.txt"
if grep -Fq 'pre|1|emby|' "$tmp/baked-pins.txt"; then
  echo "FATAL: plex render included emby pre rows" >&2
  exit 1
fi
if grep -Eq 'managed_window|post[|]' "$tmp/baked-pins.txt"; then
  echo "FATAL: baked pins included legacy managed-window or post rows" >&2
  exit 1
fi

bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  --release-tag 2026.05.28-r1 \
  --generated-at 2026-05-28T00:00:00Z \
  --sha256sums "$tmp/SHA256SUMS" \
  --pre-fragment "$tmp/pre" \
  --artifact "linux-x86_64-v3:sqlite-2026.05.28-r1-library-linux-x86_64-v3.so:$emby_artifact:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/baked-pins-emby.txt"

grep -Fq 'current|1|emby|linux-x86_64-v3|sqlite-2026.05.28-r1-library-linux-x86_64-v3.so|/app/emby/lib/libsqlite3.so.3.49.2|' "$tmp/baked-pins-emby.txt"
grep -Fq 'pre|1|emby|linux-x86_64-v3|' "$tmp/baked-pins-emby.txt"
if grep -Fq 'pre|1|plex|' "$tmp/baked-pins-emby.txt"; then
  echo "FATAL: emby render included plex pre rows" >&2
  exit 1
fi
if grep -Fq 'pool-pre|' "$tmp/baked-pins-emby.txt"; then
  echo "FATAL: emby render included pool baseline rows" >&2
  exit 1
fi

bash tools/lsio-mod/stage-lsio-mod.sh \
  --mod emby \
  --output-dir "$tmp/staged-emby" \
  --baked-pins "$tmp/baked-pins-emby.txt" \
  --artifact "linux-x86_64-v3:$emby_artifact" >/dev/null
test -f "$tmp/staged-emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/type"
test -f "$tmp/staged-emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-config/run"
test -f "$tmp/staged-emby/root-fs/etc/s6-overlay/s6-rc.d/init-mods-end/dependencies.d/init-mod-sqlite3-config"
test -f "$tmp/staged-emby/root-fs/etc/s6-overlay/s6-rc.d/user/contents.d/init-mod-sqlite3-config"
if [ -d "$tmp/staged-emby/root-fs/etc/cont-init.d" ]; then
  echo "FATAL: staged emby mod contains legacy cont-init.d" >&2
  exit 1
fi

bash tools/lsio-mod/stage-lsio-mod.sh \
  --mod plex \
  --output-dir "$tmp/staged-plex" \
  --baked-pins "$tmp/baked-pins.txt" \
  --artifact "linux-x86_64-v3:$plex_artifact" >/dev/null
test -f "$tmp/staged-plex/root-fs/opt/sqlite3-lsio-mod/lib/atomic-write.sh"
test -f "$tmp/staged-plex/root-fs/opt/sqlite3-lsio-mod/lib/plex-pool-patch.sh"

bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  --release-tag 2026.05.28-r1 \
  --generated-at 2026-05-28T00:00:00Z \
  --sha256sums "$tmp/SHA256SUMS" \
  --pre-fragment "$tmp/pre" \
  --artifact "linux-x86_64-v2:sqlite-2026.05.28-r1-library-linux-x86_64-v2.so:$tmp/missing-libsqlite3.so:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/baked-pins-unsupported.txt"

grep -Fxq 'unsupported|linux-x86_64-v2|missing-artifact:sqlite-2026.05.28-r1-library-linux-x86_64-v2.so' "$tmp/baked-pins-unsupported.txt"

cat > "$tmp/bad-pre" <<'EOF_BAD_PRE'
pre|1|emby|linux-x86_64-v3|lscr.io/linuxserver/emby:version-4.9.3.0|sha256:image|/app/emby/lib/libsqlite3.so.3.49.2|runtime|not-a-sha
EOF_BAD_PRE
if bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  --release-tag 2026.05.28-r1 \
  --generated-at 2026-05-28T00:00:00Z \
  --sha256sums "$tmp/SHA256SUMS" \
  --pre-fragment "$tmp/bad-pre" \
  --artifact "linux-x86_64-v3:sqlite-2026.05.28-r1-library-linux-x86_64-v3.so:$emby_artifact:/app/emby/lib/libsqlite3.so.3.49.2" \
  --output "$tmp/baked-pins-bad.txt" >"$tmp/bad-pre.out" 2>"$tmp/bad-pre.err"; then
  echo "FATAL: malformed pre row was accepted" >&2
  exit 1
fi
grep -Fq 'FATAL: invalid pre row SHA:' "$tmp/bad-pre.err"

if bash tools/lsio-mod/render-lsio-mod-baked-pins.sh --mod >"$tmp/render-dangling.out" 2>"$tmp/render-dangling.err"; then
  echo "FATAL: dangling renderer option was accepted" >&2
  exit 1
fi

if bash tools/lsio-mod/stage-lsio-mod.sh --mod >"$tmp/stage-dangling.out" 2>"$tmp/stage-dangling.err"; then
  echo "FATAL: dangling stager option was accepted" >&2
  exit 1
fi

printf 'baked pins renderer tests passed\n'
