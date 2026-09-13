# Plex fuzzy MATCH canonicalization implementation — 2026-09-12

Branch: `feat/plex-fuzzy-match-canon-20260912`, based directly on origin's
`feat/plex-rewrite-fixes-20260912` at `93d0ad3534bdae440dc7de3e1ce5f7a7c1baa5c1`.
The delivery is one commit; `git log --oneline feat/plex-rewrite-fixes-20260912..HEAD`
shows only `feat: canonicalize Plex fuzzy MATCH literals before dispatch`.
No pushes, PRs, pin changes, deployment, or production database probes.

## Spec sections and implementation

| Spec | Implementation and evidence |
|---|---|
| §2 — semantics | `src/plex_fuzzy_match_rewrite.h:43`; `src/plex_fuzzy_match_rewrite.h:145`. Bytewise per-group dedupe; only ASCII alphanumeric words absorb. Preserve membership, order and case; reject known FTS position/ranking consumers. |
| §3 — eligibility | `src/plex_fuzzy_match_rewrite.h:21`. One token-safe exact table/column MATCH literal; raw comment guard, DQS/quotes, single-space flat grammar, OR layout, keyword/empty-group/noop diagnostics. OR* remains a term. |
| §4 — transformation | `src/plex_fuzzy_match_rewrite.h:145`; `src/plex_fuzzy_match_rewrite.h:169`; `tests/fixtures/plex-fuzzy-match/golden.tsv:1`. Deterministic dedupe then absorption, no group/term reordering; preserve suffix stars, doubled apostrophes and every surrounding SQL byte. |
| §5 — pre-pass and verification | `src/plex_fts_rewrite.c:182`; `src/plex_fts_rewrite.c:1917`; `src/plex_fts_rewrite.c:2163`. Literal-0 gate, original real-v2 prepare for full bind/column counts, both verify flags enabled, existing dispatch after splicing, verified caller-buffer tail mapping and original fallback. |
| §5 — catalogue and packaging | `src/rewrite_modes.h:27`; `docker-library/Dockerfile:123`; `tests/abi_obsolete_config_ops_test.sh:144`; `tests/check_pin_alignment.sh:785`. Wire `fuzzy+match_canon`, index eligibility 0; 21 modes, 8 index-eligible modes, 27 Plex mode tokens, 24 project COPY lines. |
| §5 — documentation | `docs/env-vars.md:46`; `docs/architecture/rewrite-engine.md:698`; `CLAUDE.md:87`; `docs/invariants/sqlite3-builds.md:561`. Gate, invariants, fail-open behavior and integration documented. |
| §6 — tests | `tests/plex_private_rewrites_test.sh:25`; `tests/plex_fuzzy_match_test.py:1`; `tests/plex_fuzzy_match_fuzz.c:1`; `tests/plex_fuzzy_match_native.py:1`; `tests/plex_fts_rewrite_smoke.c:1907`; `docs/architecture/smoke-tests.md:432`. Corpus goldens; adversarial inputs; idempotence; bounds/allocation/faults; all three prepare APIs; bind/column/tail contracts; join/title-scope composition; native collating parity and benchmark. |

## Artifact

Built, retained, and **not installed** on rs34:

```text
/opt/scripts/sqlite3-builds-fuzzy-20260912/artifacts/plex-icu69-linux-x86_64-v3/libsqlite3.so
sha256: a689980376c0df9f00520d61cfc207674f16318f3b8561bfb3a674568426f9f3
```

The final build log is `build-verified.log` in the same scratch root. Extracted
`/app/plex_fts_rewrite.c`, `/app/plex_fuzzy_match_rewrite.h`, and
`/app/rewrite_modes.h` were byte-compared against the transferred source. Their
SHA-256 values were also checked against the local staged Git blobs:

| Source | SHA-256 |
|---|---|
| `plex_fts_rewrite.c` | `fcbed4700f229d03a9c195b627395b9dae79881dcb2d4273cdb60c6a4e5a6da1` |
| `plex_fuzzy_match_rewrite.h` | `d526fdd6040be5d528a5f4be39b5ec004622f737a255977e6f454af9185f030b` |
| `rewrite_modes.h` | `f827bdf2d663156d48f5470a707b64d83a0e5a741e0ac449e8e3bb0e092f0e6b` |

## Validation commands

All retained rs34 work is under `/opt/scripts/sqlite3-builds-fuzzy-20260912`.
The source was transferred by `git archive` plus an explicit changed-file tar,
with CRLF normalized to LF in the scratch checkout. The earlier fixes checkout
was read only to inspect its build commands and copy the pinned stock SQLite
fixture library; Docker reused its dependency cache.

```bash
# In /opt/scripts/sqlite3-builds-fuzzy-20260912/repo:
export LD_LIBRARY_PATH=/opt/scripts/sqlite3-builds-fuzzy-20260912/stock/sqlite-amalgamation-3530300
export SQLITE3_CFLAGS=-I$LD_LIBRARY_PATH
export SQLITE3_LIBS="-L$LD_LIBRARY_PATH -lsqlite3"
SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE=0 bash tests/plex_private_rewrites_test.sh
SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE=1 bash tests/plex_private_rewrites_test.sh
bash tests/check_pin_alignment.sh
bash tests/check_multi_version_pin_alignment.sh
bash tests/check_custom_adapter_guard.sh
bash tests/check_build_sh_prechecks.sh
bash tests/check_obs_counts.sh
bash tests/check_pin_alignment_negative_test.sh
bash tests/check_multi_version_pin_alignment_negative_test.sh
bash tests/sqlite_build_workflow_mod_only_test.sh
bash tests/plex_fts_stat4_eqp_repro_test.sh ../baseline/sqlite3
```

The stock fixture shared library and shell use the pinned 3.53.3 amalgamation
with FTS4 and STAT4. This layer exercises the actual production prepare wrapper
through the existing adapter. The adapter supplies path/log hooks and injected
prepare faults; it does not substitute result sets or rewrite SQL.

```bash
# In /opt/scripts/sqlite3-builds-fuzzy-20260912:
bash build.sh > build-verified.log 2>&1
python3 repo/tests/plex_fuzzy_match_native.py \
  "$PWD/artifacts/plex-icu69-linux-x86_64-v3/libsqlite3.so" \
  "$PWD/native" lscr.io/linuxserver/plex:1.43.2.10687-563d026ea-ls309
bash baseline.sh
bash trace.sh
```

`build.sh` invokes `docker build --progress=plain -f docker-library/Dockerfile`,
`LIBRARY_VARIANT=plex`, `MARCH=x86-64-v3`, `BASE_IMAGE=$BASEIMAGE_ALPINE`, all
SQLite/mimalloc/source-id/ABI values from `pins/versions.env`, and ICU source
version/hash from the `icu69` row in `pins/library-compat-groups.tsv`. The image
is `sqlite3-plex-fuzzy-20260912:local`. `baseline.sh` runs the existing linked
smokes with the gate enabled and compiles/runs the obsolete-config ABI test
against the built ICU library. After the gate-on run exposed mode-global sampling
assertions, GUID/tag applied-log and On-Deck early-miss assertions were scoped to their owning mode.
The corrected Plex smoke source is compiled against the unchanged final artifact
and executed with the gate explicitly off and on. `trace.sh` compiles the existing trace/catalogue
smoke with its mocks, production observability/auto-extension/tracker/lexer,
`SLOW_QUERY_TRACKER_TEST_API`, and the pinned stock library.

## Final test results and output tails

| Check | Result |
|---|---|
| Private suites, fuzzy gate `0` | PASS; 26,062 reported assertions; 1,000,000 ASan/UBSan mutations |
| Private suites, fuzzy gate `1` | PASS; same assertion/mutation counts |
| FTS join | 1,913 assertions per pass |
| Title scope | 9,935 assertions, 832 cases per pass |
| Collection tag | 1,397 assertions per pass |
| Recent episode | 11,337 assertions, 502 page comparisons per pass |
| Review regressions | 99 assertions per pass |
| Hub bounds/binds/faults | 656 assertions per pass |
| Fuzzy corpus/adversarial/API/composition | 725 assertions, 13 accepted / 34 rejected per pass; seven literal gate values |
| Native collating parity | 352 comparisons / 704 EXCEPT directions, all zero differences; all accepted literals non-empty |
| Full Plex Dockerfile build, gate unset/off | PASS; 199 emitted PASS lines; no emitted advisory failures |
| Corrected Plex linked smoke, gate explicitly `1` | PASS; 36 emitted PASS lines |
| All baseline executables, gate `0` | PASS; 11 executables, 210 emitted PASS lines (including executable markers), zero FAIL lines |
| Trace/catalogue smoke | PASS; all 21 catalogue rows covered by the generated case array |
| Obsolete-config ABI against final Plex ICU library | PASS; all seven return-code checks accepted |
| STAT4 FTS plan reproduction | PASS against pinned 3.53.3 fixture shell |
| Pin/multi-version/guard/build-precheck/OBS/negative-pin/workflow preflights | 8/8 PASS |
| Staged source hashes vs built image; Git whitespace check | PASS |

Representative actual tails (full logs are in the rs34 scratch root):

```text
# private-gate-0.log and private-gate-1.log
PASS: fuzzy gate None
PASS: fuzzy gate ''
PASS: fuzzy gate '0'
PASS: fuzzy gate '1'
PASS: fuzzy gate '00'
PASS: fuzzy gate 'false'
PASS: fuzzy gate ' 0'
{"status": "PASS", "accepted": 13, "rejected": 34, "assertions": 725}
PASS: 200000 mutations, fuzzy guarded prefixes, allocation failure, idempotence
PASS: four private Python suites, four ASan/UBSan fuzzers, review regressions, hub bounds/binds/faults, fuzzy corpus/grammar/composition and ASan/UBSan

# native.log
{"status": "PASS", "parity_cases": 352, "accepted_literals": 13, "tokenizer": "collating root@colStrength=primary;colAlternate=shifted", "rows_per_table": 24018}

# baseline/plex-gate-off.log
PASS [ondeck-capture-miss-log]
PASS [negative-control-vendor-prepare]
PASS [negative-control-matcher-miss]
plex fts rewrite smoke passed

# baseline/gate-on.log
slow query concurrency smoke passed entries_used=4096
PASS slow_query_concurrency_smoke

# baseline/abi.log
pagecache_pre=0
heap_pre=1
pcache_pre=0
getpcache_pre=1
initialize=0
pcache_post=21
getpcache_post=21

# trace.log
stmt trace smoke passed

# stat4.log
0
plex fts stat4 eqp repro passed using ../baseline/sqlite3

# preflight/check_pin_alignment_negative_test.log
negative pin-alignment checks passed

# pins.log
pins aligned: sqlite=3.53.3 mimalloc=3.5.0 base_cmake=3.31.12 base_ref_script=build/base_image_ref.sh toolchain_key=60C317803A41BA51845E371A1E9377A2BA9EF27F icu_source=69.1 bases=ubuntu:18.04@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98,ghcr.io/linuxserver/baseimage-alpine:3.23 generic_glibc_max=2.27 counts=29/24 sorterref=512 pmasz=8192
```

No mandatory fuzzy-rewrite check remains skipped or failing. The inherited hub
synthetic planner test did not reproduce its original-only temp sort; as in the
base branch, it reports NOT REPRODUCED while retaining mandatory text/result
checks. The three truncated corpus rows have no execution benchmark. Generic
artifact builds, unrelated LSIO/maintenance tests, and full PMS/Emby daemon
first-init/deployment checks were not run: this task builds and tests the Plex
ICU library with fixtures and does not install or start a server instance.

## Corpus benchmark and native parity

Native PMS `collating 'root@colStrength=primary;colAlternate=shifted'`, SQLite
3.53.3 from the candidate, 24,018 synthetic rows per table, plain and
`prefix='2,3'` variants. Four column/table surfaces per valid literal: tag,
title, title_sort and original_title. **352/352 comparisons passed**, each
checking EXCEPT both ways (704 zero-difference directions). All 13 accepted
literals had non-empty results on every surface. The public wrapper emitted
`fuzzy+match_canon` for all 26 accepted tag-table executions (13 × 2 table
variants), with FTS and AUTOPRAGMA disabled; the 62 bare-pass executions did not
emit applied records.

The table measures tag queries, median of nine warm runs after one untimed warm
run, in milliseconds. The embedded shell timer reports at 1 ms resolution;
0 means below that resolution. Both original and explicitly canonicalized SQL
are timed with the rewrite disabled to isolate the literal transformation.
Native enabled executions separately prove the wrapper returns the same counts
and logs the expected canonical literal. No containers/builds were run in
parallel with the final timing loop. Production workloads on the shared host
were not stopped. Raw results: [JSON](plex-fuzzy-match-20260912-results.json).

| Corpus row / literal form | Hits | Plain original ms | Plain canonical ms | Prefix original ms | Prefix canonical ms |
|---|---:|---:|---:|---:|---:|
| 20: breaking bad OR | 2826 | 5 | 2 | 3 | 2 |
| 21: harry potter and OR (â capture) | 7065 | 9 | 7 | 8 | 6 |
| 22: the office star | 1414 | 52 | 32 | 14 | 3 |
| 23: the office OR | 4240 | 5 | 4 | 4 | 3 |
| 24: star wars star | 1414 | 42 | 39 | 22 | 8 |
| 25: game of thrones OR | 2827 | 7 | 3 | 3 | 3 |
| 26: star wars OR | 5652 | 9 | 8 | 8 | 5 |
| 27: breaking bad star | 1414 | 36 | 27 | 19 | 4 |
| 28: game of thrones star | 1414 | 34 | 27 | 14 | 3 |
| 29: harry potter and star | 4240 | 152 | 108 | 32 | 10 |
| 31: harry potter and OR (accented) | 7065 | 11 | 8 | 9 | 7 |
| 32: harry potter and OR (duplicates) | 7065 | 10 | 8 | 10 | 7 |
| 36: star wars OR (damaged capture) | 5652 | 7 | 6 | 11 | 6 |

The synthetic plain-table star-form improvements are smaller than the production evidence
in spec §1; these fixtures have far fewer distinct token expansions and deliberately
repeat complete alternative sets. The benchmark is mechanism evidence, with
no attempt to reproduce the production timing claims.

The remaining complete corpus literals are exact/prefix passes and are left
unchanged. Their two timing columns measure identical SQL in independent runs;
small differences are timer granularity and run variation, not rewrites.

| Corpus row / unchanged literal | Plain run 1 / run 2 ms | Prefix run 1 / run 2 ms |
|---|---:|---:|
| 1: `tom* hanks*` | 0 / 0 | 0 / 0 |
| 2: `to*` | 0 / 0 | 0 / 0 |
| 3: `star* wars*` | 4 / 3 | 3 / 3 |
| 4: `harry* potter* and*` | 4 / 4 | 3 / 3 |
| 5: `game* of* thrones*` | 3 / 3 | 1 / 1 |
| 6: `the* office*` | 2 / 2 | 1 / 1 |
| 7: `john* wick*` | 0 / 0 | 0 / 0 |
| 8: `breaking* bad*` | 1 / 1 | 0 / 1 |
| 9: `the*` | 1 / 1 | 0 / 0 |
| 10: `avatar*` | 0 / 0 | 0 / 0 |
| 11: `The* Office*` | 2 / 2 | 1 / 1 |
| 12: `Star* Wars*` | 4 / 4 | 3 / 4 |
| 13: `John* Wick*` | 0 / 0 | 0 / 0 |
| 14: `The*` | 1 / 1 | 0 / 0 |
| 15: `Breaking* Bad*` | 1 / 1 | 1 / 0 |
| 16: `Harry* Potter* And*` | 4 / 4 | 3 / 3 |
| 17: `Game* Of* Thrones*` | 4 / 4 | 1 / 2 |
| 18: `Tom* Hanks*` | 0 / 0 | 0 / 0 |
| 19: `Avatar*` | 0 / 0 | 0 / 0 |
| 30: `To*` | 0 / 0 | 0 / 0 |
| 33: `harry` | 0 / 0 | 0 / 0 |
| 34: `potter` | 0 / 0 | 0 / 1 |
| 35: `and` | 0 / 0 | 0 / 1 |
| 38: `the` | 0 / 0 | 0 / 0 |
| 39: `office` | 0 / 0 | 0 / 0 |
| 41: `star` | 0 / 0 | 0 / 0 |
| 42: `wars` | 0 / 0 | 0 / 0 |
| 43: `breaking` | 0 / 0 | 0 / 0 |
| 44: `bad` | 0 / 0 | 0 / 0 |
| 45: `game` | 0 / 0 | 0 / 0 |
| 46: `thrones` | 0 / 0 | 0 / 0 |

Rows 37, 40 and 47 are malformed/truncated captures. They are rejection tests,
not executable benchmark candidates. The raw JSON records them explicitly.

## Accepted corpus literals: original → rewritten

All 13 complete parenthesised captures are accepted: five star-form and eight
OR-form literals. The committed goldens include all 13, not just the six minimum
worked examples. The order below is the original TSV order; row numbers are
one-based corpus line numbers. Case and damaged capture bytes are preserved.

### Corpus row 20 (captured count 30)

```text
(breaking OR freaking OR breaching OR barking OR freking OR brooking OR bragging OR breukink OR preaching OR working OR breaking*) (bad OR bhad OR badd OR baud OR baid OR baïd OR badr OR badi OR bada OR bade OR bad*)
→
(freaking OR breaching OR barking OR freking OR brooking OR bragging OR breukink OR preaching OR working OR breaking*) (bhad OR baud OR baid OR baïd OR bad*)
```

### Corpus row 21 (captured count 30)

```text
(harry OR arry OR harryâ OR arya OR ary OR hary OR hhary OR aryeh OR haryu OR arye OR harry*) (potter OR poter OR poteri OR pooter OR potereau OR potter''s OR potters OR potterf OR pottru OR petter OR potter*) (and OR hand OR aand OR andy OR hands OR andi OR ando OR anda OR andre OR handy OR and*)
→
(arry OR harryâ OR arya OR ary OR hary OR hhary OR aryeh OR haryu OR arye OR harry*) (poter OR poteri OR pooter OR potereau OR potter''s OR pottru OR petter OR potter*) (hand OR aand OR hands OR handy OR and*)
```

### Corpus row 22 (captured count 28)

```text
(the* OR* they* OR* te* OR* tea* OR* thee* OR* theo* OR* tee* OR* teh* OR* thea* OR* teo* OR* the*)* (office* OR* officer* OR* offices* OR* officers* OR* officer''s* OR* official* OR* oficial* OR* officially* OR* officials* OR* official''s* OR* office*)*
→
(OR* they* tea* thee* theo* tee* teh* thea* teo*)* (OR* offices* officers* officer''s* oficial* officially* officials* official''s*)*
```

### Corpus row 23 (captured count 28)

```text
(the OR they OR te OR theo OR thea OR teh OR thee OR tee OR tea OR teo OR the*) (office OR offices OR officer OR office''s OR officers OR officer''s OR official OR oficial OR oppice OR ofime OR office*)
→
(te OR teh OR tee OR tea OR teo OR the*) (office''s OR officer''s OR official OR oficial OR oppice OR ofime OR office*)
```

### Corpus row 24 (captured count 27)

```text
(star* OR* starr* OR* stare* OR* starry* OR* stari* OR* starro* OR* står* OR* staar* OR* stars* OR* stair* OR* star*)* (wars* OR* war''s* OR* warsch* OR* wares* OR* waris* OR* wahres* OR* warsi* OR* warsha* OR* waaris* OR* bars* OR* wars*)*
→
(OR* stare* starry* stari* starro* står* staar* stars* stair*)* (OR* war''s* warsch* wares* waris* wahres* warsi* warsha* waaris* bars*)*
```

### Corpus row 25 (captured count 27)

```text
(game OR gameau OR gaume OR games OR game''s OR gamet OR gamer OR gamed OR gamel OR gámez OR game*) (thrones OR trones OR trønnes OR tyrone''s OR terrones OR drones OR tornes OR thorne''s OR tornéus OR torneos OR thrones*)
→
(gaume OR game''s OR gámez OR game*) (trones OR trønnes OR tyrone''s OR terrones OR drones OR tornes OR thorne''s OR tornéus OR torneos OR thrones*)
```

### Corpus row 26 (captured count 26)

```text
(star OR starr OR stahr OR stare OR starrr OR starry OR stara OR stahre OR starý OR starre OR star*) (wars OR war''s OR warsh OR warsch OR waris OR wares OR warsi OR warsha OR warris OR bars OR wars*)
→
(stahr OR stahre OR starý OR star*) (war''s OR waris OR wares OR warris OR bars OR wars*)
```

### Corpus row 27 (captured count 26)

```text
(breaking* OR* braking* OR* breaking2* OR* braiking* OR* freaking* OR* breaching* OR* preaking* OR* barking* OR* briking* OR* bracing* OR* breaking*)* (bad* OR* bhad* OR* badd* OR* baad* OR* båd* OR* baaad* OR* bada* OR* bade* OR* badu* OR* badi* OR* bad*)*
→
(OR* braking* breaking2* braiking* freaking* breaching* preaking* barking* briking* bracing*)* (OR* bhad* badd* baad* båd* baaad* bada* bade* badu* badi*)*
```

### Corpus row 28 (captured count 21)

```text
(game* OR* gayme* OR* games* OR* gamer* OR* game''s* OR* gamet* OR* gamera* OR* came* OR* same* OR* shame* OR* game*)* (thrones* OR* trenes* OR* drones* OR* tørnes* OR* thereness* OR* trainees* OR* trainee''s* OR* terrines* OR* trini''s* OR* tremes* OR* thrones*)*
→
(OR* gayme* games* game''s* gamet* gamera* came* same* shame*)* (thrones* OR* trenes* drones* tørnes* thereness* trainees* trainee''s* terrines* trini''s* tremes*)*
```

### Corpus row 29 (captured count 19)

```text
(harry* OR* ary* OR* hary* OR* háry* OR* arya* OR* array* OR* arr* OR* harry''s* OR* arroyo* OR* aarya* OR* harry*)* (potter* OR* pottery* OR* potere* OR* potter''s* OR* potters* OR* pottru* OR* potteries* OR* patter* OR* putter* OR* petter* OR* potter*)* (and* OR* hand* OR* hånd* OR* aand* OR* ånd* OR* andy* OR* hands* OR* andi* OR* handy* OR* handa* OR* and*)*
→
(harry* OR* hary* háry* arya* array* harry''s* arroyo* aarya*)* (OR* pottery* potere* potter''s* potters* pottru* potteries* patter* putter* petter*)* (OR* hånd* aand* ånd* andy* hands* andi* handy* handa*)*
```

### Corpus row 31 (captured count 6)

```text
(harry OR ary OR hary OR háry OR arya OR array OR arr OR harry''s OR arroyo OR aarya OR harry*) (potter OR pottery OR potere OR potter''s OR potters OR pottru OR potteries OR patter OR putter OR petter OR potter*) (and OR hand OR hånd OR aand OR ånd OR andy OR hands OR andi OR handy OR handa OR and*)
→
(ary OR hary OR háry OR arya OR array OR arr OR harry''s OR arroyo OR aarya OR harry*) (potere OR potter''s OR pottru OR patter OR putter OR petter OR potter*) (hand OR hånd OR aand OR ånd OR hands OR handy OR handa OR and*)
```

### Corpus row 32 (captured count 6)

```text
(harry OR ary OR hary OR hary OR arya OR array OR arr OR harry''s OR arroyo OR aarya OR harry*) (potter OR pottery OR potere OR potter''s OR potters OR pottru OR potteries OR patter OR putter OR petter OR potter*) (and OR hand OR hand OR aand OR and OR andy OR hands OR andi OR handy OR handa OR and*)
→
(ary OR hary OR arya OR array OR arr OR harry''s OR arroyo OR aarya OR harry*) (potere OR potter''s OR pottru OR patter OR putter OR petter OR potter*) (hand OR aand OR hands OR handy OR handa OR and*)
```

### Corpus row 36 (captured count 3)

```text
(star OR starr OR stahr OR stare OR starrr OR starry OR stara OR stahre OR star�� OR starre OR star*) (wars OR war''s OR warsh OR warsch OR waris OR wares OR warsi OR warsha OR warris OR bars OR wars*)
→
(stahr OR stahre OR star�� OR star*) (war''s OR waris OR wares OR warris OR bars OR wars*)
```

## Interpretation, deviations and remaining limits

- §6.1 says every parenthesised capture should be accepted, but TSV rows 37, 40,
  and 47 are truncated/unbalanced. Normative §3 and adversarial §6.3 require them
  to fail open as `grammar`. They are retained unchanged in the corpus and tests.
  The other 31 rejected literals are the bare exact/prefix passes.
- To enforce §2's membership-only scope, known matchinfo/offsets/snippet/rank
  calls fail open as `grammar`. The existing private-rewrite 64 KiB limit and
  embedded-NUL guard are retained. These are conservative eligibility guards.
- A final semicolon plus trailing whitespace is supported. The real original
  prepare determines the caller's tail; a second statement fails open. SQL
  bytes outside the MATCH literal remain unchanged, including quoted identifiers.
- Plex SQLite launches PMS's embedded shell, whose baked source-id check rejects
  a replacement library before tokenizer registration. The native test therefore
  copies PMS from a disposable stopped container, requires exactly one old
  84-byte source-id, and replaces it with the existing pinned source-id in that
  scratch copy only. The copy and candidate library are mounted read-only into
  network-disabled test containers. Neither an installed executable nor any pin
  is changed. Native tokenizer registration is real PMS `collating`, not a mock
  or an ICU-tokenizer approximation.
- Benchmarks use synthetic fixtures, not production distributions. No real DB
  corpus probes were needed. They measure warm prepare+execution of count queries,
  not end-to-end Plex request latency or a production speedup guarantee.
- Initial development checks exposed the host's old SQLite 3.34.1 (missing
  MATERIALIZED support), a stale 19-COPY negative fixture, the embedded shell's
  source-id check, and mode-global smoke sampling assertions. These were resolved
  by the pinned fixture runtime, updated 24-COPY negative test, scratch-only
  shell compatibility patch, and mode-scoped assertions respectively. Final
  verification uses the pinned runtime and corrected tests.
- The referenced `C:/Users/Eric/.claude/CLAUDE.md` is absent. Repository instructions
  were followed. Unrelated pre-existing untracked report files were left intact.
