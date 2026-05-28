This file tracks deferred workstreams that the repository has explicitly
parked, with enough context to resume them without re-discovery.
Add entries when work is deferred, and remove them when the deferred item is
complete.

## ARM64 PMS pool-patch re-derivation

### Status

The pool-patched smoke lane in `.github/workflows/sqlite-build.yml` is
currently gated to non-arm64 matrix entries with
`if: matrix.arch_suffix != 'linux-arm64'` on the `Pull Plex image (PMS
pool-patched smoke)` and `PMS first-init smoke (Plex, pool-patched)` steps.
The arm64 row still runs the unpatched `PMS first-init smoke (Plex)` step.
`tools/cont-init-patch-plex-pool.sh` currently contains three amd64-specific
patch sites, and no arm64 patch sites exist yet.

### Why deferred

`lscr.io/linuxserver/plex:1.42.2` is a multi-arch image, so the arm64 runner
pulls the arm64 PMS binary.
The patch sites and 16-byte original-byte context in
`tools/cont-init-patch-plex-pool.sh` were derived from the amd64 PMS binary,
where the pool-size immediate is in x86_64 `mov esi, 0x14` instruction
context; ARM64 encodes the same constant differently.
The pre-staged `plex.binaries/` artifacts at `.claude.local/plex.binaries/`
are amd64-only, so arm64 re-derivation requires fresh extraction from the
`linuxserver/plex:1.42.2` arm64 manifest.

### Prerequisites to resume

1. Extract arm64 PMS and Plex Media Scanner binaries from the `linuxserver/plex:1.42.2` image's arm64 manifest with `docker pull --platform=linux/arm64 lscr.io/linuxserver/plex:1.42.2`, then copy the binaries from the container.
2. Adapt the EMBY-PLEX-MEMORY-INVESTIGATION methodology from section 9:
   RTTI string -> typeinfo struct -> vtable refs -> caller's
   connection-pool-size immediate-load site. The x86_64 `mov esi, 0x14` site
   maps to an ARM64 immediate-load form, typically `mov w*, #0x14` or
   `movz w*, #20`. Disassemble with aarch64-aware tooling such as
   `objdump -d --disassembler-options=force-thumb=off` or
   `llvm-objdump -d --triple=aarch64-linux-gnu`.
3. Identify the equivalent patch sites in the arm64 PMS binaries. The arm64 site count may differ from the three amd64 sites.
4. Derive the 16-byte original context and patched byte offsets for each arm64 site, then verify each original context is unique within its binary with the same `grep -obE` check method used for amd64.
5. Add a per-arch site table in `tools/cont-init-patch-plex-pool.sh` and branch on `uname -m` for `x86_64` versus `aarch64`, with each branch listing only its arch-specific sites.
6. Remove the two `if: matrix.arch_suffix != 'linux-arm64'` gates in `.github/workflows/sqlite-build.yml` on the pool-patched lane steps.

### Acceptance criteria

- CI shows `PMS first-init smoke (Plex, pool-patched)` succeeding on all three matrix jobs, including `linux-arm64`.
- The cont-init script emits `patched: <bin> @ <offset> 20 -> 16` or `already patched: <bin> @ <offset>` for every site on every architecture.
- `PMS_OPEN_MARKER_COUNT(patched)=<N>` is echoed on the arm64 lane alongside the existing x86 lanes.
- The x86 lanes show no regression: the patched-lane assertion set and count echo still pass.
- This ARM64 entry is removed from `docs/deferred.md` after the item is resolved.

### Related paths

- `.github/workflows/sqlite-build.yml:343` and `.github/workflows/sqlite-build.yml:348` -- arm64 gates for the pool-patched pull and smoke steps.
- `tools/cont-init-patch-plex-pool.sh` -- amd64 patch sites at offsets `0xae4a17`, `0x36f06d`, and `0x38b37b`.
- `docs/design/2026-05-26-plex-pool-patch-ci-experiment.md` -- patch experiment spec, scoped to the amd64-derived sites.
- `docs/design/2026-05-26-plex-pool-patch-ci-experiment-plan.md` -- implementation plan, scoped to the amd64-derived sites.
- External methodology source: `/Users/darthshadow/Personal/server-configs/docs/tuning/EMBY-PLEX-MEMORY-INVESTIGATION.md` sections 9-10.

## LSIO Docker mod for libsqlite3.so swap + Plex pool patch

### Status

Two manual operator workflows currently apply this repository's outputs to live LSIO containers:

1. `scripts/update-sqlite-library.sh` (invoked from `scripts/optimize_media_servers.sh`) replaces `/usr/lib/plexmediaserver/lib/libsqlite3.so` on Plex containers and `/app/emby/lib/libsqlite3.so.3.49.2` on Emby containers with the tuned libsqlite3.so built by this repo's CI.
2. `tools/cont-init-patch-plex-pool.sh` runs as a custom cont-init.d hook on the operator's Plex container to patch the PMS + Plex Media Scanner connection-pool immediate from 20 to 16 (amd64-only today; arm64 deferred per the entry above).

The operator must track the libsqlite3.so version, run the update script on each release, and wire the cont-init script into `/custom-cont-init.d/` per LSIO documentation. A versioned LSIO Docker mod would consolidate both workflows into a single `DOCKER_MODS=<user>/<mod>:<tag>` invocation, with the mod tag pinned to a CalVer release tag.

### Why deferred

Path A versioning revision must land first. A versioned mod needs the CalVer release pipeline to publish libsqlite3.so artifacts at stable, predictable URLs that the mod's runtime download step can consume. Until Path A is applied and a first CalVer release is cut, there is no stable artifact path to pin to.

### Prerequisites to resume

1. Path A versioning revision apply cycle complete (all 8 phases per `docs/design/2026-05-26-versioning-revision-plan.md` landed; CI green).
2. First CalVer release tag cut (format `YYYY.MM.DD-rN`); release pipeline publishes libsqlite3.so artifacts to GitHub releases at predictable URLs.
3. Decide on mod architecture: one mod per target (separate Plex and Emby mods) versus one mod with target detection. LSIO convention generally favours one mod per concern; separate mods is the likely default.
4. Study the LSIO mod template at `https://github.com/linuxserver/docker-mods` for the standard structure: Dockerfile.j2, `root/etc/cont-init.d/*` scripts, version-tagging convention (mod tags typically mirror upstream image tags or use their own semver).
5. Author the mod(s):
   - **Plex mod**: `cont-init.d` script downloads `sqlite-<calver>-library-plex-linux-<arch>.so` from the release URL (selecting arch via `uname -m`), places it at `/usr/lib/plexmediaserver/lib/libsqlite3.so`, then applies the pool-patch via the same 16-byte-context byte logic as `tools/cont-init-patch-plex-pool.sh`. On arm64, the pool-patch step is conditional on the ARM64 deferred work above being resolved.
   - **Emby mod**: `cont-init.d` script downloads `sqlite-<calver>-library-linux-<arch>.so`, places it at `/app/emby/lib/libsqlite3.so.3.49.2`. No pool patch (Emby has no equivalent connection-pool issue).
6. Publish mod images to a container registry (`ghcr.io/<user>/<mod>` or `lscr.io/linuxserver/mods` if accepted upstream).
7. Update `docs/architecture.md` and any README with the operator-facing `DOCKER_MODS=<image>:<tag>` invocation, and document the manual `scripts/update-sqlite-library.sh` as a fallback for non-LSIO setups.

### Acceptance criteria

- Operator sets `DOCKER_MODS=<user>/sqlite3-mod-plex:<calver>` (or the Emby equivalent) on an LSIO Plex or Emby container; on first start, the mod installs the tuned libsqlite3.so + (for Plex) applies the pool-patch, with no manual `scripts/update-sqlite-library.sh` invocation required.
- Mod tags track CalVer release tags one-to-one: bumping the mod tag picks up a new libsqlite3.so build and (where applicable) any updated patch sites.
- `scripts/update-sqlite-library.sh` is documented as the non-LSIO-fallback path (retained for operators not using LSIO images).
- `tools/cont-init-patch-plex-pool.sh` continues to serve as the CI smoke-lane patch source (the smoke step bind-mounts it into a fresh container); the operator-facing patch is owned by the mod thereafter.
- This entry is removed from `docs/deferred.md` once the mod is published and architecture / operator docs reference it.

### Related paths

- `scripts/update-sqlite-library.sh` -- the manual replacement script that the mod supersedes for LSIO setups.
- `scripts/optimize_media_servers.sh` -- orchestrator script that wraps `update-sqlite-library.sh` plus PRAGMA / VACUUM operations. The PRAGMA / VACUUM side remains a manual operator concern even after the mod ships; only the libsqlite3.so swap folds into the mod.
- `tools/cont-init-patch-plex-pool.sh` -- amd64 patch sites and 16-byte-context logic that the Plex mod will adopt.
- `.github/workflows/sqlite-build.yml` -- the CI build that publishes the libsqlite3.so artifacts the mod consumes. Path A's CalVer release section is the integration point.
- `docs/architecture.md` -- target for a new LSIO-mod usage section once the mod ships.
- External reference: `https://github.com/linuxserver/docker-mods` -- LSIO mod template repository and Dockerfile.j2 example.
