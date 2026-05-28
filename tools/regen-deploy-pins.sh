#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: regen-deploy-pins.sh SQLITE_LIBRARY_PINS scripts/update-sqlite-library.sh [release-tag] [template-path]

Renders the deploy script from scripts/update-sqlite-library.sh.template by
substituting __SQLITE_LIBRARY_PINS_BLOCK__ with SQLITE_LIBRARY_PINS and
__INJECTED_HELPERS__ with deploy assertion helper functions.

template-path defaults to TEMPLATE_PATH, then to scripts/update-sqlite-library.sh.template
relative to this tool.
EOF
}

fatal() { echo "FATAL: $*" >&2; exit 1; }

[[ $# -ge 2 && $# -le 4 ]] || {
  usage
  exit 2
}

pins_file=$1
deploy_script=$2
release_tag=${3:-${TAG:-${GITHUB_REF_NAME:-}}}
script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
template_path=${4:-${TEMPLATE_PATH:-"${script_dir}/../scripts/update-sqlite-library.sh.template"}}

[[ -f "${pins_file}" ]] || fatal "pins manifest not found: ${pins_file}"
[[ -f "${template_path}" ]] || fatal "template not found: ${template_path}"
[[ -n "${release_tag}" ]] || fatal "release tag required as arg, TAG, or GITHUB_REF_NAME"
command -v python3 >/dev/null 2>&1 || fatal "python3 not found"

# WHY: Normalize CRLF before the PINS_EOF collision check so PINS_EOF\r is caught.
_pins_normalized="$(mktemp)"
tr -d '\r' < "${pins_file}" > "${_pins_normalized}"
pins_file="${_pins_normalized}"
trap 'rm -f "${_pins_normalized}"' EXIT

metadata="$(
  tr -d '\r' < "${pins_file}" | awk -F'|' '$1 == "version" { print; exit }'
)"
[[ -n "${metadata}" ]] || fatal "pins manifest missing metadata row"

metadata_shape="$(
  printf "%s\n" "${metadata}" | awk -F'|' 'NF == 8 && $1 == "version" && $2 == "2" && $3 == "managed_window" && $4 == "5" && $5 == "release_tag" && $7 == "generated_at" { print "ok"; exit }'
)"
[[ "${metadata_shape}" == "ok" ]] || fatal "malformed pins metadata row: ${metadata}"

schema_version="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $2 }')"
window_n="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $4 }')"
metadata_release_tag="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $6 }')"
metadata_generated_at="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $8 }')"

[[ "${schema_version}" == "2" ]] || fatal "unsupported pins schema: ${schema_version} (expected 2)"
version_row_count="$(awk -F'|' '$1 == "version" { count++ } END { print count + 0 }' "${pins_file}")"
[[ "${version_row_count}" == "1" ]] || fatal "pins manifest must contain exactly one version row, got ${version_row_count}"
grep -Fxq 'PINS_EOF' "${pins_file}" && {
  echo "FATAL: pins manifest contains literal PINS_EOF line" >&2
  exit 1
}
awk -F'|' '
  $1 == "version" { seen_version = 1; in_components = ($2 == "2"); next }
  in_components && (/^#/ || NF == 0) { print "component block interrupted before required rows: " $0 > "/dev/stderr"; bad = 1; in_components = 0; next }
  /^#/ || NF == 0 { next }
  in_components && $1 == "component" && NF == 3 { component_count++; if (component_count == 5) in_components = 0; next }
  in_components && $1 == "component" { print "component rows malformed field count: " $0 > "/dev/stderr"; bad = 1; next }
  in_components { print "component rows must immediately follow metadata before data rows: " $0 > "/dev/stderr"; bad = 1; in_components = 0; next }
  $1 == "component" { print "component rows outside schema-2 component block: " $0 > "/dev/stderr"; bad = 1; next }
  ($1 == "pre" || $1 == "post") && NF == 9 && $2 == "1" { next }
  { print "malformed pins row: " $0 > "/dev/stderr"; bad = 1 }
  END { exit bad ? 1 : 0 }
' "${pins_file}" || fatal "pins manifest contains invalid row stream"
if [[ "${schema_version}" == "2" ]]; then
  component_rows="$(awk -F'|' '$1 == "version" { in_components = 1; next } in_components && $1 == "component" { print; next } in_components { exit }' "${pins_file}")"
  mapfile -t component_array < <(printf "%s\n" "${component_rows}" | awk -F'|' 'NF == 3')
  expected_components=(sqlite mimalloc icu plex emby)
  [[ "${#component_array[@]}" -eq 5 ]] || fatal "component rows must contain exactly 5 rows"
  for idx in "${!expected_components[@]}"; do
    row="${component_array[$idx]}"
    IFS='|' read -r kind name value <<< "${row}"
    [[ "${kind}" == "component" ]] || fatal "component rows malformed kind: ${row}"
    [[ "${name}" == "${expected_components[$idx]}" && -n "${value}" ]] || fatal "component rows out of order or empty: ${row}"
    [[ "${name}" != "plex" && "${name}" != "emby" || ( "${value}" != */* && "${value}" != *:* ) ]] || fatal "component rows ${name} value must be an image tag suffix, got ${value}"
  done
fi
[[ "${window_n}" == "5" ]] || fatal "unsupported managed window: ${window_n}"
[[ "${metadata_release_tag}" == "${release_tag}" ]] || fatal "manifest release_tag=${metadata_release_tag} does not match ${release_tag}"
if [[ "${metadata_generated_at}" == "unreleased" ]]; then
  [[ "${metadata_release_tag}" == "unreleased" ]] || fatal "generated_at=unreleased requires release_tag=unreleased"
elif [[ "${schema_version}" == "2" ]]; then
  [[ "${metadata_generated_at}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]] || fatal "manifest generated_at is not a valid ISO 8601 UTC timestamp: ${metadata_generated_at}"
fi

python3 - \
  "${template_path}" \
  "${pins_file}" \
  "${deploy_script}" \
  "${schema_version}" \
  "${window_n}" \
  "${release_tag}" \
  "${metadata_generated_at}" \
  "${script_dir}" <<'PY'
import sys
from datetime import datetime
import os
import subprocess
from pathlib import Path

template_path, pins_path, deploy_path, schema_version, window_n, release_tag, generated_at, script_dir = sys.argv[1:]
if generated_at != "unreleased" and schema_version == "2":
    try:
        datetime.strptime(generated_at, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        sys.stderr.write(f"FATAL: manifest generated_at is not parseable ISO 8601 UTC: {generated_at}\n"); sys.exit(1)
placeholder = b"__SQLITE_LIBRARY_PINS_BLOCK__"
helpers_placeholder = b"__INJECTED_HELPERS__"

template = Path(template_path).read_bytes()
if template.count(placeholder) != 1:
    sys.stderr.write("FATAL: template missing or has duplicate __SQLITE_LIBRARY_PINS_BLOCK__ placeholder\n")
    sys.exit(1)
if template.count(helpers_placeholder) != 1:
    sys.stderr.write("FATAL: template missing or has duplicate __INJECTED_HELPERS__ placeholder\n")
    sys.exit(1)

raw_pins = Path(pins_path).read_bytes()
template_control_tokens = [placeholder, helpers_placeholder]
if any(token in raw_pins for token in template_control_tokens):
    sys.stderr.write("FATAL: pins manifest contains placeholder token\n")
    sys.exit(1)

pins = raw_pins.replace(b"\r", b"")
if pins and not pins.endswith(b"\n"):
    pins += b"\n"

def double_quote(value):
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("$", "\\$")
        .replace("`", "\\`")
    )

if placeholder + b"\r\n" in template:
    rendered = template.replace(placeholder + b"\r\n", pins)
elif placeholder + b"\n" in template:
    rendered = template.replace(placeholder + b"\n", pins)
else:
    rendered = template.replace(placeholder, pins)
metadata_replacements = {
    b'SQLITE_LIBRARY_PINS_SCHEMA_VERSION="1"': f'SQLITE_LIBRARY_PINS_SCHEMA_VERSION="{double_quote(schema_version)}"'.encode(),
    b'SQLITE_LIBRARY_PINS_WINDOW_N="5"': f'SQLITE_LIBRARY_PINS_WINDOW_N="{double_quote(window_n)}"'.encode(),
    b'SQLITE_LIBRARY_PINS_RELEASE_TAG="unreleased"': f'SQLITE_LIBRARY_PINS_RELEASE_TAG="{double_quote(release_tag)}"'.encode(),
    b'SQLITE_LIBRARY_PINS_GENERATED_AT="unreleased"': f'SQLITE_LIBRARY_PINS_GENERATED_AT="{double_quote(generated_at)}"'.encode(),
}

for old, new in metadata_replacements.items():
    if rendered.count(old) != 1:
        sys.stderr.write(f"FATAL: template missing expected metadata assignment: {old.decode()}\n")
        sys.exit(1)
    rendered = rendered.replace(old, new, 1)

repo_root = os.path.dirname(os.path.abspath(script_dir))
assertion_path = os.path.join(repo_root, "scripts", "lib", "deploy-assertion.sh")
try:
    assertion_src = Path(assertion_path).read_bytes()
except OSError:
    sys.stderr.write(f"FATAL: deploy-assertion.sh not found at {assertion_path}\n")
    sys.exit(1)
def extract_function(assertion_src, function_name):
    start_marker = (function_name + "() {").encode()
    start = assertion_src.find(start_marker)
    if start == -1:
        sys.stderr.write(
            f"FATAL: could not locate {function_name}() in scripts/lib/deploy-assertion.sh\n"
        )
        sys.exit(1)
    depth = 0
    end = None
    for index in range(start, len(assertion_src)):
        byte = assertion_src[index:index + 1]
        if byte == b"{":
            depth += 1
        elif byte == b"}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end is None:
        sys.stderr.write(
            f"FATAL: could not locate end of {function_name}() in scripts/lib/deploy-assertion.sh\n"
        )
        sys.exit(1)
    function_body = assertion_src[start:end]
    if function_body.count(b"{") != function_body.count(b"}"):
        sys.stderr.write(
            f"FATAL: extracted {function_name}() has unbalanced braces\n"
        )
        sys.exit(1)
    return function_body

helper_bodies = b"\n\n".join(
    [
        extract_function(assertion_src, "emit_component_summary"),
        extract_function(assertion_src, "assert_pre_replacement_sha"),
    ]
)
rendered = rendered.replace(helpers_placeholder, helper_bodies)

tmp = deploy_path + ".tmp." + str(os.getpid())
try:
    Path(tmp).write_bytes(rendered)
    os.chmod(tmp, 0o755)
    if os.path.getsize(tmp) <= len(Path(template_path).read_bytes()):
        os.unlink(tmp)
        sys.stderr.write("FATAL: rendered file smaller than template\n")
        sys.exit(1)
    tmp_contents = Path(tmp).read_bytes()
    if b"PINS_EOF" not in tmp_contents:
        os.unlink(tmp)
        sys.stderr.write("FATAL: rendered file missing PINS_EOF sentinel\n")
        sys.exit(1)
    if b'SQLITE_LIBRARY_PINS_RELEASE_TAG="' not in tmp_contents:
        os.unlink(tmp)
        sys.stderr.write("FATAL: rendered file missing SQLITE_LIBRARY_PINS_RELEASE_TAG metadata\n")
        sys.exit(1)
    syntax = subprocess.run(["bash", "-n", tmp], text=True, capture_output=True)
    if syntax.returncode != 0:
        if syntax.stdout:
            sys.stderr.write(syntax.stdout)
        if syntax.stderr:
            sys.stderr.write(syntax.stderr)
        sys.stderr.write(f"FATAL: rendered deploy script failed bash -n: {tmp}\n")
        sys.exit(1)
    os.replace(tmp, deploy_path)
except Exception:
    if os.path.exists(tmp):
        os.unlink(tmp)
    raise
PY
