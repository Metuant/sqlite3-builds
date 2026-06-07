This file tracks deferred workstreams that the repository has explicitly
parked, with enough context to resume them without re-discovery.
Add entries when work is deferred, and remove them when the deferred item is
complete.

## ARM64 PMS pool-patch re-derivation

### Status

The Plex LSIO mod currently swaps SQLite on arm64 but does not patch PMS or
Plex Media Scanner pool-size sites there. The active Phase 04 entrypoint,
`lsio-mods/plex/root-fs/etc/cont-init.d/83-sqlite3-mod-pool-patch`, logs
`event=pool-patch-deferred arch=linux-arm64` and exits 0 for `aarch64` and
`arm64`.

The amd64 patch sites live in `lib/plex-pool-patch.sh` as caller-provided site
tuples from `83-sqlite3-mod-pool-patch`. The current baseline SHA source is
`pins/plex-pool-patch-baselines.txt`; the active Plex image tag is tracked as
`PLEX_IMAGE_TAG` in `pins/versions.env`.

### Why deferred

The LSIO Plex image tracked by `PLEX_IMAGE_TAG` in `pins/versions.env` is a
multi-arch image, so the arm64 runner pulls arm64 PMS binaries. The current
amd64 sites were derived from x86_64 instruction contexts where the pool-size
immediate appears as `mov esi, 0x14`. ARM64 encodes the same constant
differently, so arm64 requires fresh binary extraction and site derivation.

The pre-staged `plex.binaries/` artifacts at `.claude.local/plex.binaries/`
are amd64-only. Do not reuse them for arm64.

### Prerequisites to resume

1. Extract arm64 `Plex Media Server` and `Plex Media Scanner` binaries from the
   arm64 manifest for `lscr.io/linuxserver/plex:${PLEX_IMAGE_TAG}`, using the
   `PLEX_IMAGE_TAG` value from `pins/versions.env`, then copy the binaries from
   a container.
2. Adapt the EMBY-PLEX-MEMORY-INVESTIGATION methodology from section 9:
   RTTI string -> typeinfo struct -> vtable refs -> caller's
   connection-pool-size immediate-load site. The x86_64 `mov esi, 0x14` site
   maps to an ARM64 immediate-load form, typically `mov w*, #0x14` or
   `movz w*, #20`. Disassemble with aarch64-aware tooling such as
   `objdump -d --disassembler-options=force-thumb=off` or
   `llvm-objdump -d --triple=aarch64-linux-gnu`.
3. Identify the equivalent patch sites in the arm64 PMS binaries. The arm64
   site count may differ from the three amd64 sites.
4. Derive each arm64 site tuple in the current format:
   `label|offset|write_seek|original_hex|patched_hex`. Verify each original
   16-byte context is unique within its binary.
5. Add arm64 baseline rows to `pins/plex-pool-patch-baselines.txt`.
6. Update `83-sqlite3-mod-pool-patch` to route arm64 to the new site tuples
   while keeping all paths, SHAs, and sites passed as arguments into
   `lib/plex-pool-patch.sh`.
7. Keep `mod-build` bake-in smoke as the validation surface. The Plex arm64
   lane must stop emitting `event=pool-patch-deferred`.

### Acceptance criteria

- CI shows the Plex `mod-build` arm64 lane succeeding without
  `event=pool-patch-deferred`.
- The Plex arm64 lane emits `event=patched` or `event=already-patched` for
  every arm64 site in both PMS binaries.
- The x86_64 lanes show no regression: SQLite swap, pool patch, and bake-in
  smoke still pass for `linux-x86_64-v2` and `linux-x86_64-v3`.
- `pins/plex-pool-patch-baselines.txt` contains the arm64 PMS and Scanner
  baseline rows used by the rendered `baked-pins.txt`.
- This ARM64 entry is removed from `docs/deferred.md` after the item is
  resolved.

### Related paths

- `lsio-mods/plex/root-fs/etc/cont-init.d/83-sqlite3-mod-pool-patch` -- Phase
  04 entrypoint and current arm64 deferred log.
- `lib/plex-pool-patch.sh` -- args-only shared pool-patch core.
- `pins/plex-pool-patch-baselines.txt` -- PMS and Plex Media Scanner baseline
  SHA source rows rendered into `baked-pins.txt`.
- `.github/workflows/sqlite-build.yml` -- `mod-build` bake-in smoke for Plex
  amd64 and arm64 lanes.
- External methodology source:
  `/Users/darthshadow/Personal/server-configs/docs/tuning/EMBY-PLEX-MEMORY-INVESTIGATION.md`
  sections 9-10.
