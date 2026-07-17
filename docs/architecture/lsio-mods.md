# LSIO Mods

Part of the [Repository Architecture index](../architecture.md).

## LSIO Mod Architecture

The LSIO mods are the supported deployment surface for SQLite replacement.
There is no non-LSIO runtime replacement path in the current architecture.

Each mod image is `FROM scratch` and contains a staged `root-fs/` copied into
the LSIO container by the Docker Mods loader. Phase scripts run as native
s6-rc oneshots under `/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-*/run` with
`#!/usr/bin/with-contenv bash`.

Runtime installed files:

```text
/opt/sqlite3-lsio-mod/baked-pins.txt
/opt/sqlite3-lsio-mod/artifacts/<arch>/<compat_group>/libsqlite3.so
/opt/sqlite3-lsio-mod/lib/logging.sh
/opt/sqlite3-lsio-mod/lib/arch.sh
/opt/sqlite3-lsio-mod/lib/sha.sh
/opt/sqlite3-lsio-mod/lib/manifest-parser.sh
/opt/sqlite3-lsio-mod/lib/selector.sh
/opt/sqlite3-lsio-mod/lib/swap.sh
/opt/sqlite3-lsio-mod/lib/atomic-write.sh
```

Plex also installs:

```text
/opt/sqlite3-lsio-mod/lib/plex-patch.sh
/opt/sqlite3-lsio-mod/pins/library-compat-groups.tsv
/opt/sqlite3-lsio-mod/pins/versions.env
```

`baked-pins.txt` is the only runtime SHA source. It uses schema v3 rows:
`meta`, `detect`, `artifact`, `pre`, `pool-site`, and `unsupported`. The
manifest carries detector sets for per-version detector selection, group-aware
artifact paths, target runtime baselines, Plex ICU runtime baselines, and Plex
pool-site byte contexts.
Plex PMS detector identity includes exact pool-original + OLD, pool-patched +
OLD, and pool-patched + NEW source-id states. The final state is
`plex_pms:source-id-patched`; it remains version- and architecture-bound by its
full SHA. Its SHA is computed while rendering: bake-smoke exports the pristine
PMS from the pulled full Plex image, the renderer first verifies it against the
curated `plex_pms:pristine` whole-file SHA, then produces the final bytes through
the shared `plex_patch_populate_pms_tmp` runtime path and hashes them. The final
role is not a `pins/runtime-baselines.tsv` curation row.
The staged compatibility-group table is copied unchanged and supplies the OLD
Plex `sqlite_source_id_guard`; staged `versions.env` supplies the URL-encoded
NEW `SQLITE_SOURCE_ID`. Neither file adds a baked-pins row or runtime SHA
source.

Row-kind schema and all-arch manifest coverage are pinned in
`docs/invariants/sqlite3-builds.md` §2 and §8. Detailed schema v3 field rules,
runtime keying, and validator reject conditions live in
`docs/baked-pins-schema.md`. The architecture-local point is that this manifest
is the runtime map consumed by the staged LSIO files listed above.

Preflight validates the manifest, resolves the runtime architecture, runs
per-version detector selection, and verifies the selected `artifact` row names a
present target path. Later phases reuse the same selected server id and
artifact row; they do not select by runtime `libsqlite3.so` SHA.

The phase oneshots are registered with empty
`/etc/s6-overlay/s6-rc.d/user/contents.d/init-mod-sqlite3-*` markers and are
chained with dependency marker files:

```text
init-mods
  -> init-mod-sqlite3-preflight
  -> init-mod-sqlite3-verify
  -> init-mod-sqlite3-swap
  -> init-mod-sqlite3-config        (Emby)
  -> init-mods-end

init-mods
  -> init-mod-sqlite3-preflight
  -> init-mod-sqlite3-verify
  -> init-mod-sqlite3-swap
  -> init-mod-sqlite3-plexpatch     (Plex)
  -> init-mods-end
```

Because `init-mods-end` precedes `init-services` in the LSIO s6-overlay v3
graph, the SQLite replacement chain completes before `svc-emby` or `svc-plex`
starts.

Phase order and failure posture are pinned in
`docs/invariants/sqlite3-builds.md` §2. The repo-map context here is that the
native s6-rc chain above is how the Plex and Emby mod roots enter that invariant
phase sequence.

The mod preserves one `.bundled.bak` beside every mutated target. It never
overwrites or deletes that backup.

Phase 03 swap behavior is pinned in `docs/invariants/sqlite3-builds.md` §2.
This repository map retains the file placement and target context; the invariant
doc owns the row-by-row classification rules.

Plex ICU runtime files are read-only inputs to LSIO mod code. The Plex mod
checks `libicuucplex.so.69`, `libicui18nplex.so.69`, and
`libicudataplex.so.69` against `pre` rows before SQLite replacement. It must
not replace, rename, move, delete, or overwrite those files.

Phase 04 Plex patch runs only in the Plex mod. On amd64 and arm64, the phase
first verifies that the SQLite target is already current, then calls the staged
`plex-patch.sh` fragment with all paths, expected SHAs, patch sites, and source
ids as arguments. The phase reads OLD from the Plex row and
`sqlite_source_id_guard` column of the staged unchanged compatibility-group
table. It reads NEW from the staged canonical `SQLITE_SOURCE_ID` pin and
decodes `%20` to spaces. The OLD lookup requires exactly one Plex row and an
84-byte guard, and NEW requires the exact encoded source-id shape; failure
warn-skips the phase. The fragment reads no environment variables and carries
no SHA constants.

Pool-patch site tuples are
`label|offset|write_seek|original_hex|patched_hex`. Each tuple binds the site
label, the 16-byte read offset, the byte write offset, and the full original
and patched contexts for one binary. The writer derives the single byte to
write from `patched_hex` at `write_seek - offset`, writes it to a same-fs temp
copy with `dd conv=notrunc`, restores the original owner and mode on the temp,
and atomically replaces the target. Purpose: every listed site changes the
ConnectionPool size immediate from 20 to 16. On amd64, `original_hex` starts
`be14...` and `patched_hex` starts `be10...`; on arm64, `original_hex` starts
`81028052...` and `patched_hex` starts `01028052...`.

Supported pool-patch sites are data rows, not runtime-derived offsets. The
curated source rows live in `pins/plex-patch-pool-sites.tsv`; rendered runtime
rows live as `pool-site` rows in `baked-pins.txt`.

Pool-only Scanner behavior:

| Site state | Baseline SHA / backup state | Action |
|---|---|---|
| All sites patched | Any | Skip; already patched |
| All sites original | Current binary SHA matches selected pristine detector SHA; backup absent | Copy backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| All sites original | Current binary SHA matches selected pristine detector SHA; verified backup present | Reuse backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| Mixed or unknown | Any | Warn and skip that binary |
| Write, verify, or atomic replace failure | Verified backup present | Leave target unchanged, warn, and continue to the next binary |

For PMS, the phase folds the pool-site and 84-byte source-id writes into one
temp-copy operation and one atomic replace. It accepts pristine pool bytes with
the pristine detector SHA, or pool-patched bytes with the patched detector SHA
and the existing verified pristine backup. The all-post NEW-id state is
idempotent only when the current whole-file SHA matches the computed
`plex_pms:source-id-patched` detector SHA. Any other state with a NEW-id match
fails as an unknown, conflicting, or SHA-mismatched source-id state before OLD
matching. Only the no-NEW branch requires exactly one OLD-id match; zero or
multiple OLD matches fail and leave PMS unchanged. The source-id write is
length-preserving and read back from the temp copy, and the temp copy must match
the computed source-id-patched SHA before owner/mode restoration and atomic
replacement. The scanner retains the pool-only behavior above. The PMS
pool-site and source-id writes use one `${PMS}.bundled.bak`. Later phase starts
select the exact committed PMS state through `plex_pms:source-id-patched`,
including when a prior Scanner write failed after PMS succeeded.

PMS patch warnings return nonzero. Phase 04 still attempts the independent
Scanner patch, then logs `event=failed step=pms-patch` and exits nonzero when
the PMS operation failed. `event=complete` certifies that the required PMS
operation either published the exact computed final bytes or verified those
bytes were already present; Scanner outcome remains in the separate
`pool_patch event=*` per-site namespace.

Runtime command surface:

| Scope | Commands |
|---|---|
| Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |
| Plex patch | `dd od printf` |

LSIO runtime has no dependency on `curl`, `tar`, `gunzip`, Python, `jq`,
package managers, or network access.
