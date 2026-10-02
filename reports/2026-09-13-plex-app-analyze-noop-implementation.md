# Plex app ANALYZE no-op implementation — 2026-09-13

Implemented spec sections 2–5 in one local commit. No push, PR, pin change,
installation, deployment, live database probe, or change to an existing container.

Branch: `feat/plex-app-analyze-noop-20260913`
Base: `feat/plex-fuzzy-match-canon-20260912` at `48bef7cd7119e8f1deecc5a87829ec0bdda658d5`
Commit: `2af9ca46b5f14585bebab397865a5dcf4195166d`; its sole parent is the requested base.

```text
$ git log --oneline feat/plex-fuzzy-match-canon-20260912..HEAD
2af9ca4 feat: no-op bare Plex app ANALYZE behind an opt-in gate
$ git rev-list --count feat/plex-fuzzy-match-canon-20260912..HEAD
1
$ git diff feat/plex-fuzzy-match-canon-20260912..HEAD -- pins
(no output)
```

The commit body names sections 2, 3, 4 and 5 and ends with the two exact requested trailers.

## Changes by spec section

| Section | Implementation and evidence |
|---|---|
| 1 — problem | No operational changes. The supplied spec is included at `docs/specs/2026-09-13-plex-app-analyze-noop-rewrite.md:1`. |
| 2 — behavior | `src/plex_app_analyze_rewrite.h:9`: bounded byte matcher; ASCII case-insensitive ANALYZE/main, SQLite whitespace, one optional semicolon; targeted/trailing/comment/dqs rejection. `src/plex_fts_rewrite.c:2186`: existing target filter plus exact Plex basename. `src/plex_fts_rewrite.c:2192`: exact `ANALYZE sqlite_stat1` replacement; expected binds/columns both zero, both verify flags one through the existing prepare helper. Candidate failure uses the existing original-prepare retry. |
| 3 — gate/telemetry | `src/plex_fts_rewrite.c:181`: pthread_once/atomic literal-0 gate. `src/rewrite_modes.h:28`: PLEX_APP_ANALYZE, wire analyze+noop, index eligibility 0. `src/plex_fts_rewrite.c:2204`: OBS_MISS_CAPTURE with the four prescribed reasons. Successful preparation uses the common sampled applied logger. No AUTOPRAGMA/runtime-optimize/analysis_limit dependency. |
| 4 — placement | `src/plex_fts_rewrite.c:2186`: first matcher in the public entry, before the unchanged fuzzy pre-pass; successful ANALYZE returns immediately. Header inclusion at `src/plex_fts_rewrite.c:9`; Docker staging at `docker-library/Dockerfile:124`. |
| 5 — tests/docs | `tests/plex_app_analyze_test.py:1`: all required hit/miss/gate/fresh/non-target cases, all three prepare APIs, explicit lengths/tails, native-original comparisons, candidate fault injection, real stale stats. `tests/plex_fts_join_test_shim.c:17`: telemetry counters and candidate fault support; `tests/plex_private_rewrites_test.sh:26`: suite wiring. `tests/check_pin_alignment.sh:786`, `:804`, `:845`, `:926`, `:1310`: catalogue 22, index eligibility still 8, Plex producer tokens 30, project COPY count 25. Negative fixture updated at `tests/check_pin_alignment_negative_test.sh:442`; header staging at `tests/abi_obsolete_config_ops_test.sh:145`. Docs at `docs/env-vars.md:47`, `docs/architecture/rewrite-engine.md:735`, `CLAUDE.md:87`, `docs/invariants/sqlite3-builds.md:568`, `docs/architecture.md:54`. |
| 6 — deployment note | Deliberately not executed, per request. Artifact only. |

## Build and test environment

All remote work was confined to `/opt/scripts/sqlite3-builds-analyze-20260913`.
The previous fuzzy workspace was read only. Its build.sh, baseline.sh, private.sh,
preflight.sh, stat4.sh, source archive/normalization layout and stock fixture
library were inspected and reused as patterns. Its stock/ tree was copied into
the fresh workspace; dependency layers came from the existing Docker cache.
Source transfer used local tar archives containing the selected repository files,
normalized CRLF to LF, then scp and extraction under the fresh repo/ directory.
The final snapshot is `source-final.tar` there. Runtime tests used disposable,
network-disabled containers and synthetic databases only.

Build command on rs34:

```bash
cd /opt/scripts/sqlite3-builds-analyze-20260913
bash build.sh > build.log 2>&1
```

`build.sh` sources `repo/pins/versions.env`, selects `LIBRARY_VARIANT=plex`,
`MARCH=x86-64-v3`, and `docker-library/Dockerfile`, and passes the pinned SQLite,
source-id, mimalloc and base-image arguments. ICU 69.1 VERSION/SHA512 come from
the existing `icu69` row of `pins/library-compat-groups.tsv`, as in the prior build.
Image: `sqlite3-plex-analyze-20260913:local`.
Build exited 0; mandatory build/link/ICU/export and runtime checks passed.
Five advisory benchmark threshold overruns are listed below.

## Tests and output tails

Private suite commands (rs34, stock SQLite 3.53.3 with real preparation/execution):

```bash
cd /opt/scripts/sqlite3-builds-analyze-20260913/repo
export LD_LIBRARY_PATH=/opt/scripts/sqlite3-builds-analyze-20260913/stock/sqlite-amalgamation-3530300
export SQLITE3_CFLAGS=-I$LD_LIBRARY_PATH
export SQLITE3_LIBS="-L$LD_LIBRARY_PATH -lsqlite3"
SQLITE3_DISABLE_PLEX_APP_ANALYZE_REWRITE=0 bash tests/plex_private_rewrites_test.sh > ../private-gate-0.log 2>&1
SQLITE3_DISABLE_PLEX_APP_ANALYZE_REWRITE=1 bash tests/plex_private_rewrites_test.sh > ../private-gate-1.log 2>&1
```

Both exited 0. Each run passed eight Python suites: FTS join (1,913 assertions),
title scope (9,935), collection tag (1,397), recent episode (11,337), review
regressions (99), hub (656), fuzzy (725), and app ANALYZE (420): 26,482 reported
main-suite assertions, plus fresh-process gate checks. Five ASan/UBSan fuzzers
passed 1,000,000 mutations per run. No sanitizer or correctness failures.
The new suite covers six eligible forms, 18 misses, seven literal-gate states,
three never-analyzed fixtures (one per API), and four candidate-failure branches.

Additional commands, run from the fresh remote workspace:

```bash
bash preflight.sh > preflight-final.log 2>&1
bash baseline.sh > baseline.log 2>&1
bash extra.sh > extra.log 2>&1
bash stat4.sh > stat4.log 2>&1
bash verify.sh > verify.log 2>&1
```

- `preflight.sh`: eight checks passed: pin alignment, multi-version pins, custom
  adapter guard, build prechecks, OBS counts, both negative pin suites, workflow.
- Docker build ran existing linked suites with the app gate unset (disabled).
  `baseline.sh` reran all 11 executables with the gate on: ICU, config-after-dlopen,
  shutdown/reinit, auto-extension, runtime optimize, Plex rewrites, Emby rewrites,
  Emby direct tests, slow-query, atexit, and concurrency. All passed. It also ran
  Plex rewrites explicitly with gate=1 and the obsolete-config ABI probe with
  the correct ICU closure; both passed.
- `extra.sh`: statement trace and forced-no-int128 slow-query fallback passed
  with gate=0 and gate=1 (four suite executions).
- `stat4.sh`: existing FTS STAT4 EQP reproduction passed against the copied
  pinned stock fixture library.
- `verify.sh`: extracted the library and compared image source bytes to the
  final source for plex_fts_rewrite.c, plex_app_analyze_rewrite.h and rewrite_modes.h.
  It ran native.py against the finished ICU library through all public prepare
  APIs. Five disabled gate states passed 385 assertions each; literal 0 passed
  2,433 (4,358 total). Both real STAT1 and STAT4 stayed unchanged with gate=0;
  disabled ANALYZE refreshed stale stats; missing-stat1 original fallback created
  real stats. Sampled first/periodic applied SQL, fallback logging, and disabled
  telemetry silence passed.

Pass markers below count emitted PASS/smoke-passed lines, not unique test cases.
ABI uses return-code assertions and emits no PASS marker; its exit status was 0.

### build.log

Pass markers: 217; correctness failures: 0; advisory overruns: 5.

```text
#45 DONE 116.7s

#46 exporting to image
#46 exporting layers
#46 exporting layers 1.0s done
#46 writing image sha256:e3fd470cf521e7d9301ecdf58c7406fd994ef708177fad4c46500dccf7942757 done
#46 naming to docker.io/library/sqlite3-plex-analyze-20260913:local done
#46 DONE 1.0s
```

### private-gate-0.log

Pass markers: 28; correctness failures: 0; advisory overruns: 0.

```text
PASS: analyze gate '1'
PASS: analyze gate '00'
PASS: analyze gate ' 0'
PASS: analyze gate 'false'
PASS: analyze gate '0'
{"status": "PASS", "hits": 6, "misses": 18, "assertions": 420}
PASS: 200000 mutations, fuzzy guarded prefixes, allocation failure, idempotence
PASS: four private Python suites, four ASan/UBSan fuzzers, review regressions, hub bounds/binds/faults, fuzzy corpus/grammar/composition and ASan/UBSan, app ANALYZE exact bytes/stats/fail-open/gates
```

### private-gate-1.log

Pass markers: 28; correctness failures: 0; advisory overruns: 0.

```text
PASS: analyze gate '1'
PASS: analyze gate '00'
PASS: analyze gate ' 0'
PASS: analyze gate 'false'
PASS: analyze gate '0'
{"status": "PASS", "hits": 6, "misses": 18, "assertions": 420}
PASS: 200000 mutations, fuzzy guarded prefixes, allocation failure, idempotence
PASS: four private Python suites, four ASan/UBSan fuzzers, review regressions, hub bounds/binds/faults, fuzzy corpus/grammar/composition and ASan/UBSan, app ANALYZE exact bytes/stats/fail-open/gates
```

### preflight-final.log

Pass markers: 8; correctness failures: 0; advisory overruns: 0.

```text
PASS check_pin_alignment
PASS check_multi_version_pin_alignment
PASS check_custom_adapter_guard
PASS check_build_sh_prechecks
PASS check_obs_counts
PASS check_pin_alignment_negative_test
PASS check_multi_version_pin_alignment_negative_test
PASS sqlite_build_workflow_mod_only_test
```

### baseline/gate-on.log

Pass markers: 220; correctness failures: 0; advisory overruns: 0.

```text
[sqlite3-builds-obs] test 0 0 slow_query_stats sql="SELECT atexit_smoke" count=5 total_ms=5000.000000 mean_ms=1000.000000 stddev_ms=0.000000 min_ms=1000.000000 max_ms=1000.000000
PASS slow_query_atexit_smoke
BEGIN slow_query_concurrency_smoke
[sqlite3-builds-obs] 2026-09-13T18:29:48.132Z 178 178 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:48.132Z 178 178 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:48.132Z 178 178 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
slow query concurrency smoke passed entries_used=4096
PASS slow_query_concurrency_smoke
```

### baseline/plex-gate-off.log

Pass markers: 43; correctness failures: 0; advisory overruns: 0.

```text
PASS [tag-index-log-dedup]
[sqlite3-builds-obs] 2026-09-13T18:29:04.123Z 46 46 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:04.123Z 46 46 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:04.123Z 46 46 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
PASS [ondeck-capture-miss-log]
PASS [negative-control-vendor-prepare]
PASS [negative-control-matcher-miss]
plex fts rewrite smoke passed
```

### baseline/abi.log

Pass markers: 0; correctness failures: 0; advisory overruns: 0.

```text
[sqlite3-builds-obs] 2026-09-13T18:29:48.369Z 1 1 sqlite3_config op=SQLITE_CONFIG_GETPCACHE rc=21
pagecache_pre=0
heap_pre=1
pcache_pre=0
getpcache_pre=1
initialize=0
pcache_post=21
getpcache_post=21
```

### baseline/extra-gate-0.log

Pass markers: 2; correctness failures: 0; advisory overruns: 0.

```text
[sqlite3-builds-obs] 2026-09-13T18:29:33.527Z 22 22 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:33.527Z 22 22 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:33.527Z 22 22 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
stmt trace smoke passed
[sqlite3-builds-obs] 2026-09-13T18:29:33.844Z 1 1 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:33.844Z 1 1 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:33.844Z 1 1 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
slow query fallback smoke passed
```

### baseline/extra-gate-1.log

Pass markers: 2; correctness failures: 0; advisory overruns: 0.

```text
[sqlite3-builds-obs] 2026-09-13T18:29:34.414Z 22 22 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:34.414Z 22 22 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:34.414Z 22 22 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
stmt trace smoke passed
[sqlite3-builds-obs] 2026-09-13T18:29:34.712Z 1 1 sqlite3_config op=SQLITE_CONFIG_LOG rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:34.712Z 1 1 sqlite3_config op=SQLITE_CONFIG_SORTERREF_SIZE rc=0
[sqlite3-builds-obs] 2026-09-13T18:29:34.712Z 1 1 sqlite3_config op=SQLITE_CONFIG_PMASZ rc=0
slow query fallback smoke passed
```

### stat4.log

Pass markers: 1; correctness failures: 0; advisory overruns: 0.

```text
0
plex fts stat4 eqp repro passed using ../baseline/sqlite3
```

### verify.log

Pass markers: 7; correctness failures: 0; advisory overruns: 0.

```text
PASS native app ANALYZE gate=None: 385 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native app ANALYZE gate='1': 385 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native app ANALYZE gate='00': 385 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native app ANALYZE gate=' 0': 385 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native app ANALYZE gate='false': 385 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native app ANALYZE gate='0': 2433 assertions; exact bytes, APIs, STAT1/STAT4, fresh fail-open
PASS native telemetry: 2 sampled app ANALYZE records; first/periodic SQL text; fresh fallback; disabled silence
024d77c204baf5cf525f74dea4efd81fa35c3a693b99a138848823df7dd8bf6a  /opt/scripts/sqlite3-builds-analyze-20260913/artifacts/plex-icu69-linux-x86_64-v3/libsqlite3.so
```

## Artifact

Host: `rs34`

```text
/opt/scripts/sqlite3-builds-analyze-20260913/artifacts/plex-icu69-linux-x86_64-v3/libsqlite3.so
sha256: 024d77c204baf5cf525f74dea4efd81fa35c3a693b99a138848823df7dd8bf6a
```

Not installed anywhere. The checksum is also in `artifact.log` on rs34; full test
results and tails are in `validation-summary.json` there.

## Deviations, advisories and open questions

- No behavior deviations from normative sections 2–5. `PRAGMA optimize` is unrelated
  traffic, so it is unchanged and emits no app-ANALYZE capture reason. `ANALYZEX`
  is rejected as trailing. Invalid originals such as `ANALYZE main x` preserve
  SQLite's original error rather than being made valid. Multiple-statement
  fail-open tests compare native statement text and tail (SQLite prepares the
  first statement, not the entire input as one statement).
- The first preflight run found the negative test's old 24-COPY fixture.
  It was updated to 25 and the entire eight-check preflight rerun passed.
- The Docker build reported five advisory latency overruns with the new gate
  unset; they did not fail the build. No benchmark thresholds or unrelated
  implementations were changed:

```text
#44 49.58 BENCH ADVISORY FAIL [enabled-plex-observable-repeated-miss]: target_p50_ns=30627 limit=20000 target_p95_ns=34523 limit=100000
#44 50.14 BENCH ADVISORY FAIL [enabled-emby-large-miss]: target_p50_ns=169374 limit=30000 target_p95_ns=203941 limit=150000
#44 50.32 BENCH ADVISORY FAIL [enabled-all-large-miss]: target_p50_ns=182744 limit=30000 target_p95_ns=221814 limit=150000
#44 50.51 BENCH ADVISORY FAIL [enabled-mixed-large-nonmatch]: target_p50_ns=75091 limit=30000 target_p95_ns=109474 limit=150000
#44 50.82 BENCH ADVISORY FAIL [enabled-emby-observable-repeated-miss]: target_p50_ns=168148 limit=30000 target_p95_ns=181848 limit=150000
```

- The existing synthetic hub test did not reproduce an original-only ORDER BY
  sort. This was its documented best-effort plan check; mandatory byte/result
  equivalence and the separate FTS STAT4 reproduction passed.
- No required ANALYZE test was skipped. Full release-platform builds (generic,
  CLI, other architectures), PMS support-window startup/tokenizer runs, and
  deployment/maintenance operations were not run: this task requested one Plex
  ICU x86-64-v3 artifact and forbade production changes/probes. Existing private
  rewrite, linked ICU/runtime and telemetry suites were exercised as above.
- The referenced `C:/Users/Eric/.claude/CLAUDE.md` does not exist. Repository
  CLAUDE.md and the user's constraints were followed.
- No `-o` filename was supplied. This report is saved as
  `reports/2026-09-13-plex-app-analyze-noop-implementation.md`, outside the commit.
  Pre-existing untracked review reports were left untouched. No open behavior
  question blocks the implementation.
