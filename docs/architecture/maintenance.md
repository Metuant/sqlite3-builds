# Maintenance

Part of the [Repository Architecture index](../architecture.md).

## Maintenance Architecture

`scripts/optimize_media_servers.sh` is a host-side planned-downtime maintenance
script for Plex and Emby containers.

It starts with:

```text
SQLITE3_DISABLE_AUTOPRAGMA=1
SQLITE3_DISABLE_OBSERVABILITY=1
```

That keeps maintenance connections from receiving the library's automatic
PRAGMA block and keeps observability records out of maintenance command-output
captures.

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
PLEX_TRIM_FINISHED_SEASON_BLOBS
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
output `release/cli/sqlite3` to `${HOME}/bin/sqlite3`. It runs Emby maintenance
and supplies the `ENABLE_STAT4` capability gate for the optional Plex STAT4
pass.

Plex maintenance requires an operator-staged patched engine before the downtime
run. `PLEX_BINARY` points at `${HOME}/plex-sql/Plex SQLite`;
`${HOME}/plex-sql/lib/libsqlite3.so` must be the matching patched Plex/ICU
library, and the staged `Plex Media Server` sibling must carry the matching
source-id guard. Stage the coherent files from a container after the Plex LSIO
mod has completed. Do not restore `libsqlite3.so.bundled.bak` over the staged
patched library. Before any container or database work, the script executes
`PLEX_BINARY`. If `sqlite_source_id()` does not equal the required patched id,
the script skips all Plex maintenance without evaluating Plex instance gates
and continues independent Emby maintenance. The skip contributes status value
`2`, so a run cannot report success after dropping configured Plex work. The
built `release/library-plex/libsqlite3.so` can supply the library only when it
is paired with a wrapper and PMS guard patched for that exact source id.

The generic SQLite engine preflight runs before configured Emby instance work.
If it fails, the script skips all Emby maintenance without evaluating Emby
instance gates, contributes status value `4`, and preserves independent Plex
work and its status.

For a normal no-argument run after config load, the final status is a bitmask:

| Value | Meaning |
|---|---|
| `0` | No tracked final-status condition occurred. |
| `1` | At least one per-instance maintenance/lifecycle failure occurred, or every attempted Plex optimize API trigger failed. |
| `2` | Configured Plex work was dropped by the patched-engine preflight; Emby work was not dropped and no value-`1` failure occurred. |
| `3` | Configured Plex work was dropped and independent Emby work also produced a value-`1` failure. |
| `4` | Configured Emby work was dropped by the generic-engine preflight; Plex work was not dropped and no value-`1` failure occurred. |
| `5` | Configured Emby work was dropped and independent Plex work also produced a value-`1` failure. |
| `6` | Both configured Plex and Emby work were dropped by their engine preflights. |

Argument validation and config-load failures return their direct status before
this final-status bitmask applies. An unsupported argument returns direct status
`64`. This status is not composed with normal-run bits and cannot be mistaken
for any current no-argument outcome. Value `7` is not currently reachable:
when both global preflights drop their engines, no per-instance path remains to
set value `1`.

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
| `PLEX_BINARY` | `${HOME}/plex-sql/Plex SQLite` | Set in the config file | Runs all Plex maintenance, including the ICU-aware STAT4 ANALYZE pass; its patched source id is a hard preflight prerequisite. |
| `GENERIC_SQLITE_BINARY` | `${HOME}/bin/sqlite3` | Set in the config file | Runs Emby maintenance and gates the Plex main-DB STAT4 pass on `ENABLE_STAT4`. |
| `BACKUP_PATH` | `/mnt/media-backup` | Set in the config file | If the path exists, backups publish under an instance-specific subdirectory there; otherwise backups stay beside the source database. |
| `PLEX_OPTIMIZE_API` | `0` | Set in the config file | Literal `1` enables the optional post-start Plex `PUT /library/optimize?async=1` trigger. |
| `PLEX_PROCESS_BLOB_DB` | `0` | Set in the config file | Literal `1` enables the optional Plex blob database rebuild pass. |
| `PLEX_TRIM_FINISHED_SEASON_BLOBS` | `0` | Set in the config file | Together with literal `PLEX_PROCESS_BLOB_DB=1`, literal `1` enables the fixed 24-month finished-season blob trim. |
| `STATS_BANDWIDTH_RETAIN_DAYS` | `90` | Set in the config file | Plex `statistics_bandwidth` deflate keeps only rows with an account id and inside this retention window. |
| `_PAGE_SIZE` | `16384` | Edit the in-script `_PAGE_SIZE` constant | Rebuilt Plex and Emby databases target 16 KiB pages. |
| Plex `statistics_bandwidth` deflate | Enabled for the Plex main database when the table exists | Controlled by `STATS_BANDWIDTH_RETAIN_DAYS` | Runs in the staged pre-swap hook; DELETE or VACUUM failures warn, and the later unconditional final staged `integrity_check` blocks publication on corruption. |

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
selected caches, runs the sanity query, runs source FTS integrity in
warn-and-rebuild mode, and hard-fails on unsafe or unclassified FTS metadata.
It then runs a source whole-DB `PRAGMA integrity_check`, warns on foreign-key
rows, switches the source to `journal_mode=DELETE`, and publishes a dated
`.original` backup. The source whole-DB gate runs before the WAL checkpoint,
backup, and `VACUUM INTO`, so a source b-tree failure returned as a classified
result row cannot be normalized into a clean-but-lossy staged database.

The source whole-DB gate requires framed checker output before it assigns a
verdict. A sole literal `ok` passes; `ok` mixed with any other row is treated as
contradictory output and fails open. In a framed payload with no `ok`, any row
outside the accepted deferral and checker-inability patterns hard-fails with
the returned diagnostics. FTS5 xIntegrity mismatch rows in the `fts5:` message
family do not match either exception and therefore hard-abort rather than
remaining under the preceding source FTS warn-and-rebuild policy.

The malformed-index deferral is family-wide. The exact
`malformed inverted index for FTS4 table main.fts4_tag_titles_icu` arm is
immediately followed by an arm that accepts `malformed inverted index for
FTS[345] table *` for any FTS3, FTS4, or FTS5 table. The broad arm subsumes the
exact arm, so genuine malformed-index findings for any FTS table are deferred
to the source FTS warn-and-rebuild policy, not only findings for Plex's curated
external-content index. `unable to validate the inverted index for FTS[345]
table *` rows also warn and continue as checker inability.

The source gate fails open when the checker invocation exits nonzero or its
framing is missing or contradictory: it warns, returns success, and allows the
pipeline to proceed toward backup and `VACUUM INTO`. The nonzero branch cannot
distinguish a genuine engine failure from corruption that makes the checker
fail to run. This is a deliberate trade to avoid aborting maintenance on a
genuine engine failure. The gate therefore catches corruption that the checker
successfully returns in a framed, no-`ok` payload as a hard finding; it does not
prove that the source is corruption-free when the checker fails. The configured
`PLEX_BINARY` is source-id-gated to the patched SQLite 3.53.3 engine that can
emit these xIntegrity results. The bundled SQLite 3.39.4 mentioned by the
optimize-mask compatibility note is not the configured maintenance checker.

The rebuild runs under `PLEX_BINARY` (`Plex SQLite`) with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and `VACUUM INTO`
to a same-directory staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, then pass the exhaustive source-vs-staged per-table
row-count sweep. The staged pre-rebuild whole-DB `integrity_check` is
intentionally absent because the patched 3.53.3 engine sees pre-rebuild FTS
drift. The remaining staged order is FTS rebuild, the staged FTS integrity gate,
the per-database pre-swap hook, staged optimize SQL, the post-maintenance hook,
the optional final pre-publication extension hook, the unconditional final
`integrity_check == ok`, and the `user_version`/`application_id` preservation
gate before atomic replacement. The row-count sweep stays before FTS rebuild;
reordering it would compare rebuilt shadow-table segments with the source. The
final post-rebuild whole-DB gate is the second whole-DB `integrity_check` and
runs after all configured hooks and optimize SQL. Plex main and blob databases
use the patched `PLEX_BINARY`; Emby uses `GENERIC_SQLITE_BINARY`. The final gate
still runs when no hook runs or Plex STAT4 is disabled or analyzes no target.
FTS re-curation and its shadow-table structural gate remain disabled under the
rebuild-all model.

For the Plex main database, the pre-swap hook deflates
`statistics_bandwidth`, when present, with `PRAGMA threads=8` and `VACUUM`.
DELETE or VACUUM failures retain their warn-and-continue behavior; the hook has
no local integrity gate and returns success after a completed mutation. The
post-maintenance hook then runs metadata date repairs unconditionally and the
Plex main-DB STAT4 pass when enabled. The final staged integrity gate runs after
both. Every maintenance `VACUUM` invoked through Plex SQLite uses its default
memory-backed temp storage from the compiled `SQLITE_TEMP_STORE=3` profile and
retains `PRAGMA threads=8`.

The retained `run_fts_recurate` and
`run_fts_recurate_shadow_integrity_gate` functions remain directly callable for
the patched-ICU FTS re-curation rework, but the maintenance flow does not invoke
them. An incoming
curated source can therefore exercise the source exception, but no reachable
maintenance step creates that curated state. The staged FTS rebuild restores
the complete external-content `fts4_tag_titles_icu` set, and the disabled
re-curation call leaves that complete set in the published database.

Post-swap FTS maintenance is optimize-only.
`PLEX_PROCESS_BLOB_DB=1` enables the same staged rebuild for the Plex blob
database without the `statistics_bandwidth` hook, metadata date repair, or
STAT4 pass. When `PLEX_TRIM_FINISHED_SEASON_BLOBS=1` is also set, a separate
`sqlite3 -readonly` process reads the main database and writes distinct
`media_part` ids to an owned scratch file beside the staged blob database. It
selects episode rows (`metadata_type=4`) owned by season rows
(`metadata_type=3`) whose integer maximum coalesced episode air/add date is at
or before the fixed 24-month cutoff. A staged-only write connection imports
that file into a TEMP table and deletes only `blob_type=5` rows with
`linked_type='media_part'`. Candidate, deleted, total, and target counts must
conserve before COMMIT. The trim runs in the pre-swap hook after staged FTS
rebuild and its integrity gate. Staged optimize SQL runs after the trim. Zero
candidates skip its threads-only VACUUM, while nonzero or unparseable counts run
it. Read, import, DELETE, and VACUUM failures keep the existing
warn-and-continue envelope, and the trim hook has no local integrity gate. The
unconditional final staged integrity gate runs later, after staged optimize SQL,
and blocks publication before the `user_version`, `application_id`, and atomic
publication gates. Plex SQLite is required because Plex data can require the
ICU-enabled binary.

The Plex main-database staged optimize SQL creates the runbook-validated
`idx_dshadow_taggings_tag_id_metadata_item_id` and
`idx_dshadow_mis_account_updated_guid_cover`, and
`idx_dshadow_metadata_items_section_added`,
`idx_dshadow_metadata_items_guid_nocase`, and
`idx_dshadow_metadata_item_views_account_grandparent_guid` indexes before
`REINDEX`, `ANALYZE`, and `PRAGMA optimize`. The existing Plex SQLite `ANALYZE`
remains as the STAT1
floor for ICU-collated or skipped objects. The subsequent STAT4 pass uses
the patched `PLEX_BINARY`, runs only after the Plex staged SQL, discovers safe
targets from `pragma_index_list`/`pragma_index_xinfo`, skips unsupported
collations and unsafe identifier transport, sets `analysis_limit=0`, and warns
without aborting on discovery, per-target `ANALYZE`, or final `sqlite_stat4`
row-count failures. `icu_root` indexes are safe targets. The patched
`PLEX_BINARY` ICU engine auto-registers `icu_root`, so ANALYZE uses no explicit
collation-registration preamble.
`main()` derives the internal STAT4 gate from `GENERIC_SQLITE_BINARY` preflight
for configured Plex instances, so the pass runs only when that binary reports
`ENABLE_STAT4`; execution remains on `PLEX_BINARY` because only the patched ICU
engine can register `icu_root`. The explicit Plex STAT4 leader list includes all
five `idx_dshadow_*` Plex indexes.

### Emby Flow

For each Emby instance, the script resolves `/opt/<instance>/data`, runs the
sanity query, runs source FTS integrity in warn-and-rebuild mode, hard-fails on
unsafe or unclassified FTS metadata, runs the framed source whole-DB integrity
gate described above, warns on foreign-key rows, switches the source to
`journal_mode=DELETE`, and publishes a dated `.original` backup. The same gate
limits apply to Emby: malformed-index findings for any FTS3/4/5 table are
deferred, nonzero checker execution and framing failures fail open, and
unmatched FTS5 `fts5:` xIntegrity mismatch rows hard-abort.

The rebuild runs under `GENERIC_SQLITE_BINARY` with
`PRAGMA page_size=${_PAGE_SIZE}`, `PRAGMA auto_vacuum=NONE`, and
`VACUUM INTO` to a staged `<db>.new` file. The staged file must pass page-size
and auto-vacuum checks, an exhaustive source-vs-staged per-table row-count
sweep, FTS rebuild, the staged FTS integrity gate, and staged optimize SQL. The
pre-copy source whole-DB gate is present; the pre-rebuild staged whole-DB gate
is absent. The unconditional final post-rebuild staged
`integrity_check == ok` runs before the `user_version`/`application_id`
preservation gate. The script then replaces the live database with `mv` and
removes stale WAL/SHM siblings.
The Emby staged optimize SQL creates the runbook-validated
`idx_dshadow_mediaitems_parent_type`, `idx_dshadow_emby_latest_gk_dc`,
`idx_dshadow_emby_latest_movies_dcn_puk`, and
`idx_dshadow_emby_latest_movies_puk_dc_cover` indexes before `REINDEX`,
`ANALYZE`, and `PRAGMA optimize`.

Post-swap FTS maintenance is optimize-only because the database has already
traversed the source gate under the limits above and passed the staged
validation gates.

### Maintenance Posture

Before per-instance work, hard failures include unsupported arguments (direct
status `64`), absent resolved config, and config source failure. Hard
per-instance failures include
Docker stop/start verification failure, Docker query failure, running
containers after stop, source FTS discovery or metadata-classification failure,
a source integrity finding classified as hard in a framed no-`ok` payload,
including FTS5 `fts5:` xIntegrity mismatch rows, `journal_mode=DELETE` failure,
backup publication failure, `VACUUM INTO` failure, staged auto-vacuum mismatch
against `NONE`, staged FTS integrity failure, per-table row-count mismatch,
pre-swap or post-maintenance hook failure, and final staged integrity failure
after all hooks and optimize SQL. After a successful stop, these failures still
fall through to the start gate before the script continues. The source and
final post-rebuild staged gates are the two whole-DB
`PRAGMA integrity_check` calls. The source gate applies the framed verdict and
FTS exception rules above; the final gate requires literal `ok` before
publication.

Warning-and-continue cases include source FTS integrity mismatches, family-wide
`malformed inverted index` findings for any FTS3/4/5 table, FTS
checker-inability rows, and source integrity-check execution or framing
failures, which the gate classifies as not demonstrating corruption. They also
include source `foreign_key_check` rows, skipped missing instance directories,
Plex `statistics_bandwidth` table absence or DELETE/VACUUM failure before the
final staged integrity gate, staged maintenance SQL failure, staged metadata
date-repair failure, Plex STAT4 preflight/worklist/per-target/final-count
failures, Plex blob-trim read/import/DELETE/VACUUM failures before the final
staged integrity gate, and post-swap FTS maintenance failures.

For an instance that reaches publication, a green result proves that the final
staged checker returned literal `ok`. On the source side, it proves only that a
framed no-`ok` payload contained no unaccepted hard finding, or that the source
checker took one of the documented fail-open paths. It does not prove that the
original source was corruption-free: corruption that makes the source checker
exit nonzero is indistinguishable from a genuine engine failure at this gate.
