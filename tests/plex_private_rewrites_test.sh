#!/usr/bin/env bash
# Documented local/CI entrypoint: all four Python suites and sanitizer fuzzers.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
read -r -a includes <<< "${SQLITE3_CFLAGS:-}"
read -r -a libraries <<< "${SQLITE3_LIBS:--lsqlite3}"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -Wno-sign-compare -Wno-unused-const-variable -O1 -fPIC -shared \
  -DSQLITE_ENABLE_ICU "${includes[@]}" -I"$root/src" \
  "$root/src/plex_fts_rewrite.c" "$root/src/fts_lex.c" \
  "$root/tests/plex_fts_join_test_shim.c" -Wl,-z,defs \
  "${libraries[@]}" -pthread -o "$work/private-test.so"
for name in plex_fts_join plex_title_scope plex_collection_tag plex_recent_episode; do
  python3 "$root/tests/${name}_test.py" "$work/private-test.so"
  "$cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/src" \
    "$root/tests/${name}_fuzz.c" "$root/src/fts_lex.c" -o "$work/fuzz"
  ASAN_OPTIONS=detect_leaks=1 "$work/fuzz"
done
python3 "$root/tests/plex_review_regression_test.py" "$work/private-test.so"
python3 "$root/tests/plex_hub_type_or_test.py" "$work/private-test.so"
echo 'PASS: four private Python suites, four ASan/UBSan fuzzers, review regressions, hub bounds/binds/faults'
