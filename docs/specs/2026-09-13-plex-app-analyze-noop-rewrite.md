# Spec: Plex app-side ANALYZE no-op rewrite

Status: DRAFT for implementation (Codex). Author: Claude, 2026-09-13. Evidence: OPERATIONS.md 2026-09-13 13:30 CDT.

## 1. Problem

Plex Media Server runs "Database analyze: starting analyze (all tables)" after each library scan
batch that adds items. On these servers autoscan feeds scans continuously, so it fires ~7 times per
hour. With `SQLITE3_ANALYSIS_LIMIT=0` (required: sampled stats produced bogus `401 201 2` rows and a
5-11 s on-deck regression) that ANALYZE is a full-fidelity scan of an 18 GB database: 26-28 s each,
holding the write lock. Observed: server-wide stalls of 13-33 s, `Cannot begin transaction. database
is locked` from watch-state writers (`MetadataItemSetting.cpp`), scanner "Add to Database" stages of
10+ s. Planner statistics are already maintained externally by a weekly full ANALYZE with a schema
nudge (`/usr/local/sbin/plex-analyze-full.sh <container> --reload`), so the app-side ANALYZE adds
nothing except the stall.

## 2. Behaviour

When enabled, a bare application `ANALYZE` prepared on the Plex library database is replaced by a
statement that is a no-op for statistics but has the same interface: zero result columns, zero bind
parameters, no rows, success. Everything else is untouched.

Eligible statement text (after `plex_prepare_input_lengths`; comparison on the bytes between the
leading whitespace and the optional trailing `;` plus trailing whitespace; keyword comparison
ASCII-case-insensitive; exactly one statement; no comments, no double-quoted tokens):

- `ANALYZE`
- `ANALYZE main`
- `ANALYZE "main"` is NOT eligible (double-quoted token rule) — fail open.

Not eligible (fail open, `OBS_MISS_CAPTURE` sub-reason in parentheses): `ANALYZE <table-or-index>`
(`targeted`), `ANALYZE sqlite_schema`/`sqlite_master` (`targeted`), any trailing bytes other than
`;` and whitespace (`trailing`), multiple statements (`trailing`), comments (`comment`), DQS (`dqs`),
non-Plex target DB (existing target filter). Only the Plex library DB (`com.plexapp.plugins.library.db`)
is a target, via the existing `auto_extension_path_is_target` + basename check.

Replacement text: `ANALYZE sqlite_stat1` — a table that exists in every analyzed Plex DB, has no
indexes, so SQLite performs no sampling; it still opens and commits a tiny write transaction and
reloads the (unchanged) statistics, which preserves ANALYZE's observable interface. If
`sqlite_stat1` does not exist (never-analyzed DB), the rewritten prepare fails and the engine's
existing fail-open path prepares the original, so a fresh database still gets its first real
ANALYZE.

Verification through `plex_prepare_rewritten_sql` with `expected_column_count=0, verify=1` and
`expected_bind_count=0, verify=1`.

## 3. Gate and telemetry

- `SQLITE3_DISABLE_PLEX_APP_ANALYZE_REWRITE`: enabled ONLY on literal `0`; unset and every other
  value disable (same convention as the hub type-OR and fuzzy MATCH gates).
- `OBS_MODE_PLEX_APP_ANALYZE`, wire label `analyze+noop`, index-missing eligible 0.
  `rewrite_applied` sampled as for other modes (source and rewritten SQL are tiny).
- Independent of `SQLITE3_DISABLE_AUTOPRAGMA`, `SQLITE3_DISABLE_RUNTIME_OPTIMIZE` and
  `SQLITE3_ANALYSIS_LIMIT`. The mod's own maintenance/runtime-optimize code issues `ANALYZE main;`
  through its own connection in the planned-downtime tool, not through the Plex process; the rewrite
  applies only inside the Plex process prepare path like every other Plex rewrite, and the maintenance
  script must keep working (it runs with the rewrite gate unset).

## 4. Placement

Match before the fuzzy pre-pass in `plex_fts_rewrite_prepare` (cheapest check first: the statement
starts with `A`/`a` and is at most ~16 bytes), so no other matcher runs for it. Header-only
`src/plex_app_analyze_rewrite.h` in the style of the other private rewrites.

## 5. Tests (all required)

1. `ANALYZE`, `ANALYZE;`, `  analyze main ;`, `Analyze MAIN` → rewritten to `ANALYZE sqlite_stat1`
   (exact bytes), prepare succeeds, 0 columns, 0 binds, step returns SQLITE_DONE, and `sqlite_stat1`
   contents are unchanged afterwards on a fixture that has real stats.
2. Fail-open set: `ANALYZE t`, `ANALYZE main.t`, `ANALYZE sqlite_schema`, `ANALYZE "main"`,
   `ANALYZE main; select 1`, `ANALYZE /*x*/`, `-- c\nANALYZE`, `ANALYZE main x`, `ANALYZEX`,
   `PRAGMA optimize` — each prepares the original unchanged with the named sub-reason.
3. Gate semantics: unset, `1`, `00`, ` 0`, `false` → disabled; `0` → enabled.
4. Fixture without `sqlite_stat1`: rewritten prepare fails, original `ANALYZE` runs and creates the
   stats (fail-open path exercised).
5. Non-target DB filename: untouched.
6. Existing suites green with the gate on and off; `tests/check_pin_alignment.sh` counts updated;
   catalogue row, `docs/env-vars.md`, `docs/architecture/rewrite-engine.md`, CLAUDE.md gate sentence,
   invariants entry, `tests/plex_private_rewrites_test.sh` wiring.

## 6. Deployment note

Same sideload flow as `sqlite3-plex-fuzzy-20260912` (new artifact sha, compose gate `"0"` on both
servers, 7 s graceful recreate each). Stats are refreshed weekly by the existing crons; if a large
import ever needs fresh stats sooner, run `plex-analyze-full.sh <container> --reload` by hand.
