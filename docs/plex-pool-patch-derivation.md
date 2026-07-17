# Plex Pool-Patch Derivation

This document records the current derivation method and site data for the Plex
ConnectionPool pool-size patch. The runtime patch lowers each supported
ConnectionPool size immediate from 20 to 16 in `Plex Media Server` and
`Plex Media Scanner`.

## Method

Derivation starts from the RTTI name for
`__shared_ptr_emplace<ConnectionPool>`. The typeinfo reference leads to the
ConnectionPool vtable, the setup helper, and then the caller that passes the
pool size into the constructor path. The patch site is the caller-side load of
the pool-size immediate:

- amd64: `mov esi,#20`, patched to `mov esi,#16`.
- arm64: `movz w1,#20`, patched to `movz w1,#16`.

The file offset is derived from the runtime virtual address by subtracting the
binary's load delta:

| Arch | VA to file-offset delta |
|---|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `0x1000` |
| `linux-arm64` | `0x10000` |

Every patch tuple uses:

```text
label|offset|write_seek|original_hex|patched_hex
```

`offset` and `write_seek` are decimal file offsets. `label` is the file offset
in hex. `original_hex` and `patched_hex` are 16-byte contexts. The context must
be unique in the target binary.

## Supported Surface

The supported Plex pool-patch surface is keyed by `server_id` rows in
`pins/runtime-support.tsv`.

| Server ID | Image ref | Compat group |
|---|---|---|
| `plex-1.43.1` | `ghcr.io/linuxserver/plex:1.43.1` | `icu69` |
| `plex-1.43.2` | `ghcr.io/linuxserver/plex:1.43.2` | `icu69` |

For each supported `server_id` and arch, the curated Plex detector trust-anchor
SHAs live in `pins/runtime-baselines.tsv`. PMS has three exact runtime states:
`plex_pms:pristine` is pool-original with OLD source id, `plex_pms:patched` is
pool-patched with OLD source id, and `plex_pms:source-id-patched` is
pool-patched with NEW source id. Scanner has `plex_scanner:pristine` and
`plex_scanner:patched`. The first, second, and both Scanner states are curated;
the final PMS state is computed during manifest rendering from verified
pristine bytes, the curated pool sites, and the canonical NEW source-id pin. The
runtime Plex patch phase selects the server first, then passes the selected
pristine and pool-only PMS detector SHAs to the patcher. The computed final PMS
detector SHA participates in later exact selection but is not a patcher input.

## Patch Sites

Patch sites are server-scoped data rows in
`pins/plex-patch-pool-sites.tsv`:

```text
server_id arch binary_path baseline_sha256 label offset write_seek original_hex patched_hex
```

`baseline_sha256` must match the selected pristine Plex detector SHA for the
same `server_id`, arch, and binary path. The renderer and CI alignment tests
enforce that binding before a `pool-site` row is emitted into `baked-pins.txt`.

Human review approvals are server-scoped rows in
`pins/plex-pool-patch-reviews.tsv`:

```text
server_id arch binary_path label offset write_seek original_hex patched_hex review_ref reviewer status
```

A review row binds the full site tuple. Any change to `server_id`, `arch`,
`binary_path`, `label`, `offset`, `write_seek`, `original_hex`, or
`patched_hex` creates a different tuple and requires fresh approval.

## Exclusions

These pool-size-2 sites are for a different database and must not be patched:

| Arch | Binary | File offset | VA |
|---|---|---:|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Server` | `0xaf660b` | `0xaf760b` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x372be9` | `0x373be9` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Server` | `0x9f7868` | `0xa07868` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x32f6d0` | `0x33f6d0` |

## Runtime Mechanism

The Phase 04 Plex patch oneshot passes each target binary path, its selected
pristine detector SHA (`detect ...:pristine`), and one or more site tuples to
the runtime patcher. The PMS call additionally receives its selected patched
detector SHA (`detect ... plex_pms:patched`) as the expected pool-patched SHA, plus
the 84-byte OLD and NEW SQLite source ids. OLD comes from the staged
`sqlite_source_id_guard` column in `pins/library-compat-groups.tsv`; NEW comes
from the staged canonical `SQLITE_SOURCE_ID` in `pins/versions.env`, with its
`%20` sequences decoded to spaces.

The patcher reads exactly 16 bytes at each tuple offset. A pristine binary is
patchable only when every site matches `original_hex` and the current binary
SHA matches the selected pristine detector SHA. Scanner is pool-only: if every
site already matches `patched_hex`, it logs `pool_patch
event=already-patched` and skips the binary.

For each original site, the patcher computes `write_index` as
`write_seek - offset`, selects the one output byte from `patched_hex`, writes
that byte to a same-filesystem temporary copy with `dd conv=notrunc`, and
verifies the 16-byte context. It preserves the `.bundled.bak` backup, restores
owner and mode metadata on the temp copy, and atomically replaces the target.

Both patched detector SHAs are derived by applying only the pool-site tuples.
`plex_pms:patched` is the pool-patched PMS SHA while the source id is still OLD;
`plex_scanner:patched` is likewise pool-only.

The exact pool-patched plus NEW-source-id PMS state is derived during manifest
rendering as `plex_pms:source-id-patched` through the shared runtime
`plex_patch_populate_pms_tmp` function. The renderer accepts the result only
after the extracted pristine input matches the curated pristine SHA. Runtime
folds the source-id edit with the pool-site writes. The patcher checks the all-post,
exactly-one-NEW, zero-OLD state first for idempotence. Otherwise any NEW match
warn-skips, and the no-NEW branch locates OLD with `grep -aobF` and requires
exactly one match. An all-original input must match `plex_pms:pristine`; an
all-patched input must match `plex_pms:patched` and have a verified pristine
backup. One `sqlite3_mod_atomic_replace` call then populates a same-filesystem
temporary copy, applies or re-verifies every pool-site byte, writes and reads
back the 84-byte NEW id, restores owner and mode, and replaces PMS once. The
resulting binary must match the exact computed `plex_pms:source-id-patched`
detector SHA on later selection.

PMS state transitions are closed:

| Input state | Successful transition | Failure or rejection |
|---|---|---|
| Pool-pre + OLD | Pool-post + NEW | Input remains unchanged |
| Pool-post + OLD with verified pristine backup | Pool-post + NEW | Input remains unchanged |
| Pool-post + NEW | No write; already patched | State remains unchanged |
| Pool-pre + NEW, mixed/unknown pool bytes, conflicting source ids, or unpinned SHA | None | State remains unchanged |

The temporary copy prevents a partial pool/source-id state from being committed.
If PMS reaches pool-patched + NEW but Scanner later fails, the next start remains
selectable through `plex_pms:source-id-patched` plus the Scanner pristine role;
the phase can keep PMS unchanged and retry Scanner.

Unknown, mixed, mismatched, or write-failed states warn and skip without
modifying the runtime target.

## See also

[Runtime Baseline Derivation](runtime-baseline-derivation.md)
