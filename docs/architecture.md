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
- Host scripts for planned-downtime database maintenance.

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
| `docker-library/Dockerfile` | Shared-library build image: Ubuntu/glibc for the generic variant and Alpine/musl for the Plex variant. |
| `src/auto_extension.c` | Built into both library variants; registers per-connection PRAGMA tuning. |
| `src/slow_query_tracker.c` | Hidden observability satellite for PROFILE-based slow-query logging and bounded per-template stats. |
| `tests/auto_extension_smoke.c` | Runtime smoke for filter, kill switch, read-only skip, and emitted PRAGMAs. |
| `tests/slow_query_smoke.c` | Runtime smoke for the slow-query tracker threshold parser, kill switches, LRU bound, truncation, and stats dump. |
| `tests/config_after_dlopen_smoke.c` | Runtime smoke proving startup-only config remains legal after library load and before first open. |
| `tests/shutdown_reinit_smoke.c` | Runtime smoke proving lazy auto-extension registration survives `sqlite3_shutdown()` and later reopen. |
| `tests/icu_smoke.c` | Runtime smoke for Plex ICU collation registration and comparator use. |
| `tests/render_lsio_mod_baked_pins_test.sh` | Unit tests for `tools/lsio-mod/render-lsio-mod-baked-pins.sh`: current rows, pre rows, Plex ICU rows, pool-patch baselines, unsupported rows, and malformed-input rejection. |
| `tests/cont_init_fragments_test.sh` | Static and unit checks for LSIO mod runtime fragments, phase-script shebangs, no custom env-var surface, and Plex ICU read-only posture. |
| `tests/sqlite_build_workflow_mod_only_test.sh` | Static workflow check for minimal release assets and split `mod-build` / `mod-publish` jobs. |
| `tests/check_obs_counts.sh` | Pre-build lint: counts `SQLITE_CONFIG_` and `SQLITE_DBCONFIG_` decode entries in `src/observability.c` against `build/expected-sqlite-*-count.txt`. |
| `tests/check_pin_alignment.sh` | Pre-build lint: asserts mimalloc VERSION + URL + SHA512 alignment and ICU workflow/Dockerfile alignment. |
| `tests/alloc_latency_bench.c` | Advisory `sqlite3_malloc` / `sqlite3_free` microbench compiled in library images and executed only with `RUN_BENCH=1`. |
| `build/libsqlite3-version-script.ld` | Library-only linker version script: pinned public `sqlite3` API exports from `sqlite3.h` plus project-required extras, then `local: *;`. |
| `tools/lsio-mod/render-lsio-mod-baked-pins.sh` | Host-runnable renderer for per-mod `baked-pins.txt` runtime SHA data. |
| `tools/lsio-mod/stage-lsio-mod.sh` | Local and CI staging helper that assembles an ephemeral LSIO mod Docker context under `mktemp -d`. |
| `lsio-mods/` | Source-of-truth Plex and Emby Docker mod roots, shared runtime fragments, and parent README. |
| `lsio-mods/shared/cont-init-fragments/plex-pool-patch.sh` | Args-only shared Plex pool-patch core staged into the Plex mod. |
| `pins/plex-pool-patch-baselines.txt` | PMS and Plex Media Scanner baseline SHA source data rendered into Plex `baked-pins.txt`. |
| `scripts/optimize_media_servers.sh` | Planned-downtime maintenance helper for Plex and Emby databases. |
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
| Emby | .NET P/Invoke into bundled SQLite | `/app/emby/lib/libsqlite3.so.3.49.2` | `generic` | Active |
| JF | .NET SQLitePCLRaw name lookup | `/usr/lib/jellyfin/bin/libe_sqlite3.so` | `generic` | Unsupported / out of scope |

See §Out of Scope.

## Library Variants

Two shared-library variants are built from the same source tree.

| Variant | Build selector | Added source or linkage | Runtime purpose |
|---|---|---|---|
| `generic` | default `LIBRARY_VARIANT=generic` | Amalgamation plus `src/auto_extension.c` | Emby |
| `plex` | `LIBRARY_VARIANT=plex` | Generic source plus SQLite `ext/icu/icu.c`, `SQLITE_ENABLE_ICU`, and Plex-style ICU libs | Plex |

The intentional variant delta is ICU support for Plex.

Both library variants also link mimalloc v3.3.2 by link-time interposition;
the static CLI remains on the platform allocator.

Everything else is shared: the SQLite amalgamation pin, auto-extension source,
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

The source of truth for version and image pins is `pins/versions.env`; the
local wrapper sources it and the workflow loads it into `$GITHUB_ENV`.

| Input | Current value |
|---|---|
| SQLite version | `3530200` |
| SQLite release year | `2026` |
| Amalgamation URL | `https://www.sqlite.org/2026/sqlite-amalgamation-3530200.zip` |
| Amalgamation SHA3-256 | `81142986038e18f96c4a54e1a72562ae17e502a916f2a7701eff43388cbf1a40` |
| Full source URL | `https://www.sqlite.org/2026/sqlite-src-3530200.zip` |
| Full source SHA3-256 | `490ec7af32a6bfa5f3e05dc279c04286cfe3f328def4a8b7344e3fa20be18a4c` |

The Plex library image also pins ICU 69.1:

| Input | Current value |
|---|---|
| ICU tarball | `https://github.com/unicode-org/icu/releases/download/release-69-1/icu4c-69_1-src.tgz` |
| ICU SHA-512 | `d4aeb781715144ea6e3c6b98df5bbe0490bfa3175221a1d667f3e6851b7bd4a638fa4a37d4a921ccb31f02b5d15a6dded9464d98051964a86f7b1cde0ff0aab7` |
| ICU build prefix | `/opt/icu-69-plex` |
| ICU configure suffix | `--with-library-suffix=plex` |

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

For `library`, it builds `sqlite3.c` plus `/app/auto_extension.c`, links a
shared object, applies library-only feature defaults, and writes
`dist/libsqlite3.so`.

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
build then compiles `sqlite3.c`, `/app/auto_extension.c`,
`/app/observability.c`, and `/app/slow_query_tracker.c`; the CLI target does
not run this patch and does not include observability.

After the library compile, `build/Build.sh` checks every
`SQLITE_CONFIG_*` and `SQLITE_DBCONFIG_*` define in the pinned `sqlite3.h`
against the decode tables in `/app/observability.c`. Missing table entries are
build failures.

Adjacent to the preserved `sqlite3_*_real` and lazy-helper symbol gates,
post-link checks now fail on leaked `mi_*` or malloc-family exports, require
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

`docker-library/Dockerfile` uses an Ubuntu/glibc base for the generic variant
and an Alpine/musl base for the Plex variant.

It installs the shared-library build toolchain and copies `build/Build.sh`,
`src/auto_extension.c`, `tests/`, `build/sqlite-amalgamation.patch`,
`build/expected-sqlite-config-count.txt`, and
`build/expected-sqlite-dbconfig-count.txt` into `/app`, with the build files
under `/app/build/`.

The same Dockerfile builds mimalloc v3.3.2 in a dedicated stage, installs it
under `/opt/mimalloc`, writes `/opt/mimalloc/SHA512`, and exports
`MIMALLOC_OBJ` plus `MIMALLOC_LIB` into `Build.sh`. The Plex branch adds
`-DMI_LIBC_MUSL=ON`; the generic branch does not.

For the generic variant, it builds the shared library and runs the new
config-after-dlopen, shutdown/reinit, and auto-extension smokes.

For the Plex variant, it also builds ICU 69.1 under `/opt/icu-69-plex`, exposes
`PLEX_ICU_INCLUDE` and `PLEX_ICU_LIB`, checks ICU soname and symbol shape with
`ldd` and `nm`, runs `icu_smoke`, and runs the config-after-dlopen,
shutdown/reinit, and auto-extension smokes with the Plex ICU path available. The
Plex branch builds under Alpine/musl so the resulting library does not carry
glibc-versioned symbols such as `fcntl64`, which Plex's `libgcompat` shim does
not provide.

For the Plex variant, the build stage fails if the produced `libsqlite3.so`
carries `fcntl64` or any other glibc-versioned symbol.

### Workflow Matrix

`.github/workflows/sqlite-build.yml` builds on pushes to `main`,
digit-prefixed tag pushes matching `[0-9]*`, pull requests to `main`, and
manual dispatch. The release job validates `YYYY.MM.DD-rN` before archive
assembly and release asset publication; `mod-build` and `mod-publish` handle
LSIO mod images.

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

The build job no longer emits deploy pre-row fragments. Mod runtime SHA data is
rendered later by `tools/lsio-mod/render-lsio-mod-baked-pins.sh` from same-run build
artifacts, same-run runtime baseline extraction, and
`pins/plex-pool-patch-baselines.txt`.

The workflow keeps observability count and ABI obsolete-config checks after
runtime smokes. Static coverage for the LSIO mod direction lives in
`tests/render_lsio_mod_baked_pins_test.sh`,
`tests/cont_init_fragments_test.sh`, and
`tests/sqlite_build_workflow_mod_only_test.sh`.

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

The library profile also pins `-DSQLITE_SORTER_PMASZ=8192` in `build/Build.sh`,
up from the repository's prior 1024-page tuning and SQLite's stock 250-page
default. At the 16 KiB compile-default page size, that yields a 128 MiB PMA cap
per active sort worker; with `PRAGMA threads=8`, a connection may drive up to
eight concurrent sort workers at that cap.

## Per-Connection PRAGMA Injection

`src/auto_extension.c` is compiled into both library variants.

The priority-102 constructor still calls
`sqlite3_config(SQLITE_CONFIG_SORTERREF_SIZE, 16384)` before effective SQLite
initialization so `SQLITE_ENABLE_SORTER_REFERENCES` is active. Auto-extension
registration is lazy: the observability `sqlite3_open`, `sqlite3_open_v2`, and
`sqlite3_open16` wrappers call `auto_extension_register_for_open()` immediately
before the hidden real open function. That helper calls
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
connections that do not run an app-side PRAGMA pass. Analysis and optimization
remain in the planned-downtime maintenance lifecycle.

`synchronous` is not set by the callback. The compile profile already gives WAL
databases the desired synchronous mode, and a connection-level PRAGMA would
also affect rollback-journal databases.

`optimize` is not set by the callback. Optimization and analysis run in the
planned-downtime maintenance script, where application-specific collations and
tokenizers are available and the lifecycle is explicit.

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
gate, with STMT always enabled and PROFILE conditionally added when slow-query
tracking is enabled. Observability is orthogonal to
PRAGMA injection: target filtering, read-only skips, and the autopragma kill
switch do not suppress observability logs. Trace registration failure logs and
continues; it must never make `sqlite3_open*` fail.

### Slow-Query Tracker

The same per-connection trace registration uses one unified callback with
`SQLITE_TRACE_STMT` always enabled and `SQLITE_TRACE_PROFILE` added only when
slow-query tracking is enabled. SQLite allows one trace callback per
connection, so the tracker shares the existing registration site that runs
before the connection escapes to application code.

The kill-switch hierarchy is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1 -> no observability or tracker work
SQLITE3_DISABLE_SLOW_QUERY=1 -> STMT observability stays on, PROFILE tracker off
```

The helper unconditionally calls `sqlite3_auto_extension` regardless of the
`SQLITE3_DISABLE_AUTOPRAGMA=1` state. An alternative -- early-return from the
helper when `SQLITE3_DISABLE_AUTOPRAGMA=1` -- was considered and rejected: it
would have skipped `sqlite3_trace_v2(SQLITE_TRACE_STMT)` registration in
`autopragma_init`, conflating AUTOPRAGMA disable with observability disable.
The accepted per-open mutex + dedup-scan cost preserves orthogonality between
the two kill switches.

`SQLITE3_SLOW_QUERY_THRESHOLD_MS` defaults to `100`. The parser accepts only
unsigned decimal milliseconds, allows `0` for debug-only all-sample logging,
and falls back to `100` for unset, empty, signed, non-digit, trailing-junk,
ERANGE, or pre-conversion overflow values. PROFILE elapsed values come from
SQLite's millisecond-quantized timing source and are scaled to nanoseconds by
SQLite before the callback receives them.

The tracker keeps a process-global 1024-entry LRU keyed by parameterized
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
after releasing the mutex.

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
and concurrency checks. Bench binaries (`slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`, and
`alloc_latency_bench` compile unconditionally; execute only with `RUN_BENCH=1`.
For Bundle-2's mimalloc Wire-2 acceptance gate, `alloc_latency_bench` remains
advisory only.

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
bundled `ld-musl-x86_64.so.1` loader resolves the produced library against
Plex's renamed ICU.

The CI stage also verifies that the smoke binary resolves `libsqlite3.so` to
the just-built artifact mounted at Plex's deployed library path rather than a
system library.

The CI runtime smoke derives `EXPECTED_ICU_MAJOR` from `ICU_VERSION` and pins
the Plex image through `PLEX_IMAGE_TAG`. The stage-2 step bind-mounts the
extracted Plex artifacts into that container, replacing
`/usr/lib/plexmediaserver/lib/libsqlite3.so` for the smoke process and mounting
the extracted `icu_smoke` binary read-only. The smoke is invoked through Plex's
bundled `ld-musl-*.so.1` loader with Plex's library directory as the loader
path, so `libsqlite3.so` and Plex-renamed ICU resolution use the same runtime
closure as deployment. When bumping the Plex image pin, update `ICU_VERSION` if
the bundled ICU major changes.

### PMS first-init smoke

The CI PMS first-init smoke runs after Plex artifact extraction.

It proves the extracted Plex `libsqlite3.so` can replace PMS's runtime library
at `/usr/lib/plexmediaserver/lib/libsqlite3.so` and survive PMS first
initialization against a fresh `/config`.

The smoke pins the runtime image through `PLEX_IMAGE_TAG`, matching the stage-2
ICU smoke image pin. It starts PMS with the extracted Plex library
bind-mounted read-only over the deployed library path and waits for readiness
by probing TCP port 32400 for up to 90 seconds.

After readiness, the smoke copies
`com.plexapp.plugins.library.db` from PMS's config tree and verifies the runner
`sqlite3` can see at least 20 tables. The workflow prints full PMS logs and
fails when logs contain any of: `SQLITE_`, `database disk image is malformed`,
`Cannot create table`, `no such function`, `no such collation`, or
`Failed to open`.

The smoke also requires observability marker lines for an open wrapper and
`SQLITE_TRACE_STMT`. Positive-mode logs must not contain startup
`sqlite3_config` operations returning `rc=21` (CI pattern:
`[sqlite3-builds-obs] .* sqlite3_config op=SQLITE_CONFIG_[A-Z0-9_]+ rc=21`).
A duplicate disabled-mode smoke starts PMS with
`SQLITE3_DISABLE_OBSERVABILITY=1` and fails if any
`[sqlite3-builds-obs]` line is emitted.

### Emby first-init smoke

The CI Emby first-init smoke runs after generic library artifact extraction.

It proves the extracted generic `libsqlite3.so` can replace Emby's bundled
runtime library at `/app/emby/lib/libsqlite3.so.3.49.2` and survive Emby first
initialization against a fresh `/config`.

The smoke pins the runtime image through `EMBY_IMAGE_TAG`. It starts Emby with
the extracted generic library bind-mounted read-only over the deployed library
path and waits for readiness by polling startup logs for the prefix-agnostic
`Sqlite version:` message for up to 90 seconds.

After readiness, the smoke verifies the logged SQLite version equals the
workflow `SQLITE_VERSION_DOTTED`. It also prints Emby's logged SQLite compiler
options and verifies the
16 curated `compile_options` used by the tuned library are present, including
`SORTER_PMASZ=8192`. The workflow prints full Emby logs and fails when logs
contain the same bad-signal set as §PMS first-init smoke.

The smoke also requires observability marker lines for an open wrapper and
`SQLITE_TRACE_STMT`. Positive-mode logs must not contain startup
`sqlite3_config` operations returning `rc=21` (CI pattern:
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
/opt/sqlite3-lsio-mod/artifacts/<arch>/libsqlite3.so
/opt/sqlite3-lsio-mod/lib/logging.sh
/opt/sqlite3-lsio-mod/lib/arch.sh
/opt/sqlite3-lsio-mod/lib/sha.sh
/opt/sqlite3-lsio-mod/lib/swap.sh
```

Plex also installs:

```text
/opt/sqlite3-lsio-mod/lib/plex-pool-patch.sh
```

`baked-pins.txt` is the only runtime SHA source. It includes current SQLite
artifact rows, per-architecture bundled target pre rows, Plex ICU runtime pre
rows, and PMS / Plex Media Scanner pool-patch baseline rows.

Row-kind schema and the all-arches/no-`post` manifest rule are pinned in
`docs/invariants/sqlite3-builds.md` §2 and §8. The architecture-local point is
that this manifest is the runtime map consumed by the staged LSIO files listed
above.

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

Phase 04 pool patch runs only in the Plex mod. Arm64 logs
`event=pool-patch-deferred` and exits 0. On amd64, the phase first verifies
that the SQLite target is already current, then calls the staged
`plex-pool-patch.sh` fragment with all paths, expected SHAs, and patch sites as
arguments. The fragment reads no environment variables and carries no SHA
constants.

Pool-patch site tuples are
`label|offset|write_seek|original_hex|patched_hex`. Each tuple binds the site
label, the 16-byte read offset, the byte write offset, and the full original
and patched contexts for one binary. The writer derives the single byte to
write from `patched_hex` at `write_seek - offset`, writes it to a same-fs temp
copy with `dd conv=notrunc`, restores the original owner and mode on the temp,
and atomically replaces the target.

Current amd64 sites:

| Binary | Offset label | Offset decimal | Write seek |
|---|---:|---:|---:|
| `/usr/lib/plexmediaserver/Plex Media Server` | `0xae4a17` | `11422231` | `11422232` |
| `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x36f06d` | `3600493` | `3600494` |
| `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x38b37b` | `3715963` | `3715964` |

Pool-patch per-binary behavior:

| Site state | Baseline SHA / backup state | Action |
|---|---|---|
| All sites patched | Any | Skip; already patched |
| All sites original | Current binary SHA matches `pool-pre`; backup absent | Copy backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| All sites original | Current binary SHA matches `pool-pre`; verified backup present | Reuse backup, patch a same-fs temp copy, restore owner/mode, then atomically replace |
| Mixed or unknown | Any | Warn and skip that binary |
| Write, verify, or atomic replace failure | Verified backup present | Leave target unchanged, warn, and continue to the next binary |

Runtime command surface:

| Scope | Commands |
|---|---|
| Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |
| Plex amd64 pool patch only | `dd od printf` |

LSIO runtime has no dependency on `curl`, `tar`, `gunzip`, Python, `jq`,
package managers, or network access.

## Maintenance Architecture

`scripts/optimize_media_servers.sh` is a host-side planned-downtime maintenance
script.

It starts with:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
```

That keeps maintenance connections from receiving the library's automatic
PRAGMA block.

The instance arrays are empty in the repository:

```text
PLEX_INSTANCES=()
EMBY_INSTANCES=()
```

Operators populate those arrays before a maintenance run.

### Planned-Downtime Gate

For each configured Plex or Emby instance, the script queries Docker for an
exact container name. Docker query failure or a running container exits; a
missing host-side database directory skips that instance. Stop and start
operations remain operator-controlled.

### Plex Flow

For each Plex instance, the script resolves the data and database paths, cleans
selected caches, checks integrity, takes a dated backup, dumps to `dump.sql`,
aborts on a dump rollback marker, rebuilds the database from the dump, runs the
optional host-side page-size and incremental-autovacuum step, reruns integrity,
runs `REINDEX`, `PRAGMA analysis_limit=0`, `PRAGMA optimize=0x10002`, optimizes
Plex FTS4 segments, removes the dump, and runs Plex date repair SQL.

The host-side page-size step uses `HOST_SQLITE3`, defaulting to:

```text
${HOME}/bin/sqlite3
```

That step is independent from the later Plex-specific REINDEX and FTS work, so
failure of the page-size sub-step is warning-only.

### Emby Flow

For each Emby instance, the script resolves `/opt/<instance>/data`, checks
integrity, takes a dated backup, dumps to `dump.sql`, aborts on a dump rollback
marker, recreates the database, applies page size and VACUUM, reads the dump,
reruns integrity, runs `REINDEX`, `PRAGMA analysis_limit=0`,
`PRAGMA optimize=0x10002`, optimizes the Emby FTS5 table, and removes the dump.

### Maintenance Posture

Hard failures include Docker query failure, running containers, dump rollback
markers, and integrity-check command or SQL execution failures outside optional
sub-steps. Integrity-check result content is subject to manual operator review
because the script does not gate on the result row.

Warning-and-continue cases include missing optional host SQLite for Plex
page-size migration, page-size migration failure, and page-size post-condition
mismatch.

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
- Plex amd64 pool patch additionally uses `dd`, `od`, and `printf`.
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

M-series invariants:

- M7: Source-level amalgamation patching via a checked-in unified-diff `.patch`
  file applied with `patch -p1` after unzip; SQLite version bumps that change
  any of the six target signatures cause `patch` to reject hunks and fail the
  build with patch's native error message. `build/sqlite-amalgamation.patch` is
  the single locus of amalgamation modifications.

Baked pin manifest:

- `baked-pins.txt` uses schema version 2: metadata row
  `version|2|release_tag|<tag>|generated_at|<iso8601-utc>`.
- It contains current SQLite artifact rows, per-architecture bundled target
  pre rows, Plex ICU runtime pre rows, and PMS / Plex Media Scanner pool-patch
  baseline rows.
- Missing `docker image inspect` repo digest for a pinned LSIO image is a CI
  failure.
- Runtime SHA mismatch has no bypass environment variable.
- LSIO mod images are the authoritative SQLite replacement artifact for a
  release tag.

## Out of Scope

JF deployment is unsupported until a current design and validation plan land. JF
maintenance is absent; adding it requires design and validation before use.
