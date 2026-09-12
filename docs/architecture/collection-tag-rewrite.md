# Opt-in collection tag-first rewrite

`SQLITE3_ENABLE_PLEX_COLLECTION_TAG_REWRITE=1` enables three narrowly matched inner-join statement shapes in `src/plex_collection_tag_rewrite.h`. Unset/0/other values keep the original path. The flag is cached once per process and independent of other rewrite flags.

## Shapes

1. Membership lookup: `SELECT tags.id,metadata_items.id,metadata_items.absolute_index`; section and tag-type equalities; three result columns.
2. Aggregate: `SELECT tags.id,count(*),min(year),max(year),tags.tag,tags.extra_data`; section and metadata-type equalities, literal `tag_type=2`, `GROUP BY tags.id`; six result columns.
3. Grouped tag IDs: `SELECT tags.id`; the exact aggregate predicates and grouping; one result column. This is the incremental candidate extension.

The implementation preserves original SELECT and WHERE/GROUP BY bytes. It changes inner JOINs to SQLite CROSS JOINs and checks for the existing `idx_dshadow_taggings_tag_id_metadata_item_id` index at admission. Generated SQL names no index, so automatic reprepare after an index drop can choose another access path. ON equality direction, scalar bytes, parameter names/order, duplicate semantics and GROUP BY semantics remain unchanged. The grouped shapes require literal tag_type=2. Unsupported projections/predicates/aliases, comments, compounds, literal NULLs and missing indexes or probe errors fall back. Prepare/tail/column-count failures retry the original through existing cleanup. There is no new DDL, index creation, ANALYZE, or checkpoint.

## Tests

- `SQLITE3_CFLAGS=-I/path/to/sqlite/headers SQLITE3_LIBS=-l:libsqlite3.so.0 bash tests/plex_collection_tag_test.sh`: real production wrapper with real-SQLite adapter; all shapes, prepare interfaces, literals, whitespace/case, named/numbered/repeated/NULL bindings, bounded nByte/tails, injected failures, rejected forms; ASan/UBSan guarded buffers, allocation failure, idempotence and 300,000 mutations.
- `tests/plex_fts_join_test.sh` and `tests/plex_title_scope_test.sh` with the collection flag enabled: noninterference. Adapter/runtime versions are reported separately from native results.
- `tests/check_pin_alignment.sh`: unchanged runtime pins and staging/catalog contracts.
- `python3 run-isolated-build.py`: builds the separate `plex-collection-tagids-build:local` image without replacing either earlier candidate image.
- Native suite: copy `tests/collection-native/{runner.c,native_suite.py}` to a fresh /validation directory in the build image. Compile against the matching header and `/app/sqlite-amalgamation-3530300/dist/libsqlite3.so`, with `/opt/icu-69-plex/lib` in the link/runtime paths. Fixtures compare full typed row multisets, parameter metadata, result-column names and tails; include an independent nonempty tag-ID oracle, rewritten plan order, default-off/unsupported-shape/missing-index fallback.

Native legacy sqlite3_prepare compatibility does not imply that its statement was rewritten: preserve baseline behavior and separately verify v2/v3 rewrite use. Native fixtures and full build verification are distinct from application startup/installer tests.

## Evidence and limits

The prior two-shape canary is already deployed on Brutus; the third-shape extension is isolated and NOT deployed. A passive September 8 organic capture found the unrewritten third shape consumed 102,572,485 VM steps in three disjoint intervals totaling 15.498045099 seconds, matching the 15.498-second cumulative tag-sync timer. Each pass found zero collection tags. No pool-starvation event occurred in that capture. Statement-boundary intervals can include application work; this measures a compelling phase-cost target, not proof of every historical outage.

Bound NULLs can perform more work than the original fast-empty plan. Other library/data distributions remain unbenchmarked at production scale. SQL fixture improvements are not promises of HTTP latency or concurrency relief.

## Deployment boundary

Preserve `/tmp/plex-collection-rewrite`, its candidate image/package, and the original title-scope baseline. Build/package/installer testing is not rollout authorization. Any future approved canary affects Brutus only, retains the currently deployed two-shape mod/config as rollback, avoids database backup/restore/schema/statistics changes, and measures organic phase work afterward. Gaius remains unchanged. No synthetic production scans/searches are implied.
