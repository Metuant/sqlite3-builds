#!/usr/bin/env bash
set -euo pipefail

workflow=".github/workflows/sqlite-build.yml"

for forbidden in \
  SQLITE_LIBRARY_PINS \
  update-sqlite-library.sh \
  regen-deploy-pins \
  assemble-library-pins \
  cont-init-patch-plex-pool \
  sqlite-pins-pre \
  "PMS first-init smoke (Plex, pool-patched)" \
  "Fetch prior library pins" \
  "Regenerate deploy script pins" \
  "Assemble deploy library pins" \
  "DOCKER_MODS"
do
  if grep -Fq "$forbidden" "$workflow"; then
    echo "FATAL: forbidden workflow deploy surface remains: $forbidden" >&2
    exit 1
  fi
done

grep -Eq '^[[:space:]]+release:' "$workflow"
grep -Eq '^[[:space:]]+mod-build:' "$workflow"
grep -Eq '^[[:space:]]+mod-publish:' "$workflow"
grep -Fq 'ubuntu-24.04-arm' "$workflow"
grep -Fq 'linuxserver-mod-sqlite3-plex' "$workflow"
grep -Fq 'linuxserver-mod-sqlite3-emby' "$workflow"
grep -Fq 'tools/render-lsio-mod-baked-pins.sh' "$workflow"
grep -Fq 'tools/stage-lsio-mod.sh' "$workflow"
grep -Fq 'COPY root-fs /' "$workflow"
grep -Fq '${{ github.run_id }}-${{ matrix.mod }}-${{ matrix.arch_suffix }}-smoke' "$workflow"
grep -Fq 'docker save "$mod_ref"' "$workflow"
grep -Fq 'mod-image-${{ matrix.mod }}-${{ matrix.arch_suffix }}' "$workflow"

if grep -Fq ':latest' "$workflow"; then
  echo "FATAL: workflow publishes latest tag" >&2
  exit 1
fi
if grep -Fq -- '--platform' "$workflow"; then
  echo "FATAL: workflow uses docker --platform instead of native runners" >&2
  exit 1
fi

python3 - <<'PY'
import re
from pathlib import Path

text = Path(".github/workflows/sqlite-build.yml").read_text()
release = text.split("\n  release:", 1)[1].split("\n  mod-build:", 1)[0]
mod_build = text.split("\n  mod-build:", 1)[1].split("\n  mod-publish:", 1)[0]
mod_publish = text.split("\n  mod-publish:", 1)[1]

if "needs: build" not in mod_build:
    raise SystemExit("mod-build job missing needs: build")
if "needs: release" in mod_build:
    raise SystemExit("mod-build job still depends on release")
if "startsWith(github.ref, 'refs/tags/')" in mod_build:
    raise SystemExit("mod-build job is still tag-gated")
if "actions/upload-artifact@" not in mod_build:
    raise SystemExit("mod-build job missing upload-artifact step")
if "${{ matrix.platform }}" in mod_build:
    raise SystemExit("mod-build still uses matrix.platform in image refs")
if "DOCKER_MODS" in mod_build:
    raise SystemExit("mod-build still uses registry-fetch smoke")
if "printf 'FROM %s\\nCOPY root-fs /\\n' \"$lsio_image\"" not in mod_build:
    raise SystemExit("mod-build missing bake-in smoke Dockerfile")
if 'docker build --rm -t "$mod_ref" "$staged"' not in mod_build:
    raise SystemExit("mod-build missing real scratch image build")
if "docker save \"$mod_ref\"" not in mod_build:
    raise SystemExit("mod-build missing real image export")
if "docker run --rm --entrypoint sha256sum" not in mod_build:
    raise SystemExit("mod-build missing runtime pre-SHA derivation")

needs_match = re.search(r'(?ms)^\s+needs:\s*(\[[^\n]+\]|(?:\n(?:\s+- .+\n?)+))', mod_publish)
if not needs_match:
    raise SystemExit("mod-publish job missing needs block")
needs_block = needs_match.group(1)
if "release" not in needs_block or "mod-build" not in needs_block:
    raise SystemExit("mod-publish job missing release/mod-build dependency gate")
if "startsWith(github.ref, 'refs/tags/')" not in mod_publish:
    raise SystemExit("mod-publish job missing tag gate")
if "actions/download-artifact@" not in mod_publish:
    raise SystemExit("mod-publish job missing download-artifact step")
if "docker build" in mod_publish or "docker run" in mod_publish:
    raise SystemExit("mod-publish rebuilds or re-smokes images")

if "SHA256SUMS" not in release:
    raise SystemExit("release job missing SHA256SUMS")
if "icu-${{ github.ref_name }}-" in release:
    raise SystemExit("release job still publishes ICU tarball")
for forbidden in ["SQLITE_LIBRARY_PINS", "release-assets/update-sqlite-library.sh"]:
    if forbidden in release:
        raise SystemExit(f"release job contains forbidden asset {forbidden}")
print("workflow mod-only static checks passed")
PY
