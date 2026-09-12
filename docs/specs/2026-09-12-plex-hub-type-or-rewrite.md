# Spec: Plex "hub type-OR" rewrite

Status: DRAFT for implementation (Codex). Author: Claude, 2026-09-12. Owner: Eric (Metuant).

## 1. Problem

Plex's home-screen "Recently Added TV" hub issues a shows→seasons→episodes self-join, paged
by `limit 39 offset N` (N = 0, 39, 78, … up to ~312 within one hub build). On Brutus (rs34,
TV Shows section 5, 32,791 shows, 1.07 M episodes) each page costs 470–710 ms; on Gaius the
identical statement costs 1–2 ms. A cold `/hubs/promoted` on Brutus therefore takes
3.7–5.0 s, of which 4.3 s is this one statement. Real impact measured 2026-09-12: 147 promoted
hub loads in 3 h, p90 1.1 s, max 2.3 s, 16 over 1 s (the per-user hub cache hides repeats).

Root cause (bisected on the live databases, same SQL / same index DDL / same STAT1 / same
STAT4 sample counts / no existing rewrite involved): Plex emits a redundant predicate

    (metadata_items.metadata_type=2 or metadata_items.metadata_type=10)

alongside the equality `metadata_items.metadata_type=2`. With the OR present, Brutus's
planner abandons the `(library_section_id, metadata_type, added_at)` index order and emits
`USE TEMP B-TREE FOR ORDER BY`, i.e. it materialises the whole join (every episode in the
library) before taking the page. Without the OR it keeps index order and stops at the page:

| Brutus, per page            | with Plex's OR | without it |
|-----------------------------|----------------|------------|
| `offset 0`                  | 607 ms         | 40 ms      |
| `offset 273`                | 711 ms         | 2 ms       |

Gaius plans the index-ordered variant either way; the difference is a STAT4 cost knife-edge
tipped by Brutus's data. It cannot be fixed with Plex settings, and re-running ANALYZE is
not a fix (samples are random).

## 2. Goal

Add a Plex rewrite to the library (`src/plex_fts_rewrite.c`) that removes the redundant
`metadata_type` OR group from this statement shape when an equality on the same column and
one of the same literals is also present. The transformation is semantically exact:
`t=A AND (t=A OR t=B) ≡ t=A`. Everything else in the statement is copied byte-for-byte.

Non-goals: general boolean simplification; touching the duplicated `title_sort` ORDER BY
term; any other hub shape (movies hub has no join and is fast); anything outside the
Plex library.db target.

## 3. Statement shape (as Plex emits it; double spaces are literal)

Captured on Brutus 2026-09-12 (admin user; per-user restriction predicates vary, see §4.2):

```
select metadata_items.id from metadata_items  join metadata_items as children on children.parent_id=metadata_items.id  join metadata_items as grandchildren on grandchildren.parent_id=children.id  where metadata_items.library_section_id in (5) and (metadata_items.title NOT LIKE 'Naked News' and metadata_items.id not in (select taggings.metadata_item_id from taggings join tags on tags.id=taggings.tag_id where tags.id=1554) and grandchildren.added_at>1788033716 and (metadata_items.metadata_type=2 or metadata_items.metadata_type=10))  and metadata_items.metadata_type=2  order by metadata_items.added_at desc,metadata_items.title_sort collate icu_root ,metadata_items.title_sort collate icu_root  limit 39 offset 0
```

Expected rewrite (only the ` and (metadata_items.metadata_type=2 or metadata_items.metadata_type=10)` bytes are removed):

```
select metadata_items.id from metadata_items  join metadata_items as children on children.parent_id=metadata_items.id  join metadata_items as grandchildren on grandchildren.parent_id=children.id  where metadata_items.library_section_id in (5) and (metadata_items.title NOT LIKE 'Naked News' and metadata_items.id not in (select taggings.metadata_item_id from taggings join tags on tags.id=taggings.tag_id where tags.id=1554) and grandchildren.added_at>1788033716)  and metadata_items.metadata_type=2  order by metadata_items.added_at desc,metadata_items.title_sort collate icu_root ,metadata_items.title_sort collate icu_root  limit 39 offset 0
```

Verified on the live Brutus DB (read-only): the rewritten text returns the same rows and
plans without the temp b-tree.

## 4. Matcher

Follow the existing exact-shape, fail-open style of `match_ondeck_query` (raw `zSql`
byte matching against a `static const char head[]`, positional parsing, `plex_expect_bytes`,
`OBS_MISS_CAPTURE` diagnostics with `sub_reason`).

### 4.1 Head (exact bytes, must match from offset 0)

```
select metadata_items.id from metadata_items  join metadata_items as children on children.parent_id=metadata_items.id  join metadata_items as grandchildren on grandchildren.parent_id=children.id  where metadata_items.library_section_id 
```

After the head, accept the section selector as either `in (` + nonempty list of unsigned
decimal literals + `)` or `=` + unsigned decimal literal; also accept a positional `?` /
canonical `?N` in the single-value positions, using the same rules and
`SQLITE_LIMIT_VARIABLE_NUMBER` handling as `plex_parse_ondeck_value_at`. Section parse
failure → `capture_miss sub_reason=section`.

### 4.2 Body scan

Expect ` and (` next (the opening of Plex's parenthesised filter block). Then scan forward
to locate, within that block, the exact byte sequence

```
 and (metadata_items.metadata_type=<A> or metadata_items.metadata_type=<B>)
```

where `<A>` and `<B>` are unsigned decimal literals with `A != B`. The block between ` and (`
and this sequence is opaque and copied verbatim; it may be empty of predicates only in the
sense that Plex may emit the OR group as the first conjunct — in that case the sequence is
`(metadata_items.metadata_type=<A> or …)` immediately after ` and (` with no leading
` and `. Handle both:

- Preceded by ` and ` inside the block → delete ` and (…OR…)`.
- Immediately after the block's opening `(` and followed by `)` → the block would become
  `()`; fail open (`sub_reason=empty_block`). Do not attempt to restructure.

Immediately after the OR group's closing `)` expect `)` (end of the block), then the bytes
`  and metadata_items.metadata_type=<C>` (two spaces, as emitted) followed by two spaces and
`order by `. `<C>` must be an unsigned decimal literal equal to `A` or `B`; otherwise fail open
(`sub_reason=equality`). The remainder (ORDER BY … LIMIT … OFFSET …) is copied verbatim.

Scanning for the OR sequence must be token-safe: it must not match inside a string literal
(`'…'`) or a nested subquery's parentheses. Track quote state and paren depth while scanning
the block; the OR group must sit at paren depth 1 (directly inside the block). Any of the
following fail open with the listed `sub_reason`:

| condition | sub_reason |
|---|---|
| no OR group found before the block closes | `no_or_group` |
| more than one candidate OR group | `multiple_or_groups` |
| OR group contains anything but the two exact equalities on `metadata_items.metadata_type` (three terms, `?`, quoted literal, whitespace variants, other alias) | `or_shape` |
| `<C>` not equal to `<A>` or `<B>` | `equality` |
| `<C>` not a literal (bind) | `equality` |
| bytes after `)` not `  and metadata_items.metadata_type=` | `tail` |
| `order by ` not found after the equality | `tail` |
| semicolon before end of scan, or second statement | `trailing` |
| any input beyond `scan_len` bound | miss (silent, like ondeck's exact-head miss) |

A miss on the exact head is silent. `capture_miss` and `out_of_scope` records use the
existing process-global first/new/every-1024th sampling keyed by lexer-normalised shape.

### 4.3 Output

Build the rewritten SQL as: head + section selector + block bytes minus the deleted OR
segment + tail, with `scan_out_len = scan_len - deleted_len` and `rewrite_len` derived the
same way as the other candidates. Bind parameters may appear anywhere outside the deleted
segment: the section slot, the opaque user-filter block (for example `title NOT LIKE ?`), and
the LIMIT/OFFSET tail. The deleted segment must contain none (already an `or_shape` fail-open).
Because the deletion moves no bind token, SQLite's numbering of every remaining `?`, `?N` and
named parameter is unchanged; count them with SQLite's own rules across the WHOLE original
statement (review finding: the section slot alone is not the bind count) and set
`expected_bind_count` to that total with `verify_bind_count = 1`. If full counting is not
implemented, reject any bind outside the section slot with `sub_reason=bind_site` instead.
Comments (`--`, `/* */`) anywhere in the statement fail open (`sub_reason=comment`). Quoted
strings: single-quoted literals are opaque; a double-quoted token inside the WHERE block fails
open (`sub_reason=dqs`), because this build enables DQS and cannot tell a string from an
identifier. Column count is 1; set `expected_column_count = 1`, `verify_column_count = 1`.

### 4.4 Ordering caveat (from review)

The ORDER BY has ties (equal `added_at`, equal `title_sort`). Removing the OR changes the
plan, and a plan change can change WHICH tied show lands at a page boundary. That is legal SQL
and the full result bag is identical, but a page-by-page comparison on tied fixtures will show
differences. Tests must compare the complete typed multiset across all pages; docs must state
that tied-row page membership was never stable under Plex's own query either. Reuse `plex_prepare_rewritten_sql` so tail mapping, bind/column
verification, `rewrite_applied` logging and fail-open retry are inherited unchanged.

## 5. Integration points

- Env gate: `SQLITE3_DISABLE_PLEX_HUB_TYPE_OR_REWRITE`. Opt-in like ON-DECK and GUID-LIKE:
  literal `0` enables; unset, `1`, anything else disables. Implement with the same
  `pthread_once` + atomic pattern (`plex_hub_type_or_rewrite_enabled`).
- `src/rewrite_modes.h`: add `X(PLEX_HUB_TYPE_OR, "plex", "hub+type_or", "plex_fts_rewrite", 1)`
  to `OBS_REWRITE_MODE_CATALOG` (eligible = 1, it is an observability-reported rewrite).
- `plex_fts_rewrite_prepare`: add `hub_type_or_on` to the early-exit condition and dispatch
  the new matcher after the ON-DECK block, using the same `PLEX_MATCH_BUILT` /
  `PLEX_MATCH_BUILD_FAILED` handling and `OBS_MODE_PLEX_HUB_TYPE_OR`.
- Target gating is inherited (only the Plex `com.plexapp.plugins.library.db` target passes
  `auto_extension_path_is_target` + basename check).
- No pins change. `tests/check_pin_alignment.sh` must still pass.

## 6. Tests (all must be added to the existing harness, `tests/plex_fts_rewrite_smoke.c`, and
wired like the ON-DECK cases)

Positive (assert rewritten text byte-equal to expected AND identical result rows against the
original on a fixture with shows/seasons/episodes spanning `added_at` and two metadata types):

1. The captured statement in §3 (literals), offsets 0 and 273.
2. `library_section_id in (5,6)`.
3. `library_section_id = 5`.
4. Section as `?` and as `?1` — bind count preserved and verified.
5. OR literal order swapped: `(…=10 or …=2)` with equality `=2`.
6. Equality on the second literal: `(…=2 or …=10)` with equality `=10`.
7. Minimal per-user block: `where … in (5) and (grandchildren.added_at>N and (…=2 or …=10))  and metadata_items.metadata_type=2  order by …` (no NOT LIKE / NOT IN).
8. Block containing a quoted string with `)` and `and (` inside it, e.g. `title NOT LIKE 'a) and (b'` — must still locate the correct OR group.

Negative (assert the statement is prepared unchanged and, where applicable, the expected
`sub_reason` is logged):

9. Equality literal not in the OR set (`=4`) → `equality`.
10. Three-term OR → `or_shape`.
11. `?` inside the OR group → `or_shape`.
12. OR group nested one level deeper (`and ((…=2 or …=10))`) → `or_shape` or `no_or_group`.
13. OR group is the whole block → `empty_block`.
14. Different alias in the OR (`children.metadata_type`) → `or_shape`.
15. Movies hub shape (no children/grandchildren joins) → silent head miss.
16. Trailing `;` and a second statement → `trailing`, tail mapping intact.
17. Gate unset / `=1` → unchanged, no log record.

Plan regression (best effort, mirror `tests/plex_fts_stat4_eqp_repro_test.sh`): generate a
fixture with STAT4 samples that reproduce the temp-b-tree plan for the original and assert
the rewritten statement's `EXPLAIN QUERY PLAN` contains no `USE TEMP B-TREE FOR ORDER BY`.
If the planner does not reproduce the sort on a synthetic fixture, document that and keep the
text/result-equivalence tests as the gate.

Bench: add a case to `tests/plex_fts_rewrite_prepare_bench.c`; prepare overhead for a
non-matching statement of similar length must stay within the existing budget for the
ON-DECK matcher (head miss must be a single `memcmp`).

## 7. Docs

- `docs/env-vars.md`: add the row for `SQLITE3_DISABLE_PLEX_HUB_TYPE_OR_REWRITE` in the
  table's existing format (opt-in, literal `0` enables).
- `docs/architecture/rewrite-engine.md`: new section "Plex Hub Type-OR Rewrite" in the style
  of the On-Deck section: gate, exact head, capture rules, fail-open reasons, output, and
  the verification path.
- `docs/architecture/smoke-tests.md`: list the new cases.
- `docs/invariants/sqlite3-builds.md`: add the invariant "the hub type-OR rewrite only
  deletes an OR group whose equality twin is present at top level; it never restructures
  parentheses."

## 8. Build, lineage and delivery

Lineage first. The libraries deployed on rs34 are `-local` builds of unknown commit:
`sqlite3-plex-title-scope` (artifact sha `747b3c44…`, Gaius) and
`sqlite3-plex-collection-tagids` (artifact sha `613ef0a0…`, Brutus). Both expose exactly the
four existing kill-switches (FTS, GUID_LIKE, ONDECK, TAGGINGS), so their C library matches
this repo's feature set; the variant names refer to the mod's index/maintenance layer, not
to library rewrites. Builds are deterministic, so: rebuild the candidates (this fork's `main`
at 79482f7, and rs34's `/opt/scripts/sqlite3-builds` branch `temp-store-fix` at 5e5d06c) and
compare the resulting `libsqlite3.so` sha with the deployed ones. Base the feature branch on
whichever commit reproduces the Brutus artifact; if neither does, stop and report — do not
guess.

RESOLVED 2026-09-12: the deployed 09-08 state is unrecoverable (no candidate matched); the
imported production lineage is branch `hermes-lineage-20260910` (origin, head f94ef03, which
also carries `docs/reviews/2026-09-12-hermes-lineage-adversarial-review.md`). That branch is
the base. Note its dispatch order: the new matcher must be placed so it cannot be shadowed by
the recent-episode matcher (disjoint ORDER BY shapes today; add a test proving both miss each
other's statement).

Then: branch `feat/plex-hub-type-or-rewrite` from `hermes-lineage-20260910`; implement §4–§7; run the full
test suite on Linux (rs34 has the toolchain and `docker-build-base`; the repo's CI also
builds on push); produce the Plex ICU library artifact and its sha256; do not modify anything
under `/opt/docker/mods` on rs34 — staging and the sha-gated sideload swap are done separately
by Claude with the usual dry-run and env diff.

Commit message and PR body must carry the attribution lines given in the session. No PR to
upstream darthShadow (Eric's standing instruction).

## 9. Acceptance on rs34 (Claude verifies, Codex does not need rs34 write access)

- Brutus: cold `/hubs/promoted?contentDirectoryID=1,5&count=<fresh>` ≤ 1.0 s (from 3.7–5.0 s);
  per-page statement ≤ 50 ms; `rewrite_applied mode=hub+type_or` visible with observability on;
  zero `rewrite_skipped` with `bind_count_mismatch` / `column_count_mismatch` / `tail_mismatch`.
- Gaius: no change in plan or timing (already index-ordered).
- Revert: unset the gate or set it to `1` and restart the container.

## 10. Reference measurements (2026-09-12, Brutus, read-only live view)

- shows in section 5: 32,791; shows with an episode added after 2026-08-26: 594; join rows
  before LIMIT: 3,644.
- Plans: with OR → `USE TEMP B-TREE FOR ORDER BY`; without OR → index order, no sort.
- Plex also emits the same statement with per-user restriction predicates absent; the
  matcher must not depend on `NOT LIKE` / `NOT IN` being present.
