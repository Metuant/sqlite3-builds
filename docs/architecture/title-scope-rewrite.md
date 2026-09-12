# Plex title scope rewrite

`SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE=1` opts in; the baseline FTS gate must
also be enabled. Exact movie/episode title or title_sort searches with a user-tag
equality replace the two LEFT JOINs with inner CROSS JOINs, retaining duplicates,
scalar bytes, sorting and pagination. The equality remains
`taggings.tag_id=tags.id` to retain left-column collation precedence. The intended
schema uses INTEGER PRIMARY KEY metadata IDs and ordinary integer tag links;
this is not a general proof for arbitrary ID affinities or shadowing schemas.

The existing taggings index-name probe remains an admission check. Generated SQL
contains no INDEXED BY; CROSS JOIN order survives index removal without a forced
index failure during automatic reprepare. Double-quoted scalar tokens are rejected.

Recovery is prepare-only: candidate prepare, tail or column-count failures retry
the original. FTS-first evaluation can raise a step-time malformed MATCH error
where the original plan filters every row before opening FTS. Future MATCH
bindings cannot be validated at prepare time. Success/error parity for arbitrary
malformed MATCH bindings is not guaranteed; there is no step-time recovery.

Run `bash tests/plex_private_rewrites_test.sh` for the adapter regressions and
sanitizer fuzzers. These scratch tests use SQLite/unicode61 and a test collation,
not the Plex application's tokenizer.
