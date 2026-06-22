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
  `build/Build.sh`; keep ICU pins aligned across
  `.github/workflows/sqlite-build.yml` and `docker-library/Dockerfile`; keep
  `CMAKE_*` pins aligned across `pins/versions.env`,
  `.github/workflows/sqlite-build.yml`, `docker-library/Dockerfile`, and
  `tests/check_pin_alignment.sh`; keep `BASEIMAGE_UBUNTU` = digest-pinned
  `ubuntu:18.04` and `GENERIC_GLIBC_MAX=2.27` aligned across
  `pins/versions.env`, `docker-library/Dockerfile`, and
  `tests/check_pin_alignment.sh`.
- Do not create or modify `AGENTS.md` here unless explicitly asked; the root
  convention is a symlink to this file.
