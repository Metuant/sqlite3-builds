#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-fts-recurate.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-fts-recurate.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  local message expected actual
  message="$1"
  expected="$2"
  actual="$3"
  printf 'FATAL: %s: expected [%s], actual [%s]\n' "$message" "$expected" "$actual" >&2
  exit 1
}

assert_eq() {
  local expected actual message
  expected="$1"
  actual="$2"
  message="$3"
  [ "$actual" = "$expected" ] || fail "$message" "$expected" "$actual"
}

assert_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) ;;
    *) fail "$message" "contains [$needle]" "$haystack" ;;
  esac
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
sql_text=""
for arg in "$@"; do
  sql_text="${sql_text}${arg}
"
done

if [ "${SKIP_RECURATE_DELETE:-0}" = "1" ]; then
  case "$sql_text" in
    *"DELETE FROM \"fts4_tag_titles_icu\" WHERE docid IN"*)
      exit 0
      ;;
  esac
fi

exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

declare -F run_fts_recurate >/dev/null || fail "run_fts_recurate availability" "defined shell function" "missing"

create_recurate_fixture() {
  local db
  db="$1"
  "$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE tags(id INTEGER PRIMARY KEY, tag TEXT);
INSERT INTO tags(id, tag) VALUES
  (1, 'Music'),
  (2, 'Movies'),
  (3, 'imdb://tt0133093'),
  (4, 'tmdb://603'),
  (5, 'tvdb://121361'),
  (6, ''),
  (7, '   '),
  (8, NULL);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag, content='tags');
INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('rebuild');
EOF_SQL
}

bad_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag GLOB '*://*' OR t.tag IS NULL OR trim(t.tag)='';"
}

name_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize d JOIN tags t ON t.id=d.docid WHERE t.tag IN ('Music','Movies');"
}

total_doc_count() {
  local db
  db="$1"
  "$real_sqlite" "$db" "SELECT count(*) FROM fts4_tag_titles_icu_docsize;"
}

positive="$tmp/positive.db"
create_recurate_fixture "$positive"
assert_eq "8" "$(total_doc_count "$positive")" "pre-recurate indexed doc count"
run_fts_recurate sqlite3 "$positive" >"$tmp/positive.out" 2>"$tmp/positive.err"
assert_eq "" "$(cat "$tmp/positive.err")" "positive re-curation stderr"
assert_eq "0" "$(bad_doc_count "$positive")" "URI and empty tag docs purged"
assert_eq "2" "$(name_doc_count "$positive")" "name tag docs retained"
assert_eq "2" "$(total_doc_count "$positive")" "post-recurate indexed doc count"

run_fts_recurate sqlite3 "$positive" >"$tmp/idempotent.out" 2>"$tmp/idempotent.err"
assert_eq "" "$(cat "$tmp/idempotent.err")" "idempotent re-curation stderr"
assert_eq "0" "$(bad_doc_count "$positive")" "idempotent bad doc count"
assert_eq "2" "$(name_doc_count "$positive")" "idempotent name doc count"
assert_eq "2" "$(total_doc_count "$positive")" "idempotent total doc count"

no_target="$tmp/no-target.db"
"$real_sqlite" "$no_target" "CREATE TABLE tags(id INTEGER PRIMARY KEY, tag TEXT); INSERT INTO tags(id, tag) VALUES(1, 'Music');"
run_fts_recurate sqlite3 "$no_target" >"$tmp/no-target.out" 2>"$tmp/no-target.err"
assert_eq "" "$(cat "$tmp/no-target.err")" "no-target re-curation stderr"
assert_eq "1" "$("$real_sqlite" "$no_target" "SELECT count(*) FROM tags;")" "no-target content table retained"

negative="$tmp/negative.db"
create_recurate_fixture "$negative"
run_fts_recurate sqlite3 "$negative" >"$tmp/negative-initial.out" 2>"$tmp/negative-initial.err"
assert_eq "0" "$(bad_doc_count "$negative")" "negative setup starts clean"
"$real_sqlite" "$negative" "INSERT INTO fts4_tag_titles_icu(docid, tag) VALUES(3, 'imdb://tt0133093');"
assert_eq "1" "$(bad_doc_count "$negative")" "forced URI doc makes postcondition nonzero"

export SKIP_RECURATE_DELETE=1
set +e
run_fts_recurate sqlite3 "$negative" >"$tmp/negative.out" 2>"$tmp/negative.err"
rc=$?
set -e
unset SKIP_RECURATE_DELETE
assert_eq 1 "$rc" "run_fts_recurate rc when postcondition remains nonzero"
assert_contains "$(cat "$tmp/negative.err")" "ERROR: FTS re-curation postcondition failed" "re-curation postcondition diagnostic"
assert_eq "1" "$(bad_doc_count "$negative")" "negative fixture still contains forced bad doc"

printf 'optimize_media_servers FTS re-curation tests passed\n'
