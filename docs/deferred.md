This file tracks deferred workstreams that the repository has explicitly
parked, with enough context to resume them without re-discovery.
Add entries when work is deferred, and remove them when the deferred item is
complete.

## Multi-group-per-mod CI build

Current state: render, stage, validate, and manifest paths are group-aware, but
the workflow builds only the first compatibility group per mod through
`first_compat_group_for_mod` and an arch-only library build matrix. A second
production group for the same mod is not built by CI until the build matrix
becomes arch-by-group. This is a no-op for the current one-group-per-mod
support window.

## Pool-review second-reviewer enforcement

Current state: `.github/CODEOWNERS` is single-owner. The only enforced
pool-review control is the >=2 distinct reviewer count in
`tests/check_multi_version_pin_alignment.sh`, and one person can author both
review rows. A CODEOWNERS or branch-protection rule on
`pins/plex-pool-patch-*.tsv` would realize the two-independent-humans intent.
