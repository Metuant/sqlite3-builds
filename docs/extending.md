# Extending Runtime Support

This is the cold-start runbook for extending the implemented Plex and Emby
support window. Runtime code and CI are authoritative; this document names the
current edit surfaces and points to the reference docs that own field contracts
and SHA derivation.

Use:

- `docs/baked-pins-schema.md` for schema v3 row fields, validator reject rules,
  target-path rules, detector matching, curation status, moving image aliases,
  and digest provenance.
- `docs/runtime-baseline-derivation.md` for OCI extraction and SHA-256
  derivation.
- `docs/plex-pool-patch-derivation.md` for Plex ConnectionPool pool-site
  derivation and runtime patch semantics.

## Bump SQLite

Update the SQLite tuple in `pins/versions.env`: version, dotted version,
amalgamation URL and SHA3-256, full-source URL and SHA3-256, and
`SQLITE_SOURCE_ID`. Store the 84-byte `sqlite_source_id()` result in
`SQLITE_SOURCE_ID` with each space encoded as `%20`. Do not copy the NEW id
into the maintenance script or Plex phase; both read this pin.

Then refresh the amalgamation patch and config-count pins when the new source
requires it. Run `bash tests/check_pin_alignment.sh` before CI. The generic and
Plex library builds decode the pin and execute `SELECT sqlite_source_id()`
through each freshly built shared library, so CI rejects a stale or incorrect
source-id pin.

## Add A Version

Add a new supported Plex or Emby server version in this order.

1. Add one `pins/runtime-support.tsv` row.
   - `mod`: `plex` or `emby`.
   - `server_id`: stable support-window id, such as `plex-1.43.2` or
     `emby-4.9.5`.
   - `image_ref`: the canonical GHCR reference
     `ghcr.io/linuxserver/<mod>:<tag>`. `lscr.io` is a vanity front for the same
     GHCR images and is rejected by `tests/check_multi_version_pin_alignment.sh`
     for `supported` rows and every `pre` runtime-baseline row.
   - `compat_group`: an existing group covered by
     `pins/library-compat-groups.tsv`.
   - `status`: `supported` only after the remaining rows and review evidence
     exist. Otherwise keep the row out of the rendered support window.
   - `review_ref`: for Plex, exactly the pool-review `review_ref` used by the
     matching approved rows in `pins/plex-pool-patch-reviews.tsv`; for Emby and
     other non-Plex rows, a tracked detector-evidence pointer. Current Emby rows
     use `pins/emby-detector-evidence.tsv`.

2. Add all required `pins/runtime-baselines.tsv` rows.
   - Use `docs/runtime-baseline-derivation.md`; do not invent SHAs.
   - Plex requires four curated detector roles per `(server_id, arch)`:
     `plex_pms:pristine`, `plex_pms:patched`, `plex_scanner:pristine`, and
     `plex_scanner:patched`.
   - Do not add `plex_pms:source-id-patched` to
     `pins/runtime-baselines.tsv`. Manifest rendering computes that fifth
     runtime role from the verified pristine PMS bytes, curated pool sites, and
     canonical `SQLITE_SOURCE_ID` pin.
   - Emby requires two detector roles per `(server_id, arch)`: `emby_deps` and
     `emby_dll`.
   - Add the `pre target_sqlite` row for each `(server_id, arch)`.
   - For Emby, add one `pins/emby-detector-evidence.tsv` row for every
     `(server_id, arch, kind, path_role)` baseline tuple, where `kind` is
     `detect` or `pre`:
     `emby<TAB>server_id<TAB>arch<TAB>kind<TAB>path_role<TAB>image_ref<TAB>file_path<TAB>sha256<TAB>pins/runtime-baselines.tsv`.
     Copy `image_ref` from `pins/runtime-support.tsv` and `file_path` plus
     `sha256` from `pins/runtime-baselines.tsv` exactly.
     `tests/check_multi_version_pin_alignment.sh` rejects missing, duplicate,
     or mismatched rows in both directions.
   - Plex also requires `pre plex_icu_linked:<soname>` rows for the three linked
     ICU sonames and matching `icu-runtime` rows for each `(compat_group, arch,
     soname)`.

3. For Plex only, add Plex patch pool-site curation rows.
   - Add `pins/plex-patch-pool-sites.tsv` rows for each supported binary and
     arch.
   - Add at least two distinct approved reviewer rows in
     `pins/plex-pool-patch-reviews.tsv` for every exact site tuple.
   - The `review_ref` in `pins/runtime-support.tsv` must match the review rows
     for that `server_id`.
   - Use `docs/plex-pool-patch-derivation.md` and
     `docs/runtime-baseline-derivation.md`; do not copy or fork the derivation
     method here.

4. Confirm `pins/library-compat-groups.tsv` covers the selected group.
   - The row must name the current `compat_group`, `mod`, build variant,
     artifact stem, SQLite source guard, and a `smoke_server_id` that resolves to
     a supported row in the same group.
   - The current Plex patch phase requires exactly one Plex row and an 84-byte
     `sqlite_source_id_guard`; adding another Plex group also requires making
     that runtime lookup group-aware.
   - For Plex groups, the row also owns ICU source version, source SHA-512, and
     linked ICU sonames.

Run the local verifier set before treating the version as supported:

```bash
bash tests/check_multi_version_pin_alignment.sh
bash tests/check_multi_version_pin_alignment_negative_test.sh
bash tests/render_lsio_mod_baked_pins_test.sh
bash tests/manifest_parser_test.sh
bash tests/selector_test.sh
bash tests/selector_accessors_test.sh
```

## Render And Stage A Local Mod

Use the host-runnable helpers directly when validating a local mod context from
already built library artifacts. The renderer consumes `pins/*.tsv` by default,
validates the artifact SHA against the supplied `SHA256SUMS`, and writes
`baked-pins.txt`; the stager copies the mod root, shared fragments, manifest,
and matching `libsqlite3.so` artifacts into a Docker context.

Example Emby render and stage flow:

```bash
tmpdir="$(mktemp -d)"
tag="2026.05.28-r1"
generated_at="2026-05-28T00:00:00Z"
arch="linux-x86_64-v3"
compat_group="generic"
artifact_path="release/library/libsqlite3.so"
artifact_name="sqlite-${tag}-library-generic-${arch}.so"
target_path="/app/emby/lib/libsqlite3.so.3.49.2"
sha256sums="${tmpdir}/SHA256SUMS"
pre_fragment="path/to/pre-fragment.txt"
baked_pins="${tmpdir}/baked-pins.txt"
staged="${tmpdir}/emby-mod"

artifact_sha="$(sha256sum "$artifact_path" | awk '{print $1}')"
printf '%s  %s\n' "$artifact_sha" "$artifact_name" > "$sha256sums"

tools/lsio-mod/render-lsio-mod-baked-pins.sh \
  --mod emby \
  --release-tag "$tag" \
  --generated-at "$generated_at" \
  --sha256sums "$sha256sums" \
  --pre-fragment "$pre_fragment" \
  --output "$baked_pins" \
  --artifact "${arch}:${compat_group}:${artifact_name}:${artifact_path}:${target_path}"

tools/lsio-mod/stage-lsio-mod.sh \
  --mod emby \
  --output-dir "$staged" \
  --baked-pins "$baked_pins" \
  --artifact "${arch}:${compat_group}:${artifact_path}"
```

The render helper accepts one or more `--artifact` values shaped as
`ARCH:COMPAT_GROUP:NAME:PATH:TARGET_PATH`; the stage helper accepts matching
`--artifact` values shaped as `ARCH:COMPAT_GROUP:SOURCE_PATH`. Set these
environment variables before rendering to use non-default pin sources or local
fixtures:

| Variable | Default |
|---|---|
| `RENDER_LSIO_MOD_RUNTIME_SUPPORT` | `pins/runtime-support.tsv` |
| `RENDER_LSIO_MOD_COMPAT_GROUPS` | `pins/library-compat-groups.tsv` |
| `RENDER_LSIO_MOD_RUNTIME_BASELINES` | `pins/runtime-baselines.tsv` |
| `RENDER_LSIO_MOD_POOL_SITES` | `pins/plex-patch-pool-sites.tsv` |
| `RENDER_LSIO_MOD_POOL_REVIEWS` | `pins/plex-pool-patch-reviews.tsv` |

Plex rendering additionally requires one
`--plex-pms-pristine SERVER_ID:ARCH:PATH` value for every rendered server/arch
tuple. `PATH` must contain the exact pristine PMS bytes from the supported full
Plex image. The renderer rejects a missing file or a whole-file SHA mismatch
before deriving or emitting the final PMS detector SHA.

## Add A Compat Group

Adding a compatibility group is a code and CI change in the current
implementation, not a data-only edit.

Current touch points:

- `lsio-mods/shared/cont-init-fragments/manifest-parser.sh`:
  `manifest_parser_valid_compat_group` hardcodes accepted runtime pairs.
- `tools/lsio-mod/render-lsio-mod-baked-pins.sh`: Plex-flavor groups require
  updating the Plex ICU soname and `plex_icu_linked:*` role allowlists when the
  linked closure changes.
- `lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-plexpatch/run`:
  the staged source-id lookup currently accepts exactly one Plex compatibility
  row and must select by group before a second Plex group can run.
- `tests/check_multi_version_pin_alignment.sh`: required artifact-stem checks
  are hardcoded for the current groups.
- `.github/workflows/sqlite-build.yml`: CI currently resolves
  `first_compat_group_for_mod` and builds an arch-only library matrix. A second
  production group for the same mod is not built until the matrix becomes
  arch-by-group.
- `pins/library-compat-groups.tsv`: add the group row with an artifact stem and
  a `smoke_server_id` that resolves to a supported row in the group.
- `pins/runtime-baselines.tsv`: Plex-flavor groups require linked
  `icu-runtime` rows for each arch and linked ICU soname.

No-op for the current support window: there is only one production compat group
per mod.

## Add Or Re-Derive A Pool Site

Use `docs/plex-pool-patch-derivation.md` for the site derivation method and
`docs/runtime-baseline-derivation.md` for pristine, patched, and A/B SHA checks.

Each `pins/plex-patch-pool-sites.tsv` row requires at least two distinct
approved reviewer rows in `pins/plex-pool-patch-reviews.tsv` for the exact site
tuple. The implemented review control is count-only: `.github/CODEOWNERS` is
single-owner, so the repository does not currently enforce two independent
humans through ownership or branch protection.

## Cut A Release

Release tags use CalVer with an explicit revision suffix:
`YYYY.MM.DD-rN`.

Current CI gates:

1. `preflight` loads pins and compatibility-group outputs, then runs the pin and
   Build.sh guards once.
2. `build-cli`, `build-generic`, and `build-plex` compile and upload their
   producer-owned artifacts for `linux-x86_64-v2`, `linux-x86_64-v3`, and
   `linux-arm64`; the library jobs also wait for `base`.
3. `release` waits for all three artifact jobs and `mod-static-tests`, then
   runs only for matching tags.
   It selects the greatest reachable prior CalVer tag, appends oldest-first
   non-merge commit subjects to the existing compatibility body, and publishes
   `sqlite-<tag>-*.tar.gz` plus `SHA256SUMS`; `SHA256SUMS` includes tarball SHAs
   and extracted `.so` SHAs for library archives.
4. `mod-build` consumes same-run generic and Plex library artifacts, records
   support image digests, renders `baked-pins.txt`, stages and smokes the Plex
   and Emby mod roots, and uploads mod image artifacts only. It pushes nothing.
5. `mod-publish` is tag-gated and waits for both `release` and `mod-build`. It
   pushes per-arch GHCR tags plus final multi-arch manifests for Plex and Emby.
   It does not publish `latest`.

Resolved support image digests are recorded by the workflow step named
`Record support image digests` and uploaded as
`support-image-digests-<mod>-<arch_suffix>` artifacts.
