# Emby Search Observation

## Purpose And Scope

Use this runbook to capture Emby's real bound `@SearchTerm` for quoted and
unquoted Items-API searches through the deployed SQLite replacement library.
The output is the observed input for Phase-2 search rewrite design.

This runbook observes live Emby behavior. It does not modify the database,
Emby query construction, the deployed `libsqlite3.so`, or any Plex-renamed ICU
runtime files. For query timing and copied-database A/B methodology, use
`docs/runbooks/slow-query-index-testing.md`.

## Prerequisites

- The Emby LSIO mod is deployed with a `libsqlite3.so` that includes the
  expanded-SQL slow-query logging change.
- The target Emby container opens its main database as `/library.db`.
- Container logs are accessible from the Docker, journald, or CI collector that
  receives stderr from the Emby process.
- Operators understand expanded SQL inlines bound values and redact lines
  before sharing them outside the incident context.

## Default Expanded SQL Observation

Expanded-SQL slow-query detail is on by default. With no expanded-SQL env flag,
statements at or above the default `2500` ms detail threshold emit
`slow_query_expanded` lines. That catches the unquoted searches observed near
10 s.

To also catch quoted searches that run around 1 s, lower only the expanded
detail threshold and restart the container:

| Env var | Value | Effect |
|---|---|---|
| `SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS` | `500` to `1000` | Captures quoted searches that run around 1 s. Leave unset to keep the default `2500`, which usually captures only unquoted searches near 10 s. |
| `SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL` | unset or any value other than `1` | Keeps expanded-SQL detail logging enabled. Literal `1` disables expanded detail. |
| `SQLITE3_DISABLE_OBSERVABILITY` | unset or any value other than `1` | Keeps observability enabled. Literal `1` disables all observability and prevents this capture. |
| `SQLITE3_DISABLE_SLOW_QUERY` | unset or any value other than `1` | Keeps slow-query tracking enabled. Literal `1` disables the base and expanded slow-query lines. |

Keep `SQLITE3_SLOW_QUERY_THRESHOLD_MS` unset, or set it no higher than the
expanded detail threshold used for the observation. The expanded threshold is
clamped up to the effective base slow-query threshold, so a higher base
threshold can hide the quoted-search detail line.

## Reproduce The Searches

Run the same search term through both Emby paths:

1. Quoted search, for example `"naked gun"` or the operator-selected title.
2. Unquoted search, for example `naked gun` or the same title without quotes.

Use the Emby UI or API path that reproduces the Items-API search. Repeat only
as much as needed to get one `slow_query_expanded` line for each quoted and
unquoted case.

## Read The Log Line

Find the `slow_query_expanded` log line emitted by the SQLite observability
layer:

```text
[sqlite3-builds-obs] ... slow_query_expanded ... sql_expanded_source=... sql_expanded_len=... sql_expanded_hash=... sql_expanded_truncated=... sql_expanded="..."
```

Record these fields:

| Field | Meaning |
|---|---|
| `sql_expanded` | Full expanded SQL for the slow statement, including the bound `@SearchTerm` value. |
| `sql_expanded_source` | `expanded` when `sqlite3_expanded_sql()` supplied expanded SQL, `template` when the line fell back to the statement template, or `alloc_failed` when allocation failed and no sensitive partial value was emitted. |
| `sql_expanded_len` | Length of the capped expanded-SQL bytes used for detail logging. |
| `sql_expanded_hash` | Hash of the capped expanded-SQL bytes. Use it to correlate repeats without resharing the full value. |
| `sql_expanded_truncated` | `1` when the expanded SQL hit the detail cap and carries a `...[TRUNC]` suffix; otherwise `0`. |

`sql_expanded_source=template` does not close the Phase-2 observation gap
because the bound value is not present. Reproduce again or inspect the logging
failure before designing a rewrite from that line.

## Capture The Bound Expressions

For each search case, record the full `sql_expanded` value in incident notes and
extract the observed `fts_search9 MATCH` expression bound to `@SearchTerm`.
These are the Phase-2 inputs:

| Case | Emby input | `sql_expanded_source` | Observed bound `@SearchTerm` | `sql_expanded_hash` |
|---|---|---|---|---|
| quoted |  |  |  |  |
| unquoted |  |  |  |  |

Design Phase 2 against these observed expressions only. Do not use inferred
prefix-OR, phrase, or raw-phrase reproductions as the source of truth when the
expanded SQL is available.

## PII And Log Scope

Expanded SQL is max-PII for this repository's observability surface. It inlines
all bound values for the statement, not only `@SearchTerm`.

Expanded logging is on by default at `2500` ms because this fleet uses
host-local Docker `json-file` logging bounded to about 100 MB per container
(`max-file=10`, `max-size=10m`) and does not forward these logs off-host.
Disable expanded detail with `SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=1` if
logs are shipped off-host.

For a lowered-threshold observation window:

1. Set `SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS` to the temporary value.
2. Restart Emby.
3. Reproduce the quoted and unquoted searches.
4. Capture the minimum required log lines.
5. Remove the temporary threshold or set `SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=1`.
6. Restart Emby again.

Assume Docker logs, journald, and any configured CI or log-forwarding collectors
receive the expanded values. Review and redact the lines before sharing them
outside the incident context.

## Cleanup

Unset the temporary threshold and restart the Emby container:

```text
SQLITE3_SLOW_QUERY_EXPANDED_SQL_THRESHOLD_MS
```

To turn expanded detail off, set this env var and restart:

```text
SQLITE3_DISABLE_SLOW_QUERY_EXPANDED_SQL=1
```

Leave the normal observability and slow-query settings in their previous state.
After disabling expanded detail, confirm new searches no longer emit
`slow_query_expanded` lines.
