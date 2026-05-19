#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: regen-deploy-pins.sh SQLITE_LIBRARY_PINS scripts/update-sqlite-library.sh [release-tag] [template-path]

Renders the deploy script from scripts/update-sqlite-library.sh.template by
substituting __SQLITE_LIBRARY_PINS_BLOCK__ with SQLITE_LIBRARY_PINS and
__ASSERT_PRE_REPLACEMENT_SHA__ with the deploy assertion function.

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
  tr -d '\r' < "${pins_file}" | awk -F'|' '$1 == "version" && $2 == "1" { print; exit }'
)"
[[ -n "${metadata}" ]] || fatal "pins manifest missing metadata row"

schema_version="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $2 }')"
window_n="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $4 }')"
metadata_release_tag="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $6 }')"
metadata_generated_at="$(printf "%s\n" "${metadata}" | awk -F'|' '{ print $8 }')"

[[ "${schema_version}" == "1" ]] || fatal "unsupported pins schema: ${schema_version}"
[[ "${window_n}" == "5" ]] || fatal "unsupported managed window: ${window_n}"
[[ "${metadata_release_tag}" == "${release_tag}" ]] || fatal "manifest release_tag=${metadata_release_tag} does not match ${release_tag}"
[[ "${metadata_generated_at}" == "${release_tag}" ]] || fatal "manifest generated_at=${metadata_generated_at} does not match ${release_tag}"
grep -Fxq 'PINS_EOF' "${pins_file}" && {
  echo "FATAL: pins manifest contains literal PINS_EOF line" >&2
  exit 1
}

python3 - \
  "${template_path}" \
  "${pins_file}" \
  "${deploy_script}" \
  "${schema_version}" \
  "${window_n}" \
  "${release_tag}" \
  "${release_tag}" \
  "${script_dir}" <<'PY'
import sys
import os
from pathlib import Path

template_path, pins_path, deploy_path, schema_version, window_n, release_tag, generated_at, script_dir = sys.argv[1:]
placeholder = b"__SQLITE_LIBRARY_PINS_BLOCK__"
assert_placeholder = b"__ASSERT_PRE_REPLACEMENT_SHA__"

template = Path(template_path).read_bytes()
if template.count(placeholder) != 1:
    sys.stderr.write("FATAL: template missing or has duplicate __SQLITE_LIBRARY_PINS_BLOCK__ placeholder\n")
    sys.exit(1)
if template.count(assert_placeholder) != 1:
    sys.stderr.write("FATAL: template missing or has duplicate __ASSERT_PRE_REPLACEMENT_SHA__ placeholder\n")
    sys.exit(1)

raw_pins = Path(pins_path).read_bytes()
if placeholder in raw_pins:
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
start_marker = b"assert_pre_replacement_sha() {"
start = assertion_src.find(start_marker)
if start == -1:
    sys.stderr.write("FATAL: could not locate assert_pre_replacement_sha() in scripts/lib/deploy-assertion.sh\n")
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
    sys.stderr.write("FATAL: could not locate end of assert_pre_replacement_sha() in scripts/lib/deploy-assertion.sh\n")
    sys.exit(1)
function_body = assertion_src[start:end]
if function_body.count(b"{") != function_body.count(b"}"):
    sys.stderr.write("FATAL: extracted assert_pre_replacement_sha() has unbalanced braces\n")
    sys.exit(1)
rendered = rendered.replace(assert_placeholder, function_body)

tmp = deploy_path + ".tmp." + str(os.getpid())
try:
    Path(tmp).write_bytes(rendered)
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
    os.replace(tmp, deploy_path)
except Exception:
    if os.path.exists(tmp):
        os.unlink(tmp)
    raise
PY

if ! bash -n "${deploy_script}"; then
  fatal "rendered deploy script failed bash -n: ${deploy_script}"
fi
