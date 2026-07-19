#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
forbidden_api_pattern='(^|[^[:alnum:]_])(sqlite3_close|sqlite3_open|sqlite3_open_v2|sqlite3_open16|setenv|unsetenv|mkdir|rmdir|fork|execl|execlp|execv|execvp)([^[:alnum:]_]|$)'

fail() {
  printf 'FATAL: %s\n' "$1" >&2
  exit 1
}

adapter_name_violations() {
  awk '
    FNR == 1 && pending {
      print assignment_file ":" assignment_line ": incomplete assert_custom assignment"
      violations++
      pending = 0
    }
    {
      if (!pending) {
        if ($0 !~ /\.assert_custom[[:space:]]*=/) next
        pending = 1
        assignment_file = FILENAME
        assignment_line = FNR
        assignment = $0
      } else {
        assignment = assignment " " $0
      }
      value = assignment
      sub(/^.*\.assert_custom[[:space:]]*=[[:space:]]*/, "", value)
      if (value !~ /[,;]/) next
      sub(/[[:space:]]*[,;].*$/, "", value)
      gsub(/[[:space:]]/, "", value)
      if (value !~ /^rsh_custom_adapter_[[:alnum:]_]+$/) {
        print assignment_file ":" assignment_line ":" assignment
        violations++
      }
      pending = 0
      assignment = ""
    }
    END {
      if (pending) {
        print assignment_file ":" assignment_line ": incomplete assert_custom assignment"
        violations++
      }
      exit(violations ? 0 : 1)
    }
  ' "$@"
}

extract_adapter_bodies() {
  awk '
    function brace_delta(text, copy, opens, closes) {
      copy = text
      opens = gsub(/\{/, "", copy)
      copy = text
      closes = gsub(/\}/, "", copy)
      return opens - closes
    }
    {
      line = $0
      if (!pending && !active) {
        if (line ~ /rsh_custom_adapter_[[:alnum:]_]+[[:space:]]*\(/) {
          pending = 1
        } else {
          next
        }
      }
      if (pending && !active) {
        if (index(line, ";") != 0 && index(line, "{") == 0) {
          pending = 0
          next
        }
        if (index(line, "{") != 0) {
          pending = 0
          active = 1
          depth = brace_delta(line)
          print FILENAME ":" FNR ":" line
          if (depth <= 0) active = 0
        }
        next
      }
      if (active) {
        print FILENAME ":" FNR ":" line
        depth += brace_delta(line)
        if (depth <= 0) active = 0
      }
    }
  ' "$@"
}

adapter_body_violations() {
  extract_adapter_bodies "$@" | grep -E -- "${forbidden_api_pattern}"
}

if ! adapter_body_violations - <<'EOF'
static int rsh_custom_adapter_guard_negative_demo(
    const rsh_case_context *context,
    const void *immutable_data
) {
    (void)context;
    (void)immutable_data;
    return setenv("RSH_GUARD_DEMO", "1", 1);
}
EOF
then
  fail "custom-adapter negative demonstration did not detect planted setenv"
fi
printf 'custom-adapter negative demonstration: planted setenv rejected\n'

sources=("${repo_root}"/tests/*.c "${repo_root}"/tests/*.h)
if adapter_name_violations "${sources[@]}"; then
  fail "assert_custom assignment does not use the rsh_custom_adapter_ prefix"
fi
if adapter_body_violations "${sources[@]}"; then
  fail "custom assertion adapter references a runner-owned lifecycle API"
fi

printf 'custom-adapter guard: clean tree passed\n'
