#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-fts-discovery.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-fts-discovery.XXXXXX)"
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

assert_not_contains() {
  local haystack needle message
  haystack="$1"
  needle="$2"
  message="$3"
  case "$haystack" in
    *"$needle"*) fail "$message" "not contains [$needle]" "$haystack" ;;
  esac
}

run_validate_metadata_failure() {
  local name table_name module content_mode diagnostic rc
  name="$1"
  table_name="$2"
  module="$3"
  content_mode="$4"
  diagnostic="$5"

  set +e
  validate_fts_metadata "$table_name" "$module" "$content_mode" >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$name validate_fts_metadata rc" "nonzero" "$rc"
  assert_contains "$(cat "$tmp/${name}.err")" "$diagnostic" "$name validate_fts_metadata diagnostic"
}

run_discovery_failure() {
  local name binary diagnostic rc
  name="$1"
  binary="$2"
  diagnostic="$3"

  set +e
  discover_fts_tables "$binary" "$db" >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  assert_eq 1 "$rc" "$name discovery rc"
  assert_contains "$(cat "$tmp/${name}.err")" "$diagnostic" "$name discovery diagnostic"
}

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
cat > "$tmp/bin/docker" <<'EOF_DOCKER'
#!/usr/bin/env bash
printf 'docker should not be called by helper tests\n' >&2
exit 77
EOF_DOCKER
chmod +x "$tmp/bin/docker"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

db="$tmp/fts-discovery.db"
"$real_sqlite" "$db" <<'EOF_SQL'
CREATE TABLE ordinary_table(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE plain_content(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE plain_segments(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE plain_segdir(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE "table"(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE tbl(id INTEGER PRIMARY KEY, body TEXT);
CREATE TABLE external_docs(id INTEGER PRIMARY KEY, body TEXT);

CREATE VIRTUAL TABLE fts4_metadata_titles USING fts4(title);
CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title);
CREATE VIRTUAL TABLE fts4_aux_terms USING fts4aux(fts4_metadata_titles);
CREATE VIRTUAL TABLE fts4_tag_titles USING fts4(title);
CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(title);
CREATE VIRTUAL TABLE foo_content USING fts5(body);
CREATE VIRTUAL TABLE fts5_content_empty_compact USING fts5(body, content='');
CREATE VIRTUAL TABLE fts5_content_empty_spaced USING fts5(body, content = '');
CREATE VIRTUAL TABLE fts5_content_empty_upper USING fts5(body, CONTENT='');
CREATE VIRTUAL TABLE fts5_external_double USING fts5(body, content = "table");
CREATE VIRTUAL TABLE fts5_external_single USING fts5(body, content = 'tbl');
CREATE VIRTUAL TABLE fts5_normal USING fts5(body);
CREATE VIRTUAL TABLE v USING fts5vocab(fts5_normal, row);
CREATE VIRTUAL TABLE fts5_tokenizer_content_text USING fts5(body, tokenize='unicode61 tokenchars ''content=''');
EOF_SQL

expected="$tmp/expected.txt"
cat > "$expected" <<'EOF_EXPECTED'
foo_content	fts5	normal
fts4_metadata_titles	fts4	normal
fts4_metadata_titles_icu	fts4	normal
fts4_tag_titles	fts4	normal
fts4_tag_titles_icu	fts4	normal
fts5_content_empty_compact	fts5	contentless
fts5_content_empty_spaced	fts5	contentless
fts5_content_empty_upper	fts5	contentless
fts5_external_double	fts5	external
fts5_external_single	fts5	external
fts5_normal	fts5	normal
fts5_tokenizer_content_text	fts5	normal
EOF_EXPECTED

actual="$tmp/actual.txt"
discover_fts_tables sqlite3 "$db" >"$actual"
assert_eq "$(cat "$expected")" "$(cat "$actual")" "discovered FTS table rows"
actual_text="$(cat "$actual")"
assert_contains "$actual_text" $'foo_content\tfts5\tnormal' "real FTS parent with shadow-like suffix discovered"
assert_not_contains "$actual_text" "plain_content" "plain _content decoy excluded"
assert_not_contains "$actual_text" "plain_segments" "plain _segments decoy excluded"
assert_not_contains "$actual_text" "plain_segdir" "plain _segdir decoy excluded"
assert_not_contains "$actual_text" "ordinary_table" "ordinary table excluded"
assert_not_contains "$actual_text" "fts4_aux_terms" "fts4aux auxiliary table skipped"
assert_not_contains "$actual_text" $'v\t' "fts5vocab auxiliary table skipped"

control_name="bad"$'\001'"name"
run_validate_metadata_failure empty-name "" "fts5" "normal" "ERROR: unsafe FTS table name discovered"
run_validate_metadata_failure control-char-name "$control_name" "fts5" "normal" "ERROR: unsafe FTS table name discovered"
run_validate_metadata_failure bogus-module "real_fts" "fts5vocab" "normal" "ERROR: unclassified FTS module"
run_validate_metadata_failure bogus-content-mode "real_fts" "fts5" "mystery" "ERROR: unclassified FTS content mode"

cat > "$tmp/bin/sqlite3-fail" <<'EOF_FAIL'
#!/usr/bin/env bash
printf 'simulated sqlite failure\n' >&2
exit 66
EOF_FAIL
chmod +x "$tmp/bin/sqlite3-fail"

cat > "$tmp/bin/sqlite3-empty-content" <<'EOF_EMPTY_CONTENT'
#!/usr/bin/env bash
printf 'bad_content\tCREATE VIRTUAL TABLE bad_content USING fts5(body, content=)\n'
EOF_EMPTY_CONTENT
chmod +x "$tmp/bin/sqlite3-empty-content"

cat > "$tmp/bin/sqlite3-missing-module" <<'EOF_MISSING_MODULE'
#!/usr/bin/env bash
printf 'bad_missing_module\tCREATE VIRTUAL TABLE bad_missing_module USING \n'
EOF_MISSING_MODULE
chmod +x "$tmp/bin/sqlite3-missing-module"

cat > "$tmp/bin/sqlite3-unterminated-fts5" <<'EOF_UNTERMINATED'
#!/usr/bin/env bash
printf 'bad_unterminated\tCREATE VIRTUAL TABLE bad_unterminated USING fts5(body, content="tbl"\n'
EOF_UNTERMINATED
chmod +x "$tmp/bin/sqlite3-unterminated-fts5"

set +e
discover_fts_tables sqlite3-fail "$db" >"$tmp/fail.out" 2>"$tmp/fail.err"
rc=$?
set -e
assert_eq 1 "$rc" "discovery failure rc"
assert_contains "$(cat "$tmp/fail.err")" "ERROR: FTS discovery query failed" "discovery failure diagnostic"

run_discovery_failure empty-content sqlite3-empty-content "ERROR: unclassified FTS metadata for bad_content"
run_discovery_failure missing-module sqlite3-missing-module "ERROR: unclassified FTS metadata for bad_missing_module"
run_discovery_failure unterminated-fts5 sqlite3-unterminated-fts5 "ERROR: unclassified FTS metadata for bad_unterminated"

printf 'optimize_media_servers FTS discovery tests passed\n'
