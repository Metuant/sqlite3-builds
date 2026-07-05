# Maintenance

Part of the [Repository Architecture index](../architecture.md).

## Maintenance Architecture

`scripts/optimize_media_servers.sh` is a host-side planned-downtime maintenance
script for Plex and Emby containers.

It starts with:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
```

That keeps maintenance connections from receiving the library's automatic
PRAGMA block.

### Invocation and Config

Run with no arguments to maintain configured instances. `-h` and `--help`
print usage and exit 0 before any config load. Any other argument is rejected
with a short error and `--help` pointer before config load.

The script resolves its sourced Bash config as
`${OPTIMIZE_MEDIA_SERVERS_CONF:-<script-dir>/optimize_media_servers.conf}`.
The default path is `scripts/optimize_media_servers.conf` beside the script;
`scripts/optimize_media_servers.conf.example` is the committed template.
The resolved config must exist and source successfully before any binary
preflight, Docker operation, curl request, or database work.

Config load happens inside `main()` only. Sourcing the script for tests or
helper reuse does not load the config and does not fail on a missing config.

The side-car config is the sole source of these operator variables:

```text
PLEX_INSTANCES
EMBY_INSTANCES
PLEX_BINARY
GENERIC_SQLITE_BINARY
BACKUP_PATH
PLEX_OPTIMIZE_API
PLEX_PROCESS_BLOB_DB
STATS_BANDWIDTH_RETAIN_DAYS
```

`OPTIMIZE_MEDIA_SERVERS_CONF` is only the external path selector and is not
stored in the config file.

Each instance array entry is both the Docker container name and the
`/opt/<instance>` path stem:

```text
PLEX_INSTANCES=("plex" "plex-4k")
EMBY_INSTANCES=("emby")
```

For Plex, the database path resolves under
`/opt/<instance>/Library/Application Support/Plex Media Server/...`. For Emby,
the database path resolves under `/opt/<instance>/data/...`.

The generic CLI consumed by `GENERIC_SQLITE_BINARY` is staged from the build
output `release/cli/sqlite3` to `${HOME}/bin/sqlite3`. Plex maintenance uses
container-copied runtime files: `PLEX_BINARY` points at
`${HOME}/plex-sql/Plex SQLite`, and `${HOME}/plex-sql/lib/` holds the Plex
container's `/usr/lib/plexmediaserver/lib/` copy. If that copied `lib/` came
from a modded container, restore `libsqlite3.so.bundled.bak` over
`${HOME}/plex-sql/lib/libsqlite3.so` in the staging copy before running
maintenance. Copying the `Plex Media Server` sibling into `${HOME}/plex-sql/`
is recommended staging for Plex's source-id guard, but the script only
executes `PLEX_BINARY` (`Plex SQLite`); the sibling is not a code-proven
runtime dependency of the helper. The built `release/library-plex/libsqlite3.so`
is a separate Plex library-replacement output, not a maintenance input.

`scripts/optimize_media_servers.sh` owns the container lifecycle for configured
Plex and Emby instances. Each instance with a present database directory is
stopped before maintenance, verified stopped with
`docker ps --filter ... status=running`, started after the maintenance section,
and verified running with the same Docker-status idiom. Missing instance data
is skipped before any stop. Stop, maintenance, start, and start-verification
failures set the final process status nonzero for that instance and the script
continues to later instances. Once `docker stop` succeeds, the script attempts
`docker start` on every maintenance failure path before moving on.

`PLEX_OPTIMIZE_API` defaults to `0` in the config template. Literal
`PLEX_OPTIMIZE_API=1` enables an optional post-start Plex API trigger inside
the per-Plex-instance loop. For each restarted Plex instance, the script
resolves the container IP with `docker inspect`, waits up to 60 seconds for
`GET http://<container-ip>:32400/identity` to return HTTP 200, reads
`PlexOnlineToken` from
`/opt/<instance>/Library/Application Support/Plex Media Server/Preferences.xml`,
sends one `PUT http://<container-ip>:32400/library/optimize?async=1` request
with the token only in the `X-Plex-Token` header, and waits by polling
`/activities` for `type="general.db.optimize"`.

If `/activities` already reports `type="general.db.optimize"` before submit,
the trigger reports the instance as already running and does not send the PUT.
After submit, completion is credited only when that activity is observed and
then absent on consecutive successful polls. If Plex exposes a stable activity
`uuid` or `id`, the wait tracks that activity. The PUT is never retried. Failed
or indeterminate polls continue until the wall-clock five-minute wait cap, with
each curl request capped to the remaining deadline. Per-instance trigger
failures warn and continue, and the final process exits nonzero only when
`PLEX_OPTIMIZE_API=1` was attempted and no Plex instance reached accepted,
already-running, or completed.

Maintenance controls:

| Control | Default/example | How to change | Effect |
|---|---|---|---|
| `PLEX_INSTANCES` | `("plex" "plex-4k")` | Set in the config file | Plex Docker container names and `/opt/<instance>` stems. |
| `EMBY_INSTANCES` | `("emby")` | Set in the config file | Emby Docker container names and `/opt/<instance>` stems. |
| `PLEX_BINARY` | `${HOME}/plex-sql/Plex SQLite` | Set in the config file | Runs Plex maintenance with Plex's ICU-enabled SQLite binary. |
| `GENERIC_SQLITE_BINARY` | `${HOME}/bin/sqlite3` | Set in the config file | Runs Emby maintenance and the Plex main-DB STAT4 pass. |
| `BACKUP_PATH` | `/mnt/media-backup` | Set in the config file | If the path exists, backups publish under an instance-specific subdirectory there; otherwise backups stay beside the source database. |
| `PLEX_OPTIMIZE_API` | `0` | Set in the config file | Literal `1` enables the optional post-start Plex `PUT /library/optimize?async=1` trigger. |
| `PLEX_PROCESS_BLOB_DB` | `0` | Set in the config file | Literal `1` enables the optional Plex blob database rebuild pass. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | `90` | Set in the config file | Plex `statistics_bandwidth` deflate keeps only rows with an account id and inside this retention window. |
| `_PAGE_SIZE` | `16384` | Edit the in-script `_PAGE_SIZE` constant | Rebuilt Plex and Emby databases target 16 KiB pages. |
| Plex `statistics_bandwidth` deflate | Enabled for the Plex main database when the table exists | Controlled by `STATS_BANDWIDTH_RETAIN_DAYS` | Runs on the staged database before swap; DELETE or VACUUM failures warn, but a post-deflate `integrity_check` failure aborts before touching the live DB. |

### Planned-Downtime Gate

For each configured Plex or Emby instance, the script checks for the expected
instance data directory before stopping. Missing data skips without a
stop/start cycle. For instances with data, the script runs `docker stop`,
queries Docker for an exact container name with `status=running`, and skips
maintenance if the stopped verification still sees the container running or the
Docker query fails. After a successful stop, the script runs `docker start`
after maintenance succeeds or fails, verifies the container reaches running, and
sets final status nonzero for stop, maintenance, start, or verification
failures.

### Plex Flow

For each Plex instance, the script resolves the data and database paths, cleans
selected caches, runs the sanity query, hard-gates source
`PRAGMA integrity_check == ok`, hard-gates source FTS integrity, warns on
foreign-key rows, switches the source to `journal_mode=DELETE`, and publishes a
dated `.original` backup.

The rebuild runs under `PLEX_BINARY` (`Plex SQLite`) with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and `VACUUM INTO`
to a same-directory staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, staged `integrity_check == ok`, staged FTS integrity,
and an exhaustive source-vs-staged per-table row-count sweep before any swap is
attempted. If the main Plex database has `statistics_bandwidth`, the pre-swap
hook can deflate it inside the staged file with `PRAGMA temp_store=MEMORY`,
`PRAGMA threads=8`, and `VACUUM`, then hard-gates post-deflate
`integrity_check`.

After the rebuild and validation sweep, the staged Plex path runs FTS rebuild,
the staged FTS integrity gate, FTS re-curation, staged optimize SQL, the staged
metadata date-repair SQL, the Plex main-DB STAT4 pass, and, when that STAT4
pass is enabled, a post-STAT4 `integrity_check` gate before the
`user_version`/`application_id` preservation gate and atomic replacement.
Post-swap FTS maintenance is optimize-only.
`PLEX_PROCESS_BLOB_DB=1` enables the same staged rebuild for the Plex blob
database without the `statistics_bandwidth` hook, metadata date repair, or
STAT4 pass. Plex SQLite is required because Plex data can require the
ICU-enabled binary.

The Plex main-database staged optimize SQL creates the runbook-validated
`idx_dshadow_taggings_tag_id_metadata_item_id` and
`idx_dshadow_mis_account_updated_guid_cover`, and
`idx_dshadow_metadata_items_section_added` indexes before `REINDEX`, `ANALYZE`,
and `PRAGMA optimize`. The existing Plex SQLite `ANALYZE` remains as the STAT1
floor for ICU-collated or skipped objects. The subsequent STAT4 pass uses
`GENERIC_SQLITE_BINARY`, runs only after the Plex staged SQL, discovers safe
targets from `pragma_index_list`/`pragma_index_xinfo`, skips unsupported
collations and unsafe identifier transport, sets `analysis_limit=0`, and warns
without aborting on preflight, discovery, per-target `ANALYZE`, or final
`sqlite_stat4` row-count failures. `main()` derives the internal STAT4 gate
from `GENERIC_SQLITE_BINARY` preflight for configured Plex instances, so the
pass runs only when that binary reports `ENABLE_STAT4`.

### Emby Flow

For each Emby instance, the script resolves `/opt/<instance>/data`, runs the
sanity query, hard-gates source `integrity_check == ok`, hard-gates source
FTS integrity, warns on foreign-key rows, switches the source to
`journal_mode=DELETE`, and publishes a dated `.original` backup.

The rebuild runs under `GENERIC_SQLITE_BINARY` with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and
`VACUUM INTO` to a staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, staged `integrity_check == ok`, staged FTS integrity,
an exhaustive source-vs-staged per-table row-count sweep, FTS rebuild, the
staged FTS integrity gate, FTS re-curation, staged optimize SQL, and the
`user_version`/`application_id` preservation gate before the script replaces the
live database with `mv` and removes stale WAL/SHM siblings.
The Emby staged optimize SQL creates the runbook-validated
`idx_dshadow_mediaitems_parent_type` and `idx_dshadow_emby_latest_gk_dc` indexes
before `REINDEX`, `ANALYZE`, and `PRAGMA optimize`.

Post-swap FTS maintenance is optimize-only because the database has already
passed the source and staged validation gates.

### Maintenance Posture

Before per-instance work, hard failures include unsupported arguments, absent
resolved config, and config source failure. Hard per-instance failures include
Docker stop/start verification failure, Docker query failure, running
containers after stop, source integrity failure, source FTS integrity failure,
`journal_mode=DELETE` failure, backup publication failure, `VACUUM INTO`
failure, staged auto-vacuum mismatch against `NONE`, staged integrity failure,
staged FTS integrity failure, per-table row-count mismatch, pre-swap hook
integrity failure, and post-STAT4 staged integrity failure. After a successful
stop, these failures still fall through to the start gate before the script
continues. The source and staged integrity gates require the literal `ok`
result from
`PRAGMA integrity_check`.

Warning-and-continue cases include source `foreign_key_check` rows, skipped
missing instance directories, Plex `statistics_bandwidth` table absence or
DELETE/VACUUM failure before a clean post-deflate integrity result, staged
maintenance SQL failure, staged metadata date-repair failure, Plex STAT4
preflight/worklist/per-target/final-count failures, and post-swap FTS
maintenance failures.

