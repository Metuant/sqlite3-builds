# sqlite3-builds invariants (evergreen)

Repo-level architectural and behavioral rules that survive across milestones.
Reference by path from charters; do not paste this content into prompts.

Milestone-specific rules (locked decisions for in-flight workstreams, current
bug states, triage decisions) live in the milestone-scoped sibling files (e.g.,
`docs/invariants/sqlite-maintenance.md`) and are cleaned up after their
milestone lands.

## 1. Build / variant

- Plex variant builds under **Alpine/musl**; generic variant stays
  Ubuntu/glibc. Multi-stage `docker-library/Dockerfile` selects via
  `LIBRARY_VARIANT`.
- `LIBRARY_VARIANT=plex` is limited to the Plex ICU build path.
- SQLite pins must stay aligned across `build/Build.sh`,
  `build/build_static_sqlite.sh`, `.github/workflows/sqlite-build.yml`,
  `docker-cli/Dockerfile`, and `docker-library/Dockerfile`.
- ICU pin alignment is limited to `docker-library/Dockerfile`
  `ARG ICU_VERSION` plus workflow `ICU_VERSION` and is enforced by
  `tests/check_pin_alignment.sh`.
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
  v4-capable amd64 hosts use the v3 artifact.
- `SQLITE_TEMP_STORE=3` (memory) is a compile-time pin at
  `build/Build.sh:207`. `PRAGMA temp_store=2` is documentation-only
  under that profile (no-op).
- `SQLITE_DEFAULT_PAGE_SIZE=16384` (16 KiB) is the compile-default
  page-size pin in `build/Build.sh`.
- `SQLITE_DEFAULT_MMAP_SIZE=34359738368` (32 GiB) is the compile-default
  mmap-size pin in `build/Build.sh`; CI compile-option assertions keep it
  aligned with the runtime auto-PRAGMA target.
- `SQLITE_MAX_MMAP_SIZE=1099511627776` (1 TiB) is the compile-time
  mmap ceiling in `build/Build.sh:197`; the Emby compile-options
  required-flag array in `.github/workflows/sqlite-build.yml:582-601`
  asserts `MAX_MMAP_SIZE=1099511627776` in CI. The runtime
  `PRAGMA mmap_size=34359738368` (32 GiB) only takes effect because this
  compile ceiling stays above 32 GiB; reducing it below 32 GiB silently
  caps the PRAGMA and breaks the Bundle-1 observability + performance
  objective. Changes to the build pin or the workflow assertion must
  update both.
- `SQLITE_SORTER_PMASZ=8192` is the compile default in
  `build/Build.sh:205`; constructor-102 re-asserts
  `sqlite3_config(SQLITE_CONFIG_PMASZ, 8192)` in
  `src/auto_extension.c:212-249`. Emby's compile-options required-flag
  array in `.github/workflows/sqlite-build.yml:476-496` keeps the value
  pinned in CI.

## 2. LSIO mods

- Plex library replacement target: exactly
  `/usr/lib/plexmediaserver/lib/libsqlite3.so`.
- Emby library replacement target: exactly
  `/app/emby/lib/libsqlite3.so.3.49.2`.
- JF deployment is unsupported until a current design and validation plan land.
- Plex ICU runtime files MUST NOT be replaced, renamed, moved, deleted, or
  overwritten. Mod code may only read and verify `libicu*plex.so.69`.
- baked-pins.txt is the only runtime SHA source consumed by LSIO mod scripts.
- `baked-pins.txt` includes current SQLite rows, per-arch pre rows, Plex ICU
  runtime pre rows, and Plex PMS / Scanner baseline rows.
- `baked-pins.txt` row kinds:
  - `version|2|release_tag|<tag>|generated_at|<iso8601-utc>` is the single
    metadata row.
  - `current|1|<target>|<arch>|<artifact_name>|<target_path>|<sha256>` gives
    each baked `libsqlite3.so` candidate and its runtime replacement path.
  - `pre|1|<target>|<arch>|<source_image>|<image_digest>|<target_path>|runtime|<sha256>`
    gives bundled runtime baselines, including Plex ICU siblings.
  - `pool-pre|1|plex|<arch>|<binary_path>|<sha256>` gives PMS and Plex Media
    Scanner binary baselines from `pins/plex-pool-patch-baselines.txt`.
  - `unsupported|<arch>|<reason>` keeps all-arch manifests complete when a
    supported artifact is unavailable.
- Runtime mod scripts read no custom env vars. `MOD_INFO` is log-only and
  `uname -m` is the architecture input.
- Phase scripts run as native s6-rc oneshots under
  `/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-*/run` and use
  `#!/usr/bin/with-contenv bash`.
- SQLite mod oneshots are chained after `init-mods` and before
  `init-mods-end`: preflight -> verify -> swap -> config (Emby) or
  poolpatch (Plex). `init-mods-end` depends on the final SQLite oneshot, so
  the chain completes before `init-services` and `svc-*` startup.
- LSIO runtime command surface:
  | Scope | Commands |
  |---|---|
  | Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |
  | Plex amd64 pool patch only | `dd od printf` |
- LSIO runtime has no dependency on `curl`, `tar`, `gunzip`, Python, `jq`,
  package managers, or network access.
- Phase posture: malformed or missing `baked-pins.txt`, baked artifact
  mismatch, and selected baked artifact absence are fatal. Unsupported arch,
  missing target path, runtime Plex ICU mismatch, unknown current target SHA,
  backup failure, temp-copy failure, and pool-patch drift warn and exit 0.
- Phase 03 swap skips an unrecognized current target even when a
  `.bundled.bak` exists; it restores a verified backup only after a failed
  install attempt.
- Pool patch is args-only: callers pass every path, SHA, and site tuple.
  The staged `plex-pool-patch.sh` fragment reads no environment variables and
  carries no SHA constants.
- Pool-patch site tuples are
  `label|offset|write_seek|original_hex|patched_hex`. The writer derives one
  byte from `patched_hex` at `write_seek - offset`, applies it to a same-fs
  temp copy with `dd conv=notrunc`, restores target owner/mode metadata, and
  atomically replaces the target; original and patched contexts remain
  per-binary and per-site coupled.
- Pool patch skips a binary when any site is mixed or unknown. It patches only
  when all sites are original and the binary SHA matches the `pool-pre` row;
  it skips when all sites are already patched.
- Plex arm64 swaps SQLite but does not patch PMS / Scanner pool size.

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
- `release` publishes `sqlite-<tag>-*.tar.gz` and `SHA256SUMS`.
- `mod-build` builds and smokes run-scoped temporary tags only.
- `mod-publish` is tag-gated, depends on `release` and `mod-build`, and is
  the only job that pushes stable mod tags or multi-arch manifests.
- Native arm64 mod lanes use `ubuntu-24.04-arm`; no native arm64 lane uses
  `--platform`.

## 6. Scope

- User's "build scripts" scope = `build/` directory ONLY. Does NOT
  include `scripts/` (maintenance helper only) or `tools/` (amalgamation
  patch + version script + expected-count helpers + LSIO mod tooling).
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

## 8. Versioning

- Release identity uses CalVer tags matching
  `^[0-9]{4}\.[0-9]{2}\.[0-9]{2}-r[1-9][0-9]*$`; no `v` prefix; always
  include `-rN`.
- LSIO mod tags mirror repository CalVer release tags one-to-one.
- No LSIO mod publishes `latest`.
- `baked-pins.txt` metadata row shape is
  `version|2|release_tag|<tag>|generated_at|<iso8601-utc>`.
- `baked-pins.txt` contains no managed-window fields and no `post` rows.
- Plex runtime ICU `pre` rows live in `baked-pins.txt`.
- `tests/check_pin_alignment.sh` enforces mimalloc tuple alignment and ICU
  workflow env vs Dockerfile ARG/URL-construction alignment.
