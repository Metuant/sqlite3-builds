# Observability

Part of the [Repository Architecture index](../architecture.md).

## Observability Layer

`src/observability.c` is compiled into both shared-library variants and is
excluded from the CLI target.

The library target patches SQLite's amalgamation so these public APIs are
implemented by wrappers in `src/observability.c` while the original SQLite
implementations remain available as hidden `*_real` symbols:

- `sqlite3_initialize`
- `sqlite3_config`
- `sqlite3_db_config`
- `sqlite3_open`
- `sqlite3_open_v2`
- `sqlite3_open16`
- `sqlite3_prepare`
- `sqlite3_prepare_v2`
- `sqlite3_prepare_v3`

The initialize/config/db-config/open wrappers call the hidden real
implementation, return the real return code unchanged, and log after the call
with numeric `rc=N` where applicable. They do not mutate config opcodes,
db-config opcodes, open flags, filenames, handles, or return codes. The public
UTF-8 prepare wrappers chain through `src/observability.c` ->
`emby_fts_rewrite_prepare()` -> `plex_fts_rewrite_prepare()` -> the matching
hidden `*_real` prepare implementation. The helpers are result-preserving for
disabled, non-target, nonmatching, drift, and failure paths, and intentionally
prepare rewritten SQL only for enabled target matches. The Plex helper emits
`event=rewrite_applied target=plex db=%p sql="<escaped rewritten>"` through
`obs_logf` only on effective rewrite success. The Emby helper emits
`event=rewrite_applied target=emby mode=<mode> db=%p sql="<escaped rewritten>"`
only on effective rewrite success and emits `rewrite_skipped` for rewrite-path
failures after a target-shape match. Emby modes are `fts+membership`,
`fanout+resume`, `fanout+browse`, `fanout+favorites`, `fanout+people`,
`fanout+links_search`, `fanout+resume_simple`, `fanout+similar`,
`dashboard+episodes_latest`, and `dashboard+movies_latest`. The Episodes mode
name is a shipped telemetry rename with no compatibility alias. Each dashboard
family logs `capture_miss` only after its own Type discriminator; valid sibling
traffic is an unlogged clean miss. Capture-gated fan-out and dashboard misses
log `reason=capture_miss`; missing required Emby dashboard or Plex
taggings/On-Deck indexes log `reason=index_missing`.
`SQLITE3_DISABLE_OBSERVABILITY` gates helper logs independently of
`SQLITE3_DISABLE_STMT_TRACE`; pure passthrough and clean-miss paths do not log.

The `sqlite3_open`, `sqlite3_open_v2`, and `sqlite3_open16` wrappers call
`auto_extension_register_for_open()` after observability initialization and
immediately before their hidden `_real` open implementation. The helper is
per-open and may trigger effective SQLite initialization before `openDatabase`
enters its own initialization path.

The observability kill switch is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1
```

Only literal `1` disables observability. The value is cached once during the
priority-101 observability constructor, with per-wrapper `pthread_once`
fallback. Disabled mode skips wrapper log emission and skips per-connection
`sqlite3_trace_v2` registration.

The statement-trace env knob is:

```text
SQLITE3_DISABLE_STMT_TRACE=0
```

Statement trace logging is off by default. Only literal `0` enables it; unset,
empty, `1`, and every other value disable it. The value is cached by the same
observability initialization path as the master kill switch. Disabled/default
mode omits `SQLITE_TRACE_STMT` from the per-connection trace mask; PROFILE-side
slow-query tracking is unaffected. When the resulting trace mask is `0`,
`sqlite3_trace_v2` is not called.

Log lines are written to stderr:

```text
[sqlite3-builds-obs] <ISO-8601-UTC-ms> <pid> <kernel-tid> <fn> key=value ...
```

Kernel thread IDs come from `syscall(SYS_gettid)`. SQL text emitted by statement
trace logging is capped at 4 KiB and receives a `...[TRUNC]` tail when
truncated.

`sqlite3_initialize` uses a thread-local depth counter so nested initialization
still calls the real implementation but only the outer 0-to-1 transition logs
failures where `rc != SQLITE_OK`. This absorbs helper-triggered initialization
followed by `openDatabase`'s initialization check without false failure logs.

`autopragma_init()` registers a unified trace callback before the AUTOPRAGMA
gate, with STMT added only when `SQLITE3_DISABLE_STMT_TRACE=0` and PROFILE
conditionally added when slow-query tracking is enabled. Observability is
orthogonal to PRAGMA injection: target filtering, read-only skips, and the
autopragma kill switch do not suppress observability logs. Trace registration
failure logs and continues; it must never make `sqlite3_open*` fail.

### Slow-Query Tracker

The same per-connection trace registration uses one unified callback with
`SQLITE_TRACE_STMT` added only when statement tracing is explicitly enabled and
`SQLITE_TRACE_PROFILE` added only when slow-query tracking is enabled. SQLite
allows one trace callback per connection, so the tracker shares the existing
registration site that runs before the connection escapes to application code.

The kill-switch hierarchy is:

```text
SQLITE3_DISABLE_OBSERVABILITY=1 -> no observability or tracker work
SQLITE3_DISABLE_STMT_TRACE unset or !=0 -> STMT observability off, PROFILE tracker unaffected
SQLITE3_DISABLE_STMT_TRACE=0 -> STMT observability on when observability is enabled
SQLITE3_DISABLE_SLOW_QUERY=1 -> PROFILE tracker off; STMT follows SQLITE3_DISABLE_STMT_TRACE
```

The helper unconditionally calls `sqlite3_auto_extension` regardless of the
`SQLITE3_DISABLE_AUTOPRAGMA=1` state. An alternative -- early-return from the
helper when `SQLITE3_DISABLE_AUTOPRAGMA=1` -- was considered and rejected: it
would have skipped `sqlite3_trace_v2(SQLITE_TRACE_STMT)` registration in
`autopragma_init`, conflating AUTOPRAGMA disable with observability disable.
The accepted per-open mutex + dedup-scan cost preserves orthogonality between
the two kill switches.

`SQLITE3_SLOW_QUERY_THRESHOLD_MS` defaults to `500`. The parser accepts only
unsigned decimal milliseconds, allows `0` for debug-only all-sample logging,
and falls back to `500` for unset, empty, signed, non-digit, trailing-junk,
ERANGE, or pre-conversion overflow values. PROFILE elapsed values come from
SQLite's millisecond-quantized timing source and are scaled to nanoseconds by
SQLite before the callback receives them.

The tracker keeps a process-global 2048-entry LRU keyed by parameterized
`sqlite3_sql()` text. It stores and displays at most 1024 SQL bytes per
template, uses the full SQL FNV-1a hash as the probe accelerator, and uses that
full hash as the equality proof for templates longer than 1024 bytes. Existing
STMT logging keeps its separate 4 KiB SQL cap; `SLOW_QUERY_SQL_CAP` does not
change `OBS_SQL_CAP`, and config/db-config decode tables stay in
`src/observability.c`.

LRU evictions tombstone the bucket entry and trigger an amortized O(1) bucket
rebuild when tombstones reach 25% of entries_used.

Stats are retained as count, sum, sum-of-squares, min, and max. Supported
compilers use `unsigned __int128` accumulators; fallback compilers use
`uint64_t` with a per-template sample cap and one warning per capped template.
The dump path heap-allocates value snapshots, copies under the tracker mutex,
sorts by descending total elapsed time, and emits `slow_query_stats` lines
after releasing the mutex only when `mean_ns >= threshold_ns` and `count >= 5`.
Stats fields `mean_ms`, `stddev_ms`, `min_ms`, and `max_ms` render with six
digits after the decimal point.

Dumps occur at normal process exit and at most every five minutes during
PROFILE activity. The atexit path sets an atomic in-exit flag first, so later
PROFILE callbacks return before touching SQLite pointers or the mutex, and it
uses `pthread_mutex_trylock`; on contention it emits a one-line skip diagnostic
instead of stalling process shutdown.

The tracker is an observability satellite that depends on the hidden
`obs_logf` / `obs_is_disabled` ABI in this MVP. A Phase 2 sink-abstraction
layer is deferred until runtime evidence justifies persistent or alternate
export paths.

### Slow-Query Verification

Docker build compiles + runs `slow_query_smoke`, `slow_query_atexit_smoke`,
`slow_query_concurrency_smoke`. CI re-runs positive, disabled-mode, atexit,
and concurrency checks, and it compiles + runs `stmt_trace_smoke` against the
same registration path to enforce the STMT trace env contract. Bench binaries
(`slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`,
`alloc_latency_bench`, `runtime_optimize_close_bench`,
`plex_fts_rewrite_prepare_bench`, and `emby_fts_rewrite_prepare_bench`) compile
unconditionally. The Dockerfile runs `slow_query_select1_bench`,
`slow_query_metadata_bench`, `config_after_dlopen_concurrent_bench`, and
`alloc_latency_bench` as advisory steps on every library build: nonzero exits
print an `ADVISORY FAIL` line and continue. The generic library image also runs
`runtime_optimize_close_bench` as advisory output; it prints p50, p95, p99, max,
and pass/fail verdicts, then exits 0 so benchmark drift never fails CI. The Plex
and Emby FTS prepare benches follow the same advisory posture: their timing
verdicts report prepare-cost drift but do not fail CI.
