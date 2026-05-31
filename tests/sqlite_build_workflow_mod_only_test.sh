#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

workflow=".github/workflows/sqlite-build.yml"
mod_bake_script="tools/ci/mod-bake-smoke.sh"

for forbidden in \
  SQLITE_LIBRARY_PINS \
  update-sqlite-library.sh \
  regen-deploy-pins \
  assemble-library-pins \
  cont-init-patch-plex-pool \
  /etc/cont-init.d \
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
grep -Fq 'tools/lsio-mod/render-lsio-mod-baked-pins.sh' "$mod_bake_script"
grep -Fq 'tools/lsio-mod/stage-lsio-mod.sh' "$mod_bake_script"
grep -Fq 'bash tools/ci/plex-icu-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/plex-pms-first-init-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/plex-pms-killswitch-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/emby-first-init-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/slow-query-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/emby-killswitch-smoke.sh' "$workflow"
grep -Fq 'bash tools/ci/mod-bake-smoke.sh' "$workflow"
grep -Fq 'bash tests/ci_log_assertions_test.sh' "$workflow"
grep -Fq 'bash tests/mod_bake_assertions_test.sh' "$workflow"
grep -Fq 'COPY root-fs /' "$mod_bake_script"
grep -Fq 'root-fs/etc/s6-overlay/s6-rc.d' "$mod_bake_script"
grep -Fq '${GITHUB_RUN_ID}-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}-smoke' "$mod_bake_script"
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
mod_bake_script = Path("tools/ci/mod-bake-smoke.sh").read_text()
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
if "cont-init" in mod_build:
    raise SystemExit("mod-build still references legacy cont-init")
if 'bash tools/ci/mod-bake-smoke.sh' not in mod_build:
    raise SystemExit("mod-build missing extracted bake-smoke invocation")
if "printf 'FROM %s\\nCOPY root-fs /\\n' \"$lsio_image\"" not in mod_bake_script:
    raise SystemExit("mod-bake script missing bake-in smoke Dockerfile")
if 'docker build --rm -t "$mod_ref" "$staged"' not in mod_bake_script:
    raise SystemExit("mod-bake script missing real scratch image build")
if "docker save \"$mod_ref\"" not in mod_bake_script:
    raise SystemExit("mod-bake script missing real image export")
if "docker run --rm --entrypoint sha256sum" not in mod_bake_script:
    raise SystemExit("mod-bake script missing runtime pre-SHA derivation")
if "assert_runtime_load" not in mod_bake_script:
    raise SystemExit("mod-bake script missing app runtime-load assertion")

build = text.split("\n  build:", 1)[1].split("\n  mod-static-tests:", 1)[0]
for script in [
    "tools/ci/plex-icu-smoke.sh",
    "tools/ci/plex-pms-first-init-smoke.sh",
    "tools/ci/plex-pms-killswitch-smoke.sh",
    "tools/ci/emby-first-init-smoke.sh",
    "tools/ci/slow-query-smoke.sh",
    "tools/ci/emby-killswitch-smoke.sh",
]:
    if f"bash {script}" not in build:
        raise SystemExit(f"build job missing extracted script invocation: {script}")

static_tests = text.split("\n  mod-static-tests:", 1)[1].split("\n  release:", 1)[0]
if "bash tests/ci_log_assertions_test.sh" not in static_tests:
    raise SystemExit("mod-static-tests missing ci log assertions test")
if "bash tests/mod_bake_assertions_test.sh" not in static_tests:
    raise SystemExit("mod-static-tests missing mod bake assertions test")

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
