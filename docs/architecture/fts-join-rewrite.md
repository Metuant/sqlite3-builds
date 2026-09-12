# Opt-in PMS aggregate FTS tag-search join order

## Status and scope

Originally developed against experimental checkout `5e5d06c42c6a48fb36ca0c438e1b67ea9607fdd8`.
The release candidate is rebased onto deployed release **2026.08.30-r2**,
commit `79482f781ba454a928949a7b341f3892c167c24e` (base tree
`3713049671a6dbb304fc8918005b2f9cbcadbc0d`), preserving mimalloc 3.5.0,
SQLITE_TEMP_STORE=3 and analysis-limit support. **Not deployed.**

Enable only in a separately approved, rebuilt mod trial:

```text
SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE=1
```

Unset, `0`, and every value other than the exact string `1` leave this new
rewrite disabled. The existing `SQLITE3_DISABLE_PLEX_FTS_REWRITE=1` disables
it too. Flags are read once per process, following the existing wrapper's
initialization model. Changing this switch does not modify a running statement
or stop an existing request.

The opt-in addresses the observed PMS aggregate tag-search shape, not every
SQL statement or every search endpoint. The previous FTS rewrite only adds
`unlikely(tag_type=...)`; it does not fix the remaining join order.

## What changes

For the recognized one-statement `SELECT DISTINCT(tags.id)` search, replace
only the FROM/JOIN region with:

```sql
FROM fts4_tag_titles_icu
CROSS JOIN tags ON fts4_tag_titles_icu.rowid=tags.id
CROSS JOIN taggings
  ON tags.id=taggings.tag_id
CROSS JOIN metadata_items ON taggings.metadata_item_id=metadata_items.id
```

This locks the loop order to FTS results -> matching tags -> taggings ->
metadata lookup instead of allowing all matching Movies to be visited for
individual tag hits. Original equality operand direction is preserved, too.

The SELECT prefix and complete WHERE/GROUP BY/ORDER BY/LIMIT suffix are copied
byte-for-byte. There are no parameters in the replaced region. This preserves
binding order, repeated named parameters, numbered slots, grouping counts,
DISTINCT and the caller's final LIMIT. No LIMIT is pushed down. No tie-breaker
is invented: equal-count ordering and boundary ties remain unspecified by the
original SQL.

**MATCH is opaque.** The recognizer does not parse or alter Boolean operators,
quotes, NEAR, Unicode, wildcards, or parameter values. It can match this SQL
shape with or without `*`; it is not a general Boolean-language rewrite.

## Fail-closed recognition and fallback

Recognition is deliberately strict:

- Existing main-library filename/path and ICU-build eligibility gates apply.
- Exact unaliased four-table inner-join graph and single tag-ID projection.
- One MATCH literal/parameter; bare or `unlikely(...)` tag_type equality;
  1–64 scalar section arguments; metadata_type equality, or the exact album
  variant `(metadata_items.metadata_type=<scalar> AND (metadata_items.remote IS NULL))`;
  GROUP BY tags.id; ORDER BY count(*) DESC and final scalar LIMIT.
- Scalar arguments can preserve single-quoted string, decimal integer and parameter forms;
  MATCH accepts a string or parameter, never a constructed SQL expression.
- Input/output are capped at 64 KiB. Positive nByte, optional terminating NUL,
  and returned tail pointers are checked. Embedded-NUL suffixes fall back.
- Unsupported joins, aliases, extra predicates, subqueries, compound queries,
  semicolon tails, OFFSET, expressions and other SQL shapes stay on the old
  path. False negatives are intentional.
- An existing read-only index-presence probe checks the required taggings
  index. Nothing creates, alters or analyzes an index or database here.
- Missing index/probe error: fall back to the pre-existing rewrite pipeline.
- Rewritten prepare error, unexpected tail or column count: finalize the
  candidate and retry the original SQL through the existing recovery path.
- Already-rewritten SQL is not rewritten again.

Telemetry uses the new appended catalog mode `fts+tag-join-chain`; existing
mode IDs are preserved. Docker build staging and the standalone ABI test's
header copy list include the new private header.

## Reproduce local validation

Requires a C compiler, SQLite development headers/library, Python with SQLite,
and AddressSanitizer/UndefinedBehaviorSanitizer support on Linux:

```bash
bash tests/plex_fts_join_test.sh
```

For headers in a nonstandard location and an installed SONAME-only library:

```bash
SQLITE3_CFLAGS=-I/path/to/sqlite/include \
SQLITE3_LIBS=-l:libsqlite3.so.0 \
bash tests/plex_fts_join_test.sh
```

The runner creates and deletes its own temporary databases. It compiles the
actual `src/plex_fts_rewrite.c` wrapper; the test adapter supplies only telemetry,
path-eligibility and real-prepare forwarding hooks. Query execution uses real
SQLite, not fabricated results. The Python reference driver is independent.

Validation covers Boolean/prefix/phrase/Unicode payloads, intentionally invalid
MATCH syntax, escaped quotes, duplicate taggings, dangling and NULL joins,
multiple sections, final limits, all three prepare APIs, parameter identities,
positive nByte and tail mapping, feature gates, missing index, simulated
prepare/tail failures, statement cleanup and EXPLAIN join order. FTS4 retains
internal prepared statements; leak checks compare against a warmed baseline
and require successful connection closure instead of mislabeling that cache.

The separate C recognizer test runs 100,000 deterministic byte mutations under
ASan/UBSan, every SQL prefix against an unreadable guard page, forced allocation
failure and idempotence checks. Three compiler warning categories are suppressed
for the unchanged parent wrapper (unused parameter, unused constants, signed
comparison); the new recognizer's sanitizer build uses unrestricted
`-Wall -Wextra -Werror`.

## Full-library and native-runtime validation

The release-r2-based Plex/musl build completed with SQLite 3.53.3, ICU 69.1,
mimalloc 3.5.0 and TEMP_STORE=3. All regression steps in the library Dockerfile
completed, and the obsolete-config ABI test passed against the built library.
A separate public-prepare-API runner passed 56 literal/bound MATCH cases using
synthetic rows, ICU root and the captured Gaius/Brutus STAT1 profiles. It checked
results, complete count maps, errors, the selected join chain and fallbacks.
For the Brutus-profile `Th*` fixture, VM steps changed from 8,611,370 to 341,478;
this is isolated mechanism evidence, not a production speedup guarantee.

An isolated, network-disabled PMS 1.43.2.10687-563d026ea instance loaded the exact
candidate library (verified mapped inode/hash). One synthetic movie and three
genres were created through its scan/edit APIs. Its FTS table used the native
`collating 'root@colStrength=primary;colAlternate=shifted'` tokenizer. Eight
`/hubs/search` inputs returned identical genre IDs/counts in missing-index
fallback, enabled and explicitly disabled states; 36 native rewrite events
proved the hook executed. The fixture index was created only in the stopped,
synthetic database. The test container was stopped and removed afterwards.

The lower-level adapter suite separately passed 928 assertions, five gate
subprocesses and 100,000 ASan/UBSan mutations plus guard-page checks. Its stock
SQLite versions differ from the full library; do not conflate those layers.

## Album-query follow-up

The first production verification request traversed Audiobooks despite its
`sectionId=1` argument and pinned a reader on the remote-null album query. Do not
reuse that global endpoint as a supposedly section-scoped production smoke test.
The matcher now recognizes the exact captured parenthesized predicate and copies
it unchanged. Remote=0 and remote=1 must remain excluded; neither equals NULL.
The follow-up passed 1,905 wrapper assertions, 200,000 movie/album sanitizer
mutations, 112 full-library cases with both statistics profiles, and seven native
movie+audio search comparisons. Native telemetry confirmed album join rewrites.
The full library and sideload installer also passed their build/startup tests.

## Remaining release gates

No LSIO installable mod image has been published or deployed. The candidate
library is not a production rollout approval. Validate the installation package
and required index on the target, preserve the previous immutable image/config
as rollback, and obtain separate approval for any Brutus-only trial/interruption.
The small native fixture does not simulate production WAL pressure, concurrency,
large-library distributions or long-term stability. Forced order is not a
universal cost win. No production DB/configuration changes, source push or
production restart occurred during build/test.

## Execution limits (review findings 2–3)

The index-name probe is an admission check only. Generated SQL contains no
INDEXED BY; CROSS JOIN retains loop order and SQLite chooses the access paths,
including after schema invalidation between prepare and step.

Recovery is prepare-only. FTS-first evaluation can turn an empty result into a
step-time malformed MATCH error when the original plan would filter every row
before evaluating MATCH. Future MATCH bindings cannot be validated at prepare
time. Success/error parity for arbitrary malformed MATCH bindings is therefore
not guaranteed; no step-time retry is provided.
