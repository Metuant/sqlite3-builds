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

## Pre-compiled binary

Pre-compiled artifacts are published on [GitHub Releases][1] for CalVer tags
matching `YYYY.MM.DD-rN`, for example `2026.05.28-r1`. Tags have no `v`
prefix and always include `-rN`.

Signing: TODO.

## Customization

- Change SQLite source pins, the mimalloc tuple, generic CMake `CMAKE_*` pins,
  base-image pins, `GENERIC_GLIBC_MAX`, and SQLite config-count pins in
  `pins/versions.env`.
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

`scripts/optimize_media_servers.sh` operates only on instances added to its
`PLEX_INSTANCES` and `EMBY_INSTANCES` arrays. Current operator controls:

| Control | Default | Change path | Effect |
|---|---|---|---|
| `PAGE_SIZE` | `16384` | Edit the script default before the run | Target page size for staged Plex and Emby rebuilds. |
| `BACKUP_PATH` | `/mnt/media-backup` | Edit the script default before the run | Existing path receives instance-scoped `.original` backups; otherwise backups stay beside the database. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | `90` | Edit the script default before the run | Plex `statistics_bandwidth` deflate keeps only rows with an account id and inside the retention window. |
| `PLEX_PROCESS_BLOB_DB` | `0` | Set to `1` in the environment before the run | Opts into the Plex blob database rebuild pass. |
| Plex `statistics_bandwidth` deflate | Enabled for the Plex main database when the table exists | Uses `STATS_BANDWIDTH_RETAIN_DAYS` | Deletes anonymous or older bandwidth rows on the staged database, runs `VACUUM`, and aborts before swap if post-deflate integrity is not `ok`. |

## Motivation

Make portable optimized SQLite CLI, library, and LSIO mod artifacts for Plex
and Emby on current amd64 and arm64 Linux media-server containers.


[1]: https://github.com/darthShadow/sqlite3-builds/releases
