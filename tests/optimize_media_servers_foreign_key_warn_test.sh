#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
cd "$repo_root"

. ./scripts/optimize_media_servers.sh

tmp_parent="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${tmp_parent%/}/sqlite3-optimize-fk-warn.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-optimize-fk-warn.XXXXXX)"
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

real_sqlite="$(command -v sqlite3 || true)"
[ -n "$real_sqlite" ] || fail "sqlite3 availability" "sqlite3 in PATH" "missing"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/sqlite3" <<'EOF_SQLITE'
#!/usr/bin/env bash
exec "$REAL_SQLITE" "$@"
EOF_SQLITE
chmod +x "$tmp/bin/sqlite3"
PATH="$tmp/bin:$PATH"
export PATH REAL_SQLITE="$real_sqlite"

create_fk_schema() {
  local db orphan
  db="$1"
  orphan="$2"
  "$real_sqlite" "$db" <<EOF_SQL
PRAGMA foreign_keys=OFF;
CREATE TABLE parent(id INTEGER PRIMARY KEY);
CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id));
INSERT INTO parent(id) VALUES(1);
INSERT INTO child(id, parent_id) VALUES(1, 1);
INSERT INTO child(id, parent_id) VALUES(2, ${orphan});
EOF_SQL
}

run_rebuild_capture() {
  local name binary db backup_dir
  name="$1"
  binary="$2"
  db="$3"
  backup_dir="$4"
  set +e
  (
    cd "$repo_root"
    . ./scripts/optimize_media_servers.sh
    rebuild_db_vacuum_into "$binary" "$db" "$backup_dir" 4096 NONE "" "" ""
  ) >"$tmp/${name}.out" 2>"$tmp/${name}.err"
  rc=$?
  set -e
  printf '%s' "$rc" > "$tmp/${name}.rc"
}

clean="$tmp/clean.db"
"$real_sqlite" "$clean" <<'EOF_SQL'
PRAGMA foreign_keys=OFF;
CREATE TABLE parent(id INTEGER PRIMARY KEY);
CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id));
INSERT INTO parent(id) VALUES(1);
INSERT INTO child(id, parent_id) VALUES(1, 1);
EOF_SQL
run_foreign_key_check_warn sqlite3 "$clean" >"$tmp/clean.out" 2>"$tmp/clean.err"
assert_eq "" "$(cat "$tmp/clean.out")" "clean FK stdout"
assert_not_contains "$(cat "$tmp/clean.err")" "WARNING:" "clean FK warning absence"

orphan="$tmp/orphan.db"
create_fk_schema "$orphan" 99
run_foreign_key_check_warn sqlite3 "$orphan" >"$tmp/orphan.out" 2>"$tmp/orphan.err"
assert_eq "" "$(cat "$tmp/orphan.out")" "orphan FK stdout"
orphan_err="$(cat "$tmp/orphan.err")"
assert_contains "$orphan_err" "WARNING: foreign_key_check returned rows" "orphan FK warning"
assert_contains "$orphan_err" "child|2|parent|0" "orphan FK returned row"

pipeline="$tmp/pipeline-orphan.db"
create_fk_schema "$pipeline" 99
mkdir -p "$tmp/backups"
run_rebuild_capture pipeline-fk sqlite3 "$pipeline" "$tmp/backups"
assert_eq 0 "$(cat "$tmp/pipeline-fk.rc")" "FK warning pipeline rc"
assert_contains "$(cat "$tmp/pipeline-fk.err")" "WARNING: foreign_key_check returned rows" "pipeline FK warning"
assert_eq "2" "$("$real_sqlite" "$pipeline" "SELECT COUNT(*) FROM child;")" "pipeline child row count after swap"
[ ! -e "$pipeline.new" ] || fail "FK warning pipeline staged cleanup" "no staged file" "$pipeline.new exists"

printf 'optimize_media_servers foreign key warning tests passed\n'
