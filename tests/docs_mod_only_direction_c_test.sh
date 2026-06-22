#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

grep -Fq '## LSIO Mod Architecture' docs/architecture.md
grep -Fq '## 2. LSIO mods' docs/invariants/sqlite3-builds.md
grep -Fq 'baked-pins.txt is the only runtime SHA source' docs/invariants/sqlite3-builds.md
grep -Fq 'meta|3|release_tag|<tag>|generated_at|<iso8601-utc>' docs/invariants/sqlite3-builds.md
grep -Fq 'detect|1|<mod>|<server_id>|<arch>|<path_role>|<file_path>|<sha256>' docs/invariants/sqlite3-builds.md
grep -Fq 'artifact|1|<mod>|<server_id>|<arch>|<compat_group>|<artifact_relpath>|<target_path>|<artifact_sha256>' docs/invariants/sqlite3-builds.md
grep -Fq 'pre|2|<mod>|<server_id>|<arch>|<path_role>|<image_ref>|<image_digest>|<file_path>|<sha256>' docs/invariants/sqlite3-builds.md
grep -Fq 'pool-site|1|plex|<server_id>|<arch>|<binary_path>|<label>|<offset>|<write_seek>|<original_hex>|<patched_hex>' docs/invariants/sqlite3-builds.md
grep -Fq 'unsupported|1|<mod>|<server_id>|<arch>|<compat_group>|<reason>' docs/invariants/sqlite3-builds.md
grep -Fq 'ICU source VERSION/SHA512 fields live in the `icu69` row of' docs/invariants/sqlite3-builds.md
grep -Fq '`pins/library-compat-groups.tsv`. The wrapper resolves them into' docs/invariants/sqlite3-builds.md
grep -Fq '`ICU_SOURCE_VERSION` and `ICU_SOURCE_SHA512`' docs/invariants/sqlite3-builds.md
grep -Fq '/opt/sqlite3-lsio-mod/artifacts/<arch>/<compat_group>/libsqlite3.so' docs/architecture.md
grep -Fq 'per-version detector selection' docs/architecture.md
grep -Fq 'init-mod-sqlite3-preflight' docs/architecture.md
grep -Fq 'before `init-services` and `svc-*` startup' docs/invariants/sqlite3-builds.md
grep -Fq '# Plex Pool-Patch Derivation' docs/plex-pool-patch-derivation.md
if grep -Fq '## ARM64 PMS pool-patch re-derivation' docs/deferred.md; then
  echo "FATAL: closed ARM64 pool-patch deferred entry remains" >&2
  exit 1
fi

for forbidden in \
  'SQLITE_LIBRARY_PINS' \
  'tools/regen-deploy-pins.sh' \
  'tools/assemble-library-pins.sh' \
  'scripts/update-sqlite-library.sh.template' \
  'scripts/lib/deploy-assertion.sh' \
  'version|2|release_tag|<tag>|generated_at|<iso8601-utc>' \
  'current|1|' \
  'pre|1|' \
  'pool-pre' \
  'PLEX_IMAGE_TAG' \
  'EMBY_IMAGE_TAG' \
  '## LSIO Docker mod for libsqlite3.so swap + Plex pool patch'
do
  if grep -Fq "$forbidden" docs/architecture.md docs/invariants/sqlite3-builds.md docs/deferred.md; then
    echo "FATAL: stale documentation reference remains: $forbidden" >&2
    exit 1
  fi
done

printf 'documentation mod-only checks passed\n'
