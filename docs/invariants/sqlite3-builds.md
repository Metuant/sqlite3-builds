# sqlite3-builds invariants (evergreen)

Repo-level architectural and behavioral rules that survive across milestones.
Reference by path from charters; do not paste this content into prompts.

Milestone-specific rules (locked decisions for in-flight workstreams, current
bug states, triage decisions) live in the milestone-scoped sibling files (e.g.,
`docs/invariants/sqlite-maintenance.md`) and are cleaned up after their
milestone lands.

## 1. Build / variant

- Plex variant builds under **Alpine/musl**; generic variant (Emby +
  dormant JF) stays Ubuntu/glibc. Multi-stage `docker-library/Dockerfile`
  selects via `LIBRARY_VARIANT`.
- `LIBRARY_VARIANT=plex` is limited to the Plex ICU build path.
- SQLite and ICU pins must stay aligned across `build/Build.sh`,
  `build/build_static_sqlite.sh`, `.github/workflows/sqlite-build.yml`,
  `docker-cli/Dockerfile`, `docker-library/Dockerfile`.
- Mimalloc v3.3.2 Wire-2 link-time interposition applies to both library
  variants only; CLI stays on the platform allocator. Keep the full
  VERSION + URL + SHA512 pin tuple aligned across `build/Build.sh`,
  `build/build_static_sqlite.sh`, `docker-library/Dockerfile`, and
  `.github/workflows/sqlite-build.yml`; Plex adds `-DMI_LIBC_MUSL=ON` to
  the mimalloc CMake invocation.
- CLI variant excluded from observability; both library variants (Plex +
  generic) carry it.
- `SQLITE_THREADSAFE=2` (Multi-thread) is the compile default at
  `build/Build.sh:208`. Emby compile-options required-flag array contains
  `THREADSAFE=2` so any drift fails CI.
- Plex variant build fails if `libsqlite3.so` carries `fcntl64` or any
  `@GLIBC_`-versioned undefined symbol (gate at
  `docker-library/Dockerfile:180-187`, conditional on
  `LIBRARY_VARIANT=plex`). Generic variant is glibc-linked and does NOT
  run this gate.
- Matrix rows are v2 / v3 / arm64 only; **no x86-64-v4 row** (v4 lib
  SIGILLs in the auto-extension constructor on non-AVX-512 Zen hosts).
  `resolve_arch` in `scripts/update-sqlite-library.sh.template` downgrades
  v4-capable hosts to v3.
- `SQLITE_TEMP_STORE=3` (memory) is a compile-time pin at
  `build/Build.sh:207`. `PRAGMA temp_store=2` is documentation-only
  under that profile (no-op).
- `SQLITE_DEFAULT_PAGE_SIZE=16384` (16 KiB) is the compile-default
  page-size pin in `build/Build.sh`.
- `SQLITE_DEFAULT_MMAP_SIZE=34359738368` (32 GiB) is the compile-default
  mmap-size pin in `build/Build.sh`; CI compile-option assertions keep it
  aligned with the runtime auto-PRAGMA target.
- `SQLITE_SORTER_PMASZ=8192` is the compile default in
  `build/Build.sh:205`; constructor-102 re-asserts
  `sqlite3_config(SQLITE_CONFIG_PMASZ, 8192)` in
  `src/auto_extension.c:212-249`. Emby's compile-options required-flag
  array in `.github/workflows/sqlite-build.yml:476-496` keeps the value
  pinned in CI.

## 2. Deploy

- Plex library replacement target: exactly
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- Emby library replacement target: exactly
  `/app/emby/lib/libsqlite3.so.3.49.2`.
- Jellyfin replacement target would be
  `/usr/lib/jellyfin/bin/libe_sqlite3.so`. JF is deferred; deploy-script
  branch retained but `WARN+continue`.
- Plex ICU runtime files: deploy scripts MUST NOT touch
  `libicu*plex.so.69`. Only `libsqlite3.so` is replaced.
- Plex deploy verifies existing `libicu*plex.so.69` runtime file SHAs
  against Plex-only manifest rows. Mismatch or missing pin is `FATAL` and
  exits non-zero; verification MUST NOT replace, rename, move, or delete
  those ICU files.
- N=5 bounded upgrade window: deploy assertion accepts current + 4 prior
  managed-release post SHAs in addition to LSIO-original pre SHAs.
- Pin manifest schema: 9 columns
  `kind|schema|target|arch|source|source_digest|target_path|artifact|sha256`
  plus a leading
  `version|1|managed_window|5|release_tag|<tag>|generated_at|<tag>`
  metadata row.
- `assert_pre_replacement_sha` exports `OUT_ASSERTED_SHA`. Callers MUST
  reuse it (no re-hashing). TOCTOU recheck compares against the exported
  value.
- LSIO archive surface is `tar` and `gunzip` only.
- LSIO `/init` hijacks user commands: `docker run --rm <lsio-image>
  <user-cmd>` does NOT actually run `<user-cmd>`; use
  `--entrypoint <cmd>` to bypass.
- LSIO Emby tagging differs from Plex: Plex `<upstream>`; Emby
  `version-<upstream>`.
- LSIO startup hooks assume POSIX/coreutils baseline (`mktemp`, `mkdir`,
  `cp`, `mv`, `rm`, `sh`, `bash`); non-standard tools preflight-checked.

## 3. Source

- `tools/sqlite-amalgamation.patch` is the M7 invariant: source-level
  amalgamation patching via a checked-in unified diff.
- Six wrapper-target functions (`sqlite3_initialize`, `sqlite3_config`,
  `sqlite3_db_config`, `sqlite3_open`, `sqlite3_open_v2`,
  `sqlite3_open16`) renamed to `_real` with hidden visibility. Build
  fails on rename-drift.
- `sqlite3_enable_shared_cache` no-op stub at `src/auto_extension.c:6-15`
  is load-bearing for the Plex variant. **KEEP byte-for-byte intact.**
- `tools/libsqlite3-version-script.ld` is load-bearing for symbol-export
  discipline: exact list from the pinned `sqlite3.h` plus
  `auto_extension_path_is_target`, `auto_extension_sorterref_cfg_rc`,
  `auto_extension_pmasz_cfg_rc`, and `sqlite3_enable_shared_cache`, then
  `local: *;` to deny everything else including `mi_*`. The post-link
  deny gate in `build/Build.sh:230-277` stays adjacent to the preserved
  `build/Build.sh:230-248` gates as belt-and-braces.
- Six `SQLITE_API` wrappers in `src/observability.c` MUST chain to
  `*_real` and return its result UNMODIFIED.
- Every `obs_forward_config` / `obs_forward_db_config` case-arm
  preserves the exact arity, types, and argument pass-through to
  `sqlite3_config_real` / `sqlite3_db_config_real`. New SQLite opcodes
  require an explicit case with verified arity; consolidating,
  reordering, or dropping any existing case-arm corrupts the va_list
  dispatch for the affected opcodes. `tests/check_obs_counts.sh` plus
  expected-count files (`tools/expected-sqlite-config-count.txt`,
  `tools/expected-sqlite-dbconfig-count.txt`) are the drift detector.
- Observability constructor priority: 101.
- Auto-extension callback must NEVER make `sqlite3_open*` fail.
- Read-only opens stay quiet for PRAGMA emission; observability is
  independent of read-only state.
- `sqlite3_initialize` log gate:
  `if (outer && rc != SQLITE_OK && !disabled)` -- failure-only.
- Open-wrapper observability annotations of IMPLICIT default flags
  (the wrappers without a caller-supplied flags argument: `sqlite3_open`
  and `sqlite3_open16`) MUST gate the implicit-flags log line on
  `rc == SQLITE_OK` to avoid misleading NOMEM-path logs. Applies to
  both wrappers symmetrically; `sqlite3_open_v2` logs caller-supplied
  flags and is not subject to this gate.
- Word-bounded `bad_signal_re` MUST stay.
- Constructor priorities: 101 = observability cache init (`obs_init` in
  `src/observability.c`); 102 = `SQLITE_CONFIG_LOG` process-wide
  callback registration routed into the observability sink, then
  `SQLITE_CONFIG_SORTERREF_SIZE` config (no `sqlite3_auto_extension` call
  -- registration is lazy per the open-wrapper helper); 103 =
  slow-query tracker `pthread_once` priming + `atexit` registration. The
  101->102->103 order ensures env caches are populated before any
  open-wrapper-triggered callback fires. Do NOT move slow-query
  registration onto the PROFILE hot path.
- `SLOW_QUERY_SQL_CAP=1024` (tracker key + display) and `OBS_SQL_CAP=3072`
  (observability STMT log) are intentionally separate constants. Tracker
  uses its own cap to prevent collision; `tests/check_obs_counts.sh`
  decode-count gate references only `OBS_SQL_CAP`.
- `SLOW_QUERY_MAX_ENTRIES=2048` is the tracker LRU entry cap.
- `auto_extension_pmasz_cfg_rc()` mirrors
  `auto_extension_sorterref_cfg_rc()` in `src/auto_extension.c:31-37`;
  both remain default-visibility C getters consumed by
  `tests/auto_extension_smoke.c:426-459` through `-lsqlite3` linkage and
  both MUST stay in the version-script `global:` allowlist.
- Tracker registration via `sqlite3_trace_v2` MUST stay in the
  per-connection init path (`autopragma_init` in `src/auto_extension.c`)
  that runs BEFORE the connection escapes to application code. Do not move
  to a deferred / lazy / on-first-query path.
- Atexit safety: `g_in_atexit` acquire-load MUST be the FIRST executable
  instruction in the PROFILE-side dispatch (in `obs_trace_cb` PROFILE
  branch AND in `slow_query_trace_profile`), BEFORE any `sqlite3_*`
  accessor (`sqlite3_db_handle`, `sqlite3_sql`) or `*x` deref. Reordering
  risks use-after-teardown on PROFILE callbacks arriving during process
  exit.
- Tracker hot path (`slow_query_record_sql`) MUST use
  `slow_query_is_disabled_cached()` (atomic-load only). The
  `slow_query_disabled()` helper (with `pthread_once`) is reserved for
  non-hot test/registration paths; constructor(103) primes `pthread_once`
  at dlopen so the cached load sees the resolved value.
- Slow-query stats key identity: full-SQL `memcmp` for templates <=1024
  bytes; 64-bit FNV-1a hash equality for templates >1024 bytes (collision
  probability negligible at <=2048 templates). FNV-1a MUST NEVER be used
  as equality for <=1024-byte templates.
- Slow-query LRU eviction tombstones the evicted bucket slot
  (`SLOW_QUERY_NIL = UINT16_MAX`) and increments `g_slow.tombstones`.
  Bucket rebuild triggers only when `tombstones >= entries_used / 4`
  (25%-rehash gate). New inserts do NOT reuse tombstones; tombstones
  cleaned on rebuild. Steady-state eviction is amortized O(1).

## 4. Runtime

- `SQLITE3_DISABLE_OBSERVABILITY=1` is the master kill switch.
- `SQLITE3_DISABLE_AUTOPRAGMA=1` disables PRAGMA emission (orthogonal to
  the observability kill switch).
- `SQLITE3_DISABLE_SLOW_QUERY=1` disables PROFILE-side slow-query
  tracking; subordinate to `SQLITE3_DISABLE_OBSERVABILITY=1` (master) and
  orthogonal to `SQLITE3_DISABLE_AUTOPRAGMA=1` (PRAGMA emission).
- PMS uses `sqlite3_open` (2-arg) exclusively; implicit flags
  `READWRITE|CREATE`.
- Emby uses `sqlite3_open_v2` exclusively with
  `READWRITE|CREATE|NOMUTEX|PRIVATECACHE`.
- Emby's `PRAGMA locking_mode=EXCLUSIVE` in WAL mode does NOT block
  other-process readers. Any SQLite-related claim MUST qualify
  rollback-journal-mode vs WAL-mode semantics.
- Auto-PRAGMA runtime `mmap_size` is 32 GiB (`34359738368`) for supported
  media-server database connections; compile default and runtime target stay
  aligned at 32 GiB.
- Mimalloc replaces the default glibc/musl allocator for all
  SQLite-internal allocations in both library variants; the CLI continues
  using the platform allocator. `MIMALLOC_ALLOW_THP=1` is the v3.3.2
  default and fits the deploy environment's THP=always + defer+madvise
  posture.

## 5. Workflow

- Matrix `fail-fast: false` in
  `.github/workflows/sqlite-build.yml` strategy block.
- Smoke-step positive-mode marker assertions: aggregate count guard
  `sqlite3_open(_v2|16)?` AND `SQLITE_TRACE_STMT`. Do NOT assert
  `sqlite3_initialize` marker emission.
- TOCTOU recheck in `scripts/update-sqlite-library.sh.template` MUST
  stay.
- `tools/regen-deploy-pins.sh` MUST write atomically; never write
  directly to the output path.

## 6. Scope

- User's "build scripts" scope = `build/` directory ONLY. Does NOT
  include `scripts/` (deploy + maintenance) or `tools/` (amalgamation
  patch + CI manifest helpers).
- `AGENTS.md` at project root is a symlink to `CLAUDE.md`. Do not
  modify unless explicitly asked.

## 7. Performance non-regression

- New mechanisms in the observability / optimization layers
  (`src/observability.c`, `src/auto_extension.c`, future tracker /
  profiler modules) MUST NOT regress runtime performance to a
  user-noticeable degree.
- **Threshold**: nanosecond-scale per-query overhead is acceptable;
  microsecond-scale is marginal and must be justified by the gain it
  enables; millisecond-scale overhead is disqualifying.
- Hot-path cycle accounting is part of every design proposal in these
  modules; numbers without ranges or contention-path costs are
  insufficient.
- Defensive guards that add per-query branches must justify their cycle
  cost rather than be added reflexively.
- Concurrency benchmarks under N>=4 threads with KPTI on are gating
  acceptance criteria, not advisory; uncontended single-thread numbers
  do not validate the contended path.
- Kill-switch fast-paths (early-return before any work) preserve this
  invariant under disabled workloads and should be the default for any
  new feature with a runtime kill switch.
