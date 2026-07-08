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
- `docs/architecture.md` is a slim index linking to focused docs under `docs/architecture/` for build, rewrite-engine, observability, smoke-tests, lsio-mods, and maintenance.
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
  `docker-library/Dockerfile`; keep `BASEIMAGE_UBUNTU`, `CMAKE_*`, and
  `UBUNTU_TOOLCHAIN_R_TEST_KEY_FINGERPRINT` aligned across
  `pins/versions.env`, `docker-build-base/Dockerfile`,
  `.github/workflows/base.yml`, `build/base_image_ref.sh`, and
  `tests/check_pin_alignment.sh`; keep `docker-library/Dockerfile`,
  `.github/workflows/sqlite-build.yml`, and `build/build_static_sqlite.sh`
  consuming `BASE_IMAGE` dynamically with no static generic base digest pin;
  keep `BASEIMAGE_ALPINE` = `ghcr.io/linuxserver/baseimage-alpine:3.23` and
  `GENERIC_GLIBC_MAX=2.27` aligned across `pins/versions.env`,
  `docker-cli/Dockerfile`, `docker-library/Dockerfile`, and
  `tests/check_pin_alignment.sh`.
- Keep runtime optimize opt-in: literal `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=0`
  enables; unset, literal `1`, and every other value disable. Keep maintenance
  defaults exact: `PLEX_OPTIMIZE_API=0`. For configured Plex instances,
  `main()` derives internal STAT4 capability state from `GENERIC_SQLITE_BINARY`
  preflight; the Plex main-DB STAT4 pass runs only when that state is `1`.
- Keep Plex FTS rewrite opt-out (default-on in the Plex/ICU build): literal
  `SQLITE3_DISABLE_PLEX_FTS_REWRITE=1` disables; unset, literal `0`, and every
  other value enable -- matching the `SQLITE3_DISABLE_OBSERVABILITY`,
  `SQLITE3_DISABLE_SLOW_QUERY`, and `SQLITE3_DISABLE_AUTOPRAGMA` kill-switches.
  Keep Plex taggings/On-Deck rewrites opt-in:
  `SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE` (taggings-membership) and
  `SQLITE3_DISABLE_PLEX_ONDECK_REWRITE` (On-Deck) each enable on literal `0`;
  unset, literal `1`, and every other value disable. Both fail open and are
  independent of `SQLITE3_DISABLE_AUTOPRAGMA` and
  `SQLITE3_DISABLE_PLEX_FTS_REWRITE`.
  Keep Emby FTS rewrite (`SQLITE3_DISABLE_EMBY_FTS_REWRITE`) opt-out (default-on
  in the Emby build): literal `1` disables; unset, literal `0`, and every other
  value enable. Keep the two Emby membership/dashboard knobs opt-in:
  `SQLITE3_DISABLE_EMBY_FANOUT_REWRITE` (Browse-by-name / Favorites-first / RES-A /
  People-Studios-Type-29) and `SQLITE3_DISABLE_EMBY_DASHBOARD_REWRITE`
  (Episode-Latest) each enable on literal `0`; unset, literal `1`, and every
  other value disable. All three are fail-open and independent of
  `SQLITE3_DISABLE_AUTOPRAGMA`. Advisory: to also optimize the Emby fan-out
  families, enable `SQLITE3_DISABLE_EMBY_FANOUT_REWRITE=0`; it is not required
  and is not code-enforced. Knob naming:
  `SQLITE3_DISABLE_<ENGINE>_<PURPOSE>_REWRITE`.
- Do not create or modify `AGENTS.md` here unless explicitly asked; the root
  convention is a symlink to this file.
