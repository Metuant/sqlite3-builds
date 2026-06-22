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

- Change SQLite and mimalloc pins in `pins/versions.env`.
- Change ICU compatibility-group source fields in
  `pins/library-compat-groups.tsv`. Edit the canonical pin tables, not the
  wrapper, workflow, Dockerfile, or other derived consumers.
- Follow `docs/extending.md` when adding supported runtime versions,
  compatibility groups, Plex pool-patch sites, or release support evidence.
- Change CLI and library compile-time flags in `build/Build.sh`.
- Keep `SQLite_compressor` empty in `build/build_static_sqlite.sh` to disable
  CLI compression.

## Motivation

Make portable optimized SQLite CLI, library, and LSIO mod artifacts for Plex
and Emby on current amd64 and arm64 Linux media-server containers.


[1]: https://github.com/darthShadow/sqlite3-builds/releases
