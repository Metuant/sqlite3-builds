# Plex related-tag join-order implementation — 2026-10-02

Implemented the exact related-tag count rewrite on
`feat/plex-related-tag-order-20261002`, based on production tag
`rs34-live-20260913` (peeled commit `2af9ca46b5f14585bebab397865a5dcf4195166d`).
All required checks pass. The two requested live, read-only comparisons return
identical ordered rows and improve from 22.567 s to 0.022 s and from 24.401 s
to 0.399 s. Nothing was pushed or deployed; no existing container, compose
configuration, or `/opt/docker/mods` content was changed or restarted.

Implementation commit: `59247b461a74be8b36b2caaecf885ffc8f468397` —
`feat: add opt-in Plex related-tag join-order rewrite`.
The report is committed separately as
`docs: record rs34 related-tag rewrite validation`; its hash is available with
`git log -1 --format='%h %s' -- reports/2026-10-02-plex-related-tag-order-implementation.md`.
Both commits use `Metuant <hold.accounts@protonmail.com>` and end with:

```text
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01644Hv4EH76SxwLJdzKgDn1
```

The matcher uses the exact head, the existing On-Deck value parser and variable
limit handling, and 1..16 exclusions. It copies the body unchanged and replaces
only the FROM prefix with taggings → tags → related → metadata_items using
CROSS JOINs. SQLite prepares the entire original without stepping it to obtain
the bind count and tail. The shared helper verifies binds, two columns and tail,
and retries the original on failure. Semicolon-plus-whitespace input retains
its bytes and maps SQLite's tail correctly. The new gate is literal-0 opt-in;
the new catalogue mode is `related+join_order`, eligible=1. Pins are unchanged.

Files changed:

- `src/plex_fts_rewrite.c`: gate, exact matcher, fail-open diagnostics and dispatch.
- `src/rewrite_modes.h`: new mode.
- `tests/plex_related_tag_order_test.py`: exact bytes, ordered typed rows,
  positive/negative cases, bind identities, bounds, failures, gates, disjoint
  matcher heads and fixed-order plan regression.
- `tests/plex_fts_join_test_shim.c`: related-mode telemetry and fault injection.
- `tests/plex_private_rewrites_test.sh`: suite registration.
- `tests/plex_fts_rewrite_prepare_bench.c`: related head-miss and hit cases.
- `tests/check_pin_alignment.sh`: 23 catalogue rows, nine eligible modes and
  exact producer counts.
- `tests/check_pin_alignment_negative_test.sh`: rejects related-mode eligibility drift.
- `CLAUDE.md`: gate and catalogue conventions.
- `docs/architecture.md`: catalogue counts.
- `docs/architecture/rewrite-engine.md`: related-tag rewrite architecture.
- `docs/architecture/smoke-tests.md`: test coverage and plan/bench contracts.
- `docs/env-vars.md`: gate documentation.
- `docs/invariants/sqlite3-builds.md`: unchanged WHERE/GROUP BY invariant.
- `reports/2026-10-02-plex-related-tag-order-implementation.md`: this report.

The requested September 13 implementation report was absent from this checkout.
The actual September 13 build workspace, scripts and logs on rs34 supplied the
recipe and comparison evidence. The architecture sections, spec, conventions,
and commits `93d0ad3` and `2af9ca4` were reviewed.

## Build and artifact

Fresh workspace: `/opt/scripts/sqlite3-builds-related-20261002/` on rs34.
Source was packaged with Python tarfile from `git ls-files -s -z`, preserving
tracked executable modes and normalizing textual CRLF to LF. No `git archive`
was used. A remote check confirmed no textual CRLF files. The unrelated,
pre-existing untracked `reports/fts-prefix-evidence/` directory was excluded
and left untouched.

Build source tar: `source.tar`, SHA-256
`7b7b03ba7497840a9caecec107b0bc1129edd4b91ee74084922abfbe16d8fddc`.
The workspace's `build.sh` adapts the September 13 recipe and uses
`LIBRARY_VARIANT=plex`, `MARCH=x86-64-v3`, all version arguments from
`pins/versions.env`, and ICU version/SHA from the `icu69` row of
`pins/library-compat-groups.tsv` (ICU 69.1). SQLite is 3.53.3; mimalloc is 3.5.0.

Image: `sqlite3-plex-related-20261002:local`.
Image ID: `sha256:9f896346d43483ea8336ec4780819f6d3ffda12432631a14a4f923c999006459`.

Extracted artifact on rs34:

```text
/opt/scripts/sqlite3-builds-related-20261002/artifacts/plex-icu69-linux-x86_64-v3/libsqlite3.so
SHA256 e0c731d3aa7ef85b93b1a2f9bbc341dcd50d340a8c1af12b1e5f805d1e51f7e4
```

`verify.sh` extracts the library using a new ephemeral image container and
compares the image's `plex_fts_rewrite.c` and `rewrite_modes.h` against the
normalized source. It does not modify any existing container.

## Commands and results

The following commands were run on rs34 from the fresh workspace; scripts and
logs remain there for inspection. The build and baseline scripts use only
new, disposable containers for validation.

| Command | Result |
|---|---|
| `bash preflight.sh > preflight.log 2>&1` | PASS: all eight repository checks listed below. |
| `bash private.sh` | PASS: full private Plex suite with the new gate `0` and `1`; `private-gate-0.log`, `private-gate-1.log`. |
| `bash build.sh > build.log 2>&1` | PASS: final Docker build, required image smokes and new benchmark. Five existing advisory threshold failures are detailed below. |
| `bash baseline.sh > baseline.log 2>&1` | PASS: all eleven linked suites with new gate enabled, Plex suite also with gate disabled, and obsolete-config ABI checks. |
| `bash extra.sh > extra.log 2>&1` | PASS: STMT-trace and slow-query fallback suites with gate `0` and `1`. |
| `bash stat4.sh > stat4.log 2>&1` | PASS: `bash tests/plex_fts_stat4_eqp_repro_test.sh ../baseline/sqlite3`. |
| `bash verify.sh > verify.log 2>&1` | PASS: extraction/source comparison, six fresh native-library gate processes and native telemetry verification. |
| `python3 live.py > live.log 2>&1` | PASS: original Plex statements, read-only live database, timings and ordered-row equality. |
| `git diff --check` | PASS. |

`preflight.sh` runs these repository commands under `repo/`:

```bash
bash tests/check_pin_alignment.sh
bash tests/check_multi_version_pin_alignment.sh
bash tests/check_custom_adapter_guard.sh
bash tests/check_build_sh_prechecks.sh
bash tests/check_obs_counts.sh
bash tests/check_pin_alignment_negative_test.sh
bash tests/check_multi_version_pin_alignment_negative_test.sh
bash tests/sqlite_build_workflow_mod_only_test.sh
```

`private.sh` sets `SQLITE3_CFLAGS`, `SQLITE3_LIBS` and `LD_LIBRARY_PATH` to the
copied September 13 pinned stock SQLite 3.53.3 test build, then runs
`SQLITE3_DISABLE_PLEX_RELATED_TAG_ORDER_REWRITE=$gate bash tests/plex_private_rewrites_test.sh`
for gates `0` and `1`. All existing join, title-scope, collection, recent-episode,
review, hub, fuzzy and ANALYZE suites pass. The four private ASan/UBSan fuzzers
plus fuzzy fuzzer pass 1,000,000 mutations per gate run (2,000,000 total).

The new suite passes 3,604 assertions per full run: 324 positive API/length/tail
variants, 21 negative shapes, eight fresh gate processes, and 93 foreign SQL
heads. It verifies both projected columns as typed rows in the same order,
including duplicates and cross-section tags with no matching related rows.
The plan check asserts the rewritten order
`taggings → tags → related → metadata_items`. The synthetic original does not
put tags last; this uses the spec's permitted fixed-order assertion.

The eleven baseline executables are `icu_smoke`, `config_after_dlopen_smoke`,
`shutdown_reinit_smoke`, `auto_extension_smoke`, `runtime_optimize_smoke`,
`plex_fts_rewrite_smoke`, `emby_fts_rewrite_smoke`,
`emby_fts_rewrite_direct_test`, `slow_query_smoke`, `slow_query_atexit_smoke`,
and `slow_query_concurrency_smoke`. The additional executables are
`stmt_trace_smoke`, `slow_query_smoke_fallback`, and the ABI test extracted from
`tests/abi_obsolete_config_ops_test.sh` and linked against the final ICU artifact.

`native.py` loads the final image's actual library and passes 1,057 checks per
process for unset, `1`, `00`, ` 0`, `false`, and `0` (6,342 total). It verifies
three prepare APIs, exact statement bytes, binds, two columns, tail mapping,
rows, and misses. Actual stderr has `mode=related+join_order` with first and
periodic sampling, source/rewritten SQL, and filter/DQS/comment/trailing
sub-reasons. Disabled processes contain no record for that mode.

The initial build attempt failed compiling a benchmark call with extra
arguments; that test edit was corrected before the successful final build.
Initial private-test adapter/corpus issues were also corrected. No required
test failures remain.

## Prepare benchmark

The new related head-miss case passes: 4 threads, 1,600 samples, baseline
p50 995 ns, target p50 2,021 ns, delta 1,026 ns, p95 2,061 ns. Budgets are
20,000 ns p50 and 100,000 ns p95. The new matching-prepare case measures
p50 41,450 ns and p95 63,749 ns, including the complete original prepare.

These existing cases still report advisory threshold failures. All five also
failed their advisory thresholds in the September 13 build; these are not
required build/test failures.

| Existing advisory case | September 13 p50 ns | October 2 p50 ns |
|---|---:|---:|
| enabled-plex-observable-repeated-miss | 30,627 | 30,532 |
| enabled-emby-large-miss | 169,374 | 172,994 |
| enabled-all-large-miss | 182,744 | 183,215 |
| enabled-mixed-large-nonmatch | 75,091 | 75,113 |
| enabled-emby-observable-repeated-miss | 168,148 | 173,132 |

## Live read-only evidence

`live.py` copied `Plex SQLite`, `Plex Media Server`, and `lib/` from
`/opt/scripts/plexcli-brutus/` into the new workspace's `plexcli-related/` and
replaced only that scratch copy's `lib/libsqlite3.so` with the artifact above.
It verified the copied library's hash. This preserves the launcher's
`$ORIGIN` server and library layout.

Both processes used the following database URI, with no writes or maintenance:

```text
file:/opt/plex/Library/Application Support/Plex Media Server/Plug-in Support/Databases/com.plexapp.plugins.library.db?mode=ro
```

The environment set `SQLITE3_DISABLE_OBSERVABILITY=1`,
`SQLITE3_DISABLE_AUTOPRAGMA=1`, and `SQLITE3_DISABLE_RUNTIME_OPTIMIZE=1`.
The baseline left `SQLITE3_DISABLE_PLEX_RELATED_TAG_ORDER_REWRITE` unset;
the enabled run set it to `0`. SQL was fed on stdin with `.timer on`,
`.headers off`, `.mode list`, and `.bail on`, using Plex's ORIGINAL statement:

```sql
select taggings.id, count(*) as taggings_count from tags   join taggings on taggings.tag_id=tags.id   join taggings as related on taggings.tag_id=related.tag_id   join metadata_items on metadata_items.id=related.metadata_item_id  where taggings.metadata_item_id=1459870 and metadata_items.library_section_id=11  and tags.tag != ''  and tags.tag_type not in (313, 312) group by taggings.id;
```

The second statement substitutes item `638005` and section `10`; exclusions
remain `313, 312`. These are one before/after observation per item on the live
database, not a repeated statistical benchmark.

| Item / section | Gate unset: CLI real seconds | Gate `0`: CLI real seconds | Rows | Ordered rows equal |
|---|---:|---:|---:|---|
| 1459870 / 11 | 22.567 | 0.022 | 4 | Yes, byte-identical |
| 638005 / 10 | 24.401 | 0.399 | 128 | Yes, byte-identical |

Launcher-inclusive wall times were 22.587402 / 0.045417 s and
24.424400 / 0.416298 s respectively. Row-only SHA-256, identical before/after:

```text
1459870: e3ff80e1707b56359de336f5e78181e750420507f2a0dd55aa5cc541d42adb24
638005:  9d6a37705ae8da889b237cc75d5e89147455cc65bdbbc0cc603d5b5a7d94088a
```

Raw SQL, stdout, stderr and machine-readable results remain in
`/opt/scripts/sqlite3-builds-related-20261002/live/`; summary output is `live.log`.
Production deployment, container recreation, and Brutus production validation
were not performed. The artifact is ready for the user's separate deployment.
