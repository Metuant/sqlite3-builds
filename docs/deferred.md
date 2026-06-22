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

## Clang/LLVM build toolchain via zig cc

Current state: the build compiles with gcc on all variants (generic on a
digest-pinned `ubuntu:18.04` + `ubuntu-toolchain-r` PPA gcc for the glibc 2.27
symbol floor, plex on `baseimage-alpine`). The generic variant must reference
only glibc `<= 2.27` symbols so it loads against Emby 4.9.5's bundled glibc
2.27, and no maintained base ships glibc `<= 2.27` (lowest maintained is
`manylinux_2_28` at glibc 2.28), so `base-generic` pins the EOL `ubuntu:18.04`
image. `zig cc -target x86_64-linux-gnu.2.27` would emit the same floored DSO
from a maintained modern base with no EOL pin, and the user wants to move to
clang/LLVM generally. Deferred because it is a toolchain swap (clang, not gcc):
resuming requires re-validating codegen and the no-perf-regression benches,
adapting the build pipeline (mimalloc CMake `CC`, the observability/slow-query C
wrappers, the `@GLIBC_` floor gate) to zig cc, and confirming the
x86-64-v2/v3 + arm64 matrix.
