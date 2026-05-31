#!/bin/sh
# shellcheck disable=SC1007,SC2016
set -eu

# Strategy: invoke a temporary copy of Build.sh with a minimal local
# sqlite-amalgamation zip so execution reaches the library precheck block and
# exits before patching or compiling. The temp copy only redirects the
# hardcoded mimalloc sentinel read into a tmpdir and, for case 1, drops the
# MIMALLOC_SHA512 default so the existing precheck string is reachable.

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
good_sha="$(sed -n 's/^MIMALLOC_SHA512="\${MIMALLOC_SHA512:-\([a-f0-9]\{128\}\)}"$/\1/p' "$repo_root/build/Build.sh")"
if [ -z "$good_sha" ] || [ "${#good_sha}" -ne 128 ]; then
  echo "FATAL: could not extract MIMALLOC_SHA512 default from build/Build.sh" >&2
  exit 1
fi

make_archive() {
  archive_dir="$1/sqlite-amalgamation-test"
  archive_path="$1/sqlite-amalgamation-test.zip"

  mkdir -p "$archive_dir"
  : > "$archive_dir/sqlite3.c"
  python3 - "$archive_path" "$archive_dir" <<'PY'
import pathlib
import sys
import zipfile

archive = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(archive, "w") as zf:
    for path in root.rglob("*"):
        zf.write(path, path.relative_to(root.parent).as_posix())
PY
}

make_build_sh_copy() {
  mode="$1"
  dst="$2"

  python3 - "$repo_root/build/Build.sh" "$dst" "$mode" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1]).read_text()
src = src.replace(
    'mimalloc_link_inputs=""\n',
    'mimalloc_link_inputs=""\nMIMALLOC_SENTINEL="${MIMALLOC_SENTINEL:-/opt/mimalloc/SHA512}"\n',
    1,
)
src = src.replace(
    'if [ ! -f /opt/mimalloc/SHA512 ]; then',
    'if [ ! -f "${MIMALLOC_SENTINEL}" ]; then',
    1,
)
src = src.replace(
    'if [ "$(cat /opt/mimalloc/SHA512)" != "${MIMALLOC_SHA512}" ]; then',
    'if [ "$(cat "${MIMALLOC_SENTINEL}")" != "${MIMALLOC_SHA512}" ]; then',
    1,
)
if sys.argv[3] == "missing-sha":
    src = re.sub(
        r'MIMALLOC_SHA512="\$\{MIMALLOC_SHA512:-[^"]+\}"',
        'MIMALLOC_SHA512="${MIMALLOC_SHA512:-}"',
        src,
        count=1,
    )
pathlib.Path(sys.argv[2]).write_text(src)
PY
  chmod 755 "$dst"
}

assert_expected() {
  stderr="$1"
  expected="$2"

  if ! grep -Fq "$expected" "$stderr"; then
    echo "FAIL: expected fatal line not found" >&2
    printf "expected: %s\n" "$expected" >&2
    printf "observed:\n" >&2
    cat "$stderr" >&2
    exit 1
  fi
}

(
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
  make_archive "$tmpdir"
  make_build_sh_copy missing-sha "$tmpdir/Build.sh"
  cd "$tmpdir"
  stderr="$tmpdir/stderr"
  if env -i PATH="$PATH" \
    MIMALLOC_VERSION=3.3.2 \
    MIMALLOC_URL=https://github.com/microsoft/mimalloc/archive/refs/tags/v3.3.2.tar.gz \
    SQLITE_AMALG_SHA3_256=dummy \
    /bin/sh "$tmpdir/Build.sh" --target=library sqlite-amalgamation-test.zip \
    2>"$stderr"; then
    echo "FAIL: missing-sha case unexpectedly succeeded" >&2
    exit 1
  fi
  assert_expected \
    "$stderr" \
    "FATAL: MIMALLOC_VERSION, MIMALLOC_URL, and MIMALLOC_SHA512 are required for library builds"
)

(
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
  make_archive "$tmpdir"
  make_build_sh_copy current "$tmpdir/Build.sh"
  : > "$tmpdir/mimalloc.o"
  : > "$tmpdir/libmimalloc.a"
  printf "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\n" > "$tmpdir/SHA512"
  cd "$tmpdir"
  stderr="$tmpdir/stderr"
  if env -i PATH="$PATH" \
    MIMALLOC_SHA512="$good_sha" \
    MIMALLOC_SENTINEL="$tmpdir/SHA512" \
    MIMALLOC_OBJ="$tmpdir/mimalloc.o" \
    MIMALLOC_LIB="$tmpdir/libmimalloc.a" \
    SQLITE_AMALG_SHA3_256=dummy \
    /bin/sh "$tmpdir/Build.sh" --target=library sqlite-amalgamation-test.zip \
    2>"$stderr"; then
    echo "FAIL: sentinel-drift case unexpectedly succeeded" >&2
    exit 1
  fi
  assert_expected \
    "$stderr" \
    "FATAL: mimalloc SHA512 sentinel drift at /opt/mimalloc/SHA512; ensure the Dockerfile mimalloc stage used the current MIMALLOC_SHA512"
)

(
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
  make_archive "$tmpdir"
  make_build_sh_copy current "$tmpdir/Build.sh"
  : > "$tmpdir/libmimalloc.a"
  printf "%s\n" "$good_sha" > "$tmpdir/SHA512"
  missing_obj="$tmpdir/missing-mimalloc.o"
  cd "$tmpdir"
  stderr="$tmpdir/stderr"
  if env -i PATH="$PATH" \
    MIMALLOC_VERSION=3.3.2 \
    MIMALLOC_URL=https://github.com/microsoft/mimalloc/archive/refs/tags/v3.3.2.tar.gz \
    MIMALLOC_SHA512="$good_sha" \
    MIMALLOC_SENTINEL="$tmpdir/SHA512" \
    MIMALLOC_OBJ="$missing_obj" \
    MIMALLOC_LIB="$tmpdir/libmimalloc.a" \
    SQLITE_AMALG_SHA3_256=dummy \
    /bin/sh "$tmpdir/Build.sh" --target=library sqlite-amalgamation-test.zip \
    2>"$stderr"; then
    echo "FAIL: missing-object case unexpectedly succeeded" >&2
    exit 1
  fi
  assert_expected \
    "$stderr" \
    "FATAL: mimalloc object not found at $missing_obj; ensure the Dockerfile mimalloc stage ran"
)

printf "PASS check_build_sh_prechecks\n"
