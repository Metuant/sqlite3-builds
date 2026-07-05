# Smoke Tests

Part of the [Repository Architecture index](../architecture.md).

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
- FULL writes STAT1 and STAT4 rows; LIMITED writes STAT1 and STAT4 rows when
  optimize analyzes stale or missing tables.
- Inline optimize fires from `sqlite3_step` DONE and `sqlite3_reset` OK before
  connection close when the trigger is a fast plain read; directly finalized
  statements pass NULL identity and are close-backstop only.
- `analysis_limit`, `busy_timeout`, the application progress handler, and the
  caller-visible error state are restored after inline optimize success and
  failure.
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
- Inline runtime optimize defers to close when the triggering statement is not a
  provably fast plain read, including telemetry-off, write, transaction-control,
  and unclassifiable finalized-statement paths.
- Runtime optimize's own statements are suppressed from STMT trace and
  slow-query logs.
- The per-path cadence suppresses immediate repeated successful target attempts.
- Runtime optimize still works after `sqlite3_shutdown()` and a later reopen.

Execution points:

- Generic library Docker build.
- Plex library Docker build with Plex ICU paths in the loader environment.

### `plex_fts_rewrite_smoke`

`tests/plex_fts_rewrite_smoke.c` runs in generic/Plex library Docker builds.

It proves:

- ICU-only rewrite enablement.
- literal env kill switch, default-enabled, non-`1` env, and exact-path
  negatives.
- three UTF-8 prepare entries with `nByte`/NUL and `pzTail == NULL`.
- quoted-string/`?` RHS.
- scope/clause/RHS-continuation negatives.
- prepare-denial fail-open.
- grouped-digest row identity.

### `emby_fts_rewrite_smoke`

`tests/emby_fts_rewrite_smoke.c` runs in generic/Plex library Docker builds.
The Dockerfile also builds `emby_fts_rewrite_direct_test` by linking the smoke
directly with `src/emby_fts_rewrite.c`, `src/fts_lex.c`, and the pristine
amalgamation.

It proves:

- FTS rewrite default-on behavior: unset, literal `0`, and garbage values
  enable; literal `1` disables.
- FANOUT default-off behavior and enabled Browse-by-name, Favorites-first,
  RES-A, People, Studios, and Type-29 membership rewrites.
- DASHBOARD default-off behavior and enabled Episode-Latest rewrite with
  LIMIT/projection variation.
- exact `library.db` target basename and non-target negatives.
- three UTF-8 prepare entries with `nByte`/NUL and tail handling.
- MATCH scalar insertion plus membership `EXISTS` rewrite for type and
  presentation shapes.
- scalar OR-to-AND behavior and unchanged fallback for unsupported expressions,
  NULL, integer, and blob values.
- same-name scalar collision, authorizer-denied probe, and ownership replacement
  fail open.
- structural misses for fast-form input, duplicate MATCH sites, literal RHS
  outside direct-test mode, over-cap slots, ambiguous anchors, semicolon tails,
  and embedded NUL inputs.
- Latest index-absent fail-open, capture-on-miss fixtures, aggregate/window
  projection negatives, and series-browse no-misfire fixtures.
- fixture canaries under `tests/fixtures/emby-fts-rewrite/`.
- row parity between original and rewritten seeded data, including Latest
  `(gk,maxdc)` row identity.

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

