#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
# Override for nonstandard include/link paths, without installing anything:
# SQLITE3_CFLAGS='-I/path/to/include' SQLITE3_LIBS='-l:libsqlite3.so.0'
read -r -a includes <<< "${SQLITE3_CFLAGS:-}"
read -r -a libraries <<< "${SQLITE3_LIBS:--lsqlite3}"
cc=${CC:-cc}
# These three warning classes exist in the unmodified parent wrapper.
"$cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -Wno-sign-compare -Wno-unused-const-variable -O1 -fPIC -shared \
  -DSQLITE_ENABLE_ICU "${includes[@]}" -I"$root/src" \
  "$root/src/plex_fts_rewrite.c" "$root/src/fts_lex.c" \
  "$root/tests/plex_fts_join_test_shim.c" -Wl,-z,defs \
  "${libraries[@]}" -pthread -o "$work/join-test.so"
python3 "$root/tests/plex_fts_join_test.py" "$work/join-test.so"
"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/src" \
  "$root/tests/plex_fts_join_fuzz.c" "$root/src/fts_lex.c" \
  -o "$work/fuzz"
ASAN_OPTIONS=detect_leaks=1 "$work/fuzz"
