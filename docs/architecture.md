# Repository Architecture

## Purpose

This repository builds and ships tuned SQLite artifacts for Linux media-server
containers.

It produces:

- A static SQLite CLI binary.
- A generic `libsqlite3.so` shared library for Emby.
- A Plex-specific `libsqlite3.so` shared library linked to Plex-style ICU 69.
- Release archives and SHA-256 manifests for build outputs.
- LSIO Docker mod images for Plex and Emby SQLite replacement.
- Host script support for planned-downtime database maintenance and optional
  post-start Plex database optimize triggering.

The repository is centered on replacing bundled SQLite libraries while keeping
the application-owned database semantics intact. Runtime tuning lives in the
library through SQLite's auto-extension mechanism, so it works even when the
host application loads SQLite by handle rather than by symbol interposition.

## Layout

| Path | Role |
|---|---|
| `build/Build.sh` | Container-internal build driver for CLI and library targets. |
| `build/build_static_sqlite.sh` | Local wrapper that builds Docker images and extracts artifacts into `release/`. |
| `docker-cli/Dockerfile` | Alpine-based static CLI build image. |
| `docker-library/Dockerfile` | Shared-library build image: digest-pinned `ubuntu:18.04` (bionic, glibc 2.27) plus `gcc-13` for the generic variant, and LSIO Alpine/musl for the Plex variant. |
| `docs/extending.md` | Cold-start runbook for adding runtime versions, compatibility groups, pool sites, and releases. |
| `docs/baked-pins-schema.md` | Current schema v3 `baked-pins.txt` contract, validator reject list, and runtime keying rules. |
| `src/auto_extension.c` | Built into both library variants; owns open-time auto-extension registration, trace-mask setup, target filtering, and the TLS seam into runtime optimize. |
| `src/runtime_optimize.c` | Built into both library variants; owns runtime optimize state, cadence, eligibility, and hook helpers. |
| `src/auto_extension_internal.h` | Shared internal seam header between `src/auto_extension.c` and `src/runtime_optimize.c`. |
| `src/slow_query_tracker.c` | Hidden observability satellite for PROFILE-based slow-query logging and bounded per-template stats. |
| `tests/auto_extension_smoke.c` | Runtime smoke for filter, kill switch, read-only skip, and emitted PRAGMAs. |
| `tests/runtime_optimize_smoke.c` | Runtime smoke for close and inline `PRAGMA optimize` target gating, STAT1/STAT4 tiers, kill switches, skip gates, cadence, close semantics, and shutdown/reinit. |
| `tests/slow_query_smoke.c` | Runtime smoke for the slow-query tracker threshold parser, kill switches, LRU bound, truncation, and stats dump. |
| `tests/stmt_trace_smoke.c` | Runtime smoke for the STMT trace env contract and trace mask registration against the shared STMT/PROFILE callback. |
| `tests/config_after_dlopen_smoke.c` | Runtime smoke proving startup-only config remains legal after library load and before first open. |
| `tests/shutdown_reinit_smoke.c` | Runtime smoke proving lazy auto-extension registration survives `sqlite3_shutdown()` and later reopen. |
| `tests/icu_smoke.c` | Runtime smoke for Plex ICU collation registration and comparator use. |
| `tests/render_lsio_mod_baked_pins_test.sh` | Unit tests for `tools/lsio-mod/render-lsio-mod-baked-pins.sh`: schema v3 metadata, detector, artifact, pre, Plex pool-site, unsupported rows, and malformed-input rejection. |
| `tests/cont_init_fragments_test.sh` | Static and unit checks for LSIO mod runtime fragments, phase-script shebangs, no custom env-var surface, and Plex ICU read-only posture. |
| `tests/sqlite_build_workflow_mod_only_test.sh` | Static workflow check for minimal release assets and split `mod-build` / `mod-publish` jobs. |
| `tests/check_obs_counts.sh` | Pre-build lint: counts `SQLITE_CONFIG_` and `SQLITE_DBCONFIG_` decode entries in `src/observability.c` against `build/expected-sqlite-*-count.txt`. |
| `tests/check_pin_alignment.sh` | Pre-build lint: asserts mimalloc VERSION + URL + SHA512 alignment, forbids retired scalar pin keys, checks group-owned ICU source defaults, and checks SORTERREF/PMASZ compile defaults against constructor runtime config values. |
| `tests/alloc_latency_bench.c` | Advisory `sqlite3_malloc` / `sqlite3_free` microbench compiled and run in library images without failing the build. |
| `tests/runtime_optimize_close_bench.c` | Advisory runtime optimize hook microbench for close-adjacent inline exits, compiled and run for the generic library variant on every build without failing the build. |
| `build/libsqlite3-version-script.ld` | Library-only linker version script: pinned public `sqlite3` API exports from `sqlite3.h` plus project-required extras, then `local: *;`. |
| `tools/lsio-mod/render-lsio-mod-baked-pins.sh` | Host-runnable renderer for per-mod `baked-pins.txt` runtime SHA data. |
| `tools/lsio-mod/stage-lsio-mod.sh` | Local and CI staging helper that assembles an ephemeral LSIO mod Docker context under `mktemp -d`. |
| `lsio-mods/` | Source-of-truth Plex and Emby Docker mod roots, shared runtime fragments, and parent README. |
| `lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh` | Args-only shared Plex pool-patch core staged into the Plex mod. |
| `scripts/optimize_media_servers.sh` | Planned-downtime maintenance helper for Plex and Emby databases, including container stop/start ownership and optional post-start Plex optimize API triggering. |
| `.github/workflows/sqlite-build.yml` | CI build, smoke, artifact upload, release archive, and SHA manifest workflow. |
| `.dockerignore` | Tight Docker context allowlist for build, Dockerfile, source, script, and test inputs. |

## Artifact Set

| Artifact | Source path | Output shape | Consumer |
|---|---|---|---|
| Static CLI | `docker-cli/Dockerfile` + `build/Build.sh cli` | `sqlite3`, `sqlite3_orig`, CLI archive | Host-side diagnostics and maintenance |
| Generic library | `docker-library/Dockerfile` + `build/Build.sh library` | `libsqlite3.so`, library archive | Emby |
| Plex library | `docker-library/Dockerfile` + `LIBRARY_VARIANT=plex` | `libsqlite3.so`, Plex library archive | Plex |
| `SHA256SUMS` | Release job | Archive SHA plus extracted `.so` SHA for library archives | Release users and mod-build verification |
| Plex LSIO mod image | `lsio-mods/plex` staged by `tools/lsio-mod/stage-lsio-mod.sh` | GHCR image `linuxserver-mod-sqlite3-plex:<tag>` | LSIO Plex containers |
| Emby LSIO mod image | `lsio-mods/emby` staged by `tools/lsio-mod/stage-lsio-mod.sh` | GHCR image `linuxserver-mod-sqlite3-emby:<tag>` | LSIO Emby containers |

## Runtime Targets

| Target | SQLite binding | Library file | Variant | State in this cycle |
|---|---|---|---|---|
| Plex | Native process using bundled `libsqlite3.so` | `/usr/lib/plexmediaserver/lib/libsqlite3.so` | `plex` | Active |
| Emby | .NET P/Invoke into bundled SQLite | Selected artifact-row `target_path` (current rows use `/app/emby/lib/libsqlite3.so.3.49.2`) | `generic` | Active |
| JF | .NET SQLitePCLRaw name lookup | `/usr/lib/jellyfin/bin/libe_sqlite3.so` | `generic` | Unsupported / out of scope |

See §Out of Scope.

## Library Variants

Two shared-library variants are built from the same source tree.

| Variant | Build selector | Added source or linkage | Runtime purpose |
|---|---|---|---|
| `generic` | default `LIBRARY_VARIANT=generic` | Amalgamation plus `src/auto_extension.c` and `src/runtime_optimize.c`, with seam header `src/auto_extension_internal.h` | Emby |
| `plex` | `LIBRARY_VARIANT=plex` | Generic source plus SQLite `ext/icu/icu.c`, `SQLITE_ENABLE_ICU`, and Plex-style ICU libs | Plex |

The intentional variant delta is ICU support for Plex.

Both library variants also link mimalloc v3.3.2 by link-time interposition;
the static CLI remains on the platform allocator.

Everything else is shared: the SQLite amalgamation pin, `src/auto_extension.c`,
`src/runtime_optimize.c`, seam header `src/auto_extension_internal.h`,
feature families, tuning defaults, high-capacity limits, shared-cache omission,
and page-cache overflow stat posture.

Plex needs a separate variant because its runtime uses ICU 69 libraries built
with a `plex` library suffix. Those files expose suffixed C symbols and Plex
also depends on them directly. The `plex` variant links against:

```text
libicuucplex.so.69
libicui18nplex.so.69
libicudataplex.so.69
```

The Plex variant's `src/auto_extension.c` exports a no-op
`sqlite3_enable_shared_cache(int)` returning `SQLITE_OK` because PMS's bundled
`libpython27.so` dynamically links that symbol at load time. The stub does
NOT touch `sqlite3GlobalConfig.sharedCacheEnabled`, and
`SQLITE_OMIT_SHARED_CACHE` keeps the shared-cache consumer compiled out, so
process-level `sqlite3_enable_shared_cache` calls, the
`SQLITE_OPEN_SHAREDCACHE` open flag, and the `cache=shared` URI parameter
are all inert -- connections stay private regardless of caller.

Plex `icu.c` is compiled with `-DU_HAVE_LIB_SUFFIX=1` and
`-DU_LIB_SUFFIX_C_NAME=_plex`. Those defines select ICU's suffix-aware public
names so ICU calls resolve to the `_69_plex`-suffixed symbols exported by the
suffix-built ICU libraries.

No supported LSIO target in this repository requires Plex-renamed ICU except
Plex. Emby uses the generic library. JF deployment is out of scope until a
current design and validation plan land.

## Build Inputs

The source of truth for SQLite, CMake, and mimalloc build pins is
`pins/versions.env`; the local wrapper sources it and the workflow loads it into
`$GITHUB_ENV`.
Runtime support images live in `pins/runtime-support.tsv`. Library
compatibility groups and Plex ICU source pins live in
`pins/library-compat-groups.tsv`.

For extension procedures, use `docs/extending.md`. For the schema v3 manifest
field contract and fail-closed validator rules, use
`docs/baked-pins-schema.md`.

| Input | Current value |
|---|---|
| SQLite version | `3530300` |
| SQLite release year | `2026` |
| Amalgamation URL | `https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip` |
| Amalgamation SHA3-256 | `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9` |
| Full source URL | `https://www.sqlite.org/2026/sqlite-src-3530300.zip` |
| Full source SHA3-256 | `2daecfa16e3b19e058dc2e2cb717b80ade361e0315aa5376c3619f66aa81e181` |

The current Plex library compatibility group pins ICU 69.1:

| Input | Current value |
|---|---|
| ICU tarball | `https://github.com/unicode-org/icu/releases/download/release-69-1/icu4c-69_1-src.tgz` |
| ICU SHA-512 | `d4aeb781715144ea6e3c6b98df5bbe0490bfa3175221a1d667f3e6851b7bd4a638fa4a37d4a921ccb31f02b5d15a6dded9464d98051964a86f7b1cde0ff0aab7` |
| ICU build prefix | `/opt/icu-69-plex` |
| ICU configure suffix | `--with-library-suffix=plex` |

The generic library build pins Kitware CMake:

| Input | Current value |
|---|---|
| CMake version | `3.31.12` |
| CMake Linux x86_64 SHA-256 | `0dc2e9a6860f06bf10bd8fadc03e35d9eeb4df46e33763a7e480e987758f385c` |
| CMake Linux aarch64 SHA-256 | `83f8fd91d2038a56556e1400390fcfe42f79602940c494f6c6f1cdae7f9e7f40` |
| Build scope | Generic library Ubuntu base only |

The build-image and generic ABI-floor pins are:

| Input | Current value |
|---|---|
| Generic Ubuntu base | `ubuntu:18.04@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98` |
| LSIO Alpine base | `ghcr.io/linuxserver/baseimage-alpine:3.23` |
| Generic glibc max | `2.27` |

The library build also pins mimalloc v3.3.2:

| Input | Current value |
|---|---|
| Mimalloc version | `3.3.2` |
| Mimalloc URL | `https://github.com/microsoft/mimalloc/archive/refs/tags/v3.3.2.tar.gz` |
| Mimalloc SHA-512 | `226bbd51eca36d7737ce5e2edba7e0a3beeca448462a861bcbfb6726a0994bc077b4c684d7ff8b0805d71bf770e00df14f10ed598256ee54a154d8cc08e6a5c1` |
| Mimalloc install prefix | `/opt/mimalloc` |

The same SQLite pins must stay aligned across:

- `build/build_static_sqlite.sh`.
- `.github/workflows/sqlite-build.yml`.
- Docker build args.
- `docker-cli/Dockerfile`.
- `docker-library/Dockerfile`.
- `build/Build.sh`.

The mimalloc VERSION + URL + SHA512 tuple must stay aligned across
`build/Build.sh`, `build/build_static_sqlite.sh`, `docker-library/Dockerfile`,
and `.github/workflows/sqlite-build.yml`; `tests/check_pin_alignment.sh`
fails on any drift in the full tuple.

`CMAKE_VERSION` and `CMAKE_SHA256_*` fields must stay aligned across
`pins/versions.env`, the generic library workflow build args,
`docker-library/Dockerfile`, and `tests/check_pin_alignment.sh`.

Plex ICU source VERSION and SHA512 fields are group-owned by
`pins/library-compat-groups.tsv`; the workflow passes those values as Plex
library `ICU_SOURCE_*` build args.

## Build Pipeline

### Local Wrapper

`build/build_static_sqlite.sh` is the local orchestration entrypoint.

It checks Docker access, builds the CLI and library images, passes SQLite pins
and architecture settings, extracts outputs into `release/cli`,
`release/library`, or `release/library-plex`, runs local inspection, and removes
the temporary images.

`LIBRARY_VARIANT=plex` switches the local library output directory to
`release/library-plex` and passes the extra SQLite source inputs needed for
`ext/icu/icu.c`.

For library builds, it also forwards the mimalloc VERSION + URL + SHA512 pins
into `docker-library/Dockerfile`.

### Build Driver

`build/Build.sh` accepts a target and source URL.

```text
./Build.sh cli <amalgamation-url> [compressor]
./Build.sh library <amalgamation-url>
```

It also accepts `--target=cli` and `--target=library`.

For both targets, it downloads and verifies the SQLite amalgamation when
absent, extracts it, creates `dist/`, and compiles with GCC, `-O3`,
architecture tuning, LTO, and shared compile flags.

For `cli`, it builds from `shell.c sqlite3.c`, links statically, preserves
`sqlite3_orig`, strips `sqlite3`, and optionally compresses the stripped binary.

For `library`, it builds `sqlite3.c` plus `/app/auto_extension.c` and
`/app/runtime_optimize.c`, with `/app/auto_extension_internal.h` as the
internal seam header, links a shared object, applies library-only feature
defaults, and writes `dist/libsqlite3.so`.

For `library`, the link line prepends `MIMALLOC_OBJ`
(`/opt/mimalloc/lib/mimalloc.o`) and `MIMALLOC_LIB`
(`/opt/mimalloc/lib/libmimalloc.a`) so both shared-library variants interpose
mimalloc at link time while the CLI target stays unchanged.

The same library-only link adds
`-Wl,--version-script=/app/build/libsqlite3-version-script.ld`. The script
enumerates the pinned public `sqlite3` API exports from `sqlite3.h` plus
`auto_extension_path_is_target`, `auto_extension_sorterref_cfg_rc`,
`auto_extension_pmasz_cfg_rc`, and `sqlite3_enable_shared_cache`, then ends
with `local: *;`.

For `library`, it also applies `build/sqlite-amalgamation.patch` to the
extracted SQLite amalgamation with `patch -p1` before compile. The patch
renames the six wrapped SQLite API definitions to hidden `*_real` symbols:

- `sqlite3_initialize`
- `sqlite3_config`
- `sqlite3_db_config`
- `sqlite3_open`
- `sqlite3_open_v2`
- `sqlite3_open16`

SQLite version bumps that change any target definition shape cause `patch` to
reject hunks and fail the build with patch's native error message. The library
patch also inserts internal runtime optimize hooks into `sqlite3_step`,
`sqlite3_reset`, `sqlite3_finalize`, and the immediate-close path. Hook
movement on SQLite version bumps is therefore a patch rejection. The library
build then compiles `sqlite3.c`, `/app/auto_extension.c`,
`/app/runtime_optimize.c`, `/app/observability.c`, and
`/app/slow_query_tracker.c`; `/app/auto_extension_internal.h` is the shared
internal seam header. `/app/auto_extension.c` keeps open-time registration,
trace-mask setup, target filtering, and the TLS seam, and reaches runtime
optimize through `runtime_optimize_seed_path`; `/app/runtime_optimize.c` owns
the runtime optimize logic. The CLI target does not run this patch and does
not include observability or runtime optimize.

After the library compile, `build/Build.sh` checks every
`SQLITE_CONFIG_*` and `SQLITE_DBCONFIG_*` define in the pinned `sqlite3.h`
against the decode tables in `/app/observability.c`. Missing table entries are
build failures.

Adjacent to the preserved `sqlite3_*_real` and lazy-helper symbol gates,
post-link checks fail on leaked `mi_*` or malloc-family exports and require
local mimalloc implementation symbols to be present.

For the Plex variant, it additionally requires `SQLITE_SRC_URL`, verifies the
SQLite full source archive, adds `ext/icu/icu.c`, enables ICU, and links with
Plex-suffixed ICU libraries.

### CLI Docker Image

`docker-cli/Dockerfile` is Alpine based.

It installs the static-build toolchain, copies `build/Build.sh`, and invokes
the CLI target with the amalgamation URL, compressor, architecture baseline,
and SHA pin. The CLI image is only responsible for the static CLI artifact.

### Library Docker Image

`docker-library/Dockerfile` uses digest-pinned `ubuntu:18.04` (bionic,
glibc 2.27) plus `gcc-13` from `ubuntu-toolchain-r` for the generic variant
and the LSIO Alpine/musl base for the Plex variant.

It installs the shared-library build toolchain and copies `build/Build.sh`,
`src/auto_extension.c`, `src/runtime_optimize.c`,
`src/auto_extension_internal.h`, `tests/`,
`build/sqlite-amalgamation.patch`, `build/expected-sqlite-config-count.txt`,
and `build/expected-sqlite-dbconfig-count.txt` into `/app`, with the build
files under `/app/build/`.

The generic Ubuntu base downloads pinned Kitware CMake 3.31.12 with a
per-architecture SHA-256 check before the mimalloc CMake stage. The Plex Alpine
base uses its package CMake.

The same Dockerfile builds mimalloc v3.3.2 in a dedicated stage, installs it
under `/opt/mimalloc`, writes `/opt/mimalloc/SHA512`, and exports
`MIMALLOC_OBJ` plus `MIMALLOC_LIB` into `Build.sh`. The Plex branch adds
`-DMI_LIBC_MUSL=ON`; the generic branch does not.

For the generic variant, it builds the shared library, requires at least one
`@GLIBC_`-versioned undefined symbol, rejects any observed `@GLIBC_` reference
above `GENERIC_GLIBC_MAX` (`2.27`), and runs the config-after-dlopen,
shutdown/reinit, auto-extension, and runtime optimize smokes.

For the Plex variant, it also builds ICU 69.1 under `/opt/icu-69-plex`, exposes
`PLEX_ICU_INCLUDE` and `PLEX_ICU_LIB`, checks ICU soname and symbol shape with
`ldd` and `nm`, runs `icu_smoke`, and runs the config-after-dlopen,
shutdown/reinit, auto-extension, and runtime optimize smokes with the Plex ICU
path available. The Plex branch builds under Alpine/musl so the resulting
library does not carry glibc-versioned symbols such as `fcntl64`, which Plex's
`libgcompat` shim does not provide.

For the Plex variant, the build stage fails if the produced `libsqlite3.so`
carries `fcntl64` or any other glibc-versioned symbol. This zero-GLIBC gate is
distinct from the generic variant's bounded glibc floor gate.

### Workflow Matrix

`.github/workflows/sqlite-build.yml` builds on pushes to `main`,
digit-prefixed tag pushes matching `[0-9]*`, pull requests to `main`, and
manual dispatch. The workflow jobs are `build`, `mod-static-tests`, `release`,
`mod-build`, and `mod-publish`. The release job validates `YYYY.MM.DD-rN`
before archive assembly and release asset publication; `mod-build` and
`mod-publish` handle LSIO mod images.

The build job uses an architecture matrix:

| Runner | `MARCH` | Suffix |
|---|---|---|
| `ubuntu-24.04` | `x86-64-v2` | `linux-x86_64-v2` |
| `ubuntu-24.04` | `x86-64-v3` | `linux-x86_64-v3` |
| `ubuntu-24.04-arm` | `armv8-a` | `linux-arm64` |

Each matrix row builds three Docker images: CLI, generic library, and Plex
library.

x86-64-v4 is intentionally absent. GitHub `ubuntu-24.04` runners commonly use
AMD Zen 3 hosts without AVX-512, and a v4 library SIGILLs in the
auto-extension constructor. v4-capable amd64 hosts receive the v3 artifact.

Each build output is extracted with the pinned Docker-extract action and
uploaded as a workflow artifact.

Mod runtime SHA data is rendered by
`tools/lsio-mod/render-lsio-mod-baked-pins.sh` from same-run build artifacts,
same-run runtime baseline extraction, and the Plex pool-patch pins
(`pins/plex-pool-patch-sites.tsv` and `pins/plex-pool-patch-reviews.tsv`).

The workflow keeps observability count and ABI obsolete-config checks after
runtime smokes. The `mod-static-tests` job runs the repo-only LSIO mod test
suite, including
`tests/render_lsio_mod_baked_pins_test.sh`,
`tests/cont_init_fragments_test.sh`, and
`tests/sqlite_build_workflow_mod_only_test.sh`.
Mandatory multi-version gates include
`tests/check_multi_version_pin_alignment.sh` in the workflow, the parser and
selector suites (`tests/manifest_parser_test.sh` and
`tests/selector_test.sh`), support-image digest artifacts, and the skopeo OCI
static check in `tools/ci/skopeo-mod-image-oci-check.sh`.

### Release Job

The release job runs only for CalVer tags matching
`YYYY.MM.DD-rN`. It downloads same-run sqlite artifacts, creates the public
CLI and library tarballs, writes `SHA256SUMS`, and publishes only:

- `sqlite-<tag>-*.tar.gz`
- `SHA256SUMS`

It does not fetch prior release metadata, assemble runtime manifests, or
publish runtime mod metadata.

### LSIO Mod Jobs

`mod-build` runs on every push, pull request, and tag; depends on `build`, not
`release`. It uses a four-lane matrix:

| Mod | Runner | Arch suffix |
|---|---|---|
| Plex | `ubuntu-24.04` | `amd64` |
| Plex | `ubuntu-24.04-arm` | `arm64` |
| Emby | `ubuntu-24.04` | `amd64` |
| Emby | `ubuntu-24.04-arm` | `arm64` |

Each lane renders `baked-pins.txt`, stages an ephemeral Docker context under
`mktemp -d`, builds a run-scoped temporary image tag, and smokes by building a
throwaway image `FROM` the pinned LSIO base with the staged `root-fs/` copied
in, then running it so the native s6-rc `init-mod-sqlite3-*` oneshots execute
in the real LSIO environment before the app service starts. `mod-build` pushes
nothing and uploads each mod image as a workflow artifact.

`mod-publish` is tag-gated; depends on `release` and `mod-build`. It pushes
stable per-arch tags and the final multi-arch manifests. It never pushes
`latest`.

## Compile Profile

The compile profile is tuned by category rather than by one-off per-app forks.

Feature categories include search/indexing, app compatibility, operational
tooling, planner and sorter tuning, file and memory behavior, high-capacity
limits, and library safety posture. The profile avoids per-app forks except for
Plex ICU linkage.

The CLI target adds interactive diagnostics such as database-stat and bytecode
virtual tables. The library target keeps those out of application processes.

The shared compile flags pin `-DSQLITE_SORTER_PMASZ=8192` and
`-DSQLITE_DEFAULT_SORTERREF_SIZE=512` in `build/Build.sh`, so both apply to all
artifacts including the static CLI. At the 16 KiB compile-default page size,
PMASZ yields a 128 MiB PMA cap per active sort worker; with `PRAGMA threads=8`, a
connection may drive up to eight concurrent sort workers at that cap. The
SORTERREF compile default activates the 512 B threshold even on code paths that
do not run the runtime `sqlite3_config` re-assert.

The library profile pins `-DSQLITE_DEFAULT_AUTOVACUUM=0`.
`scripts/optimize_media_servers.sh` sets `PRAGMA auto_vacuum=NONE` explicitly
during planned-downtime database rebuilds, matching the compile default.

## Per-Connection PRAGMA Injection

`src/auto_extension.c` and `src/runtime_optimize.c` are compiled into both
library variants; `src/auto_extension_internal.h` is the shared internal seam
header. `src/auto_extension.c` owns open-time registration, trace-mask setup,
target filtering, and the TLS seam, and reaches runtime optimize through
`runtime_optimize_seed_path`; `src/runtime_optimize.c` owns runtime optimize
logic.

The priority-102 constructor first registers `SQLITE_CONFIG_LOG` with
`sqlite_log_to_observability`. `build/Build.sh` sets
`-DSQLITE_DEFAULT_SORTERREF_SIZE=512`, so the sorter-reference threshold is
active for code paths that do not run this constructor. The constructor
re-asserts `sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 512)` before
effective SQLite initialization for visibility and drift protection.
Auto-extension registration is lazy: the observability `sqlite3_open`,
`sqlite3_open_v2`, and `sqlite3_open16` wrappers call
`auto_extension_register_for_open()` immediately before the hidden real open
function. That helper calls
`sqlite3_auto_extension(autopragma_init)`, which performs the first effective
`sqlite3_initialize()` when needed; `openDatabase` then reaches
`sqlite3AutoLoadExtensions(db)` and runs the callback for the new connection.
Embedders may call startup-only `sqlite3_config()` verbs after `dlopen` and
before their first public open.

The same constructor also re-asserts `sqlite3_config(SQLITE_CONFIG_PMASZ, 8192)`
and stores the rc in `auto_extension_pmasz_cfg_rc()`, mirroring
`auto_extension_sorterref_cfg_rc()` for smoke verification and keeping the
runtime PMA target aligned with the compile default.

The callback is best-effort:

- It never makes `sqlite3_open*` fail on tuning failure.
- It logs failed PRAGMA execution with the database filename.
- It returns `SQLITE_OK` in all application-facing cases.

The callback applies tuning only when the main database filename matches one of
the supported media-server database suffixes:

| Suffix | Target |
|---|---|
| `com.plexapp.plugins.library.db` | Plex |
| `/library.db` | Emby and any same-name current target |
| `/jellyfin.db` | Legacy JF-style filename filter; deployment unsupported |

Filter details:

- Empty filenames are ignored.
- In-memory, temporary, and attached-only cases are ignored.
- A `?` query suffix is stripped defensively before matching.
- Matching is case-sensitive.
- Very long paths that truncate inside the fixed buffer fail closed as filter
  misses.

The kill switch is:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
```

Only literal `1` disables the callback. Unset values and other strings leave
the callback active.

Read-only opens are skipped before mutating PRAGMAs are run:

```text
sqlite3_db_readonly(db, "main") == 1
```

The emitted PRAGMA block is:

```sql
PRAGMA cache_size=-1048576;
PRAGMA mmap_size=34359738368;
PRAGMA temp_store=2;
PRAGMA threads=8;
PRAGMA wal_autocheckpoint=16000;
PRAGMA journal_size_limit=67108864;
PRAGMA busy_timeout=10000;
PRAGMA analysis_limit=1024;
```

Per-connection injection sets `analysis_limit=1024`.
`scripts/optimize_media_servers.sh` exports `SQLITE3_DISABLE_AUTOPRAGMA=1` and
re-sets `analysis_limit=0` during planned-downtime runs so maintenance
`optimize()` calls execute without the per-connection ceiling.

The same callback recognizes Plex, Emby, and the legacy JF-style filename.
JF deployment is unsupported; the filter remains a library behavior, not a
current deployment promise. PMS issues its own per-connect PRAGMAs after 2-arg
`sqlite3_open` returns with implicit `SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE`,
so the injected settings are best-effort for Plex and sticky for Emby
connections that do not run an app-side PRAGMA pass. Open-time analysis remains
absent; runtime incremental optimization runs later on eligible close and inline
safe-idle paths, and planned-downtime maintenance remains the explicit full
database rebuild lifecycle.

`synchronous` is not set by the callback. The compile profile already gives WAL
databases the desired synchronous mode, and a connection-level PRAGMA would
also affect rollback-journal databases.

`optimize` is not set by the open callback. Runtime optimize is gated to later
safe-idle boundaries so Plex's per-connection ICU/collation/tokenizer
registration can already be present on the app-owned connection.

## Runtime Optimize

`src/runtime_optimize.c` exports hidden
`auto_extension_optimize_before_close(sqlite3 *db)` and
`auto_extension_optimize_after_stmt(sqlite3 *db, sqlite3_stmt *self_stmt)` for
the library-only amalgamation patch. `src/auto_extension.c` reaches runtime
optimize through the single hidden `runtime_optimize_seed_path(sqlite3 *db,
const char *raw_fn)` seam declared in `src/auto_extension_internal.h`, while
keeping open-time registration, trace-mask setup, target filtering, and the
TLS seam on its side. The mechanism is internal, variant-agnostic C. Plex adds
no runtime branch here; `LIBRARY_VARIANT=plex` remains limited to build-time
ICU linkage.

Runtime optimize runs on safe idle boundaries only:

- Internal immediate-close path, before SQLite converts the handle to zombie.
- Public `sqlite3_step()` when it returns `SQLITE_DONE`.
- Public `sqlite3_reset()` when it returns `SQLITE_OK`.
- Public `sqlite3_finalize()` when it returns `SQLITE_OK`.

The helpers never change public SQLite return codes and swallow optimize
failures after logging. NULL close, already-busy `sqlite3_close`, and busy
`sqlite3_close_v2` zombie paths retain native SQLite behavior.

Eligibility gates:

- `SQLITE3_DISABLE_AUTOPRAGMA=1` disables open-time autopragmas and all runtime
  optimize hooks.
- `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0` enables close and inline runtime
  optimize. Unset, literal `1`, and every other value disable runtime optimize.
- The main database filename must pass `auto_extension_path_is_target`.
- `sqlite3_db_readonly(db, "main") == 1` skips readonly and immutable opens.
- `sqlite3_get_autocommit(db) == 0` skips open transactions.
- Close requires `sqlite3_next_stmt(db, NULL) == NULL`.
- Inline allows prepared idle cached statements but skips when another
  statement is busy.

A process-local per-path registry stores separate successful LIMITED and FULL
cadence stamps plus inflight and failure-backoff state. LIMITED runs at most
once per target path per `SQLITE3_RUNTIME_OPTIMIZE_LIMITED_SECONDS` successful
cadence, default `1800`. FULL runs at most once per target path per
`SQLITE3_RUNTIME_OPTIMIZE_FULL_SECONDS` successful cadence, default `86400`.
A successful FULL pass also satisfies the LIMITED cadence. Failures set only a
short backoff and never stamp either cadence, so an early missing-collation
failure can retry later on a connection that has registered the required
collation or tokenizer. A due no-op can still spend work at a safe idle
boundary; SQLite's `PRAGMA optimize` remains the table-staleness gate.

Each enrolled target connection also carries a clientdata next-due cache for
its own path. Inline hot exits check that per-connection atomic value before
filename copy, readonly/autocommit checks, and the busy-peer scan, so a due
different target path does not force unrelated not-due target connections onto
the slower reserve path. When an enrolled connection is plausibly due but a
pre-reserve guard blocks it before shared path reservation, the hook pushes only
that connection's atomic next-due value out by 30 seconds. This re-arms the
cheap inline hot skip without touching shared per-path success cadence or
failure backoff state and bounds repeated O(statement-cache) blocked scans and
skip logs while the connection stays blocked; if per-connection state is
absent, the block keeps the old behavior and does not create re-arm state.

Execution tiers:

```sql
-- LIMITED
PRAGMA main.analysis_limit=1024;
PRAGMA main.optimize;

-- FULL
PRAGMA main.analysis_limit=0;
PRAGMA main.optimize=0x10002;
```

Both tiers save and restore the prior `analysis_limit` and the caller-visible
connection error state. Inline optimize also saves and restores the
application's progress handler and uses it as a deadline guard. The close
trigger keeps the close-only `sqlite3_busy_timeout(db, 1)` behavior. Runtime
optimize never runs `ANALYZE` directly, never optimizes attached schemas, and
never starts a background thread or separate connection. It uses the
application's own connection, so Plex ICU/collation/tokenizer registrations are
in scope when they exist.

On the optimize execution path, observability emits `runtime_optimize`
`event=optimize_start` before the `PRAGMA optimize` call, then
`event=optimize_done` on success or `event=optimize_failed` on failure. These
lines are gated by `SQLITE3_DISABLE_OBSERVABILITY=1` through the shared
observability sink and include `tier`, monotonic `elapsed_ms`, `stat_rows` from
the `sqlite3_total_changes64()` delta, and `since_last_ms` (`-1` when that tier
has no prior success). A due pre-reserve block on an enrolled connection emits
`event=optimize_skipped reason=readonly|open_txn|busy_peer` at the same point it
re-arms the connection-local next-due value, so repeated inline statements log
at most once per 30-second re-arm interval. Not-due hot skips emit no optimize
log.

Worst-case per-connection stall budget at a safe idle boundary is approximately:

- LIMITED: 23 ms.
- FULL: 4 s.

Observed stable STAT4 behavior: with any nonzero `analysis_limit`, SQLite's
`analyze.c` suppresses fresh STAT4 sampling. LIMITED therefore updates STAT1
only. FULL temporarily sets `analysis_limit=0` and uses
`PRAGMA main.optimize=0x10002`, so it can refresh STAT4 rows inline or at close
without changing the planned-downtime maintenance path.

## Observability Layer

`src/observability.c` is compiled into both shared-library variants and is
excluded from the CLI target.

The library target patches SQLite's amalgamation so these public APIs are
implemented by wrappers in `src/observability.c` while the original SQLite
implementations remain available as hidden `*_real` symbols:

- `sqlite3_initialize`
- `sqlite3_config`
- `sqlite3_db_config`
- `sqlite3_open`
- `sqlite3_open_v2`
- `sqlite3_open16`

Apart from lazy auto-extension registration in the open wrappers, the wrappers
are log-only. They call the hidden real implementation, return the real return
code unchanged, and log after the call with numeric `rc=N`. They do not mutate
config opcodes, db-config opcodes, open flags, filenames, handles, or return
codes.

The `sqlite3_open`, `sqlite3_open_v2`, and `sqlite3_open16` wrappers call
`auto_extension_register_for_open()` after observability initialization and
immediately before their hidden `_real` open implementation. The helper is
per-open and may trigger effective SQLite initialization before `openDatabase`
enters its own initialization path.

The observability kill switch is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1
```

Only literal `1` disables observability. The value is cached once during the
priority-101 observability constructor, with per-wrapper `pthread_once`
fallback. Disabled mode skips wrapper log emission and skips per-connection
`sqlite3_trace_v2` registration.

The statement-trace env knob is:

```text
SQLITE3_DISABLE_STMT_TRACE=0
```

Statement trace logging is off by default. Only literal `0` enables it; unset,
empty, `1`, and every other value disable it. The value is cached by the same
observability initialization path as the master kill switch. Disabled/default
mode omits `SQLITE_TRACE_STMT` from the per-connection trace mask; PROFILE-side
slow-query tracking is unaffected. When the resulting trace mask is `0`,
`sqlite3_trace_v2` is not called.

Log lines are written to stderr:

```text
[sqlite3-builds-obs] <ISO-8601-UTC-ms> <pid> <kernel-tid> <fn> key=value ...
```

Kernel thread IDs come from `syscall(SYS_gettid)`. SQL text emitted by statement
trace logging is capped at 3 KiB and receives a `...[TRUNC]` tail when
truncated.

`sqlite3_initialize` uses a thread-local depth counter so nested initialization
still calls the real implementation but only the outer 0-to-1 transition logs
failures where `rc != SQLITE_OK`. This absorbs helper-triggered initialization
followed by `openDatabase`'s initialization check without false failure logs.

`autopragma_init()` registers a unified trace callback before the AUTOPRAGMA
gate, with STMT added only when `SQLITE3_DISABLE_STMT_TRACE=0` and PROFILE
conditionally added when slow-query tracking is enabled. Observability is
orthogonal to PRAGMA injection: target filtering, read-only skips, and the
autopragma kill switch do not suppress observability logs. Trace registration
failure logs and continues; it must never make `sqlite3_open*` fail.

### Slow-Query Tracker

The same per-connection trace registration uses one unified callback with
`SQLITE_TRACE_STMT` added only when statement tracing is explicitly enabled and
`SQLITE_TRACE_PROFILE` added only when slow-query tracking is enabled. SQLite
allows one trace callback per connection, so the tracker shares the existing
registration site that runs before the connection escapes to application code.

The kill-switch hierarchy is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1 -> no observability or tracker work
SQLITE3_DISABLE_STMT_TRACE unset or !=0 -> STMT observability off, PROFILE tracker unaffected
SQLITE3_DISABLE_STMT_TRACE=0 -> STMT observability on when observability is enabled
SQLITE3_DISABLE_SLOW_QUERY=1 -> PROFILE tracker off; STMT follows SQLITE3_DISABLE_STMT_TRACE
```

The helper unconditionally calls `sqlite3_auto_extension` regardless of the
`SQLITE3_DISABLE_AUTOPRAGMA=1` state. An alternative -- early-return from the
helper when `SQLITE3_DISABLE_AUTOPRAGMA=1` -- was considered and rejected: it
would have skipped `sqlite3_trace_v2(SQLITE_TRACE_STMT)` registration in
`autopragma_init`, conflating AUTOPRAGMA disable with observability disable.
The accepted per-open mutex + dedup-scan cost preserves orthogonality between
the two kill switches.

`SQLITE3_SLOW_QUERY_THRESHOLD_MS` defaults to `500`. The parser accepts only
unsigned decimal milliseconds, allows `0` for debug-only all-sample logging,
and falls back to `500` for unset, empty, signed, non-digit, trailing-junk,
ERANGE, or pre-conversion overflow values. PROFILE elapsed values come from
SQLite's millisecond-quantized timing source and are scaled to nanoseconds by
SQLite before the callback receives them.

The tracker keeps a process-global 2048-entry LRU keyed by parameterized
`sqlite3_sql()` text. It stores and displays at most 1024 SQL bytes per
template, uses the full SQL FNV-1a hash as the probe accelerator, and uses that
full hash as the equality proof for templates longer than 1024 bytes. Existing
STMT logging keeps its separate 3 KiB SQL cap; `SLOW_QUERY_SQL_CAP` does not
change `OBS_SQL_CAP`, and config/db-config decode tables stay in
`src/observability.c`.

LRU evictions tombstone the bucket entry and trigger an amortized O(1) bucket
rebuild when tombstones reach 25% of entries_used.

Stats are retained as count, sum, sum-of-squares, min, and max. Supported
compilers use `unsigned __int128` accumulators; fallback compilers use
`uint64_t` with a per-template sample cap and one warning per capped template.
The dump path heap-allocates value snapshots, copies under the tracker mutex,
sorts by descending total elapsed time, and emits `slow_query_stats` lines
after releasing the mutex only when `mean_ns >= threshold_ns` and `count >= 5`.
Stats fields `mean_ms`, `stddev_ms`, `min_ms`, and `max_ms` render with six
digits after the decimal point.

Dumps occur at normal process exit and at most every five minutes during
PROFILE activity. The atexit path sets an atomic in-exit flag first, so later
PROFILE callbacks return before touching SQLite pointers or the mutex, and it
uses `pthread_mutex_trylock`; on contention it emits a one-line skip diagnostic
instead of stalling process shutdown.

The tracker is an observability satellite that depends on the hidden
`obs_logf` / `obs_is_disabled` ABI in this MVP. A Phase 2 sink-abstraction
layer is deferred until runtime evidence justifies persistent or alternate
export paths.

### Slow-Query Verification

Docker build compiles + runs `slow_query_smoke`, `slow_query_atexit_smoke`,
`slow_query_concurrency_smoke`. CI re-runs positive, disabled-mode, atexit,
and concurrency checks, and it compiles + runs `stmt_trace_smoke` against the
same registration path to enforce the STMT trace env contract. Bench binaries
(`slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`,
`alloc_latency_bench`, and `runtime_optimize_close_bench`) compile
unconditionally. The Dockerfile runs `slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`, and
`alloc_latency_bench` as advisory steps on every library build: nonzero exits
print an `ADVISORY FAIL` line and continue. The generic library image also runs
`runtime_optimize_close_bench` as advisory output; it prints p50, p95, p99, max,
and pass/fail verdicts, then exits 0 so benchmark drift never fails CI.

## Smoke Tests

### `auto_extension_smoke`

`tests/auto_extension_smoke.c` links against the just-built library inside the
library Docker image.

It proves:

- Plex, Emby, and legacy JF-style database filenames hit the filter.
- A non-target filename misses the filter.
- Literal kill-switch value `1` disables tuning.
- A non-`1` kill-switch value does not disable tuning.
- Read-only opens skip tuning.
- Overlong paths that truncate inside the filter buffer miss the filter instead
  of receiving PRAGMA tuning.
- Constructor-only `SQLITE_CONFIG_SORTERREF_SIZE` and `SQLITE_CONFIG_PMASZ`
  both report `SQLITE_OK` before first SQLite initialization.
- Observable PRAGMA values match the active and disabled profiles.
- Setup and SQLite API failures print concrete diagnostics.

The smoke checks `busy_timeout`, `cache_size`, `mmap_size`,
`wal_autocheckpoint`, `journal_size_limit`, `threads`, and `temp_store` where
SQLite exposes useful runtime state.

Execution points:

- Generic library Docker build.
- Plex library Docker build with Plex ICU paths in the loader environment.

### `runtime_optimize_smoke`

`tests/runtime_optimize_smoke.c` links against the just-built library inside the
library Docker image.

It proves:

- Eligible target close and inline boundaries run bounded runtime optimize.
- FULL writes STAT1 and STAT4 rows; LIMITED writes STAT1 only.
- Inline optimize fires from `sqlite3_step` DONE, `sqlite3_reset` OK, and
  `sqlite3_finalize` OK before connection close.
- `analysis_limit` and the application progress handler are restored after
  inline optimize success and failure.
- Caller-visible connection error state is preserved after a swallowed inline
  optimize failure.
- Runtime optimize re-entrancy is blocked.
- A busy peer cursor suppresses inline optimize for that attempt, emits one
  throttled `event=optimize_skipped reason=busy_peer`, and re-arms the connection-local
  hot skip so repeated blocked attempts re-scan no more than once per re-arm interval.
- Missing ICU collation failures are swallowed, back off briefly, and do not
  cadence-stamp success.
- Non-target paths do not run runtime optimize.
- `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0` enables runtime optimize.
- Unset `SQLITE3_DISABLE_RUNTIME_OPTIMIZE`, literal `1`, and every other value
  disable runtime optimize.
- `SQLITE3_DISABLE_AUTOPRAGMA=1` also disables runtime optimize.
- Readonly/immutable target opens skip runtime optimize.
- Non-autocommit target closes skip runtime optimize.
- `sqlite3_close` with an open statement still returns `SQLITE_BUSY` and skips
  runtime optimize.
- `sqlite3_close_v2` with an open statement still enters the native zombie path
  and skips runtime optimize.
- The per-path cadence suppresses immediate repeated successful target attempts.
- Runtime optimize still works after `sqlite3_shutdown()` and a later reopen.

Execution points:

- Generic library Docker build.
- Plex library Docker build with Plex ICU paths in the loader environment.

### `config_after_dlopen_smoke`

`tests/config_after_dlopen_smoke.c` links against the just-built library inside
the library Docker image.

It proves:

- `SQLITE_CONFIG_MULTITHREAD` and `SQLITE_CONFIG_MEMSTATUS` still return
  `SQLITE_OK` after library load and before first intentional open.
- A target-matched first open still receives the active PRAGMA profile.
- Three fresh-process concurrent first opens, one per public open wrapper
  (`sqlite3_open`, `sqlite3_open_v2`, `sqlite3_open16`), all succeed and
  receive the active PRAGMA profile.

### `shutdown_reinit_smoke`

`tests/shutdown_reinit_smoke.c` links against the just-built library inside the
library Docker image.

It proves:

- A target-matched first open receives the active PRAGMA profile.
- After `sqlite3_shutdown()`, a later public open path re-registers the lazy
  auto-extension and receives the active PRAGMA profile again.

### `icu_smoke`

`tests/icu_smoke.c` exists for the Plex variant.

It proves:

- `icu_load_collation()` is available.
- A root numeric ICU collation can be registered as `icu_root`.
- A query using `COLLATE icu_root` prepares and executes.
- The comparator path returns the expected row order for a small mixed-case
  dataset.

Execution points:

- In `docker-library/Dockerfile` against the ICU built inside the Docker image.
- In CI after Plex artifact extraction, mounted into the pinned Plex container
  and run under Plex's bundled runtime loader.

The bind-mount smoke exercises the same runtime closure as deployment: Plex's
bundled `ld-musl-*.so.1` loader resolves the produced library against Plex's
renamed ICU.

The CI stage also verifies that the smoke binary resolves `libsqlite3.so` to
the just-built artifact mounted at Plex's deployed library path rather than a
system library.

The CI runtime smoke derives the expected ICU major from the Plex compatibility
group and runs against the group's smoke server image from the support window.
The stage-2 step bind-mounts the extracted Plex artifacts into that container,
replacing `/usr/lib/plexmediaserver/lib/libsqlite3.so` for the smoke process and
mounting the extracted `icu_smoke` binary read-only. The smoke is invoked
through Plex's bundled `ld-musl-*.so.1` loader with Plex's library directory as
the loader path, so `libsqlite3.so` and Plex-renamed ICU resolution use the same
runtime closure as deployment.

### PMS first-init smoke

The CI PMS first-init smoke runs after Plex artifact extraction.

It proves the extracted Plex `libsqlite3.so` can replace PMS's runtime library
at `/usr/lib/plexmediaserver/lib/libsqlite3.so` and survive PMS first
initialization against a fresh `/config`.

The smoke runs for each supported Plex server row. It starts PMS with the
extracted Plex library bind-mounted read-only over the deployed library path and
waits for readiness by probing TCP port 32400 for up to 90 seconds.

After readiness, the smoke copies
`com.plexapp.plugins.library.db` from PMS's config tree and verifies the runner
`sqlite3` can see at least 20 tables. The workflow prints full PMS logs and
fails when logs contain any of: `SQLITE_`, `database disk image is malformed`,
`Cannot create table`, `no such function`, `no such collation`, or
`Failed to open`.

The smoke sets `SQLITE3_DISABLE_STMT_TRACE=0` and requires observability marker
lines for an open wrapper and `SQLITE_TRACE_STMT`. Positive-mode logs must not
contain startup `sqlite3_config` operations returning `rc=21` (CI pattern:
`[sqlite3-builds-obs] .* sqlite3_config op=SQLITE_CONFIG_[A-Z0-9_]+ rc=21`).
A duplicate disabled-mode smoke starts PMS with
`SQLITE3_DISABLE_OBSERVABILITY=1` and fails if any
`[sqlite3-builds-obs]` line is emitted.

### Emby first-init smoke

The CI Emby first-init smoke runs after generic library artifact extraction.

It proves the extracted generic `libsqlite3.so` can replace Emby's bundled
runtime library at `/app/emby/lib/libsqlite3.so.3.49.2` and survive Emby first
initialization against a fresh `/config`.

The smoke runs for each supported Emby server row. It starts Emby with the
extracted generic library bind-mounted read-only over the deployed library path
and waits for readiness by polling startup logs for the prefix-agnostic
`Sqlite version:` message for up to 90 seconds.

After readiness, the smoke verifies the logged SQLite version equals the
workflow `SQLITE_VERSION_DOTTED`. It also prints Emby's logged SQLite compiler
options and verifies the
16 curated `compile_options` used by the tuned library are present, including
`SORTER_PMASZ=8192`. The workflow prints full Emby logs and fails when logs
contain the same bad-signal set as §PMS first-init smoke.

The smoke sets `SQLITE3_DISABLE_STMT_TRACE=0` and requires observability marker
lines for an open wrapper and `SQLITE_TRACE_STMT`. Positive-mode logs must not
contain startup `sqlite3_config` operations returning `rc=21` (CI pattern:
`[sqlite3-builds-obs] .* sqlite3_config op=SQLITE_CONFIG_[A-Z0-9_]+ rc=21`).
A duplicate disabled-mode smoke starts Emby with
`SQLITE3_DISABLE_OBSERVABILITY=1` and fails if any
`[sqlite3-builds-obs]` line is emitted.

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
```

Plex also installs:

```text
/opt/sqlite3-lsio-mod/lib/plex-pool-patch.sh
```

`baked-pins.txt` is the only runtime SHA source. It uses schema v3 rows:
`meta`, `detect`, `artifact`, `pre`, `pool-site`, and `unsupported`. The
manifest carries detector sets for per-version detector selection, group-aware
artifact paths, target runtime baselines, Plex ICU runtime baselines, and Plex
pool-site byte contexts.

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
  -> init-mod-sqlite3-poolpatch     (Plex)
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

Phase 04 pool patch runs only in the Plex mod. On amd64 and arm64, the phase
first verifies that the SQLite target is already current, then calls the staged
`plex-pool-patch.sh` fragment with all paths, expected SHAs, and patch sites as
arguments. The fragment reads no environment variables and carries no SHA
constants.

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
curated source rows live in `pins/plex-pool-patch-sites.tsv`; rendered runtime
rows live as `pool-site` rows in `baked-pins.txt`.

Pool-patch per-binary behavior:

| Site state | Baseline SHA / backup state | Action |
|---|---|---|
| All sites patched | Any | Skip; already patched |
| All sites original | Current binary SHA matches selected pristine detector SHA; backup absent | Copy backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| All sites original | Current binary SHA matches selected pristine detector SHA; verified backup present | Reuse backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| Mixed or unknown | Any | Warn and skip that binary |
| Write, verify, or atomic replace failure | Verified backup present | Leave target unchanged, warn, and continue to the next binary |

Runtime command surface:

| Scope | Commands |
|---|---|
| Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |
| Plex pool patch | `dd od printf` |

LSIO runtime has no dependency on `curl`, `tar`, `gunzip`, Python, `jq`,
package managers, or network access.

## Maintenance Architecture

`scripts/optimize_media_servers.sh` is a host-side planned-downtime maintenance
script for Plex and Emby containers.

It starts with:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
```

That keeps maintenance connections from receiving the library's automatic
PRAGMA block.

### Invocation and Config

Run with no arguments to maintain configured instances. `-h` and `--help`
print usage and exit 0 before any config load. Any other argument is rejected
with a short error and `--help` pointer before config load.

The script resolves its sourced Bash config as
`${OPTIMIZE_MEDIA_SERVERS_CONF:-<script-dir>/optimize_media_servers.conf}`.
The default path is `scripts/optimize_media_servers.conf` beside the script;
`scripts/optimize_media_servers.conf.example` is the committed template.
The resolved config must exist and source successfully before any binary
preflight, Docker operation, curl request, or database work.

Config load happens inside `main()` only. Sourcing the script for tests or
helper reuse does not load the config and does not fail on a missing config.

The side-car config is the sole source of these operator variables:

```text
PLEX_INSTANCES
EMBY_INSTANCES
PLEX_BINARY
GENERIC_SQLITE_BINARY
BACKUP_PATH
PLEX_OPTIMIZE_API
PLEX_PROCESS_BLOB_DB
STATS_BANDWIDTH_RETAIN_DAYS
```

`OPTIMIZE_MEDIA_SERVERS_CONF` is only the external path selector and is not
stored in the config file.

Each instance array entry is both the Docker container name and the
`/opt/<instance>` path stem:

```text
PLEX_INSTANCES=("plex" "plex-4k")
EMBY_INSTANCES=("emby")
```

For Plex, the database path resolves under
`/opt/<instance>/Library/Application Support/Plex Media Server/...`. For Emby,
the database path resolves under `/opt/<instance>/data/...`.

The generic CLI consumed by `GENERIC_SQLITE_BINARY` is staged from the build
output `release/cli/sqlite3` to `${HOME}/bin/sqlite3`. Plex maintenance uses
container-copied runtime files: `PLEX_BINARY` points at
`${HOME}/plex-sql/Plex SQLite`, and `${HOME}/plex-sql/lib/` holds the Plex
container's `/usr/lib/plexmediaserver/lib/` copy. If that copied `lib/` came
from a modded container, restore `libsqlite3.so.bundled.bak` over
`${HOME}/plex-sql/lib/libsqlite3.so` in the staging copy before running
maintenance. Copying the `Plex Media Server` sibling into `${HOME}/plex-sql/`
is recommended staging for Plex's source-id guard, but the script only
executes `PLEX_BINARY` (`Plex SQLite`); the sibling is not a code-proven
runtime dependency of the helper. The built `release/library-plex/libsqlite3.so`
is a separate Plex library-replacement output, not a maintenance input.

`scripts/optimize_media_servers.sh` owns the container lifecycle for configured
Plex and Emby instances. Each instance with a present database directory is
stopped before maintenance, verified stopped with
`docker ps --filter ... status=running`, started after the maintenance section,
and verified running with the same Docker-status idiom. Missing instance data
is skipped before any stop. Stop, maintenance, start, and start-verification
failures set the final process status nonzero for that instance and the script
continues to later instances. Once `docker stop` succeeds, the script attempts
`docker start` on every maintenance failure path before moving on.

`PLEX_OPTIMIZE_API` defaults to `0` in the config template. Literal
`PLEX_OPTIMIZE_API=1` enables an optional post-start Plex API trigger inside
the per-Plex-instance loop. For each restarted Plex instance, the script
resolves the container IP with `docker inspect`, waits up to 60 seconds for
`GET http://<container-ip>:32400/identity` to return HTTP 200, reads
`PlexOnlineToken` from
`/opt/<instance>/Library/Application Support/Plex Media Server/Preferences.xml`,
sends one `PUT http://<container-ip>:32400/library/optimize?async=1` request
with the token only in the `X-Plex-Token` header, and waits by polling
`/activities` for `type="general.db.optimize"`.

If `/activities` already reports `type="general.db.optimize"` before submit,
the trigger reports the instance as already running and does not send the PUT.
After submit, completion is credited only when that activity is observed and
then absent on consecutive successful polls. If Plex exposes a stable activity
`uuid` or `id`, the wait tracks that activity. The PUT is never retried. Failed
or indeterminate polls continue until the wall-clock five-minute wait cap, with
each curl request capped to the remaining deadline. Per-instance trigger
failures warn and continue, and the final process exits nonzero only when
`PLEX_OPTIMIZE_API=1` was attempted and no Plex instance reached accepted,
already-running, or completed.

Maintenance controls:

| Control | Default/example | How to change | Effect |
|---|---|---|---|
| `PLEX_INSTANCES` | `("plex" "plex-4k")` | Set in the config file | Plex Docker container names and `/opt/<instance>` stems. |
| `EMBY_INSTANCES` | `("emby")` | Set in the config file | Emby Docker container names and `/opt/<instance>` stems. |
| `PLEX_BINARY` | `${HOME}/plex-sql/Plex SQLite` | Set in the config file | Runs Plex maintenance with Plex's ICU-enabled SQLite binary. |
| `GENERIC_SQLITE_BINARY` | `${HOME}/bin/sqlite3` | Set in the config file | Runs Emby maintenance and the Plex main-DB STAT4 pass. |
| `BACKUP_PATH` | `/mnt/media-backup` | Set in the config file | If the path exists, backups publish under an instance-specific subdirectory there; otherwise backups stay beside the source database. |
| `PLEX_OPTIMIZE_API` | `0` | Set in the config file | Literal `1` enables the optional post-start Plex `PUT /library/optimize?async=1` trigger. |
| `PLEX_PROCESS_BLOB_DB` | `0` | Set in the config file | Literal `1` enables the optional Plex blob database rebuild pass. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | `90` | Set in the config file | Plex `statistics_bandwidth` deflate keeps only rows with an account id and inside this retention window. |
| `_PAGE_SIZE` | `16384` | Edit the in-script `_PAGE_SIZE` constant | Rebuilt Plex and Emby databases target 16 KiB pages. |
| Plex `statistics_bandwidth` deflate | Enabled for the Plex main database when the table exists | Controlled by `STATS_BANDWIDTH_RETAIN_DAYS` | Runs on the staged database before swap; DELETE or VACUUM failures warn, but a post-deflate `integrity_check` failure aborts before touching the live DB. |

### Planned-Downtime Gate

For each configured Plex or Emby instance, the script checks for the expected
instance data directory before stopping. Missing data skips without a
stop/start cycle. For instances with data, the script runs `docker stop`,
queries Docker for an exact container name with `status=running`, and skips
maintenance if the stopped verification still sees the container running or the
Docker query fails. After a successful stop, the script runs `docker start`
after maintenance succeeds or fails, verifies the container reaches running, and
sets final status nonzero for stop, maintenance, start, or verification
failures.

### Plex Flow

For each Plex instance, the script resolves the data and database paths, cleans
selected caches, runs the sanity query, hard-gates source
`PRAGMA integrity_check == ok`, hard-gates source FTS integrity, warns on
foreign-key rows, switches the source to `journal_mode=DELETE`, and publishes a
dated `.original` backup.

The rebuild runs under `PLEX_BINARY` (`Plex SQLite`) with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and `VACUUM INTO`
to a same-directory staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, staged `integrity_check == ok`, staged FTS integrity,
and an exhaustive source-vs-staged per-table row-count sweep before any swap is
attempted. If the main Plex database has `statistics_bandwidth`, the pre-swap
hook can deflate it inside the staged file with `PRAGMA temp_store=MEMORY`,
`PRAGMA threads=8`, and `VACUUM`, then hard-gates post-deflate
`integrity_check`.

After the rebuild and validation sweep, the staged Plex path runs FTS rebuild,
the staged FTS integrity gate, FTS re-curation, staged optimize SQL, the staged
metadata date-repair SQL, the Plex main-DB STAT4 pass, and, when that STAT4
pass is enabled, a post-STAT4 `integrity_check` gate before the
`user_version`/`application_id` preservation gate and atomic replacement.
Post-swap FTS maintenance is optimize-only.
`PLEX_PROCESS_BLOB_DB=1` enables the same staged rebuild for the Plex blob
database without the `statistics_bandwidth` hook, metadata date repair, or
STAT4 pass. Plex SQLite is required because Plex data can require the
ICU-enabled binary.

The Plex main-database staged optimize SQL creates the runbook-validated
`idx_dshadow_taggings_tag_id_metadata_item_id` and
`idx_dshadow_mis_account_updated_guid_cover` indexes before `REINDEX`,
`ANALYZE`, and `PRAGMA optimize`. The existing Plex SQLite `ANALYZE` remains as
the STAT1 floor for ICU-collated or skipped objects. The subsequent STAT4 pass
uses `GENERIC_SQLITE_BINARY`, runs only after the Plex staged SQL, discovers
safe targets from `pragma_index_list`/`pragma_index_xinfo`, skips unsupported
collations and unsafe identifier transport, sets `analysis_limit=0`, and warns
without aborting on preflight, discovery, per-target `ANALYZE`, or final
`sqlite_stat4` row-count failures. `main()` derives the internal STAT4 gate
from `GENERIC_SQLITE_BINARY` preflight for configured Plex instances, so the
pass runs only when that binary reports `ENABLE_STAT4`.

### Emby Flow

For each Emby instance, the script resolves `/opt/<instance>/data`, runs the
sanity query, hard-gates source `integrity_check == ok`, hard-gates source
FTS integrity, warns on foreign-key rows, switches the source to
`journal_mode=DELETE`, and publishes a dated `.original` backup.

The rebuild runs under `GENERIC_SQLITE_BINARY` with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and
`VACUUM INTO` to a staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, staged `integrity_check == ok`, staged FTS integrity,
an exhaustive source-vs-staged per-table row-count sweep, FTS rebuild, the
staged FTS integrity gate, FTS re-curation, staged optimize SQL, and the
`user_version`/`application_id` preservation gate before the script replaces the
live database with `mv` and removes stale WAL/SHM siblings.
The Emby staged optimize SQL creates the runbook-validated
`idx_dshadow_mediaitems_parent_type` index before `REINDEX`, `ANALYZE`, and
`PRAGMA optimize`.

Post-swap FTS maintenance is optimize-only because the database has already
passed the source and staged validation gates.

### Maintenance Posture

Before per-instance work, hard failures include unsupported arguments, absent
resolved config, and config source failure. Hard per-instance failures include
Docker stop/start verification failure, Docker query failure, running
containers after stop, source integrity failure, source FTS integrity failure,
`journal_mode=DELETE` failure, backup publication failure, `VACUUM INTO`
failure, staged auto-vacuum mismatch against `NONE`, staged integrity failure,
staged FTS integrity failure, per-table row-count mismatch, pre-swap hook
integrity failure, and post-STAT4 staged integrity failure. After a successful
stop, these failures still fall through to the start gate before the script
continues. The source and staged integrity gates require the literal `ok`
result from
`PRAGMA integrity_check`.

Warning-and-continue cases include source `foreign_key_check` rows, skipped
missing instance directories, Plex `statistics_bandwidth` table absence or
DELETE/VACUUM failure before a clean post-deflate integrity result, staged
maintenance SQL failure, staged metadata date-repair failure, Plex STAT4
preflight/worklist/per-target/final-count failures, and post-swap FTS
maintenance failures.

## Hard Constraints

Plex ICU:

- Plex's renamed ICU 69 files are runtime dependencies.
- LSIO mod code must not replace, delete, move, rename, or overwrite
  `libicu*plex.so.69`; it may only read and verify those files.
- The Plex variant may link to those files but only replaces `libsqlite3.so`.
- The Plex variant links `-licuucplex -licui18nplex -licudataplex` inside
  `-Wl,--push-state,--no-as-needed` and `-Wl,--pop-state` so modern binutils
  default `--as-needed` does not drop the ICU `DT_NEEDED` entries.
- The Plex variant carries `-Wl,-z,defs` so unresolved ICU symbols fail at link
  time rather than at runtime.

Variant gating:

- `LIBRARY_VARIANT` defaults to `generic`.
- `LIBRARY_VARIANT=plex` requires the SQLite full source URL and SHA.
- The Plex variant is the only build path that downloads SQLite full source.
- The generic variant does not link ICU.

SHA pins:

- SQLite amalgamation pins drive CLI and both library variants.
- SQLite full-source pins drive only the Plex variant.
- ICU pins drive only the Plex variant.
- Workflow, local wrapper, Dockerfiles, and `Build.sh` must stay aligned.

Release artifacts:

- Release archive names must match workflow release output.
- Library archives must have extracted-`.so` SHA entries in `SHA256SUMS`.
- LSIO mod runtime verification uses `baked-pins.txt`, not release archive
  basename rules.

Plex target path:

- Plex library replacement target is exactly
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- Plex ICU files in the same directory are not deploy targets.

LSIO tool surface:

- Perform no runtime archive download or extraction.
- Common phases use `awk`, `chmod`, `chown`, `cp`, `grep`, `mkdir`, `mktemp`,
  `mv`, `rm`, `sed`, `sha256sum`, `stat`, `tr`, and `uname`.
- Plex pool patch additionally uses `dd`, `od`, and `printf`.
- Do not depend on `curl`, `tar`, `gunzip`, Python, `jq`, package managers, or
  network access at runtime.

Auto-extension:

- Tuning must never block an application database open.
- The kill switch must remain literal `SQLITE3_DISABLE_AUTOPRAGMA=1`.
- Read-only opens must stay quiet.
- Maintenance must set the kill switch.

Observability:

- Observability wrappers must chain to the hidden real SQLite implementation
  and return its result unchanged.
- Observability is log-only in the initial pass; it must not inject config,
  db-config, open-flag, filename, handle, or return-code behavior.
- Trace registration failure must log and continue; it must never fail
  `sqlite3_open*`.
- Observability log emission is independent of target filtering, read-only
  filtering, and `SQLITE3_DISABLE_AUTOPRAGMA`.
- The observability kill switch must remain literal
  `SQLITE3_DISABLE_OBSERVABILITY=1`.
- CLI builds must not carry the observability wrapper layer.
- CLI builds must not carry runtime optimize.

M-series invariants:

- M7: Source-level amalgamation patching via a checked-in unified-diff `.patch`
  file applied with `patch -p1` after unzip; SQLite version bumps that change
  any of the six wrapped API target signatures or the internal runtime optimize
  hook hunks cause `patch` to reject hunks and fail the build with patch's
  native error message. `build/sqlite-amalgamation.patch` is the single locus of
  amalgamation modifications.

Baked pin manifest:

- `baked-pins.txt` uses schema v3 with metadata row
  `meta|3|release_tag|<tag>|generated_at|<iso8601-utc>`.
- It contains `detect`, `artifact`, `pre`, `pool-site`, and `unsupported` rows.
- `artifact` rows use
  `artifacts/<arch>/<compat_group>/libsqlite3.so`.
- Per-version detector selection chooses the server id before target-specific
  verification or mutation.
- Missing image digest evidence for a supported runtime image is a CI failure.
- Runtime SHA mismatch has no bypass environment variable.
- LSIO mod images are the authoritative SQLite replacement artifact for a
  release tag.

## Out of Scope

JF deployment is unsupported until a current design and validation plan land. JF
maintenance is absent; adding it requires design and validation before use.
