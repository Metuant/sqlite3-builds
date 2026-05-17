# sqlite3-builds

This repository builds tuned SQLite artifacts for media-server containers: a
static CLI, a generic shared library, a Plex shared library, deployment helpers,
and planned-downtime maintenance scripts.

Read ARCHITECTURE.md first.

Project-specific guidance:

- The global kernel still applies; read `~/.claude/CLAUDE.md`.
- Treat `ARCHITECTURE.md` as the current repository map for build, deploy,
  smoke-test, and maintenance behavior.
- JF is deferred. Its deploy branch is retained but not current-cycle
  validated; JF maintenance is absent from `scripts/optimize_media_servers.sh`
  and requires design and validation before use.
- Plex uses renamed ICU 69 runtime files. Deploy scripts MUST NOT touch
  `libicu*plex.so.69`.
- Plex library replacement targets only
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- The LSIO archive surface is `tar` and `gunzip` only.
- LSIO startup hooks assume the POSIX/coreutils baseline `mktemp`, `mkdir`,
  `cp`, `mv`, `rm`, standard `sh`, and `bash`; non-standard tools must be
  preflight-checked before use.
- Keep `LIBRARY_VARIANT=plex` limited to the Plex ICU build path.
- Keep SQLite and ICU pins aligned across wrapper, workflow, Dockerfiles, and
  `build/Build.sh`.
- Do not create or modify `AGENTS.md` here unless explicitly asked; the root
  convention is a symlink to this file.
