#!/usr/bin/env bash
set -euo pipefail

grep -Fq '## LSIO Mod Architecture' docs/architecture.md
grep -Fq '## 2. LSIO mods' docs/invariants/sqlite3-builds.md
grep -Fq 'baked-pins.txt is the only runtime SHA source' docs/invariants/sqlite3-builds.md
grep -Fq 'init-mod-sqlite3-preflight' docs/architecture.md
grep -Fq 'before `init-services` and `svc-*` startup' docs/invariants/sqlite3-builds.md
grep -Fq '## ARM64 PMS pool-patch re-derivation' docs/deferred.md

for forbidden in \
  'SQLITE_LIBRARY_PINS' \
  'tools/regen-deploy-pins.sh' \
  'tools/assemble-library-pins.sh' \
  'scripts/update-sqlite-library.sh.template' \
  'scripts/lib/deploy-assertion.sh' \
  '## LSIO Docker mod for libsqlite3.so swap + Plex pool patch'
do
  if grep -Fq "$forbidden" docs/architecture.md docs/invariants/sqlite3-builds.md docs/deferred.md; then
    echo "FATAL: stale documentation reference remains: $forbidden" >&2
    exit 1
  fi
done

printf 'documentation mod-only checks passed\n'
