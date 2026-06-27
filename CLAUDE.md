# sqlite3-builds

This repository builds tuned SQLite artifacts for media-server containers: a
static CLI, a generic shared library, a Plex shared library, LSIO Docker mod
images for Plex and Emby SQLite replacement, and planned-downtime maintenance
scripts.

Read docs/architecture.md first.

Project-specific guidance:

- The global kernel still applies; read `~/.claude/CLAUDE.md`.
- Treat `docs/architecture.md` as the current repository map for build, LSIO
  mod, smoke-test, and maintenance behavior.
- JF deployment is unsupported until a current binding design and validation
  plan land.
- Plex uses renamed ICU 69 runtime files. LSIO mod code MUST NOT replace,
  rename, move, delete, or overwrite `libicu*plex.so.69`; it may only read and
  verify them.
- Plex library replacement targets only
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- LSIO mods perform no runtime archive download or extraction. Common runtime
  command surface: `awk`, `chmod`, `chown`, `cp`, `grep`, `mkdir`, `mktemp`,
  `mv`, `rm`, `sed`, `sha256sum`, `stat`, `tr`, and `uname`; Plex pool patch additionally uses
  `dd`, `od`, and `printf`.
- Keep `LIBRARY_VARIANT=plex` limited to the Plex ICU build path.
- Keep SQLite pins aligned across wrapper, workflow, Dockerfiles, and
  `build/Build.sh`; keep the SORTERREF and PMASZ compile defaults
  (`-DSQLITE_DEFAULT_SORTERREF_SIZE`, `-DSQLITE_SORTER_PMASZ` in
  `build/Build.sh`) aligned with their runtime `sqlite3_config` values in
  `src/auto_extension.c`, enforced by `tests/check_pin_alignment.sh`; keep ICU
  pins aligned across `.github/workflows/sqlite-build.yml` and
  `docker-library/Dockerfile`; keep
  `CMAKE_*` pins aligned across `pins/versions.env`,
  `.github/workflows/sqlite-build.yml`, `docker-library/Dockerfile`, and
  `tests/check_pin_alignment.sh`; keep `BASEIMAGE_UBUNTU` =
  `ubuntu:18.04@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98`,
  `BASEIMAGE_ALPINE` = `ghcr.io/linuxserver/baseimage-alpine:3.23`, and
  `GENERIC_GLIBC_MAX=2.27` aligned across `pins/versions.env`,
  `docker-cli/Dockerfile`, `docker-library/Dockerfile`, and
  `tests/check_pin_alignment.sh`.
- Keep runtime optimize opt-in: literal `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0`
  enables; unset, literal `1`, and every other value disable. Keep maintenance
  defaults exact: `PLEX_OPTIMIZE_API=0`. For configured Plex instances,
  `main()` derives internal STAT4 capability state from `GENERIC_SQLITE_BINARY`
  preflight; the Plex main-DB STAT4 pass runs only when that state is `1`.
- Do not create or modify `AGENTS.md` here unless explicitly asked; the root
  convention is a symlink to this file.
