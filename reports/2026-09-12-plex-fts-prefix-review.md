# Review: Plex FTS4 prefix indexes

2026-09-12 America/Chicago; measurements continued into 2026-09-13 UTC. Repository reviewed: `93d0ad3534bdae440dc7de3e1ce5f7a7c1baa5c1`.

## 1. Executive summary

1. Recommend a controlled, stopped-server rollout of `prefix='2,3'` for tag titles, owned by the maintenance policy.
2. Recommend the same for Gaius metadata titles **after a stopped-copy integrity/content-parity gate**; leave Brutus’s existing option in place.
3. Full actor queries improved **15% for “to” and 38% for “the”** in the focused paired scratch comparison; selective multi-word statements improved much more.
4. Identical-data title statements fell from **122 ms to 16–17 ms** for the two requested multi-word inputs before segment merging.
5. These are statement measurements. An HTTP-search speedup or new p90 cannot be established from the available request-level evidence.
6. Prefer `2,3` over `2,3,4`: the fourth-byte index adds storage and writes for little gain on the requested phrases.
7. Prefix indexes remove repeated term-expansion work; taggings fan-out, metadata lookups, ranking, and repeated Plex search passes remain.
8. A narrowly validated rare-group-first fuzzy rewrite is a promising complement: **82→68 ms** plain, **20→4 ms** with prefix indexes.
9. Principal risks are application-owned DDL/content conventions, migration drift, write amplification, and verification—not a demonstrated Unicode result change.
10. This review changed only local report artifacts and the authorized scratch directory; production SQL probes were read-only and timeout-bounded.

## 2. Evidence, measurements, and commands

### Scope and method

Both containers report SQLite **3.53.3**, source ID `2026-06-26 20:14:12 d4c0e51e4aeb96955b99185ab9cde75c339e2c29c3f3f12428d364a10d782c62`; binary strings identify PMS **1.43.2.10687-563d026ea** on both. Live plans and environment were inspected before timing. Both containers have the FTS join-order gate enabled, despite the repository document’s older “not deployed” status. All measured live tag statements used FTS → covering tag-type lookup → covering taggings lookup → metadata primary key, followed by the three aggregation/sort temporary b-trees. Title statements used FTS → metadata primary key → sort.

The original scratch `tags_copy` has `id INT`, **not** `INTEGER PRIMARY KEY`. Its rowid is not the Plex tag ID: its first row has rowid 1 and ID 6091899. Its existing FTS rowids therefore must not be joined directly to production taggings. I built a new `/opt/brutus/tmp/ftsexp/review.db` with proper Plex IDs, using:

* 6,657,492 tag strings from the supplied scratch copy, with tag types collected from short live indexed reads; no missing tag types in the resulting copy.
* 2,324,142 metadata rows, retaining ID, library section, metadata type, title, title_sort, and original_title.
* The union of all five tested tag MATCH result sets: 380,468 candidate IDs.
* **Every** taggings row for all 36,494 candidate actor/genre tags: **500,740 rows**, including duplicates and references to every section. This is an exact candidate subset for these inputs/types, not a sampled fan-out.

The copy used **259 separate live connections**, median **0.096 s**, maximum **0.365 s** per extraction, with no encompassing transaction. It is a rolling copy, not a cross-table point-in-time snapshot: five additional live tags existed by the type extraction. This limits production parity claims but does not bias the identical-data index comparisons. No source DB was attached to a writable scratch connection. [Extraction commands and durations](fts-prefix-evidence/copy-command-audit.out).

Scratch FTS variants use identical content, real IDs, column order, and Plex tokenizer: plain, `2,3`, `2,3,4`, and `3`. Scratch filenames do not activate target-name SQL rewrites. For the tag benchmark I explicitly used the deployed FTS-first CROSS JOIN order; the title planner independently selected its corresponding live order. **No section-first CROSS JOIN with MATCH was run.**

Warm scratch tables below use the median of five executions after discarding the first, alternating variant order. Live tables use three warm executions after discarding the first. Timings come from the Plex CLI `.timer`, exclude SSH/process startup, and have 1 ms displayed wall-time resolution. “0” therefore means below useful displayed resolution. This shared production host was not quiesced; the broad join timings varied considerably. A focused 12-execution paired repeat was added because the merged “to” join results were noisy. No cache was flushed.

### Full tag statements: identical data

Main matrix before explicit FTS segment merging, TV section 5 / metadata type 2 on the Brutus copy. FTS-only is `count(*) WHERE tag MATCH ...`; full statements retain DISTINCT, GROUP BY, descending count, and LIMIT 30.

| MATCH input | FTS plain / 2,3 / 2,3,4, ms | Full actor plain / 2,3 / 2,3,4, ms | Full genre plain / 2,3 / 2,3,4, ms | Materialized-ID join only, actor / genre, ms |
|---|---:|---:|---:|---:|
| `to*` | 39 / 5 / 5 | 244 / 181 / 169 | 66 / 33 / 32 | 182 / 28 |
| `the*` | 77 / 12 / 13 | 155 / 97 / 94 | 133 / 74 / 74 | 85 / 64 |
| `The* Amazing* Burt*` | 68 / 7 / 7 | 68 / 7 / 8 | 67 / 7 / 8 | <1 / <1 |
| `The* Office*` | 67 / 8 / 8 | 68 / 8 / 9 | 68 / 7 / 8 | <1 / <1 |
| Supplied fuzzy three-group expression | 92 / 26 / 54 | 92 / 26 / 40 | 93 / 29 / 46 | <1 / <1 |

The materialized-ID control replaces FTS enumeration with an already populated primary-key table and forces the same ordinary-table loop order. It is **not an exact internal profiler**: FTS `count(*)` includes row enumeration, the materialized scan has its own cost, and cache/locality differ. Those columns should not be added as disjoint time accounts. For example, 39 ms FTS + 182 ms join is compatible with a 244 ms full query, but does not assign its remaining 23 ms to a single engine operation.

The focused paired repeat, after all variants were merged, produced:

| Full actor statement | Plain median [min–max], ms | 2,3 median [min–max], ms | 2,3,4 median [min–max], ms | 2,3 reduction |
|---|---:|---:|---:|---:|
| `to*` | 215 [197–227] | 182 [164–206] | 172 [166–224] | 15.3% |
| `the*` | 156 [150–165] | 97 [90–108] | 98 [94–106] | 37.8% |

The extra `4` index cannot accelerate the `to*` lookup when the identical `2` index is already present. Its apparent 10 ms advantage here is not evidence for choosing `4`. The larger matrix’s merged `to*` actor medians were 372/277/210 ms, with wide overlapping ranges; I do not attribute those differences to prefix length. [Raw matrix](fts-prefix-evidence/bench-unmerged.out), [merged matrix](fts-prefix-evidence/bench-merged.out), [focused repeat](fts-prefix-evidence/full-paired-repeat.out), [summaries](fts-prefix-evidence/summary-other.json).

| Input | All tag FTS matches | Actor candidates | Actor taggings rows before section filtering | Rows / distinct tags after TV-section filtering |
|---|---:|---:|---:|---:|
| `to*` | 114,825 | 28,796 | 469,944 | 15,948 / 6,239 |
| `the*` | 287,163 | 4,233 | 21,060 | 1,621 / 763 |
| `The* Amazing* Burt*` | 0 | 0 | 0 | 0 / 0 |
| `The* Office*` | 1,355 | 0 | 0 | 0 / 0 |
| Supplied fuzzy expression | 0 | 0 | 0 | 0 / 0 |

Thus the empty multi-word actor results are particularly favorable to this change. They do not measure the cost of a different fuzzy expansion that returns many actor candidates. [Plans, counts, and complete FTS rowid-set parity checks](fts-prefix-evidence/scratch-plans-counts-parity.out).

### Title statements and live-server comparison

Identical-data scratch results before explicit merging; columns are `title`, with the real title_sort ICU ORDER BY and LIMIT 31:

| Input | FTS-only plain / 2,3 / 2,3,4, ms | Full title plain / 2,3 / 2,3,4, ms |
|---|---:|---:|
| `to*` | 42 / 6 / 7 | 75 / 37 / 37 |
| `the*` | 132 / 28 / 28 | 223 / 133 / 128 |
| `The* Amazing* Burt*` | 122 / 16 / 17 | 122 / 16 / 17 |
| `The* Office*` | 122 / 17 / 17 | 122 / 17 / 18 |
| Supplied fuzzy expression | 162 / 63 / 67 | 165 / 59 / 54 |

Live warm results, TV section 3 on Gaius and section 5 on Brutus. The production title tables have **different libraries, segment histories, and prefix DDL**, so the inter-server ratio is corroboration, not a controlled estimate:

| Input | Gaius tag FTS / actor / genre, ms | Brutus tag FTS / actor / genre, ms | Gaius title FTS / full title / full title_sort, ms | Brutus title FTS / full title / full title_sort, ms |
|---|---:|---:|---:|---:|
| `to*` | 40 / 302 / 70 | 39 / 299 / 68 | 41 / 109 / 100 | 7 / 49 / 49 |
| `the*` | 71 / 191 / 139 | 74 / 162 / 141 | 139 / 323 / 249 | 27 / 157 / 112 |
| `The* Amazing* Burt*` | 66 / 67 / 66 | 73 / 68 / 67 | 131 / 123 / 92 | 17 / 17 / 15 |
| `The* Office*` | 65 / 68 / 67 | 69 / 70 / 70 | 127 / 127 / 95 | 17 / 18 / 16 |
| Supplied fuzzy expression | 86 / 88 / 86 | 99 / 101 / 102 | 182 / 168 / 126 | 61 / 49 / 41 |

The few FTS-only figures above a corresponding full query illustrate measurement/plan-context variation, not negative join cost. [Live summary including ranges](fts-prefix-evidence/live-summary.json); the ten `live-times-*.sh`/`.out` files contain every executed statement and timer result.


A subsequent merged-index **title_sort MATCH** check on identical scratch data gave the following full-statement medians:

| Input | Plain / 2,3 / 2,3,4, ms |
|---|---:|
| to | 90 / 46 / 43 |
| the | 183 / 131 / 104 |
| the amazing burt | 92 / 15 / 14 |
| the office | 94 / 14 / 15 |
| fuzzy | 115 / 35 / 36 |

[Exact title_sort commands](fts-prefix-evidence/title-sort-bench.sh), [results](fts-prefix-evidence/title-sort-bench.out).

### Build, storage, writes, and merging

Rebuild times on the complete copied content:

| FTS table | Plain | 2,3 | 2,3,4 | 3 only |
|---|---:|---:|---:|---:|
| Tags, seconds | 37.944 | 45.020 | 56.758 | 46.476 |
| Three-column metadata titles, seconds | 28.187 | 32.570 | 35.279 | 28.693 |

These are single build observations, not a ranking between close results such as `3` versus `2,3`. Explicit merge took 1.815/2.934/4.124/2.628 s for tags and 0.914/1.992/2.622/1.413 s for titles in the same variant order. Each finished with one segment per index tree. [Build](fts-prefix-evidence/build-review.out), [merge](fts-prefix-evidence/optimize-review.out).

| Allocated FTS storage, including shadow b-trees | Plain, MiB | 2,3, MiB | 2,3,4, MiB | 3, MiB | 2,3 increase over plain |
|---|---:|---:|---:|---:|---:|
| Tags | 166.6 | 276.4 | 332.5 | 224.5 | 109.8 MiB (66%) |
| Metadata titles | 108.4 | 227.9 | 281.5 | 168.1 | 119.4 MiB (110%) |

Adding `4` beyond `2,3` costs another **56.1 MiB for tags** and **53.6 MiB for metadata titles** on this copy. Both `2,3` changes together add about **229.2 MiB** over the rebuilt plain tables, roughly 1.3% of an 18 GB library. These are rebuilt/merged allocation comparisons; current live fragmentation and free-page reuse can change the actual file-growth delta.

Neither available SQLite build exposed `dbstat`. I counted b-tree and overflow pages from the stable scratch file, using the documented file format, and reconciled all allocated pages plus freelist pages plus SQLite’s reserved locking page to the database page count. [Exact allocation output](fts-prefix-evidence/page-sizes.out), [page-counting code](fts-prefix-evidence/page_sizes_remote.py), [SQLite file format](https://www.sqlite.org/fileformat2.html). The independent compressed-posting totals are in [physical metrics](fts-prefix-evidence/physical-metrics.out).

Write experiment: the same 50,000 evenly spaced real tag strings, FTS inserts in 500 transactions of 100 rows, WAL, synchronous NORMAL, auto-checkpoint disabled **only in scratch**. The external content was preloaded equally. This isolates FTS insertion and commit cost; it is not whole-Plex scan throughput or a steady-state 6.6-million-row update benchmark.

| Option | Sum of engine wall timers, s | CPU, s | WAL generated, bytes | WAL / plain | FTS CPU / plain |
|---|---:|---:|---:|---:|---:|
| Plain | 0.465 | 0.466 | 43,021,808 | 1.00× | 1.00× |
| 2,3 | 0.581 | 0.578 | 52,604,080 | 1.22× | 1.24× |
| 2,3,4 | 0.754 | 0.757 | 58,314,064 | 1.36× | 1.62× |
| 3 | 0.495 | 0.500 | 48,173,920 | 1.12× | 1.07× |

Wall sums have accumulated millisecond rounding; CPU timers provide finer resolution. Adding `2,3` cost about **2.25 microseconds of CPU per indexed row** in this batch workload. More terms/positions, different transaction sizes, and larger merge histories can change amplification. The observed total segment counts were 20/60/80/40: one corresponding hierarchy for each complete-term/prefix index. [Exact write-test generator](fts-prefix-evidence/write_bench.py), [results](fts-prefix-evidence/summary-other.json).

### Exact command surface

Every live SQL invocation used this shell wrapper, substituting only `plex`/`brutus` and SQL. No live integrity command, ANALYZE, optimize, rebuild, or write PRAGMA was run:

```bash
timeout 60 docker exec -i -u abc -e SQLITE3_DISABLE_OBSERVABILITY=1 plex sh -c 'cd "/config/Library/Application Support/Plex Media Server/Plug-in Support/Databases" && "/usr/lib/plexmediaserver/Plex SQLite" "file:com.plexapp.plugins.library.db?mode=ro"' <<'SQL'
.bail on
.timeout 2000
EXPLAIN QUERY PLAN
select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'to*' and tag_type=6  and metadata_items.library_section_id in (3) and metadata_items.metadata_type=2   group by tags.id order by count(*) desc limit 30;
SQL
```

For the timed form, replace the EXPLAIN prefix with `.timer on`, as recorded verbatim in [Gaius to commands](fts-prefix-evidence/live-times-plex-to.sh). Representative extraction:

```sql
select id,tag_type from tags where id>0 order by id limit 100000;
select id,library_section_id,metadata_type,title,title_sort,original_title
from metadata_items where id>0 order by id limit 20000;
-- Each next connection resumes after the last ID actually returned.
-- Taggings extraction uses batches of 500 known IDs and its covering index.
```

Scratch execution used:

```bash
docker exec -i -u abc -e SQLITE3_DISABLE_OBSERVABILITY=1 brutus sh -c 'cd /tmp/ftsexp && "/usr/lib/plexmediaserver/Plex SQLite" review.db' <<'SQL'
.bail on
CREATE VIRTUAL TABLE tag_p23 USING fts4(
 content='tags',tag,
 tokenize=collating 'root@colStrength=primary;colAlternate=shifted',prefix='2,3');
INSERT INTO tag_p23(tag_p23) VALUES('rebuild');
.timer on
select count(*) from tag_p23 where tag match 'to*';
select distinct(tags.id) from tag_p23
CROSS JOIN tags ON tag_p23.rowid=tags.id
CROSS JOIN taggings ON tags.id=taggings.tag_id
CROSS JOIN metadata_items ON taggings.metadata_item_id=metadata_items.id
where tag_p23.tag match 'to*' and tag_type=6
and metadata_items.library_section_id in (5) and metadata_items.metadata_type=2
group by tags.id order by count(*) desc limit 30;
SQL
```

The generated shell files are the authoritative expanded commands; they include every repetition and exact fuzzy SQL escaping. Use the generators on a **fresh** scratch database, not by re-executing CREATE statements against this populated copy:

| Purpose | Exact commands / generator |
|---|---|
| Schema, migration rows, triggers, compile options | [Gaius](fts-prefix-evidence/inventory-plex.sh), [Brutus](fts-prefix-evidence/inventory-brutus.sh) |
| Complete copy process and per-read timings | [Copy code](fts-prefix-evidence/collect_remote.py), [taggings code](fts-prefix-evidence/collect_taggings_remote.py), [259 expanded commands](fts-prefix-evidence/copy-command-audit.out) |
| All eight DDL/rebuilds | [build-review.sh](fts-prefix-evidence/build-review.sh) |
| Full statements and join-only controls | [before merge](fts-prefix-evidence/bench-unmerged.sh), [after merge](fts-prefix-evidence/bench-merged.sh), [paired repeat](fts-prefix-evidence/full-paired-repeat.sh) |
| Vocabulary/doclist measurements | [vocabulary SQL](fts-prefix-evidence/token-metrics.sh), [physical decoder](fts-prefix-evidence/physical_metrics_remote.py) |
| Fuzzy decomposition | [fuzzy-breakdown.sh](fts-prefix-evidence/fuzzy-breakdown.sh) |
| Connection-setting experiments | [connection_bench.py](fts-prefix-evidence/connection_bench.py) |
| Unicode/null canaries | [parity generator](fts-prefix-evidence/parity.py), [isolation commands](fts-prefix-evidence/integrity-isolation.sh), [parity results](fts-prefix-evidence/unicode-parity-results.out) |
| Storage allocation and final integrity | [page walker](fts-prefix-evidence/page_sizes_remote.py), [integrity commands](fts-prefix-evidence/validate-review.sh) |

## 3. A — Impact and end-to-end expectations

For a broad actor query such as `to*`, the live FTS-only count is about **13%** of the 299–302 ms statement. Eliminating all of that measured component still leaves roughly 260 ms of candidate filtering, fan-out, metadata access, and ranking. A simple unchanged-join model using the measured 5 ms prefix lookup gives **about 12% lower live statement latency**, consistent with the focused scratch reduction of 15%. Do not market its 8× FTS-only acceleration as an 8× actor-search improvement.

For `the*`, live tag FTS accounts for roughly **37–46%** of the actor statement; full scratch actor latency improves about **38%**. Genre searches can remain expensive even with zero section results because the engine must inspect global matches and check their eligibility. Prefix indexing does not avoid that work. For the two requested multi-word inputs, candidate fan-out is absent in the tested actor scope, so most of the 67–68 ms statement can disappear: `2,3` gives roughly **88–90%** savings there. The supplied fuzzy expression similarly has zero scratch tag matches, and its large reduction primarily measures FTS evaluation.

Title searches are a separate opportunity on Gaius. The controlled scratch title reductions are approximately **51%** for `to*`, **40%** for `the*`, **86–87%** for the two multi-word inputs, and **64%** for the supplied fuzzy expression before merging. Brutus already benefits from its prefix index; adding one again offers no new prefix benefit. This also makes Brutus the simpler first **tag** canary, while Gaius has the greater total search opportunity from both tables.

An HTTP request contains many statements and additional work. Let `f` be the fraction of its current time spent in FTS work actually accelerated by these options, and let that component retain `r` of its original cost. Then:

`T_new / T_old = (1 - f) + f*r`.

For illustration, `r=0.2` gives:

| Measured affected-time fraction, if later established | Request latency reduction | Speedup |
|---|---:|---:|
| 25% | 20% | 1.25× |
| 50% | 40% | 1.67× |
| 75% | 60% | 2.5× |

These are **conditional scenarios, not measured HTTP forecasts**. The supplied 79-/129-statement captures do not assign every outer-step duration to tag/title/prefix/fuzzy components or account for the unexplained PMS/CLI difference. It would be invalid to sum a fresh CLI delta across all 129 statements, apply a blanket 3× multiplier, or predict a new LG p90 from 318 historical searches. No new HTTP search traffic was generated for this review.

The 1–2-byte edge rule does not make the 2-byte FTS tree redundant: a longer request may contain short tokens, such as “to …”, and input bytes are not tokenizer-key bytes. `prefix='3'` measured **20 ms versus 5 ms** for `to*`, though both versions take about 11 ms for `the*`. Preserve the existing edge controls; they address request arrival and supersession, while prefix indexes reduce service time.

## 4. B — Why fuzzy evaluation is slow; tokenizer semantics

The expensive part is not edit-distance calculation inside these SQL statements. Plex has already supplied the spelling alternatives. FTS evaluates the resulting Boolean expression.

The pinned `fts3.c` does three relevant things. `fts3TermSegReaderCursor()` selects an exact-length prefix tree, or an N+1-byte prefix tree plus the exact full term, otherwise scans the full-term prefix range. `fts3TermSelectMerge()` combines the matching term doclists pairwise, including positions. `fts3EvalNextRow()` advances OR child iterators and intersects AND children. Crucially, `fts3EvalSelectDeferred()` immediately disables token deferral for **external-content** tables; it cannot safely assume raw content equals indexed text. These tables therefore cannot rely on retokenizing only a few rare candidates to avoid loading the broad terms. [Pinned source](https://github.com/sqlite/sqlite/blob/version-3.53.3/ext/fts3/fts3.c), [local inspected source](fts-prefix-evidence/fts3.c).

Measured tag vocabulary and compressed doclists after merging:

| Prefix | Full dictionary terms expanded | Sum of document frequencies / occurrences | Plain doclist bytes | Exact prefix-tree doclist bytes |
|---|---:|---:|---:|---:|
| `to*` | 4,477 | 117,582 / 120,277 | 436,898 | 364,357 (`2`) |
| `the*` | 823 | 290,742 / 317,565 | 924,602 | 902,341 (`3`) |
| `amazing*` | 2 | 168 / 168 | 759 | Not covered by 2,3,4 |
| `burt*` | 58 | 1,370 / 1,370 | 5,693 | 5,254 (`4`) |
| `office*` | 7 | 3,836 / 3,857 | 12,818 | Not covered by 2,3,4 |

Document-frequency sums count a document more than once when it contains different matching terms. The full `the*` result is 287,163 distinct documents. Its prefix doclist is still about 0.90 MB: the gain comes largely from avoiding hundreds of dictionary lookups/merges, not eliminating the result postings. The `3` tree has only 26 `to...` keys, explaining why N+1 fallback helps `to*`, but still loses to the exact 2-byte tree. [Vocabulary](fts-prefix-evidence/token-metrics.out), [decoded compressed lengths](fts-prefix-evidence/physical-metrics.out).

The three fuzzy groups individually match **288,187**, **287**, and **1,349** tag documents, yet their conjunction matches zero. A fresh merged-index run gave:

| Expression | Plain / 2,3 / 2,3,4, ms |
|---|---:|
| First OR group alone | 103 / 42 / 40 |
| Second OR group alone | ≤1 / ≤1 / ≤1 |
| Third OR group alone | 1 / 1 / 1 |
| Original three-group order | 82 / 20 / 20 |
| Remove absorbed exact alternatives within groups | 71 / 10 / 10 |
| Move whole groups to second, third, first order | 68 / 4 / 3 |

The full-statement follow-up also retained the gain: actor queries were 80.5→68.5 ms plain and 19→3 ms with `2,3`; title queries were 147.5→128 and 40.5→5 ms respectively. These scope-specific results are selective/empty cases, not a broad fuzzy-hit workload. [Full-query follow-up](fts-prefix-evidence/fuzzy-full-rare-first.out).

The group-alone times are not additive: the full conjunction can finish before fully enumerating each group. Reordering the AND groups makes that early exhaustion cheaper, while initialization still bears prefix expansion cost without a prefix tree. The redundant-alternative experiment preserves all nonabsorbed spellings; it does not replace fuzzy OR with AND. For example, `the OR they OR theo ... OR the*` contains alternatives already covered by `the*` for the observed token bytes. A general implementation should not infer this solely from Unicode character-prefix spelling.

On asymptotics: an exact prefix index removes the query-time dependence on enumerating `K` distinct full dictionary terms for that prefix and their repeated union work. That is a structural improvement in the **prefix-expansion subproblem**, purchased at write time. It does not remove the dependence on the size of the surviving postings or on `V` explicit fuzzy alternatives. OR iteration, AND intersection, positions, and downstream joins still scale with their input/output sizes. It is not an exponential enumeration of every Cartesian combination of spelling variants, nor does it make a broad query O(1).

FTS4 lengths are **token byte lengths**. With this custom tokenizer, the bytes are ICU primary collation keys, not necessarily UTF-8 spelling bytes. Observed keys include `to → 5046` (2), `the → 503832` (3), `café/CAFE → 2E2A3432` (4), `æ/ae → 2A32` (2), `é/é → 32` (1), and `東京 → FBEC7AFB0934` (6). Consequently, `2,3,4` is not a promise to optimize all two-, three-, and four-character words. ICU expansions, script-dependent key lengths, word boundaries, and contractions require testing with the actual tokenizer. [Tokenizer output](fts-prefix-evidence/tokenizer-canary.out), [FTS4 prefix selection semantics](https://www.sqlite.org/fts3.html#the_prefix_option).

Prefix insertion slices the same token bytes used by normal matching; it does not introduce a new normalization rule. Complete rowid-set comparisons found no difference for all five requested expressions across all four full-size variants. The Unicode canary compared 22 queries in both directions for `2,3` and `2,3,4`, including accents, composed/decomposed text, ligatures, German, Japanese, Korean, punctuation, and phrases: **44 comparisons, all zero differences**. This is useful compatibility evidence, not exhaustive proof for all ICU text or future tokenizer versions.

## 5. C — DDL, Plex lifecycle, and maintenance risks

### Migrations and provenance

Both deployed binaries contain the named classes `DatabaseMigrationsSwitchSearchToICU`, `DatabaseMigrationsReindexForCompoundSearch`, `DatabaseMigrationsRebuildFtsTablesForTagSearch`, and `DatabaseMigrationsRebuildFtsTablesForImprovedMusicSearch`. They also contain DROP/DELETE operations for these FTS tables, their trigger definitions, and canonical CREATE statements **without `prefix=`**. This proves relevant recreate/reindex machinery exists; strings alone do not prove which branch will execute on a future upgrade. [Binary evidence](fts-prefix-evidence/pms-strings.out).

Gaius has **441** migration records, Brutus **443**. Every shared record is identical, including rollback SQL. Brutus alone has `20210207150000` (rollback `select 1`) and `202204181200` (NULL rollback). Neither explains a prefix DDL change. The retained `Plex Media Server*.log` files contained no matches for the four migration names. No `prefix='2,3'` DDL string was found in the current binary search. Therefore **Brutus’s prefix provenance remains unverified**. The difference is not evidence that this PMS version guarantees that schema, and the two migration IDs cannot be assigned to named classes from these observations. Do not edit migration records to “register” the mod’s change.

A recreate operation using the current canonical string would remove the option; a plain FTS `rebuild` or `optimize` retains it. Future column/tokenizer changes can invalidate an exact hardcoded replacement even if the prefix setting itself remains valid.

### Optimize/Butler and indexed-content conventions

Plex documents a scheduled database-optimization task. Both inspected preferences have `ButlerTaskOptimizeDatabase='0'`. Current binary strings explicitly contain FTS `INSERT ... VALUES('optimize')` for both ICU tables. Segment optimization does not change prefix DDL. I did not trigger the manual endpoint or Butler, and did not establish the complete current internal task call graph. [Plex scheduled-task documentation](https://support.plex.tv/articles/201553286-scheduled-tasks/), [local settings evidence](fts-prefix-evidence/prefs-and-log-settings.out).

The binary also contains special metadata indexing SQL that concatenates episode/show titles, album/artist titles, and track/album/artist/original titles. It contains a tag population path limited by `tag_type IN (...)`. Those are material reasons not to assume `content='metadata_items'` always means the indexed text is exactly its three raw columns, or that the tag FTS always indexes every tag type. Raw external-content `rebuild` can change those conventions. Limited live `office*` episode samples contained that word in their own titles and did not demonstrate compound-only matches; they cannot prove the whole index is raw-content-equivalent.

The repository already deliberately uses the **rebuild-all** model. Its disabled `run_fts_recurate()` would restore a curated subset, but it is not invoked. A prefix rollout should not quietly introduce yet another content convention. For an in-place DDL replacement, require a stopped-copy baseline FTS integrity comparison and complete relevant result parity first. If that baseline reports a genuine content mismatch, decide how to preserve Plex’s indexed text before proceeding, especially for metadata titles.

### Writes, external content, and integrity

Each inserted token contributes its full token and each eligible configured prefix to separate pending indexes. That explains the extra storage, merge hierarchies, and measured WAL cost. Live triggers on `tags` and `metadata_items` are **unconditional BEFORE UPDATE/DELETE and AFTER INSERT/UPDATE**. Even an update to another column can delete and reinsert FTS entries. Prefix write cost therefore applies to more than newly discovered tag names. The small insert benchmark cannot bound worst-case scan/update/merge pauses. [Pinned write implementation](https://github.com/sqlite/sqlite/blob/version-3.53.3/ext/fts3/fts3_write.c), [captured trigger DDL](fts-prefix-evidence/inventory-brutus.out).

CREATE of an external-content FTS table does not populate its inverted index. A plain `SELECT count(*) FROM fts_table` can read the external content and look correct despite an empty inverted index. Preserve rowids and all content-table triggers; populate via the FTS interface and verify MATCH results. The BEFORE trigger ordering matters because deletion needs the old text. An ordinary `REINDEX` is not an FTS rebuild. [External-content rules](https://www.sqlite.org/fts3.html#external_content_fts4_tables).

The NULL canary exposed a pre-existing checker limitation: the current tokenizer produces `SQL logic error` when the integrity path opens NULL input. The write path explicitly skips NULL; the integrity path invokes the tokenizer without that skip. Plain, `2,3`, and `2,3,4` all fail identically on the NULL fixture; removing that row and rebuilding makes all checks pass. Whole-DB checking reports `unable to validate ...: SQL logic error`, not a proved malformed index. Indexed, LIMIT-1 probes found **no NULL tag, title, title_sort, or original_title values** on either live server in this session. Keep this preflight: a future NULL could block maintenance independently of the prefix choice. Do not replace missing values in live content or relax a hard gate merely to install the index. [Isolation](fts-prefix-evidence/integrity-isolation.out), [nonnull checks](fts-prefix-evidence/unicode-parity-nonnull-integrity.out), [Gaius NULL probes](fts-prefix-evidence/null-probes-plex.sh).

### Where the option belongs

`scripts/optimize_media_servers.sh:790` discovers FTS tables, then at line 822 runs only `INSERT INTO <table>(<table>) VALUES('rebuild')`. It does **not** DROP/CREATE these tables. `VACUUM INTO` also preserves the existing CREATE statement. Therefore the existing routine would **preserve**, not undo, a one-off prefix option.

At lines 1450–1455, the staged rebuild is immediately followed by a hard FTS integrity gate. The per-table source/staged count comparison runs earlier; that is intentional because rebuilding changes shadow-table rows. The source whole-DB “malformed inverted index for FTS4 table main.fts4_tag_titles_icu” exception is followed by a broader FTS3/4/5 exception. It is a deferral to rebuild, not a prefix-specific repair and not proof the source is clean. Source checker execution/framing failures also fail open; staged FTS and final whole-DB checks are the hard publication gates. [Maintenance architecture](../docs/architecture/maintenance.md), [script](../scripts/optimize_media_servers.sh).

The natural implementation point is a **new explicit staged-schema policy step after the source/staged row-count comparison and before `run_fts_rebuild_hard()`**. Validate the known columns, content target, locale, and triggers; change only the desired option; let the existing rebuild and hard checks run afterward. Do not put a CREATE-only FTS operation into `_PLEX_INDEXES`/late optimize SQL, which runs after that rebuild/gate. Keep it opt-in and mod-owned, with an explicit desired schema and a read-only drift audit after PMS upgrades. No such policy step was implemented in this review.

## 6. D — Alternatives and the PMS/CLI gap

**Segment optimize complements prefix indexing but did not replace it.** In the controlled runs, plain `the*` FTS was 77→74 ms, and plain title `the*` stayed 132 ms. The prefixed fuzzy tag case changed from 26/54 ms (`2,3`/`2,3,4`) to 25/24 ms after merging; its separate focused run gave 20/20 ms. This shows why segment history must be controlled before attributing a fuzzy difference to `4`. Fresh bulk-built scratch segments do not model all production churn. Existing live segment counts also exceeded the initial supplied numbers as scans continued; the claimed Gaius “rows=6?” was not a total (the supplied level counts already sum to 22). No live FTS optimization was attempted.

**Automerge is a write-latency policy, not a free search optimization.** The live `_stat` tables contained only ID 0, consistent with no explicitly persisted automerge setting. Stock FTS4 defaults incremental automerge to 0; ordinary full-level merging still occurs. An offline trial of `automerge=8` or `merge=100,8` could compare scan latency and segment growth, but this review did not run a long-term workload to choose a value. Avoid changing it simultaneously with the prefix canary. SQLite documents both the added write work and persistence of the setting. [FTS merge controls](https://www.sqlite.org/fts3.html#the_automerge_n_command).

**Prefix size:** `3` alone saves writes/storage but loses substantially on 2-byte keys, including short tokens within long queries. `4` helps some separate inputs: `john*` improved 9→2 ms; `star*` about 2→1 ms; `love*` about 3→<1 ms. In contrast, `amazing*`, `office*`, and `burt*` were already at about 0–1 ms individually. Add `4` only if a future traffic-weighted test demonstrates enough frequent costly 4-byte keys. The requested phrase set does not justify it.

**ANALYZE on shadow tables:** both live STAT1 inventories already include the FTS segment, segdir, stat, and docsize tables. The FTS dictionary expansion and Boolean evaluator are not a relational selectivity estimate that STAT4 can eliminate. Keep existing full ANALYZE for ordinary joins; no evidence supports an extra shadow-table ANALYZE as the solution. Statistics are also not a substitute for the already selected covering taggings index and FTS-first order.

**Fuzzy rewrite candidate:** reorder the three intact parenthesized AND groups to evaluate the rare two before the broad `the...` group. Their intersection is commutative for these valid membership-only queries. Preserve the original group bytes, spelling alternatives, bindings, outer filters, count multiplicities, and final LIMIT. Admit only the recognized ID-producing statement and a correctly parsed supported MATCH grammar; return the original text on uncertainty. Never rewrite OR alternatives into AND, remove stopwords, add a premature LIMIT, or split/recombine the query in a way that changes phrase/NEAR/column semantics. A production implementation still needs nonempty-result tests, Unicode/parser/bind/error tests, and an isolated PMS canary; it was not implemented. A nonempty synthetic match also produced the same one-row set under original and reordered forms for plain, `2,3`, and `2,3,4` indexes. [Nonempty parity](fts-prefix-evidence/fuzzy-nonempty-parity.out). The existing matcher explicitly treats MATCH as opaque, so this is a new feature, not a switch already available in `plex_fts_rewrite.c`.

**Other structural ideas:** a tag-type-specific index could avoid scanning matches from unrelated tag types, and a maintained section/tag count table could avoid repeated fan-out. Both require new synchronized state and materially more correctness work around duplicate taggings, metadata changes, and filters. The present measurements support the mechanism but provide no validated throughput benefit or operational implementation. They are later projects. The rejected section-first MATCH strategy and IN-materialization proposal should remain rejected for this workload.

**The 3× in-process slowdown remains unexplained.** Useful exclusions from this session:

* Live CLI defaults were cache_size `-1048576`, mmap_size `34359738368`, threads 8, and 16 KiB pages. `temp_store=0` does not imply disk temporary tables: the build reports `TEMP_STORE=3`.
* The loaded build reports `OMIT_SHARED_CACHE` and `THREADSAFE=2`. The repository’s shared-cache API stub cannot enable shared cache; it is not a shared-cache lock explanation for this build.
* A read-only `/proc/.../maps` audit matched each running PMS SQLite mapping to its currently installed inode: 267932 on Gaius, 267559 on Brutus. This excludes an obviously stale/unlinked library mapping in this session. The two installed mod libraries have different sizes and should not be called byte-identical builds. [Mapping audit](fts-prefix-evidence/mapped-library-host-audit.out), [installed inodes](fts-prefix-evidence/mapped-library-audit.out).
* Both preferences specify `DatabaseCacheSize=1024`; PMS binary strings contain `PRAGMA cache_size=%d` and a separate `PRAGMA cache_size = 4096`. They do not establish the values of a particular active PMS connection after all application initialization.
* Scratch sensitivity tests gave the following medians. None reproduced a 3× change:

| Scratch setting | FTS `to*` | Genre `to*` | Actor `to*` | Fuzzy FTS |
|---|---:|---:|---:|---:|
| 1 GiB cache / mmap on / observability off | 38 | 64 | 215 | 80 |
| 4096-page cache / mmap on | 37 | 64 | 202 | 80 |
| 1 GiB cache / mmap off | 38 | 64 | 208 | 80 |
| Observability and slow tracker on / STMT trace off | 38 | 64 | 197 | 79 |

The last row is a CLI configuration experiment, not proof about PMS’s callback ownership or contention. Single-process tests cannot exclude mutex contention in the tracker, application-installed page caches/allocators/callbacks, busy waits, CPU/memory contention, or connection-specific overrides. Both the custom PCACHE2/MALLOC configuration calls and normal MULTITHREAD configuration are visible in the scratch launcher’s diagnostics, but I did not map them to the running server’s state. No tracer was attached. Measure application request latency in a later controlled canary instead of multiplying CLI savings by three.

## 7. E — Recommended procedure, SQL, downtime, and rollback

### Decision and gates

Proceed as a planned maintenance feature, starting with Brutus tags, then Gaius tags and—if its indexed-content gate passes—Gaius metadata titles. Keep `2,3` as the initial policy and leave automerge unchanged. The query-result evidence supports the prefix option; preservation of application-owned schema/content remains an operational gate.

Before the downtime run, prepare the staged-schema integration described above, an immutable backup/restore plan, result-comparison scripts, and a read-only schema-drift check. Do not execute a standalone DROP/CREATE in a serving database. The following SQL is a **future stopped/staged procedure, not executed on either live DB**.

1. Stop PMS and every writer to the library DB, or use the existing maintenance lifecycle that verifies the container is stopped. Stage the coherent patched Plex CLI/library outside the stopped container, as the repository requires. A `docker exec` into the stopped container is not available.
2. Checkpoint with the stopped-owner procedure, take a coherent dated backup including the required database state, and work on a staged copy. Never make a raw main-file-only copy while an active WAL contains committed state, use `immutable=1` against a live WAL DB, or delete a live WAL/SHM file.
3. Record table/trigger DDL, source ID, migration rows, user_version/application_id, content counts, and baseline query results on the stopped copy. Run the FTS integrity command for the existing target before modifying its DDL. If it fails, classify content drift/checker failure first; do not call that a successful preservation test.
4. Apply the option after the maintenance row-count comparison, then rebuild and run all existing hard staged checks. For an isolated staged experiment, the exact target-table SQL is below.

```sql
.bail on
.timeout 10000
BEGIN IMMEDIATE;
DROP TABLE fts4_tag_titles_icu;
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(
  content='tags', tag,
  tokenize=collating 'root@colStrength=primary;colAlternate=shifted',
  prefix='2,3'
);
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('rebuild');
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('optimize');
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('integrity-check');
COMMIT;
```

A disposable canonical-name canary successfully exercised this DROP/CREATE sequence, all four original-style triggers, insert/update/delete, and transaction rollback; final FTS and whole-DB checks passed. [Canary SQL and results](fts-prefix-evidence/ddl-trigger-rollback-canary.sh), [output](fts-prefix-evidence/ddl-trigger-rollback-canary.out). The four triggers are attached to `tags`; retain them and verify their definitions against the baseline. Do not INSERT into or UPDATE `tags` while the FTS table is temporarily absent. On SQL failure with `.bail on`, connection closure rolls back the uncommitted transaction. Do not continue to publication after a failed invocation.

For Gaius metadata titles only, after confirming the three-column raw-content rebuild preserves the required indexed content:

```sql
.bail on
BEGIN IMMEDIATE;
DROP TABLE fts4_metadata_titles_icu;
CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(
  content='metadata_items', title, title_sort, original_title,
  tokenize=collating 'root@colStrength=primary;colAlternate=shifted',
  prefix='2,3'
);
INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('rebuild');
INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('optimize');
INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('integrity-check');
COMMIT;
```

The integration should avoid a duplicate rebuild: its schema step does DROP/CREATE, and the existing generic rebuild/gate supplies the subsequent operations. The standalone SQL above is complete for a separately staged target experiment.

### Verification before publication

```sql
SELECT name,sql FROM sqlite_master
WHERE name IN ('fts4_tag_titles_icu','fts4_metadata_titles_icu');
SELECT name,sql FROM sqlite_master WHERE type='trigger'
AND tbl_name IN ('tags','metadata_items');
SELECT level/1024 AS index_number,count(*)
FROM fts4_tag_titles_icu_segdir GROUP BY level/1024;
-- Expect populated main, 2-byte, and 3-byte trees: index numbers 0,1,2.

SELECT count(*) FROM fts4_tag_titles_icu WHERE tag MATCH 'to*';
SELECT count(*) FROM fts4_tag_titles_icu WHERE tag MATCH 'the*';
SELECT count(*) FROM fts4_tag_titles_icu WHERE tag MATCH 'The* Amazing* Burt*';
SELECT count(*) FROM fts4_tag_titles_icu WHERE tag MATCH 'The* Office*';
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('integrity-check');
-- Also run the metadata target's integrity command if changed.
PRAGMA integrity_check;
```

Require a sole `ok` from the final whole-DB checker, with successful process exit. `.bail on` alone does not reject a diagnostic result row, so retain the script’s explicit output gate. Compare baseline versus candidate MATCH **rowid sets in both directions**, and full tag **(ID,count) multisets before LIMIT**, including nonempty broad cases and all requested inputs. Compare titles and title_sort, accents/Unicode, fuzzy queries, and section/type filters. Compare final limited IDs allowing only the original query’s unspecified tied-count/title ordering; never add a tie-breaker as part of this change. Preserve content counts, migration records, user_version, and application_id.

Verify updates/inserts/deletes in a disposable staged canary through the original triggers, with the same before/after text and ID behavior. Publish atomically only after the existing final gate passes, then restart through the controlled lifecycle. Run a small sequential application-search canary and compare complete results, outer request time, scan throughput, WAL growth, and errors; do not repeat the earlier global search endpoint as if it were safely section-scoped. Keep rollback available through this check.

### Downtime and capacity

WAL technically permits readers during a rebuild, but the rebuild is a long write transaction and schema replacement affects prepared statements. PMS writers can stall, existing snapshots can retain large WAL state, and a running application can initiate its own maintenance. **For these servers, stop PMS for the staged rebuild/publication.** The prior WAL incident makes an online rebuild an especially poor operational choice.

On the complete scratch copy, the `2,3` tag FTS integrity check took **18.952 s**, and the metadata-title check **19.007 s**. The whole scratch database, which contains **all eight experimental FTS tables**, returned a sole `ok` in **162.365 s**. The measured tag-only rebuild + merge + target integrity subtotal is **66.9 s**; the metadata-title subtotal is **53.6 s**. These subtotals exclude baseline checks, backup/staging, whole-DB validation, and startup. [Successful validation](fts-prefix-evidence/validate-review.out).

Do not describe the 45-second tag build as total downtime. Backup/copy, source/staged count gates, integrity checks, publication, and startup must be added; the full repository maintenance also performs unrelated rebuilds, REINDEX, ANALYZE, and hooks. Allow a **10–30 minute planning window** for an initial full maintenance rollout, explicitly an unmeasured scheduling allowance; measure the exact stopped-copy workflow before booking a tighter window. A focused prefix-only staged workflow may be shorter.

For the full maintenance route, reserve roughly **45 GB of additional free space per 18 GB instance** for a full dated backup, a full staged copy, new FTS allocation, journals/temporary growth, and margin. That is conservative provisioning, not measured peak usage. Sequential rollout can reuse working space, but retained backups accumulate. A targeted in-place stopped transaction needs less temporary space but still needs a coherent full rollback backup. Do not run VACUUM merely to reclaim the dropped FTS pages; SQLite can reuse them. The review stayed around 3.1 GB in the authorized scratch directory, well below 20 GB.

### Rollback and preventing silent reversal

Before publication, discard only the owned staged candidate and keep the original. Immediately after publication, stop writers and restore the coherent pre-change backup, handling WAL/SHM only under the stopped-owner restore procedure. If service has accepted new writes, an old full backup loses those writes: instead consider a new stopped/staged transaction recreating only the FTS target with its **recorded original DDL**, rebuilding/verifying it from the current approved content convention. For Brutus metadata titles that recorded DDL already includes `2,3`; rollback must not accidentally remove it.

Keep the desired prefix policy in versioned mod/maintenance configuration and apply it at the staged-schema point on every authorized maintenance run. Add a read-only audit of exact table columns/content/tokenizer/prefix and trigger definitions after each PMS update and after its migrations. Warn on drift and schedule reapplication under downtime; do not perform DDL in an open-time extension or prepare hook. Ordinary `rebuild`, `optimize`, and VACUUM preserve the option. A vendor DROP/CREATE may remove it, and a vendor schema change should require review rather than blind string replacement.

## 8. What remains unverified

* Actual HTTP-search latency after the change, traffic-weighted FTS fractions, and new p90/p99: no production mutation or application load test was authorized/performed.
* The exact fuzzy expansions for “to”, “the”, and “the office” were not supplied or captured; their measurements use the exact prefix form. The fuzzy benchmark uses only the supplied three-group expression, not invented Plex alternatives.
* The cause of the PMS/CLI timing gap and effective settings/callbacks on existing PMS connections: no process tracing or injection was used. Connection sensitivity tests excluded several simple explanations without proving the cause.
* Brutus prefix provenance, mapping of the two migration-only IDs to named code, and the complete future migration/Butler call paths: migration rows, current strings, and retained logs were insufficient.
* Whole-live-index raw-content parity and corruption status: comprehensive live integrity checks would violate the short-read restriction. The required stopped-copy gates remain part of rollout.
* A production-size long-running scan/update/automerge workload, cold-cache behavior, and mixed-client concurrency: the write test is a bounded insert experiment and the host remained active.
* A deployable fuzzy rewriter and exhaustive tokenizer proof: only isolated expression equivalence/performance experiments were done.
* The global `~/.claude/CLAUDE.md` referenced by the checkout does not exist in this Windows environment. The checkout’s `CLAUDE.md`, requested architecture documents, maintenance script, and rewrite implementation were read.

No source implementation, running configuration, service lifecycle, or live database was changed; no commits, pushes, or PRs were created.
