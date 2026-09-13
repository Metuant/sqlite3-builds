# Spec: Plex fuzzy MATCH literal canonicalization rewrite

Status: DRAFT for implementation (Codex). Author: Claude, 2026-09-12. Evidence: OPERATIONS.md
entries 18:40 / ~22:30 CDT on rs34, `reports/2026-09-12-plex-fts-prefix-review.md` §6, and the
captured corpus `docs/specs/2026-09-12-plex-search-match-literals-corpus.tsv` (count, literal).

## 1. Problem

Plex search (`/library/search`, `/hubs/search`) runs, per library section and per tag type, up to
three FTS4 passes. When exact hits are insufficient it runs a *fuzzy* pass whose MATCH literal is
built from spelling alternatives. Two forms were captured on production (Gaius, PMS 1.43.2):

```
OR-form:   (the OR they OR te OR theo OR thea OR teh OR thee OR tee OR tea OR teo OR the*) (office OR offices OR officer OR office''s OR officers OR officer''s OR official OR oficial OR oppice OR ofime OR office*)
star-form: (the* OR* they* OR* te* OR* tea* OR* thee* OR* theo* OR* tee* OR* teh* OR* thea* OR* teo* OR* the*)* (office* OR* officer* OR* offices* OR* officers* OR* officer''s* OR* official* OR* oficial* OR* officially* OR* officials* OR* official''s* OR* office*)*
```

In the star-form, `OR*` is **not** the OR keyword: FTS3's keyword scanner requires the keyword to be
followed by whitespace, `"`, `(`, `)` or end of input, so `OR*` lexes as the prefix term `or*`.
Verified on the scratch copy of Brutus tags: `(the* OR* they*)` returns 26 rows, identical to
`(the* they* or*)`; a `*` after `)` is tolerated. The star-form group is therefore an all-AND
conjunction in which `or*` (18,180 tag documents) appears 10 times and the query token twice.
FTS3 materializes a doclist per *phrase occurrence*, so every duplicate costs a full doclist load
and an extra AND iterator. Measured on identical data (scratch `review.db`, real Plex ids):

| literal (star-form) | tag FTS plain | after dedupe | tag FTS prefix='2,3' | after dedupe |
|---|---:|---:|---:|---:|
| the office | 257 ms | 100 ms | 10 ms | 6 ms |
| star wars | 115 ms | 18 ms | 15 ms | 7 ms |
| breaking bad | 113 ms | 14 ms | 6 ms | 2 ms |
| game of thrones | 106 ms | 13 ms | 7 ms | 3 ms |
| harry potter and | 226 ms | 67 ms | 21 ms | 7 ms |

Row sets were identical in both directions (`EXCEPT`) for tags and titles in every case. Absorbing
OR-alternatives already covered by a prefix term in the same group (`the`, `they`, `theo` … next to
`the*`) is also result-identical and gives ~2x on prefixed tables (19→9, 41→21, 22→7 ms) and 5-15%
on plain ones. **Reordering** AND groups (the earlier "rare-first" idea) produced no measurable gain
on any captured literal and is explicitly out of scope.

Not in scope, by design: the exact/prefix passes (`'The* Office*'`, `'the* office*'`) have nothing
to canonicalize; their cost is the FTS prefix expansion and is addressed by the FTS prefix index.

## 2. Semantics that make this safe

FTS4 MATCH here is used for row membership only (no `matchinfo`, `offsets`, `snippet`, `rank`).
For membership:

- `A AND A ≡ A`, `A OR A ≡ A` (exact-byte duplicate terms).
- In an OR group, an alternative `X` is redundant if another alternative `P*` exists with `X`
  beginning with `P` (bytewise), because every token matching `X` (exact or prefix) matches `P*`.
- In an AND group, a prefix term `P*` is redundant if another term `T` (exact or prefix) exists
  with `T`'s text beginning with `P`, because any token matching `T` also matches `P*`, so
  `T AND P*` ≡ `T` for document membership.

Prefix matching compares tokenizer output (ICU primary-strength collation keys), not literal
bytes. The byte-prefix relation implies the key-prefix relation only when both terms are plain
ASCII `[A-Za-z0-9]+`; for anything else (accents, `''`-escaped apostrophes, non-Latin scripts,
digits mixed with letters are fine) absorption MUST NOT be applied. Duplicate removal is bytewise
and safe for any term.

## 3. Eligibility (exact, fail-open)

Apply only when ALL hold; otherwise return the original text untouched:

1. The statement contains exactly one `match '` occurrence, of the form
   `fts4_tag_titles_icu.tag match '<lit>'` or `fts4_metadata_titles_icu.<col> match '<lit>'` with
   `<col>` ∈ {`title`, `title_sort`, `original_title`}, `<lit>` a single-quoted SQL string in which
   the only quote characters are `''` pairs. No bind parameter is allowed as the MATCH operand.
2. No `--` or `/*` anywhere in the statement; no double-quoted token anywhere in the statement
   (existing `plex_fts_rewrite.c` conventions).
3. `<lit>` parses under this grammar exactly (single ASCII spaces, no leading/trailing space):
   ```
   literal := group (' ' group)*
   group   := '(' body ')' ['*']
   body    := term (' ' term)*
   term    := word ['*']
   word    := 1+ bytes, none of: space ( ) " : ^ , and no lone ' (an apostrophe must be the '' pair)
   ```
   A term whose bytes are exactly `OR` is the OR keyword. Reject the literal if any term is `AND`,
   `NOT`, `NEAR` or begins with `NEAR/`; if any body is empty; if `OR` is first, last or adjacent
   to another `OR` in its body; if a body mixes keyword `OR` with non-alternating layout
   (an OR group MUST be `term OR term (OR term)*`); if there is any byte outside the ranges
   above at the top level (bare terms outside parentheses ⇒ ineligible: those are the exact
   passes). Groups with the trailing `*` are accepted and the `*` is preserved verbatim.
4. After transformation the literal must be non-empty and differ from the original; if nothing
   changed, do not rewrite (count as `noop`, not as applied).

Sub-reasons for `OBS_MISS_CAPTURE` diagnostics: `no_match`, `multi_match`, `bind_operand`,
`quote`, `comment`, `dqs`, `grammar`, `keyword`, `or_layout`, `empty_group`, `noop`.

## 4. Transformation (deterministic; original term order preserved)

For each group, in order:

1. **Dedupe** — remove any term that is bytewise identical to an earlier term in the same group
   (OR keyword tokens are not terms). In an OR group, also collapse the `OR` separators
   accordingly so the body remains `term OR term …`.
2. **Absorb (OR group)** — remove term `X` if another surviving term `P*` in the group has
   `P` a proper or equal byte-prefix of `X`'s word (X may itself be `X*`; then require `P` to be a
   prefix of `X`'s word and `P` ≠ `X`'s word, i.e. drop the narrower prefix term). Only when both
   words are ASCII `[A-Za-z0-9]+`. Never remove the last term of a group.
3. **Absorb (AND group)** — remove prefix term `P*` if another surviving term `T` in the group has
   a word that begins with `P` and is longer than `P` (T exact or prefix). Same ASCII rule. Never
   remove the last term.

Rebuild the literal with single spaces, the same parentheses, the same `)*` suffixes, the same
`''` escapes, and splice it back in place of the original literal; every other byte of the
statement is copied verbatim.

Worked examples (from the corpus):

```
(the* OR* they* OR* te* OR* tea* OR* thee* OR* theo* OR* tee* OR* teh* OR* thea* OR* teo* OR* the*)*
  → dedupe: (the* or* they* te* tea* thee* theo* tee* teh* thea* teo*)*  [note: OR* is a term "OR*"; keep its bytes]
  → AND-absorb: te* (tea*, tee*, teh*, teo* start with te) and the* (they*, thee*, theo*, thea*) are dropped
  → (OR* they* tea* thee* theo* tee* teh* thea* teo*)*
(the OR they OR te OR theo OR thea OR teh OR thee OR tee OR tea OR teo OR the*)
  → OR-absorb: the, they, theo, thea, thee dropped (covered by the*)
  → (te OR teh OR tee OR tea OR teo OR the*)
(harry OR ary OR hary OR háry OR arya OR array OR arr OR harry''s OR arroyo OR aarya OR harry*)
  → only `harry` is dropped; `háry`, `harry''s` are non-ASCII/escaped and must stay.
```

Case: byte-prefix comparison is case-sensitive; Plex emits consistent case within a literal
(`The* OR* They*` vs `the OR they`). Do not case-fold.

## 5. Integration

- New file `src/plex_fuzzy_match_rewrite.h` (header-only like the other private rewrites), called
  from the Plex dispatch as a **pre-pass**: canonicalize the literal first, then hand the (possibly
  rewritten) text to the existing matchers (fts-join, title-scope, …), which treat MATCH as opaque.
  If the pre-pass fails open the downstream matchers see the original text.
- Gate: `SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE`, opt-in on literal `0` (unset/other = disabled),
  same semantics as the hub type-OR gate. Independent of `SQLITE3_DISABLE_AUTOPRAGMA` and
  `SQLITE3_DISABLE_PLEX_FTS_REWRITE`.
- Observability: `OBS_MODE_PLEX_FUZZY_MATCH` ("fuzzy+match_canon", index-missing eligible 0);
  emit `rewrite_applied` with source/rewritten text through the existing sampler; misses through
  `OBS_MISS_CAPTURE` with the sub-reasons above.
- Verification: dispatch through `plex_prepare_rewritten_sql` with `expected_column_count` equal to
  the original's and `expected_bind_count` from `sqlite3_prepare_v2_real` of the original, verify=1.
- Catalogue/docs/tests plumbing as for the hub rewrite: `src/rewrite_modes.h` row, `docs/env-vars.md`
  row, `docs/architecture/rewrite-engine.md` section, `CLAUDE.md` gate sentence, invariants entry,
  `tests/check_pin_alignment.sh` counts, `tests/plex_private_rewrites_test.sh` wiring.

## 6. Tests (all must pass; fail-open cases prepare the original unchanged)

1. Corpus: every parenthesised literal in the TSV is accepted; every unparenthesised one is
   `no_match`/`grammar`. Golden outputs for the five worked/star-form literals above.
2. Parity: on a fixture built with the Plex ICU build and `tokenize=collating 'root@colStrength=primary;colAlternate=shifted'`,
   for every accepted corpus literal, `SELECT rowid … MATCH original` and `… MATCH rewritten` are
   equal as sets (EXCEPT both ways = 0), on both a plain and a `prefix='2,3'` table, including
   non-empty results (seed rows so that at least: `the office`, `star wars`, `harry potter and` hit).
3. Adversarial literals (each fails open with the named sub-reason): `NEAR`, `NOT`, `AND` keyword,
   `"phrase"`, `title:term`, `^first`, nested `((a))`, unbalanced, empty `()`, `(OR a)`, `(a OR)`,
   `(a OR OR b)`, lone apostrophe, bare terms `a* b*`, two MATCH operands, bound operand `match ?`,
   comments, double-quoted identifiers, non-ASCII absorption candidates (`(há OR ha*)` must keep `há`).
4. Idempotence: rewriting the rewritten text is a `noop`.
5. Bind/column counts unchanged; existing suites (fts-join, title-scope, collection-tag,
   recent-episode, hub type-OR, baseline smoke) still pass with the gate on and off.
6. Bench entry: per-literal timing (plain and prefix) over the corpus, reporting the table above.

## 7. Deployment note (not part of the implementation)

Rollout is via the LSIO sideload like `sqlite3-plex-fixes-20260912` (new artifact sha, gate set to
`0` in compose). The rewrite is independent of the FTS prefix index; the two compose. Expected
production effect: fuzzy passes only (queries with insufficient exact hits, ~40% of fuzzy
statements are star-form), 3-8x on those statements without the prefix index, 1.5-3x with it.
