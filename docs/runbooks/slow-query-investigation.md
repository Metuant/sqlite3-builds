# Slow-Query Investigation

## Purpose And Scope

This runbook investigates slow Plex or Emby SQLite queries and tests whether a
candidate planner-visible change improves them. Candidate changes include
indexes, refreshed statistics, compile/config tuning, and structural query
rewrites issued by a validated library-layer wrapper.

It is for diagnostics on copied databases. It is not a deployment procedure and
does not modify live application databases, replacement libraries, or
Plex-renamed ICU runtime files.

Use the inline generic two-copy harness skeleton in this runbook as the
starting point. Adapt it by replacing:

- The literal-setup query that derives realistic bind values from the copied
  database.
- The query body under test.
- The candidate DDL, PRAGMA, stat state, or rewrite SQL applied only to the
  candidate arm.
- The candidate-name mapping and copy-prep proof when testing more than one
  candidate.
- The row-identity digest query when validating a structural rewrite.

## Environment

Run on the SSH host against writable database copies. Never run experiments on
the original application database, the deployed `libsqlite3.so`, or
`libicu*plex.so.69`. Plex library replacement targets only
`/usr/lib/plexmediaserver/lib/libsqlite3.so`; tests do not write there.

Use the binary that matches the planner being tested. Planner fidelity means the
same SQLite version family, STAT4 posture, relevant compile options, registered
extensions, collations, tokenizers, and runtime PRAGMA profile as the runtime
path under investigation.

| Target | Binary | Planner fidelity |
|---|---|---|
| Emby / generic | `~/bin/sqlite3` | Generic build with STAT4; faithful for Emby/generic plan choice. |
| Plex bundled shell | `/home/darthshadow/plex-sql/Plex SQLite` | SQLite 3.39.4 with ICU and no STAT4; useful for bundled-shell behavior, not faithful for this repository's deployed Plex planner. |
| Plex deployed replacement | A STAT4+ICU build of the deployed Plex library, with `icu_root` and required PMS tokenizers registered | Required for faithful Plex plan-choice tests. |
| Generic FTS mirror | `~/bin/sqlite3` against a copied `unicode61` mirror | Useful for generic FTS row-shape and rewrite experiments when Plex ICU is unavailable; not faithful for Plex ICU ranking, collation, or plan-choice claims. |

The production query config is baked into the built artifacts by
`build/Build.sh` and `src/auto_extension.c`. Current compile/runtime values
include:

| Setting | Value | Source |
|---|---:|---|
| `SQLITE_DEFAULT_CACHE_SIZE` / `PRAGMA cache_size` | `-1048576` KiB, 1 GiB | `build/Build.sh`, `src/auto_extension.c` |
| `SQLITE_DEFAULT_MMAP_SIZE` / `PRAGMA mmap_size` | `34359738368` bytes, 32 GiB | `build/Build.sh`, `src/auto_extension.c` |
| `SQLITE_DEFAULT_PAGE_SIZE` | `16384` bytes | `build/Build.sh` |
| `SQLITE_DEFAULT_SORTERREF_SIZE` / `SQLITE_CONFIG_SORTERREF_SIZE` | `512` bytes | `build/Build.sh`, priority-102 constructor in `src/auto_extension.c` |
| `SQLITE_SORTER_PMASZ` / `SQLITE_CONFIG_PMASZ` | `8192` pages | `build/Build.sh`, priority-102 constructor in `src/auto_extension.c` |
| `SQLITE_DEFAULT_WORKER_THREADS` / `PRAGMA threads` | `8` | `build/Build.sh`, `src/auto_extension.c` |
| `SQLITE_TEMP_STORE` | `3` | `build/Build.sh` |

`~/bin/sqlite3` from this repository uses these compile defaults, so a plain
CLI timing run uses the production query config. The library constructor
reasserts startup-only sorter settings for deployed shared libraries.

The auto-extension PRAGMA block also sets `temp_store=2`,
`wal_autocheckpoint=16000`, `journal_size_limit=67108864`,
`busy_timeout=10000`, and `analysis_limit=0`. The compile-time
`SQLITE_TEMP_STORE=3` profile is stronger than the `temp_store=2` PRAGMA and
keeps temp storage in memory.

Before timing, prove the copied database is being opened by the intended binary:

```sql
SELECT sqlite_version();
PRAGMA compile_options;
PRAGMA cache_size;
PRAGMA mmap_size;
PRAGMA page_size;
```

Record the output. Existing databases keep their header page size; rebuilt or
new databases should reflect the 16 KiB default.

Production `sqlite_stat1` can be sampled or stale. Treat the copied database's
stats as the current production baseline unless the copy proves otherwise. A
sampled stat1 fan-out can resist otherwise-good indexes or adopt harmful ones.
Run the full A/B test twice:

1. `stat_state=current`: current copied stat state, with candidate indexes
   prepared but no fresh `ANALYZE main`.
2. `stat_state=analyzed`: after running `PRAGMA analysis_limit=0; ANALYZE main;`
   on both copies.

If an index helps only in the analyzed state, the recommendation is not "deploy
the index" alone. It is "deploy the index plus an ANALYZE maintenance path",
which is a separate deployment decision.

## Capture The Query

Start from the slow-query log and capture one shape as a portable fact packet.
Use the inline shape-packet template below, and write the new packet so it
stands alone:

```text
### Shape [stable-id]
- n=<samples> | max=<ms> | avg=<ms> | total=<ms>
- dbs=<database names> | containers=<count>

Query source:
- slow_query / slow_query_expanded / trace_stmt source line references.
- Captured literal or bind source.

Schema:
- Relevant CREATE TABLE statements.
- Relevant existing CREATE INDEX statements.
- Row counts for each referenced table.

Query shape:
- The normalized vendor SQL with placeholders.
- Notes for literal derivation.
- Full expanded SQL when available and safe to retain in incident notes.

Plan:
- EXPLAIN QUERY PLAN for the faithful binary and stat state.
- Per-value EQP matrix for representative predicate values.
```

The least invasive way to extract the exact app query is to read the
container's existing stdout/stderr log stream. Reading Docker logs is not live
database access and does not touch production database files. When statement
trace is already enabled, extract `trace_stmt` lines from the container log or
the collector that receives the app process stderr:

```sh
docker logs --since 30m <container-name> 2>&1 | grep ' trace_stmt '
```

When statement trace is disabled and an observation window is approved, enable
only the statement-trace knob and restart the container:

```text
SQLITE3_DISABLE_STMT_TRACE=0
```

Capture the minimum required lines, then restore the prior setting. Prefer
`slow_query_expanded` lines when literal bound values are required and the
incident context allows holding expanded SQL. Expanded SQL is max-PII because it
inlines bound values.

Derive literals from the copied database, not from memory or arbitrary ids.
Write a setup query that finds hot realistic values: for example, the most
common ancestor list, parent/type pair, user id, media type, tag type, or
played state that matches the slow shape. Prefer the highest matching row
counts that still represent the logged predicate family.

STAT4 can choose different plans for different predicate values. Build a
per-value EQP matrix before declaring a plan stable:

| Label | Value source | Cardinality | Faithful EQP summary | Candidate adopted |
|---|---|---:|---|---|
| hot | Highest realistic row count |  |  |  |
| typical | Median-ish realistic row count |  |  |  |
| rare | Low but present row count |  |  |  |
| empty | Valid predicate with no rows | 0 |  |  |

The timed SQL uses literal values, not `?` placeholders. Reproduce the full
vendor query:

- Keep the full projection, even if it has 25-60 columns.
- Keep the real `ORDER BY`.
- Keep `count(*) OVER()` when the vendor SQL has it.
- Do not reduce the query to a narrow projection.
- Do not wrap it in `SELECT count(*) FROM (...)`; SQLite can drop or reshape the
  sort, and the test stops measuring the logged work.

## Harness Methodology

Use a two-copy candidate A/B harness. The baseline copy has the candidate
absent. The candidate copy has the candidate present. Do not toggle with
`NOT INDEXED` or `INDEXED BY`; SQLite can ignore `NOT INDEXED` for a covering
index on the join-driver table, which silently corrupts the baseline.

The skeleton below is index-shaped by default because index tests need a
concrete `sqlite_master` state proof. For stat, PRAGMA, or rewrite candidates,
replace the prep and state-proof SQL with candidate-specific assertions before
running.

Do not execute a known-spinning original query. For a structural rewrite whose
baseline is already known to spin, capture EQP for the original, prove the
candidate plan flip, prove row identity, and time only the non-spinning
candidate arm. If a copied-database test leaves an orphaned CLI process, find it
by the exact copy database path and kill only the inspected matching pid:

```sh
pgrep -af -- '/path/to/copied.db'
kill <pid>
```

Do not use a broad `pkill sqlite3`; other diagnostics can be running on the
same host.

Copy this generic harness skeleton into temporary host scratch and fill in the
placeholders for the shape under test:

```sh
#!/usr/bin/env bash
set -euo pipefail
sqlite_bin=${SQLITE3_BIN:-"$HOME/bin/sqlite3"}
baseline_db=${1:?baseline writable copy}
candidate_db=${2:?candidate writable copy}
query_name=${QUERY_NAME:-query_under_test}
query_sql=${QUERY_SQL:-query_under_test.sql}
setup_sql=${SETUP_SQL:-setup-literals.sql}
candidate_index=${CANDIDATE_INDEX:-candidate_index_name}
candidate_ddl=${CANDIDATE_DDL:-'CREATE INDEX IF NOT EXISTS candidate_index_name ON table_name(cols);'}
run_baseline=${RUN_BASELINE:-1}
run_dir=${RUN_DIR:-slow-query-run-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$run_dir"
die(){ printf 'error: %s\n' "$*" >&2; exit 1; }
db_for(){ [ "$1" = baseline ] && printf '%s\n' "$baseline_db" || printf '%s\n' "$candidate_db"; }
should_execute(){ [ "$1" = candidate ] || [ "$run_baseline" = 1 ]; }
run_sql(){ "$sqlite_bin" -batch "$1" < "$2" > "$3" 2>&1 || die "$4 failed; see $3"; }
for copy in baseline candidate; do db=$(db_for "$copy"); [ -f "$db" ] && [ -w "$db" ] || die "$copy copy must be writable"; done
[ ! "$baseline_db" -ef "$candidate_db" ] || die "baseline and candidate copies must be distinct"
[ -x "$sqlite_bin" ] || die "faithful sqlite3 binary is not executable: $sqlite_bin"
printf 'DROP INDEX IF EXISTS %s;\n' "$candidate_index" > "$run_dir/baseline-prep.sql"
printf '%s\n' "$candidate_ddl" > "$run_dir/candidate-prep.sql"
cat > "$run_dir/candidate-state.sql" <<SQL
.mode tabs
.headers off
SELECT name, replace(replace(sql, char(10), ' '), char(13), ' ')
FROM sqlite_master WHERE type='index' AND name IN ('$candidate_index') ORDER BY name;
SQL
for copy in baseline candidate; do run_sql "$(db_for "$copy")" "$run_dir/$copy-prep.sql" "$run_dir/$copy-prep.out" "$copy prep"; run_sql "$(db_for "$copy")" "$run_dir/candidate-state.sql" "$run_dir/$copy-candidate-state.tsv" "$copy state"; done
[ ! -s "$run_dir/baseline-candidate-state.tsv" ] || die "baseline still has $candidate_index"
cut -f1 "$run_dir/candidate-candidate-state.tsv" | grep -Fx "$candidate_index" >/dev/null || die "candidate copy lacks $candidate_index"
run_sql "$baseline_db" "$setup_sql" "$run_dir/setup-literals.tsv" "setup literals"
# Convert setup output into realistic literal values, then write the full vendor SELECT to $query_sql.
[ -s "$query_sql" ] || die "missing materialized query SQL: $query_sql"
prewarm(){ cat "$1" "$1-wal" "$1-shm" 2>/dev/null >/dev/null || true; }
analyze(){ printf 'PRAGMA analysis_limit=0;\nANALYZE main;\n' > "$run_dir/analyze.sql"; run_sql "$baseline_db" "$run_dir/analyze.sql" "$run_dir/analyze-baseline.out" "baseline analyze"; run_sql "$candidate_db" "$run_dir/analyze.sql" "$run_dir/analyze-candidate.out" "candidate analyze"; }
write_eqp(){ { printf 'EXPLAIN QUERY PLAN\n'; cat "$query_sql"; } > "$1"; }
write_run(){ { printf '.timer on\n.print TAG stat_state=%s query=%s copy=%s adoption=%s phase=%s iter=%s\n' "$2" "$3" "$4" "$5" "$6" "$7"; cat "$query_sql"; printf '\n.timer off\n'; } > "$1"; }
timing_tsv="$run_dir/timings.tsv"; : > "$timing_tsv"
for stat_state in current analyzed; do
  [ "$stat_state" = analyzed ] && analyze
  prewarm "$baseline_db"; prewarm "$candidate_db"
  for copy in baseline candidate; do db=$(db_for "$copy"); eqp_sql="$run_dir/$stat_state-$copy-eqp.sql"; eqp_raw="$run_dir/$stat_state-$copy-eqp.raw"; write_eqp "$eqp_sql"; run_sql "$db" "$eqp_sql" "$eqp_raw" "$copy $stat_state eqp"; adoption=not-adopted; grep -F "$candidate_index" "$eqp_raw" >/dev/null && adoption=adopted; sed "s/^/EQP stat_state=$stat_state query=$query_name copy=$copy adoption=$adoption /" "$eqp_raw" >> "$run_dir/eqp-tagged.log"; if should_execute "$copy"; then write_run "$run_dir/$stat_state-$copy-warm.sql" "$stat_state" "$query_name" "$copy" "$adoption" warm 0; run_sql "$db" "$run_dir/$stat_state-$copy-warm.sql" "$run_dir/$stat_state-$copy-warm.out" "$copy $stat_state warm"; else printf 'SKIP stat_state=%s query=%s copy=%s reason=baseline_execution_disabled\n' "$stat_state" "$query_name" "$copy" >> "$timing_tsv"; fi; done
  for iter in 1 2 3 4; do [ $((iter % 2)) -eq 1 ] && order="baseline candidate" || order="candidate baseline"; for copy in $order; do should_execute "$copy" || { printf 'SKIP stat_state=%s query=%s copy=%s iter=%s reason=baseline_execution_disabled\n' "$stat_state" "$query_name" "$copy" "$iter" | tee -a "$timing_tsv" >/dev/null; continue; }; db=$(db_for "$copy"); adoption=$(grep -F "stat_state=$stat_state query=$query_name copy=$copy adoption=adopted" "$run_dir/eqp-tagged.log" >/dev/null && printf adopted || printf not-adopted); sql="$run_dir/$stat_state-$copy-$iter.sql"; out="$run_dir/$stat_state-$copy-$iter.out"; write_run "$sql" "$stat_state" "$query_name" "$copy" "$adoption" timed "$iter"; run_sql "$db" "$sql" "$out" "$copy $stat_state iter $iter"; awk -v s="$stat_state" -v q="$query_name" -v c="$copy" -v a="$adoption" -v i="$iter" '/Run Time/ { print "TIME stat_state=" s, "query=" q, "copy=" c, "adoption=" a, "iter=" i, $0 }' "$out" | tee -a "$timing_tsv" >/dev/null; done; done
done
awk '/^TIME /{key=$2 FS $3 FS $4 FS $5; for(i=1;i<=NF;i++) if($i=="real") vals[key,++n[key]]=$(i+1)+0} END{for(key in n){split(key,k,FS); for(i=1;i<=n[key];i++) for(j=i+1;j<=n[key];j++) if(vals[key,j]<vals[key,i]){t=vals[key,i]; vals[key,i]=vals[key,j]; vals[key,j]=t}; m=int((n[key]+1)/2); print "MEDIAN", k[1], k[2], k[3], k[4], "runs=" n[key], "real=" vals[key,m]}}' "$timing_tsv" > "$run_dir/medians.txt"
```

Prepare the copies:

```sql
-- baseline copy
DROP INDEX IF EXISTS candidate_index_name;

-- candidate copy
CREATE INDEX IF NOT EXISTS candidate_index_name
ON table_name (leading_column, next_column);
```

Prove candidate state separately on each copy:

```sql
SELECT name,
       replace(replace(sql, char(10), ' '), char(13), ' ')
FROM sqlite_master
WHERE type = 'index'
  AND name IN ('candidate_index_name')
ORDER BY name;
```

Run natural plans only. Do not force adoption. For each query, copy, and
`stat_state`, print `EXPLAIN QUERY PLAN` before timing and confirm whether the
planner adopted the candidate. An unadopted candidate will not help production
unless the deployment also changes the planner state that made it unattractive.

Measure warm behavior. Production media-server databases are usually days-warm,
so pre-warm the OS page cache before each stat-state block:

```sh
cat "$copy" "$copy-wal" "$copy-shm" 2>/dev/null >/dev/null
```

Then run one untimed warm-up query per arm before timed iterations.

Counterbalance order. A single first-run pair can mispredict because the first
arm warms shared cache state for the second. Interleave the arms and run both
orders, for example:

```text
iteration 1: baseline, candidate
iteration 2: candidate, baseline
iteration 3: baseline, candidate
iteration 4: candidate, baseline
```

Use at least three timed iterations per arm per stat state and report the
median, not the fastest run.

Capture output to a host file and parse `.timer` output from that file:

```sh
"$sqlite_bin" -batch "$db_copy" < "$sql_file" > "$out_file" 2>&1
grep "Run Time" "$out_file"
```

The SQL file should enable `.timer on`, print a step label, run the full query,
and disable `.timer off`. Never use `.once /dev/null`; it can swallow the
`Run Time` line. Keep result rows in the capture file so the timed statement is
the actual query, not a wrapper.

Run the complete sequence in this order:

1. Prepare baseline and candidate copies; prove candidate state.
2. Run setup SQL on the baseline copy and materialize literal values.
3. Print the per-value EQP matrix for representative values under
   `stat_state=current`.
4. `stat_state=current`: pre-warm both copies, print EQP for both copies, run one
   warm-up per executable copy, run timed iterations in counterbalanced order,
   compute medians.
5. Run `PRAGMA analysis_limit=0; ANALYZE main;` on both copies.
6. Print the per-value EQP matrix again under `stat_state=analyzed`.
7. `stat_state=analyzed`: pre-warm again, repeat EQP, warm-up, timed
   iterations, and medians.

Every EQP, timing row, and median line includes `stat_state`, query name, copy
name, iteration, and candidate adoption status.

## Structural Query-Rewrite Validation

Validate a structural rewrite before timing it. Correctness comes before speed.
The validation has two required proofs:

1. Plan-flip proof: `EXPLAIN QUERY PLAN` shows the rewrite naturally moves the
   faithful planner from the slow shape to the intended shape.
2. Row-identity proof: the original and rewritten rowsets produce the same
   grouped `(id, count(*))` digest.

Do not use a top-N diff as the row-identity proof. Ties in the vendor
`ORDER BY` can produce different top-N rows without proving a semantic
difference, and a matching top-N can hide wrong rows deeper in the rowset.

Build the identity proof from the stable result id for the query family. Use
`tags.id`, `A.Id`, or the equivalent application identity column, not row
position. Preserve joins, filters, grouping, and HAVING clauses. Remove only
presentation-only order/limit clauses when proving the full candidate rowset.

Example digest shape:

```sql
DROP TABLE IF EXISTS temp.original_identity;
DROP TABLE IF EXISTS temp.rewrite_identity;

CREATE TEMP TABLE original_identity AS
SELECT id, count(*) AS n
FROM (
  SELECT A.Id AS id
  FROM ...
  -- original query core, without presentation-only ORDER BY / LIMIT
)
GROUP BY id;

CREATE TEMP TABLE rewrite_identity AS
SELECT id, count(*) AS n
FROM (
  SELECT A.Id AS id
  FROM ...
  -- rewritten query core, without presentation-only ORDER BY / LIMIT
)
GROUP BY id;

SELECT 'original' AS arm,
       count(*) AS distinct_ids,
       coalesce(sum(n), 0) AS row_count,
       lower(hex(sha3_query('SELECT id, n FROM temp.original_identity ORDER BY id', 256))) AS digest
FROM temp.original_identity;

SELECT 'rewrite' AS arm,
       count(*) AS distinct_ids,
       coalesce(sum(n), 0) AS row_count,
       lower(hex(sha3_query('SELECT id, n FROM temp.rewrite_identity ORDER BY id', 256))) AS digest
FROM temp.rewrite_identity;

SELECT o.id, o.n AS original_n, r.n AS rewrite_n
FROM temp.original_identity AS o
LEFT JOIN temp.rewrite_identity AS r USING (id)
WHERE r.id IS NULL OR r.n <> o.n
UNION ALL
SELECT r.id, o.n AS original_n, r.n AS rewrite_n
FROM temp.rewrite_identity AS r
LEFT JOIN temp.original_identity AS o USING (id)
WHERE o.id IS NULL
LIMIT 20;
```

The final diff query must return no rows before the rewrite is eligible for
timing or implementation. If `sha3_query` is unavailable, emit both grouped
tables in sorted order and hash those host files with `sha256sum`.

For planner-nudge rewrites, preserve SQLite affinity. Wrapping the boolean
predicate is affinity-safe:

```sql
WHERE unlikely(tag_type = 13)
```

Unary plus on the column is not equivalent:

```sql
WHERE +tag_type = 13
```

`unlikely(tag_type = 13)` evaluates the comparison with the column's normal
affinity, then wraps the boolean result. `+tag_type` strips the column affinity
before comparison and can change text/numeric equality semantics. Use unary
plus only when the affinity change itself is the tested behavior and the
row-identity proof covers it.

## Interpreting Results

Separate adoption from benefit:

- Adoption: EQP shows the natural planner chose the candidate.
- Benefit: median warm runtime improves consistently across both orders and
  multiple iterations.

Adoption is not success. A planner can adopt a candidate index and become
catastrophically slower. Test each candidate standalone under the current
copied stat state, because that is exactly where a mis-adopted index fires.

Compare current and analyzed states:

- Helps in `current`: the candidate can help the current copied planner state.
- Helps only in `analyzed`: the candidate needs maintenance ANALYZE to pay off.
- Adopted and harmful in `current`, rejected in `analyzed`: sampled or stale
  stats both hide good indexes and trigger bad ones.

Compare predicate values:

- Helps only for one hot value: the candidate is workload-specific, not proven
  for the query family.
- Plan flips across values under STAT4: report the matrix and choose a
  deployment posture from the observed fleet distribution.
- Empty and rare values remain important: they catch rewrites and indexes that
  optimize the hot path by breaking edge cases.

For structural rewrites, row identity is the gate. A plan flip without a
matching grouped `(id, count(*))` digest is not a valid optimization.

Generic example: a multi-column secondary index can lead with a column that
looks selective under sampled or stale stats. The planner may pivot the join
driver to a high-fan-out table, adopt the index, and run orders of magnitude
slower. After `ANALYZE main`, accurate STAT1 and STAT4 can reveal the fan-out
and make the planner reject that path.

Distinguish warm planner work from cold I/O. A wall-clock slow-query log entry
may be cold while the faithful warm natural plan is sub-second. Arbitrary
id-list drives that are cold-I/O-bound are not fixed by an inner-probe index,
and the sort can be a minor fraction of the wall time.

For sort-heavy shapes, the planner-visible lever is often sort elimination:
lead an index with the actual sort column, then add equality or join columns
when that still preserves the desired order. This is one of the few benefits the
current copied planner can see before refreshed stats.

## Dead Ends

Do not re-try these paths without a new vendor SQL surface or deployment design:

- Vendor SQL is fixed unless a validated library-layer prepare wrapper changes
  it; do not depend on hints, column-list changes, or rewrites that the
  application will not issue.
- Sampled or stale `sqlite_stat1` is a production state to test, not an argument
  to ignore planner behavior.
- SQLite expression indexes cannot contain subqueries.
- Materialized-view or table-substitution rewrites do not apply when the vendor
  SQL never names the substitute table.
- Full covering indexes are usually uneconomical for wide media projections.
- `SORTERREF` is a compile/config trade-off: it stores sort keys plus rowid
  references and performs post-sort random row refetches for wide records. It
  has no runtime PRAGMA toggle.
- A top-N row diff is not a structural rewrite proof; use grouped
  `(id, count(*))` row identity.
- A known-spinning original query is not a timing baseline; do not execute it
  after EQP and identity evidence already prove the slow path.
- Unary-plus planner nudges are affinity-changing; prefer boolean-wrapper
  nudges such as `unlikely(col = value)` when the intent is plan influence
  without comparison-semantics drift.

## Cleanup

After recording the shape packet, EQP, per-value matrix, medians, row-identity
digests, and recommendation, delete the database copies and harness scratch
files from the SSH host. Leave the original database, deployed replacement
library, and Plex-renamed ICU files untouched.
