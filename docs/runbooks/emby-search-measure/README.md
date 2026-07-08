# Emby Search Measurement Harness

## Purpose

This harness measures candidate SQL rewrites for the Emby FTS fan-out query. It renders baseline and candidate SQL from templates, verifies row identity first, and records EQP and median warm timing output on a host-side database copy.

## Prerequisites

- A readable Emby library database copy.
- A tuned `sqlite3` binary with read-only access to that copy.
- A writable scratch directory on the host.
- POSIX `sh`, `awk`, `grep`, `mkdir`, `sed`, `sha256sum`, `tee`, `tr`, and `date`.

The scripts open the database with `-readonly -batch` and generated SQL starts with `PRAGMA query_only=1`. They do not run `ANALYZE`, create indexes, use `INDEXED BY`, or wrap timed SQL in `count(*)`.

## Env Setup

1. Copy `env.example` to `env` in this directory.
2. Fill `env` with host-local paths, the Emby user id, ancestor id list, and FTS MATCH literals.
3. Keep `env` uncommitted. It is ignored by the repository.

Both scripts source `./env` and fail before doing any work when it is absent or missing required values.

## Run Order

Run from this directory on the host that can read the configured database copy:

```sh
sh identity.sh
sh timing.sh
```

Run `identity.sh` first for all default candidates, variants, and match cases. Run `timing.sh` only after identity output is clean. To narrow an expensive timing pass, set `CANDIDATES`, `VARIANTS`, or `MATCH_CASES` to space-separated names before invoking the script.

Default candidates are `COMBINED B8 B1 B2 B3 B4 B5 B5_INLINE B7`. Default variants are `type presentation`. Default match cases are `case1_or case1_and case2_or case2_and`.

## Candidates

| Name | Rewrite |
|---|---|
| `BASE` | Original query with only the MATCH literal varied. |
| `B1` | `WithItemLinkItemIds` top-level `union` becomes `union all`. |
| `B2` | `itemPeople2` membership `IN` becomes correlated `EXISTS`. |
| `B3` | `ListItemsExemptionForPlaylists` membership `IN` becomes correlated `EXISTS`. |
| `B4` | `A.Id in WithAncestors` becomes correlated `EXISTS`. |
| `B5` | `A.Id in WithItemLinkItemIds` becomes correlated `EXISTS` against the CTE. |
| `B5_INLINE` | Deletes `WithItemLinkItemIds` and uses the two-level correlated `EXISTS` form. |
| `B7` | Adds `AS NOT MATERIALIZED` to both CTEs. |
| `B8` | Adds `FtsCandidates AS MATERIALIZED`, then joins `mediaitems`. |
| `COMBINED` | Applies `B1+B2+B3+B4+B5+B7`, using direct `B5`. |

## Outputs

Each run writes a timestamped directory under `SCRATCH_ROOT`.

`identity.sh` writes `identity-summary.tsv`. A candidate is eligible for timing only when every row is `IDENTITY_OK` and the final status row is `STATUS identity_complete mismatches=0`.

`timing.sh` writes:

- `schema-audit.out`: existing relevant indexes plus the current CTE EQP audit.
- `eqp-summary.tsv`: baseline and candidate EQP materialization markers.
- `timings.tsv`: raw timer captures by arm and iteration.
- `medians.tsv`: median warm real time by candidate, variant, match case, and arm.

Check for empty identity diffs, candidate EQP changes that match the intended rewrite, median drops on OR residual cases, and no regression on AND controls.
