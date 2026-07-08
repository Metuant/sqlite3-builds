# Emby Search-Measure Status

This harness is a host-side measurement runbook for Emby FTS fan-out SQL rewrites.

## Current Scope

- `identity.sh` renders baseline and candidate SQL, compares grouped identity output, and hard-fails on mismatches.
- `timing.sh` runs EQP capture, warm-up passes, counterbalanced timed passes, and median reporting.
- `sql/base-type.sql`, `sql/base-presentation.sql`, and `sql/candidate-fragments.sql` are committed templates. Runtime values come from `env`.
- `env.example` documents the required local values. `env` contains real host-local values and is ignored.

## Operating Rule

Run identity before timing. Treat any identity failure as blocking. Treat non-adoption notes and timing regressions as investigation input, not as automatic rewrite acceptance.

STATUS: ready for local host measurement
