# Rewrite Canary Runbooks

## Purpose

The canary harnesses validate production-shaped Plex and Emby rewrite candidates against copied databases. They preserve hard identity gates before timing and report adoption, medians, and non-fatal concerns for state or adoption drift.

## Prerequisites

- Readable source database files for Emby and Plex.
- A generic tuned `sqlite3` binary.
- A Plex-compatible SQLite binary for Plex grouped identity checks.
- A writable scratch directory with enough space for database copies and run output.
- Bash plus standard host tools used by the scripts: `awk`, `cat`, `date`,
  `df`, `grep`, `kill`, `mkdir`, `pgrep`, `rm`, `sed`, `sha256sum`, `stat`,
  `tee`, `timeout`, and `wc`.

The scripts create scratch database copies, run all SQL against those copies, and leave the original databases read-only.

## Env Setup

1. Copy `env.example` to `env` in this directory.
2. Fill `env` with local database paths, binary paths, scratch path, shape literals, expected counts, and index names.
3. Keep `env` uncommitted. It is ignored by the repository.

Both canary scripts source `../env` from their script directory and fail before doing any work when it is absent or incomplete.

## Run Order

Run the Emby canary from the repository checkout on the host that can read the configured Emby database:

```sh
bash emby/emby_rewrite_canary.sh
```

Run the Plex canary from the same canary directory on the host that can read the configured Plex database and Plex SQLite binary:

```sh
bash plex/plex_ef_canary.sh
```

The Emby canary refreshes a copy, materializes configured A, C, and D shape queries, tests current stats, runs `ANALYZE main` on the copy, and repeats under analyzed stats. The Plex canary refreshes a copy, creates the configured On Deck candidate index on the copy, tests current stats, runs `ANALYZE main`, and repeats under analyzed stats.

## Output Interpretation

Each run writes a timestamped directory under `SCRATCH_ROOT`.

Emby outputs include `results.tsv`, `identity.log`, `eqp-tagged.log`, `timings.tsv`, `medians.log`, `literals.tsv`, `concerns.tsv`, and `notes.log`. Identity mismatches hard-fail. Missing adoption is recorded as a concern and the run continues where the existing contract permits.

Plex outputs include `results.tsv`, `capture.log`, `concerns.tsv`, and `STATUS.txt`. Identity failures hard-fail with `DONE_WITH_CONCERNS` where applicable. Cleanup may report non-fatal orphan/state notes during failure handling.

Use `results.tsv` for median comparisons, the identity logs for pass/fail gates, EQP logs for adoption evidence, and concerns files for follow-up triage.

## Guardrail Compliance

- Per-query timeout: both canary scripts wrap warm-up and timed query execution
  with `timeout`, record timeout markers, and skip remaining iterations for a
  timed-out arm.
- Disk footprint: both scripts use one scratch database copy and run a disk
  preflight before refreshing it. Free space must be at least 2x the source DB
  size plus the configured margin.
- Cleanup: both scripts delete their copied database files at run end, including
  on failure. Run output directories remain for inspection.
- Pre-warm: both scripts pre-warm the copied database before timing blocks.
