#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build"
read -r -a includes <<< "${SQLITE3_CFLAGS:-}"
read -r -a libraries <<< "${SQLITE3_LIBS:--lsqlite3}"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-const-variable -O1 -fPIC -shared -DSQLITE_ENABLE_ICU "${includes[@]}" -I"$root/src" "$root/src/plex_fts_rewrite.c" "$root/src/fts_lex.c" "$root/tests/plex_fts_join_test_shim.c" "${libraries[@]}" -lpthread -o "$root/build/collection-test.so"
python3 "$root/tests/plex_collection_tag_test.py" "$root/build/collection-test.so"
"$cc" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/src" "$root/tests/plex_collection_tag_fuzz.c" "$root/src/fts_lex.c" -o "$root/build/collection-fuzz"
ASAN_OPTIONS=detect_leaks=1 "$root/build/collection-fuzz"
