```
     ____     ___    _       _   _            _____
    / ___|   / _ \  | |     (_) | |_    ___  |___ /
    \___ \  | | | | | |     | | | __|  / _ \   |_ \
     ___) | | |_| | | |___  | | | |_  |  __/  ___) |
    |____/   \__\_\ |_____| |_|  \__|  \___| |____/
                                                    
```
# sqlite3-builds

[![Build sqlite](https://github.com/darthShadow/sqlite3-builds/actions/workflows/sqlite-build.yml/badge.svg?event=push)](https://github.com/darthShadow/sqlite3-builds/actions/workflows/sqlite-build.yml)

This fork builds tuned SQLite artifacts for Linux media-server containers:

- A statically linked `sqlite3` CLI shell binary.
- A generic `libsqlite3.so` shared library for Emby.
- A Plex-specific `libsqlite3.so` linked against Plex-renamed ICU 69.
- Release tarballs plus `SHA256SUMS`.
- LSIO Docker mod images that replace SQLite in Plex and Emby containers.

CI builds x86-64-v2, x86-64-v3, and arm64 artifacts. x86-64-v4 is not built;
amd64 hosts that support v4 use the v3 artifact.

## Compilation Requirements

Docker.

## Compiling

```bash
git clone https://github.com/darthShadow/sqlite3-builds.git
cd sqlite3-builds
./build/build_static_sqlite.sh
```

Compiled files are placed in your local `release` directory:

- `release/cli/` contains `sqlite3` and `sqlite3_orig`
- `release/library/` contains `libsqlite3.so`
- `release/library-plex/` contains the Plex `libsqlite3.so` when
  `LIBRARY_VARIANT=plex` is used

Build the Plex variant locally with:

```bash
LIBRARY_VARIANT=plex ./build/build_static_sqlite.sh
```

The LSIO Docker mod path is CI-owned. `mod-build` stages the Plex and Emby mod
roots, bakes the selected `libsqlite3.so` and `baked-pins.txt` into scratch mod
images, and smokes the staged root filesystem inside the pinned LSIO base
images. `mod-publish` publishes stable multi-arch mod tags only for release
tags.

For cold-start runtime support changes, see `docs/extending.md`. For the
schema v3 `baked-pins.txt` contract and validator rules, see
`docs/baked-pins-schema.md`.
For runtime kill-switches and numeric tunables, see `docs/env-vars.md`.

## Pre-compiled binary

Pre-compiled artifacts are published on [GitHub Releases][1] for CalVer tags
matching `YYYY.MM.DD-rN`, for example `2026.05.28-r1`. Tags have no `v`
prefix and always include `-rN`.

Signing: TODO.

## Customization

- Change SQLite source pins, the URL-encoded `SQLITE_SOURCE_ID`, the mimalloc
  tuple, `CMAKE_*` inputs for the content-addressed GHCR generic base,
  base-image source pins,
  `GENERIC_GLIBC_MAX`, and SQLite config-count pins in `pins/versions.env`.
  On every SQLite bump, replace `SQLITE_SOURCE_ID` with the new 84-byte
  `sqlite_source_id()` value, encoding each space as `%20`. The library build
  decodes the pin and runs `SELECT sqlite_source_id()` through the freshly
  built shared library; a stale pin fails the build.
  The generic library build consumes `BASE_IMAGE` dynamically from CI or the
  local wrapper; do not add a static generic base digest pin to
  `docker-library/Dockerfile`.
- Keep `SQLITE_EXPECTED_CONFIG_COUNT` and `SQLITE_EXPECTED_DBCONFIG_COUNT`
  aligned with `build/expected-sqlite-config-count.txt`,
  `build/expected-sqlite-dbconfig-count.txt`, and `tests/check_pin_alignment.sh`
  when an SQLite bump changes the decoded config surface.
- Change ICU compatibility-group source fields in
  `pins/library-compat-groups.tsv`. Edit the canonical pin tables, not the
  wrapper, workflow, Dockerfile, or other derived consumers.
- Follow `docs/extending.md` when adding supported runtime versions,
  compatibility groups, Plex pool-patch sites, or release support evidence.
- Change CLI and library compile-time flags in `build/Build.sh`.
- Keep `SQLite_compressor` empty in `build/build_static_sqlite.sh` to disable
  CLI compression.

## Planned-Downtime Maintenance

`scripts/optimize_media_servers.sh` is a host-side helper for stopped Plex and
Emby containers. It consumes a generic SQLite CLI at `${HOME}/bin/sqlite3` and
an operator-staged patched Plex engine under `${HOME}/plex-sql/`.

Stage the generic CLI directly from the local build output:

```bash
mkdir -p "${HOME}/bin"
install -m 0755 release/cli/sqlite3 "${HOME}/bin/sqlite3"
```

Copy the Plex maintenance prerequisites from a container after the Plex LSIO
mod has swapped SQLite and patched the PMS source-id guard. The wrapper, PMS
sibling, and `libsqlite3.so` must be a coherent patched set.

```bash
mkdir -p "${HOME}/plex-sql"
rm -rf "${HOME}/plex-sql/lib"
docker cp plex:"/usr/lib/plexmediaserver/lib/" "${HOME}/plex-sql/lib/"
docker cp plex:"/usr/lib/plexmediaserver/Plex SQLite" "${HOME}/plex-sql/"
docker cp plex:"/usr/lib/plexmediaserver/Plex Media Server" "${HOME}/plex-sql/"
```

Do not restore `libsqlite3.so.bundled.bak` over the staged patched library.
Before stopping any container, the script executes `PLEX_BINARY`. If its
`sqlite_source_id()` is not the required patched id, the script skips all Plex
maintenance without evaluating Plex instance gates, then continues independent
Emby maintenance.
The built `release/library-plex/libsqlite3.so` is usable here only when paired
with the matching patched wrapper and PMS guard.

Operator config is a sourced Bash file. Copy
`scripts/optimize_media_servers.conf.example` to
`scripts/optimize_media_servers.conf`, or set
`OPTIMIZE_MEDIA_SERVERS_CONF=/path/to/optimize_media_servers.conf`. An absent
resolved config file is an error before any database or container work. Run
`scripts/optimize_media_servers.sh --help` for usage; help does not require a
config file. Keep the copied or host-local config out of version control; do
not commit it.

Each `PLEX_INSTANCES` or `EMBY_INSTANCES` entry is both the Docker container
name and the `/opt/<instance>` path stem:

```bash
PLEX_INSTANCES=("plex" "plex-4k")
EMBY_INSTANCES=("emby")
```

Current operator controls:

| Control | Default | Change path | Effect |
|---|---|---|---|
| `_PAGE_SIZE` | `16384` | Edit the in-script `_PAGE_SIZE` constant in `scripts/optimize_media_servers.sh` | Target page size for staged Plex and Emby rebuilds. |
| `BACKUP_PATH` | `/mnt/media-backup` | Set in the config file | Existing path receives instance-scoped `.original` backups; otherwise backups stay beside the database. |
| `GENERIC_SQLITE_BINARY` | `${HOME}/bin/sqlite3` | Set in the config file | Runs Emby maintenance and gates the Plex main-DB STAT4 pass on `ENABLE_STAT4`. |
| `PLEX_BINARY` | `${HOME}/plex-sql/Plex SQLite` | Set in the config file | Runs all Plex maintenance, including STAT4 ANALYZE with `icu_root` registered in each invocation; the patched source id is required. |
| `PLEX_OPTIMIZE_API` | `0` | Set in the config file | Literal `1` enables the optional post-start Plex `PUT /library/optimize?async=1` trigger. |
| `PLEX_PROCESS_BLOB_DB` | `0` | Set in the config file | Literal `1` opts into the Plex blob database rebuild pass. |
| `PLEX_TRIM_FINISHED_SEASON_BLOBS` | `0` | Set in the config file | Literal `1`, together with `PLEX_PROCESS_BLOB_DB=1`, enables the fixed 24-month finished-season blob trim in the staged pre-swap hook. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | `90` | Set in the config file | Plex `statistics_bandwidth` deflate keeps only rows with an account id and inside the retention window. |
| Plex `statistics_bandwidth` deflate | Enabled for the Plex main database when the table exists | Uses `STATS_BANDWIDTH_RETAIN_DAYS` | Deletes anonymous or older bandwidth rows on the staged database and runs `VACUUM`; the later final post-rebuild integrity gate blocks publication on corruption. |

The rebuild keeps the source-vs-staged row-count sweep before FTS rebuild,
drops the pre-copy source and pre-rebuild staged whole-DB integrity gates, and
retains the final post-rebuild `integrity_check` as the publication barrier.

## Motivation

Make portable optimized SQLite CLI, library, and LSIO mod artifacts for Plex
and Emby on current amd64 and arm64 Linux media-server containers.


[1]: https://github.com/darthShadow/sqlite3-builds/releases
