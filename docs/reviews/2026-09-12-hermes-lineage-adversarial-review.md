# Adversarial review of the Hermes sqlite3-builds snapshot

Review date: 2026-09-12.

Candidate: C:/Users/Eric/sqlite3-builds-hermes-20260910 (no git repository).
Baseline: C:/Users/Eric/sqlite3-builds-fork, branch main, HEAD 79482f7, following bc7591f and upstream 5af690c.

Unless prefixed BASELINE, file:line references refer to the candidate. The supplied statement that this snapshot produced deployed libraries is the review premise; I did not independently verify deployed binaries.

Method: read both rewrite-engine architecture documents, run recursive diff excluding .git, repeat with --strip-trailing-cr, inspect all four new headers, dispatch, shared lexer/prepare/probe paths, and the tests. The architecture documents differ only by the candidate's three-line introduction/link to the new FTS-join document. Normalized hashes also confirm the unchanged shared files listed below.

Neither source tree was modified. No build, deployment, network request, or remote-host contact was performed. No C compiler was found on PATH, and the candidate contains no ready-to-run SQLite library/amalgamation. Counterexamples below ran in memory using this machine's Python SQLite 3.50.4, including direct SQLite C API calls for the prepare/drop/step case. SQL replacements were reconstructed from the actual C string constants and splice boundaries; matcher acceptance was traced statically. These are reproducible SQL/API counterexamples, not claims that the candidate wrapper or deployed SQLite 3.53.3 was executed. The pinned candidate version is 3.53.3 (pins/versions.env:1).

## 1. Verdict per rewrite

**Title scope — SAFE-WITH-FIXES.** For ordinary Plex INTEGER PRIMARY KEY IDs and valid constant/bound MATCH expressions, the core relational transformation is sound: the tags.id equality excludes NULL-extended rows, the two LEFT JOINs can become inner joins, duplicate links remain, and the final metadata_items.id ordering fixes the sequence of distinct projected IDs (src/plex_title_scope_rewrite.h:5, :27, :31, :55). However, the forced INDEXED BY introduces an uncaught failure after schema invalidation (finding 2); moving MATCH ahead of metadata_type can change an empty result into a step-time error (finding 3). The tag equality is also reversed, which changes results under differing text collations on schemas the matcher does not reject (finding 6). No buffer overrun, lifetime defect, bind renumbering, or tail-mapping defect was found in the new builder.

**FTS join — UNSAFE.** Its scalar recognizer accepts every double-quoted token as a string (src/plex_fts_join_rewrite.h:33), even though SQLite resolves a matching name as a column. A fully accepted MATCH "title" query returns a tag originally, but the forced FTS-first loop cannot use the later metadata row's title and fails during sqlite3_step (finding 1). Preparation succeeds, so the common prepare-time fallback is ineffective. It additionally has the forced-index and MATCH-evaluation-order failures in findings 2–3. With genuine scalar constants/binds and a usable persistent index, its unchanged inner-join equalities preserve the pre-limit group counts; ORDER BY count(*) alone still does not promise the same selected IDs at a tied LIMIT boundary.

**Collection tag — SAFE-WITH-FIXES.** The three transformations preserve all joins, operand directions, scalar bytes, grouping, and projections (src/plex_collection_tag_rewrite.h:10, :16, :24). With the Plex tags.id primary key, tags.tag and tags.extra_data are constant within each group, so their bare aggregate projection is not an arbitrary representative problem; counts, min/max, and duplicate membership rows are preserved. The decisive defect is the post-prepare forced-index failure in finding 2. There is also no promise of identical row sequence: none of these shapes has ORDER BY, and a two-link fixture changes sequence after rewriting, as shown below. That is legal SQL behavior rather than changed relational contents, but it fails a literal requirement that every returned sequence remain identical.

**Recent episode — SAFE-WITH-FIXES.** For the expected integer-ID schema and real scalar operands, the added semijoin is logically redundant: every original show/season/episode output row supplies its own witness to the added recent_e/recent_c subquery. Original joins and multiplicities remain, and each WHERE bind occurs exactly once in its original order in the parameter CTE (src/plex_recent_episode_rewrite.h:21, :29, :43). However, it repairs malformed numeric tokens that should fail preparation (finding 5), and its non-total title ordering permits demonstrably different page contents (finding 4). The main comparison test does not fail on those differences (finding 7). Its unrestricted global recent-item prefilter also needs cost validation. No new C memory-safety defect was identified. It does not currently match the added_at-ordered spec query.

These are acceptance judgments for the code as supplied. SQL-defined equivalence and equality to one particular planner-selected sequence are distinguished explicitly: the latter is impossible to promise for these unordered/tied shapes merely by retaining the ORDER BY text.

## 2. Findings, most severe first

### 1. High: FTS-join matcher admits a column-dependent MATCH, then makes it unexecutable

**Locations:** src/plex_fts_join_rewrite.h:33, :36, :47, :72; src/fts_lex.c:101; src/plex_fts_rewrite.c:1858, :1713. The project explicitly enables DQS for DML in build/Build.sh:196.

The lexer calls both single- and double-quoted tokens STRING. plex_join_scalar accepts them without checking the quote character. Thus "title" below is accepted as a supposedly constant MATCH RHS, while SQLite resolves it to metadata_items.title. The original can put metadata_items before FTS; CROSS JOIN forces the reverse dependency.

Minimal fixture:

~~~sql
CREATE TABLE tags(id INTEGER PRIMARY KEY, tag_type INTEGER, tag TEXT);
CREATE TABLE metadata_items(
  id INTEGER PRIMARY KEY, library_section_id INTEGER,
  metadata_type INTEGER, title TEXT
);
CREATE TABLE taggings(tag_id INTEGER, metadata_item_id INTEGER);
CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id
  ON taggings(tag_id, metadata_item_id);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);
INSERT INTO tags VALUES(1,1,'Theater');
INSERT INTO metadata_items VALUES(1,1,1,'Th*');
INSERT INTO taggings VALUES(1,1);
INSERT INTO fts4_tag_titles_icu(rowid,tag) VALUES(1,'Theater');
~~~

Pass this statement without a terminal semicolon:

~~~sql
select distinct(tags.id)
from metadata_items
join taggings on taggings.metadata_item_id=metadata_items.id
join tags on tags.id=taggings.tag_id
join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id
where fts4_tag_titles_icu.tag match "title"
and tag_type=1
and metadata_items.library_section_id in (1)
and metadata_items.metadata_type=1
group by tags.id order by count(*) desc limit 100
~~~

The candidate replaces FROM through WHERE with:

~~~sql
FROM fts4_tag_titles_icu
CROSS JOIN tags ON fts4_tag_titles_icu.rowid=tags.id
CROSS JOIN taggings INDEXED BY idx_dshadow_taggings_tag_id_metadata_item_id
  ON tags.id=taggings.tag_id
CROSS JOIN metadata_items ON taggings.metadata_item_id=metadata_items.id
WHERE
~~~

**Observed:** original returns [(1,)]; replacement prepares successfully but stepping raises "unable to use function MATCH in the requested context". EQP shows original FTS index 2 (MATCH) versus rewritten index 0 (unconstrained scan).

**Fix:** use the single-quote-or-bind restriction already implemented at src/plex_title_scope_rewrite.h:12 for every FTS-join scalar. Reject double-quoted scalar tokens before constructing SQL. Add this exact positive-original/step-error regression; preparation-only tests are insufficient.

### 2. High: INDEXED BY survives into automatic reprepare, outside the fallback boundary

**Locations:** src/plex_fts_join_rewrite.h:50; src/plex_title_scope_rewrite.h:26; src/plex_collection_tag_rewrite.h:10; src/plex_fts_rewrite.c:838, :1689, :1826, :1840, :1857.

The presence probe protects the current preparation only. An idle prepared statement can outlive an index drop/rebuild. SQLite v2/v3 then automatically recompiles the saved rewritten SQL at step time, including its now-invalid hard-coded index. The original query has no such dependency and can recompile to a table scan.

Minimal fixture and admitted query:

~~~sql
CREATE TABLE tags(id INTEGER PRIMARY KEY,tag_type INTEGER);
CREATE TABLE taggings(tag_id INTEGER,metadata_item_id INTEGER);
CREATE TABLE metadata_items(
  id INTEGER PRIMARY KEY,library_section_id INTEGER,absolute_index INTEGER
);
CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id
  ON taggings(tag_id,metadata_item_id);
INSERT INTO tags VALUES(1,2);
INSERT INTO metadata_items VALUES(1,5,11);
INSERT INTO taggings VALUES(1,1);
~~~

~~~sql
select tags.id,metadata_items.id,metadata_items.absolute_index
from tags
join taggings on taggings.tag_id=tags.id
join metadata_items on metadata_items.id=taggings.metadata_item_id
where library_section_id=5 and tag_type=2
~~~

C API sequence: prepare the original and its generated replacement with sqlite3_prepare_v2; execute DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id on the connection while both are idle; then step them.

**Observed:** both prepare calls return SQLITE_OK. Original step returns SQLITE_ROW. Rewritten step returns SQLITE_ERROR: "no such index: idx_dshadow_taggings_tag_id_metadata_item_id". No concurrency is required; another connection changing the schema creates the same class of invalidation.

This is a new exposure for these three shapes. The inherited baseline taggings rewrite uses the index as an admission check but does not insert this INDEXED BY into its added membership predicate.

**Fix:** remove the mandatory INDEXED BY from these candidates if fail-open behavior across schema changes is required; retain CROSS JOIN and let SQLite select an available access path. A prepare-time existence recheck cannot repair this race. A statement-lifetime recovery mechanism would be substantially more invasive because it must retain original SQL, bindings, API flags and execution state; do not simply retry after rows may have escaped to the caller.

### 3. Medium: changing MATCH evaluation order changes success/error behavior even with genuine bind operands

**Locations:** src/plex_title_scope_rewrite.h:25, :65; src/plex_fts_join_rewrite.h:48, :72; src/plex_fts_rewrite.c:1716, :1752.

MATCH syntax is interpreted during execution. A malformed expression is harmless to a plan that eliminates every row before opening the FTS cursor, but raises an error if the rewrite opens FTS first. The builder deliberately leaves MATCH opaque and cannot know a future bind's contents.

For the fixture in finding 1, add:

~~~sql
CREATE INDEX tag_type_idx ON tags(tag_type);
ANALYZE;
~~~

Use its original admitted query with MATCH ?1 and tag_type=?2, binding ?1 to the text consisting of a double quote followed by unterminated, and ?2 to 999 or NULL.

**Observed:** original returns []; FTS-first replacement raises "malformed MATCH expression". Both preparations succeed.

The same problem exists in title scope. An independently checked minimal fixture has one metadata item of type 1, one tag of ID 1, their link, an FTS title row, an index on metadata_items(metadata_type), and ANALYZE statistics. This admitted statement binds ?1 to that malformed expression and ?2 to 999:

~~~sql
select metadata_items.id from metadata_items
join fts4_metadata_titles_icu
  on metadata_items.id=fts4_metadata_titles_icu.rowid
left join taggings on taggings.metadata_item_id=metadata_items.id
left join tags on taggings.tag_id=tags.id
where fts4_metadata_titles_icu.title match ?
and metadata_items.metadata_type=? and tags.id=1
order by metadata_items.title_sort collate icu_root asc,
metadata_items.originally_available_at asc,metadata_items.id asc
limit 100 offset 0
~~~

**Observed:** original metadata-type-first plan returns []; replacement raises a malformed-MATCH error. This requires suitable data/statistics, not any unusual ID schema. The existing tests exercise malformed MATCH predominantly where the original also reaches FTS.

**Fix:** do not describe prepare-only recovery as complete fail-open execution. If exact success/error preservation for arbitrary bound MATCH input is a hard requirement, these prepare-only forced-order rewrites cannot satisfy it as written. Constrain eligibility to a guaranteed valid-input contract or redesign evaluation/recovery with explicit statement and binding support. Parsing a few sample MATCH strings at prepare time does not validate later bindings.

### 4. Medium: recent-episode LIMIT/OFFSET can return different show IDs at title ties

**Locations:** src/plex_recent_episode_rewrite.h:22, :34, :51.

The original only orders by title_sort COLLATE icu_root. The added IN candidate set can change the outer iteration order, thereby changing which equally sorted show falls within a page. This is a concrete observable difference, though both outputs obey SQLite's SQL ordering contract.

Minimal integer-ID fixture:

~~~sql
CREATE TABLE metadata_items(
  id INTEGER PRIMARY KEY,parent_id INTEGER,library_section_id INTEGER,
  metadata_type INTEGER,added_at INTEGER,title TEXT,title_sort TEXT
);
CREATE TABLE tags(id INTEGER PRIMARY KEY);
CREATE TABLE taggings(metadata_item_id INTEGER,tag_id INTEGER);
CREATE INDEX parent_idx ON metadata_items(parent_id);
INSERT INTO metadata_items VALUES
  (1,NULL,5,2,0,'Show','same'),
  (2,NULL,5,2,0,'Show','same'),
  (1001,1,5,3,0,NULL,NULL),
  (1002,2,5,3,0,NULL,NULL),
  (2001,1001,5,4,2,NULL,NULL),
  (2000,1002,5,4,1,NULL,NULL);
~~~

Register icu_root as an ordinary lexical comparator for this ASCII fixture; both sort keys are identical under any normal ICU root collation. No ANALYZE or added_at index is needed.

~~~sql
select metadata_items.id from metadata_items
join metadata_items as children on children.parent_id=metadata_items.id
join metadata_items as grandchildren on grandchildren.parent_id=children.id
where metadata_items.library_section_id in (?1)
and (metadata_items.title NOT LIKE ?2
and metadata_items.id not in (
  select taggings.metadata_item_id from taggings
  join tags on tags.id=taggings.tag_id where tags.id=?3)
and grandchildren.added_at>?4
and (metadata_items.metadata_type=?5 or metadata_items.metadata_type=?6))
order by metadata_items.title_sort collate icu_root limit ?7 offset ?8
~~~

Bind (5, 'Exclude', 10, 0, 2, 10, 1, 0).

**Observed:** original [(2,)]; replacement [(1,)]. Original scans grandchildren then looks up ancestors; replacement iterates a show-ID set before descendants. Both still use a temporary ORDER BY b-tree in this fixture. The full pre-page bag is unchanged.

The 200 comparisons reconstructed from the checked-in recent-episode fixture produced no differences on local SQLite; this smaller alternate-plan fixture did. Passing those 200 cases therefore is not proof of identical pages.

**Fix:** specify whether the contract is SQL-defined equivalence or exact current page identity. For exact identity, reject the current non-total ordering unless a verified uniqueness condition applies, or change the application's query contract to include a unique tie-breaker and match that new shape. Silently adding an ID tie-breaker only to the replacement also changes the original query's observable choice and is not a proof of parity. Test tied pages and compare both the complete bag and the actual page.

FTS-join's ORDER BY count(*) DESC has the analogous unspecified tie-boundary issue (src/plex_fts_join_rewrite.h:98). Collection queries have no ORDER BY at all. For example, tags 1/2, metadata IDs 1/2, and links inserted as (tag_id=2,metadata_item_id=1), then (1,2), returned collection triples [(2,1,11),(1,2,22)] originally and the reverse after rewriting on the local integer-ID fixture. Its full bag remains identical.

### 5. Medium: recent-episode turns invalid numeric-token SQL into a successful query

**Locations:** src/fts_lex.c:92; src/plex_recent_episode_rewrite.h:10, :32, :43, :46.

The shared lexer tokenizes 0AND as NUMBER(0), IDENT(AND); SQLite rejects it as an invalid numeric token. Other builders copy the affected suffix unchanged, but recent-episode extracts the 0 and inserts a parenthesized scalar subquery before AND. The reconstructed text becomes valid.

Accepted original input, without a terminal semicolon:

~~~sql
select metadata_items.id from metadata_items
join metadata_items as children on children.parent_id=metadata_items.id
join metadata_items as grandchildren on grandchildren.parent_id=children.id
where metadata_items.library_section_id in (5)
and (metadata_items.title NOT LIKE 'Exclude'
and metadata_items.id not in (
  select taggings.metadata_item_id from taggings
  join tags on tags.id=taggings.tag_id where tags.id=10)
and grandchildren.added_at>0AND
(metadata_items.metadata_type=2 or metadata_items.metadata_type=10))
order by metadata_items.title_sort collate icu_root limit 10 offset 0
~~~

The matcher consumes the number and expected AND normally. The replacement contains:

~~~sql
WITH recent_parameters(p0,p1,p2,p3,p4,p5)
AS MATERIALIZED (SELECT 5,'Exclude',10,0,2,10)
...
grandchildren.added_at>(SELECT p3 FROM recent_parameters)AND
...
~~~

**Observed:** original errors with 'unrecognized token: "0AND"'; replacement succeeds and returns an eligible show in the one-show fixture.

**Fix:** validate the entire numeric lexical token and its termination before relocating it. In particular, reject a decimal capture immediately followed by SQLite identifier characters, including non-ASCII identifier bytes; do not infer a valid boundary merely because the next simplified lexer token spells AND or OR. Add 0AND and 2OR regression cases against real prepare, asserting original error behavior is preserved. A shared lexer fix needs regression coverage for its other consumers.

### 6. Medium, schema-dependent: title scope reverses collation precedence in a join equality

**Locations:** src/plex_title_scope_rewrite.h:27 versus :58; src/plex_fts_rewrite.c:838, :1786.

The original uses taggings.tag_id=tags.id; the replacement uses tags.id=taggings.tag_id. SQLite's left-operand column collation can decide text equality, so these are not interchangeable for every schema admitted by the filename/index-name gates.

Repro schema: metadata_items.id INTEGER PRIMARY KEY; tags.id TEXT COLLATE NOCASE; taggings.tag_id TEXT COLLATE BINARY; taggings.metadata_item_id INTEGER; the expected taggings index; a matching FTS title row at rowid 1. Insert tags.id='a', taggings.tag_id='A', metadata_item_id=1. Use the title statement from finding 3 with literal MATCH 'Th*', metadata_type=1 and tags.id='a'.

**Observed:** original []; replacement [(1,)]. No missing-index or preparation error invokes fallback.

This is not evidence that deployed Plex uses text IDs: the ordinary INTEGER PRIMARY KEY schema avoids this example. It refutes unconditional schema-independent equivalence, and the admission code does not verify that schema. TEMP tables can also shadow the unqualified names.

**Fix:** retain the original equality direction, an inexpensive correction. State and enforce the ID-affinity/schema assumptions needed for the additional transitive equality to FTS rowid. Recent-episode's added joins/membership also assume normal ID comparisons; preserve comparison semantics rather than claiming a general schema-independent relational proof.

### 7. Medium: the main recent-episode equivalence test succeeds with recorded mismatches

**Location:** tests/plex_recent_episode_test.py:30, :46.

On a row difference, the test only appends a diagnostic object to mismatches. It prints that array at the end and has no assertion or nonzero exit status tied to its contents. This weakens its central claimed release gate. The bare-bind check and one post-index-drop comparison do assert equality, but do not cover the full matrix.

**Fix:** fail on any difference that violates the intended contract. For deliberately unspecified tied order, separately assert the full typed multiset, sort-key ordering, and defined pagination expectations; make any accepted tie variance explicit rather than silently printing all differences.

### 8. Medium: the exact snapshot cannot satisfy its checked-in pin-alignment assertion

**Locations:** tests/check_pin_alignment.sh:14, :1037; CLAUDE.md:48.

The script requires the exact CLAUDE.md line saying there are 23 project COPY lines. The candidate line says 22. require_line uses grep -Fxq and exits through fail on mismatch. Thus this assertion necessarily fails if reached, independent of build environment. This is a source consistency/validation defect, not a runtime SQLite bug.

**Fix:** align the asserted source contract and its count; rerun the relevant non-build check after an authorized code change. I did not change or run the build pipeline. Docker actually received four new header COPY lines; the release evidence in narrative documents cannot be treated as proof that this exact final snapshot passed every gate.

### Cross-cutting safety conclusions and non-findings

**Bind numbering/count/name preservation.** FTS join, title scope, and collection tag replace a FROM region containing no bind sites, and copy the complete projection and predicate/order/page suffix. Token spelling and first occurrence order therefore remain identical for ?, ?N, :name, @name, $name, repeated names, and numbered holes, including SQLite-style extended variable names recognized by src/fts_lex.c:141. The title/collection restriction on double-quoted strings is intentional and safe. Recent-episode only admits ? and ?N (1–32766, with a 128-byte token cap), rejects named binds, and relocates the first six operands once in source order, ahead of unchanged LIMIT/OFFSET operands. Consequently bare binds do not gain names or new slots, and explicit/repeated numbered indices keep their first occurrence order. Out-of-range variables that reach SQLite still fail preparation and fall back. This is a structural proof, not a runtime verification: all four new dispatch calls pass verify_bind_count=0 (src/plex_fts_rewrite.c:1818, :1829, :1844, :1861). They do verify result-column counts: 1, 1/3/6, 1, and 1 respectively.

**nByte and pzTail.** Input lengths come from the unchanged helper at src/plex_fts_rewrite.c:1505: negative nByte uses strlen; positive lengths can include exactly one final NUL. The new builders reject lengths over 64 KiB, inconsistent scan/bounded spans and embedded NULs. Successful output records exclude that optional original NUL from scan_out_len, while retaining its byte in rewrite_len. The common helper always obtains the real tail and requires the rewritten end pointer, then returns zSql+scan_len, never a pointer into freed storage (src/plex_fts_rewrite.c:1643, :1707, :1723, :1750). Semicolon/multi-statement candidates are rejected; original preparation supplies its native tail. Partial buffers can still be valid complete statements, such as a shortened decimal LIMIT, and must be treated as such. UTF-16 prepares remain outside this rewrite path. No new boundary defect was found for valid caller-supplied buffer extents.

**Buffer sizes and ownership.** In the three FROM-splice builders, 0<=a<=b<=scan<=bounded<=65536; output size is a+fixed_insert+bounded-b, capped again before malloc(n+1), and the last byte is explicitly NUL. Their additions remain far below size_t/INT_MAX on supported platforms. Recent-episode moves the six original value byte strings into the CTE while removing the same bytes from their old locations. Its exact growth is 583 bytes: prefix 66, five commas, two closing/separator bytes, six 34-byte references, and extra predicate 306. It allocates bounded+774, leaving 190 bytes beyond the actual final NUL (src/plex_recent_episode_rewrite.h:39). The fixed %u formatting receives only 0–5 and fits ref[64]. Sequential span subtraction is monotone. No overrun, off-by-one, integer-size overflow, or unterminated successful buffer was identified.

Allocated candidates are either transferred to plex_prepare_rewritten_sql or freed after a failed index probe. The common helper frees its SQL buffer and finalizes any partial SQLite statement before retrying the original (src/plex_fts_rewrite.c:1673, :1716). Freeing that input buffer before finalization is not itself a use-after-free: SQLite's prepared representation does not borrow the caller's input buffer for its lifetime. Accepted v2/v3 statements retain their own SQL copy. No new leak/double free was found. These are static conclusions; no ASan/UBSan run was performed in this review.

**Recognition/fallback completeness.** All four require EOF after their fixed grammar. They reject compound SELECTs, terminal semicolons, extra expressions/predicates, unsupported aliases, input INDEXED BY/NOT INDEXED, and projection drift. Only the hard-coded parents/grandparents or children/grandchildren aliases are accepted. Title/FTS skip genuine comments through the lexer; comments removed from the replaced FROM region do not alter SQL token meaning. Strings are consumed as whole tokens, including doubled quotes, so embedded keywords/parentheses cannot create grammar anchors. Collection/recent reject any raw -- or /* occurrence even inside a quoted value: conservative false negatives, not injection. The explicit failures are FTS DQS admission and recent numeric token relocation. Builder allocation failure is indistinguishable from a miss and continues the inherited pipeline; index absence also continues it, so "fallback" can still mean an older baseline rewrite, not byte-identical original SQL. Failure after a candidate's real prepare retries the original directly. No step-time recovery exists.

**Concurrency.** The new gates at src/plex_fts_rewrite.c:167 use pthread_once before every read. Its initialization synchronization makes the recent-episode plain int safe and also makes collection's relaxed atomic store/load sufficient; title/FTS additionally use release/acquire. There is no evidence of a data race merely because these styles differ. Flag values are literal-1, cached once per process; changing environment variables later does not update them. The code does not protect against arbitrary concurrent process-environment mutation, nor promise support for violating SQLite's own connection-use rules. Builders keep per-call state on the stack/heap. The index check is not a schema lock spanning the statement lifetime, as finding 2 demonstrates.

**Crash/corruption exposure.** I found no new memory-corruption primitive or SQL path that directly updates library.db: admitted application statements and index probes are SELECTs. Wrong result/error behavior remains a production correctness issue even without file corruption. New execution plans can allocate more SQLite temporary structures or hold a read transaction longer; the unchanged SQLITE_TEMP_STORE=3 profile forces temporary storage to memory (build/Build.sh:232), so there is no general bounded-memory or bounded-latency guarantee. Allocation failure handling in the string builders does not cover SQLite execution-time memory pressure. Existing optional runtime ANALYZE hooks remain a separate inherited source of writes, discussed below.

## 3. Shared-engine changes and risk flags

The requested "shared-engine changes" are largely absent. CRLF/LF differences made the raw recursive diff misleading. After stripping trailing CR, the following files are identical to baseline, independently confirmed by normalized SHA-256 comparison:

| File | Behavioral delta and risk assessment |
|---|---|
| src/observability.c | No source behavior change. Its catalogue-driven metadata, per-connection applied counters and global miss/index counters grow automatically through OBS_MODE_COUNT (lines 37, 64, 100). Four appended modes preserve existing numeric IDs. Low incremental memory overhead; no new array-bound defect found. |
| src/slow_query_tracker.c | None. Existing PROFILE handling, runtime elapsed-time notification and internal-optimize suppression remain (line 921). Rewritten SQL can naturally change trace text/correlation keys and observed execution metrics. |
| src/runtime_optimize.c | None. Existing opt-in enablement (line 125), recursion/kill-switch gates (line 934), and 3s/15s execution deadlines (lines 70–71) remain. Do not attribute them to Hermes. |
| src/auto_extension.c and src/auto_extension_internal.h | None. Existing filename filter, lazy registration, readonly skip, tuning PRAGMAs and SQLITE3_ANALYSIS_LIMIT support are already in baseline (auto_extension.c:110, :173, :219, :246). There is no separate new-rewrite registration here. |
| src/fts_lex.c and src/fts_lex.h | None. New matchers reuse the existing simplified scanner. Its DQS classification and numeric-token limitations become dangerous in the new uses described above. |
| build/sqlite-amalgamation.patch | None. Existing public UTF-8 prepare redirection and step/reset/finalize/close optimize hooks remain. No new SQLite VM, pager, allocator or file-format patch. |
| build/build_static_sqlite.sh | None. No new wrapper behavior, artifact extraction behavior or build flags. |
| build/Build.sh, pins/versions.env | None. SQLite 3.53.3, mimalloc 3.5.0, DQS=1 and TEMP_STORE=3 are common to both trees. Candidate retains both small baseline commits. |
| docker-library/Dockerfile | Exactly four new COPY lines, 119–122, staging the four private headers. No changed base image, dependency version, runtime flags, linking, installation or library-selection behavior. Runtime risk arises from the included code, not a new Docker setting. |

Other substantive engine/wiring changes:

* src/plex_fts_rewrite.c:3 includes the four headers only for SQLITE_ENABLE_ICU. Its new once-only environment gates are at line 167. The public path remains observability -> Emby dispatcher -> Plex dispatcher -> matching hidden real prepare, using the original API kind/flags.
* Dispatch order is GUID-LIKE, recent episode, collection tag, title scope, FTS join, then the prior FTS-prefix/taggings/On-Deck pipeline (src/plex_fts_rewrite.c:1795–1905). Each successful new candidate returns immediately; these are alternatives, not automatically composed rewrites.
* Recent and collection are independent literal-1 opt-ins and are included in the all-disabled early exit. Title and FTS join additionally require the existing FTS gate (lines 1781, 1835, 1851). Their omission as independent terms from the early exit is correct given that dependency. SQLITE3_DISABLE_AUTOPRAGMA is not a rewrite kill switch.
* The existing target-filename and exact Plex basename tests still apply to all four (line 1786). New matching does not check the production schema/ID definitions or a table's resolved schema. It also applies to readonly connections when the target name matches.
* Collection/title/FTS allocate the candidate, then issue the existing read-only sqlite_master index-name/table-name probe; this does not check index DDL or provide a cost guarantee. Unusable partial indexes normally fail candidate preparation and fall back. Missing-index skip records use custom reason strings rather than the dedicated index-missing sampler. Recent has no index readiness probe.
* src/rewrite_modes.h:22 appends fts+tag-join-chain, fts+title-user-scope, collection+tag-first, and recent-episode-prefilter. The last two are marked index-missing eligible, although recent performs no index probe and collection uses obs_log_rewrite_skipped. This is imprecise telemetry metadata, not a memory error.
* tests/abi_obsolete_config_ops_test.sh:140 adds four header copies. tests/check_pin_alignment.sh updates catalogue cardinality 15->19, eligible count 5->7, producer counts, and COPY count 19->23; its contradictory CLAUDE assertion is finding 8. The workflow and build driver themselves are unchanged.

**Inherited-hook interaction worth retaining in the risk model:** index probes call the public sqlite3_step and sqlite3_finalize (src/plex_fts_rewrite.c:826, :833). A missing-index probe reaching DONE can therefore traverse the existing inline runtime-optimize hook if runtime optimization and telemetry are enabled and all its eligibility gates permit it. This is not new ANALYZE code, and optimization is opt-in, but new probe traffic broadens opportunities to reach that inherited machinery. Calling the probe SQL itself read-only is accurate; claiming the entire wrapped prepare path can never trigger inherited maintenance would be too strong.

**Recent prefilter cost:** src/plex_recent_episode_rewrite.h:22 searches every metadata row with added_at above the cutoff and joins it to its parent, without section or metadata-type restriction. It preserves the original filters outside the semijoin, so these extra candidates do not by themselves create output rows. They can still require a large global candidate set, a full scan without a suitable added_at access path, and another traversal of the surviving descendant joins. This can regress small/empty-section or broad-cutoff requests and cannot guarantee avoidance of an ORDER BY sort. No execution-cost fallback, progress bound, or cardinality admission gate is added.

## 4. Test coverage: useful evidence and missing gates

The tests are meaningful in several areas; they are not sufficient for the claimed universal equivalence/fail-open contract.

* **FTS join:** tests/plex_fts_join_test.py:144 exercises Boolean, prefix, phrase, Unicode and invalid MATCH strings; :157 independently compares aggregate counts; :187 covers bind variants; :213 checks EQP join order; :221 covers misses and bounds; :245 checks cleanup around injected preparation/tail/column failures. It uses actual SQLite execution through a C adapter, not mocked rows. Missing are the DQS column-dependent case, error-suppressing empty plans under alternate statistics, and index invalidation after successful preparation.
* **Title scope:** tests/plex_title_scope_test.py:20 uses both integer-affinity and no-affinity taggings, dangling/NULL links, duplicates and unusual bound values; :51 covers movie/episode title and title_sort cases with exact row comparison; :73 explicitly rejects DQS scalars; :77 checks comment-safe parsing. It does not vary the tags.id collation/schema, test the empty metadata-type/malformed MATCH combination under alternate plans, or drop the forced index while a rewritten statement is cached.
* **Collection adapter:** tests/plex_collection_tag_test.py:17 reduces expected results to column zero; plex_prepare_test_support.py's run helper similarly reads only column zero. Thus its main result check cannot detect wrong metadata_item_id/absolute_index or count/min/max/tag/extra_data values. It does exercise the three shapes, bind syntax, prepare API/nByte combinations, faults and rejects.
* **Collection native suite:** tests/collection-native/native_suite.py:28 addresses that weakness by comparing complete typed row multisets, parameter metadata, column names and tails; :43 adds an independent nonempty tag-ID oracle; :44 checks plan order. It is stronger semantic evidence. It uses fresh processes/connections, a small fixture, and only negative nByte in runner.c:12. Its missing-index test at :51 drops the index before a fresh preparation, not between prepare and step. It intentionally does not require a rewrite hit for legacy sqlite3_prepare (:40). Its text serializer stops at NUL (runner.c:8), so embedded-NUL text is not a faithful full-byte oracle.
* **Recent episode:** tests/plex_recent_episode_test.py uses 40 seeded fixtures and five cutoff/page cases, but only reports matrix mismatches (finding 7). Schema fields other than id have no affinity (:17), and the principal predicates use type 2 OR type 2 (:27), leaving mixed-type inclusion insufficiently tested. Sparse/reused/leading-zero numbered binds, named-variable misses across every slot, unusual scalar storage classes, near-cap input growth, numeric-keyword adjacency, and alternative-index tied-page cases are not covered. Its "concurrent drop" check drops an added_at index after earlier run() calls have finalized their statements (:42); it is a sequential fresh-prepare survival test. The off test covers unset only.
* **Mutation harnesses:** the four *_fuzz.c programs genuinely guard each seed prefix with an unreadable page, inject builder malloc failure, assert successful output termination and idempotence, and perform deterministic byte mutations. FTS and title cover two seeds each (200,000 mutations); collection covers three (300,000); recent covers one (100,000). They exercise the builders, not the production prepare helper or SQLite execution. Their 4 KiB buffers and short fixed seeds do not test accepted inputs near 64 KiB. The mutations mostly cause rejects; an accepted semantic change such as 0AND can pass every asserted fuzz invariant. There is no differential prepare/execute oracle, SQLite allocator-fault sweep, automatic-reprepare test or thread sanitizer coverage.
* **Adapter limitations:** tests/plex_fts_join_test_shim.c:44 stubs path eligibility and telemetry. Fault mode 1 returns a generic preparation error with no partial statement; modes 2/3 fake tail/column mismatches. These do not cover SQLite OOM during candidate compilation, authorizer callback behavior, schema auto-reprepare, parameter rebinding/reset cycles, or full observability/runtime-hook interaction.
* **Integration gap:** the new test runners are standalone. Neither the unchanged build/Build.sh nor .github/workflows/sqlite-build.yml calls the four new Python/fuzz suites, and no recent-episode shell runner is supplied. Header staging and the old regression suite do not imply these new cases execute automatically. Some shell runners write artifacts under the source build directory; none was run during this read-only review.
* **Native claims versus checked-in reproducibility:** title_native_full_suite.py checks the full library version and rewrite use but identifies its tokenizer as ICU root rather than PMS application's collating tokenizer. native_c_generated_validation.py:3 depends on external /tmp and /opt backup/query-pair paths. Narrative native-PMS success claims in docs are useful context, not executable evidence present in this snapshot. This review did not run those environment-dependent suites.
* **Remaining API/memory gates:** verify all bind names/indices, column names/types/origins, true bounded non-NUL input, optional pzTail=NULL, rebind/reset/clear_bindings, prepare flags, denied schema probes, unavailable collations/tokenizers, post-prepare schema changes, and allocation failure through the wrapper. Test the process-once gates concurrently and in combinations. Negative-case assertions must allow an inherited rewrite when the new mode correctly misses.

No test result from a different tokenizer, SQLite version or adapter should be represented as an execution test of the deployed candidate library. The local counterexamples in this review have the same explicit limitation.

## 5. Added_at-ordered statement: applicability, safer approach, coexistence

**The candidate does not handle the spec's statement today.** It requires ORDER BY metadata_items.title_sort COLLATE icu_root immediately after the closing filter parentheses (src/plex_recent_episode_rewrite.h:33–35). The spec's captured statement has an additional top-level AND metadata_items.metadata_type=2 and then ORDER BY metadata_items.added_at DESC, ... (BASELINE docs/specs/2026-09-12-plex-hub-type-or-rewrite.md:49). Merely accepting another ORDER BY list would still miss the intervening equality. The candidate also requires a single IN section value and both user-restriction predicates, unlike the broader statement family in the spec.

**The prefilter idea can be extended semantically, but is not a demonstrated fix for that plan.** A surviving original show/season/episode join row proves that the show has a descendant whose added_at exceeds the cutoff. That witness is independent of whether the final ordering uses show title or show added_at, so an equivalent semijoin can be added while preserving the outer equality, all joins, restrictions, order, LIMIT/OFFSET and duplicates. Do not confuse grandchildren.added_at (the recent-episode filter) with metadata_items.added_at (the show's ordering key), and do not push LIMIT into the prefilter or turn the final result into DISTINCT shows. But computing the global recent-ID set may scan many rows, alter access paths and still require the final sort. No code-level argument guarantees it solves the spec's avoidable temp-b-tree problem.

**Prefer the narrower redundant-OR deletion for the spec's exact case.** With C equal to A or B and the same column/comparison semantics, t=C implies (t=A OR t=B); if t is NULL, the outside equality already prevents the row passing WHERE. Removing the redundant conjunct therefore preserves SQL's three-valued WHERE semantics without moving scalar expressions, creating a CTE, adding a join/index dependency, changing bind order or replacing the statement's FROM clause. This directly targets the plan trigger described by the spec, although its production timings and plan observations were not independently verified here.

The spec still needs implementation scrutiny. Its opaque user-filter block may contain binds (for example title NOT LIKE ?), and its copied ORDER/LIMIT/OFFSET tail can also contain them. BASELINE spec:127 says binds are "section slot only" despite leaving those regions opaque. A correct implementation must either reject additional bind sites or count/verify all unchanged bind tokens using SQLite's numbering rules; validating only the section count would create false fallback. Scanning must also handle or reject comments and every accepted quote form, keep the OR at the required Boolean depth, verify the complete top-level equality token, and reject statement tails. Its tied added_at/title ordering still cannot promise identical tie-selected page IDs after a plan change.

**The approaches can coexist as separately gated, disjoint matchers.** Give the top-level-equality/added_at family the small OR-deletion transformation, and keep a corrected recent-prefilter mode for the title-ordered family where its benefit is separately established. Existing dispatch returns after the first successful candidate, so two enabled gates do not compose automatically. If a future matcher overlaps, define priority explicitly and test it. A combined rewrite would need one deliberate builder with fresh equivalence, bind, tail and plan tests; do not recursively feed generated SQL back through the public prepare pipeline.

Recommendation: implement the spec's narrow OR deletion for the added_at family, with the matcher/count corrections above. Do not broaden the current Hermes CTE matcher as the default solution to that separate planner problem. Before accepting any of these candidates as preserving arbitrary input behavior, address the proven lexical, execution-time fallback and ordering-contract gaps.

