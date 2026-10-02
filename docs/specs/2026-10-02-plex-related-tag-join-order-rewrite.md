# Spec: Plex "related tag count" join-order rewrite

Status: DRAFT for implementation (Codex). Author: Claude, 2026-10-02. Owner: Eric (Metuant).

## 1. Problem

Opening an item's detail page makes Plex count, for every tag on the item, how many items in
the same library section share it. On Gaius (rs34, container `plex`) this one statement was
the #1 consumer of database time for the week to 2026-10-02: 1,763 slow executions,
1,270 s total, mean 721 ms, max 38.5 s. It is 20x the next statement shape.

Statement as Plex emits it (bind form; double spaces are literal):

```
select taggings.id, count(*) as taggings_count from tags   join taggings on taggings.tag_id=tags.id   join taggings as related on taggings.tag_id=related.tag_id   join metadata_items on metadata_items.id=related.metadata_item_id  where taggings.metadata_item_id=? and metadata_items.library_section_id=?  and tags.tag != ''  and tags.tag_type not in (?, ?) group by taggings.id;
```

Root cause (measured 2026-10-02 on the live Gaius DB, read-only, with the deployed mod
library `024d77c2…` through `Plex SQLite`): many items carry tags whose name is empty
(tag_type 9: 35 such tags on one item, each shared by ~2.8 M taggings, 99 M related rows in
total) plus tags of the excluded types 312/313 (1.4–2.7 M rows each). The WHERE clause throws
all of those away (`tags.tag != ''`, `tags.tag_type not in (…)`), but for some sections the
planner joins `tags` LAST:

```
SEARCH taggings USING INDEX index_taggings_on_metadata_item_id (metadata_item_id=?)
SEARCH related USING COVERING INDEX idx_dshadow_taggings_tag_id_metadata_item_id (tag_id=?)
SEARCH metadata_items USING COVERING INDEX idx_dshadow_metadata_items_section_id_type (library_section_id=? AND id=?)
SEARCH tags USING INTEGER PRIMARY KEY (rowid=?)
```

so it walks every related row of the junk tags before discarding them (24 s for one item).
For other sections it picks `taggings → tags → related → metadata_items` and is fast. It is a
cost knife-edge, not missing statistics.

## 2. Goal

Pin the good join order. All four joins are inner joins, so reordering the first two tables
and writing them as `CROSS JOIN` (SQLite's documented way to fix join order; semantically an
inner join) returns exactly the same rows. Only the FROM clause bytes change; the WHERE,
GROUP BY and any trailing `;` are copied byte-for-byte.

Rewritten statement:

```
select taggings.id, count(*) as taggings_count from taggings cross join tags on taggings.tag_id=tags.id cross join taggings as related on taggings.tag_id=related.tag_id cross join metadata_items on metadata_items.id=related.metadata_item_id  where taggings.metadata_item_id=? and metadata_items.library_section_id=?  and tags.tag != ''  and tags.tag_type not in (?, ?) group by taggings.id;
```

Measured with `Plex SQLite` (deployed library), original vs rewritten, same literals:

| DB | sample | original total | rewritten total | worst original | worst rewritten | rewritten slower |
|---|---|---|---|---|---|---|
| Gaius | 40 random items + 3 known slow | 78.74 s | 8.44 s | 27.94 s | 0.86 s | 0 items |
| Brutus | 40 random items | 14.17 s | 14.13 s | 1.99 s | 2.03 s | 0 items |

Known slow items: 1459870/§11 24.18 s → 0.017 s; 638005/§10 27.94 s → 0.51 s; 637838/§10
19.23 s → 0.58 s. Result equivalence (rows AND order) was checked on 240 random items across
both servers (see §9).

Non-goals: changing the count semantics, caching counts, touching any other related/hub
statement (the `tags.id in (…) … order by cnt desc` similar-items query is a different shape).

## 3. Matcher

Follow the exact-shape, fail-open style of `match_ondeck_query` / the hub type-OR matcher
(raw `zSql` byte matching against `static const char` pieces, positional parsing,
`plex_expect_bytes`, `OBS_MISS_CAPTURE` with `sub_reason`).

### 3.1 Exact head (must match from offset 0; miss is silent)

```
select taggings.id, count(*) as taggings_count from tags   join taggings on taggings.tag_id=tags.id   join taggings as related on taggings.tag_id=related.tag_id   join metadata_items on metadata_items.id=related.metadata_item_id  where 
```

(three spaces before each `join`, two spaces before `where`, one space after it — exactly as
emitted).

### 3.2 Body

After the head, require exactly (with `<V>` = a positional `?`, canonical `?N`, or an
unsigned decimal literal; reuse the ondeck value parser and its `SQLITE_LIMIT_VARIABLE_NUMBER`
handling):

```
taggings.metadata_item_id=<V> and metadata_items.library_section_id=<V>  and tags.tag != ''  and tags.tag_type not in (<V>[, <V>]...) group by taggings.id
```

then optional `;` and optional trailing whitespace to the end of `scan_len`. The NOT IN list
accepts 1..16 values separated by `, ` (comma space, as emitted). Anything else fails open:

| condition | sub_reason |
|---|---|
| item/section value not `?`/`?N`/literal | `value` |
| filter bytes differ (`!= ''`, spacing, extra predicate) | `filter` |
| NOT IN list empty, >16 values, or other separator | `not_in` |
| bytes after `group by taggings.id` other than `;`/whitespace | `trailing` |
| comment anywhere | `comment` |
| double-quoted token anywhere (DQS build) | `dqs` |

### 3.3 Output

Output = the rewritten FROM prefix (§2, up to and including `  where `) + the original bytes
from the end of the head to the end of the statement, unchanged. No bind token moves, so
SQLite's numbering is unchanged: set `expected_bind_count` to the statement's full bind count
(counted with SQLite's rules over the whole original) with `verify_bind_count = 1`;
`expected_column_count = 2`, `verify_column_count = 1`. Reuse `plex_prepare_rewritten_sql`
so tail mapping, bind/column verification, `rewrite_applied` logging and fail-open retry are
inherited.

## 4. Integration points

- Env gate `SQLITE3_DISABLE_PLEX_RELATED_TAG_ORDER_REWRITE`, opt-in like the hub type-OR and
  app-ANALYZE gates: literal `0` enables; unset, `1` and every other value disable. Same
  `pthread_once` + atomic pattern.
- `src/rewrite_modes.h`: add `X(PLEX_RELATED_TAG_ORDER, "plex", "related+join_order", "plex_fts_rewrite", 1)`.
- `plex_fts_rewrite_prepare`: include the new gate in the early-exit condition; dispatch the
  matcher so no existing matcher can shadow it (heads are disjoint; add a test that each
  existing private/public Plex matcher misses this statement and this matcher misses theirs).
- Target gating inherited (Plex `com.plexapp.plugins.library.db` only).
- No pin changes; `tests/check_pin_alignment.sh` and the negative pin suite must still pass
  (update the catalogue counts the way the app-ANALYZE change did).

## 5. Tests (wire into the existing harness like the hub type-OR / app-ANALYZE cases)

Positive — assert rewritten text byte-equal to expected AND identical result rows **in the
same order** against the original on a fixture with: an item tagged with a normal genre, a
collection, an empty-name tag of type 9 shared by many items, and tags of the excluded types;
items in two sections.

1. Bind form exactly as §1 (`?` x4), item in section A, section A.
2. Same with section B (the item's tags counted in another section; rows with zero related
   rows in that section must be absent in both).
3. Literal form (`=1459870`, `=11`, `not in (313, 312)`).
4. `?N` canonical binds.
5. NOT IN with 1 value and with 3 values.
6. No trailing `;`; trailing `;` + whitespace.

Negative — prepared unchanged, expected `sub_reason` logged where applicable:

7. Extra predicate after the filter → `filter`.
8. `tags.tag != ""` (double quotes) → `dqs`.
9. Comment inside → `comment`.
10. Second statement after `;` → `trailing`.
11. Head with different spacing → silent miss.
12. Gate unset / `1` → unchanged, no log record.

Plan regression: on a fixture whose stats make the original plan put `tags` last (or, if the
synthetic planner will not reproduce it, by asserting the rewritten statement's
`EXPLAIN QUERY PLAN` order is taggings → tags → related → metadata_items), document which.

Bench: add a prepare-overhead case; a head miss must cost one `memcmp`.

## 6. Docs

- `docs/env-vars.md`: row for the new gate in the existing format.
- `docs/architecture/rewrite-engine.md`: section "Plex Related Tag Join-Order Rewrite"
  (gate, head, body rules, fail-open reasons, output, measured effect).
- `docs/architecture/smoke-tests.md`: new cases.
- `docs/invariants/sqlite3-builds.md`: invariant "the related-tag rewrite only reorders the
  four inner joins of this exact statement into CROSS JOINs; WHERE/GROUP BY bytes are never
  touched."

## 7. Build and delivery

- Base: tag `rs34-live-20260913` (2af9ca4, branch `feat/plex-app-analyze-noop-20260913`) — the
  exact source of the library live on both servers. Branch `feat/plex-related-tag-order-20261002`.
- Build with the recorded recipe (`reports/2026-09-13-plex-app-analyze-noop-implementation.md`,
  `build.sh` on rs34: `LIBRARY_VARIANT=plex`, `MARCH=x86-64-v3`, pins from `pins/versions.env`,
  ICU 69 from `pins/library-compat-groups.tsv`) in a fresh rs34 workspace
  `/opt/scripts/sqlite3-builds-related-20261002/`; run the private suites as the 09-13 build
  did; report the artifact sha256.
- Do NOT touch `/opt/docker/mods`, compose, or any container on rs34. Claude stages the
  sideload and deploys.
- Commit messages carry the session attribution lines. No PR to upstream darthShadow.

## 8. Acceptance (Claude verifies on rs34)

- Gaius: `rewrite_applied mode=related+join_order` visible (observability is on); the
  related-count statement disappears from the top of the slow-query ranking; item 1459870's
  detail page loads without a multi-second stall.
- Brutus: no regression.
- Revert: set the gate to `1` (or remove it) and recreate the container.

## 9. Evidence (2026-10-02)

- Weekly ranking: Gaius `docker logs` slow_query lines grouped by statement shape.
- Equivalence: `related_equiv.py` (python sqlite3, read-only) on 120 random items per server
  incl. 10 % deliberate cross-section probes, comparing original vs rewritten rows in order.
- Timing: `related_bench.sh` with the deployed `Plex SQLite`.
