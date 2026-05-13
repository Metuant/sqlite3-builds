```
     ____     ___    _       _   _            _____
    / ___|   / _ \  | |     (_) | |_    ___  |___ /
    \___ \  | | | | | |     | | | __|  / _ \   |_ \
     ___) | | |_| | | |___  | | | |_  |  __/  ___) |
    |____/   \__\_\ |_____| |_|  \__|  \___| |____/
                                                    
```
# sqlite3-builds

[![Build sqlite](https://github.com/darthShadow/sqlite3-builds/actions/workflows/sqlite-build.yml/badge.svg?event=push)](https://github.com/darthShadow/sqlite3-builds/actions/workflows/sqlite-build.yml)

This fork builds both:

1. A statically-linked `sqlite3` CLI shell binary (Alpine/musl, x86_64-v3, UPX-compressed)
2. A `libsqlite3.so` shared library (Ubuntu 24.04/glibc, x86_64-v3)

## Compilation Requirements

Docker.

## Compiling

```bash
git clone https://github.com/darthShadow/sqlite3-builds.git
cd sqlite3-builds
./build_static_sqlite.sh
```

Compiled files are placed in your local `release` directory:

- `release/cli/` contains `sqlite3` and `sqlite3_orig`
- `release/library/` contains `libsqlite3.so`

## Pre-compiled binary

Pre-compiled artifacts are published on [GitHub Releases][1] when a `v*` tag is pushed, for example `v3.51.0`.

Signing: TODO.

## Customization

- Change the SQLite version by editing `SQLITE_ZIP_URL` in `build_static_sqlite.sh`.
- Change CLI compile-time flags in `docker-cli/Build.sh`.
- Change library compile-time flags in `docker-library/Build.sh`.
- Keep `SQLite_compressor` empty in `build_static_sqlite.sh` to disable CLI compression.

## Motivation

Make a portable optimized `sqlite3` CLI and `libsqlite3.so` library for Linux x86_64-v3 hosts.


[1]: https://github.com/darthShadow/sqlite3-builds/releases
