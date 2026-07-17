#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: skopeo-mod-image-oci-check.sh <mod> <publish-arch> <image-archive> <image-ref>" >&2
}

if [ "$#" -ne 4 ]; then
  usage
  exit 2
fi

mod="$1"
publish_arch="$2"
image_archive="$3"
image_ref="$4"

case "$mod" in
  plex|emby) ;;
  *)
    echo "FATAL: unsupported mod: $mod" >&2
    exit 1
    ;;
esac

case "$publish_arch" in
  amd64|arm64) ;;
  *)
    echo "FATAL: unsupported publish arch: $publish_arch" >&2
    exit 1
    ;;
esac

if [ ! -f "$image_archive" ]; then
  echo "FATAL: missing image archive: $image_archive" >&2
  exit 1
fi

if [ -z "$image_ref" ]; then
  echo "FATAL: image ref is empty" >&2
  exit 1
fi

skopeo_bin="${SKOPEO_BIN:-skopeo}"
if ! command -v "$skopeo_bin" >/dev/null 2>&1; then
  echo "FATAL: skopeo not found: $skopeo_bin" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

oci_dir="${tmpdir}/oci"
oci_ref="oci:${oci_dir}:mod-check"
archive_ref="docker-archive:${image_archive}:${image_ref}"
manifest_json="${tmpdir}/manifest.json"
config_json="${tmpdir}/config.json"

"$skopeo_bin" copy --format oci "$archive_ref" "$oci_ref" >/dev/null
"$skopeo_bin" inspect --raw "$oci_ref" > "$manifest_json"
"$skopeo_bin" inspect --config "$oci_ref" > "$config_json"

python3 - "$mod" "$publish_arch" "$manifest_json" "$config_json" "$oci_dir" <<'PY'
import json
import pathlib
import re
import sys
import tarfile

mod, publish_arch, manifest_path, config_path, oci_dir = sys.argv[1:]
oci_root = pathlib.Path(oci_dir)

with open(manifest_path, "r", encoding="utf-8") as handle:
    manifest = json.load(handle)
with open(config_path, "r", encoding="utf-8") as handle:
    config = json.load(handle)

def fail(message):
    raise SystemExit(f"FATAL: {message}")

def require_sha256_descriptor(descriptor, label):
    if not isinstance(descriptor, dict):
        fail(f"{label} descriptor is not an object")
    digest = descriptor.get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        fail(f"{label} digest is not sha256: {digest}")
    size = descriptor.get("size")
    if not isinstance(size, int) or size <= 0:
        fail(f"{label} size is not positive")
    return digest

config_digest = require_sha256_descriptor(manifest.get("config"), "config")
layers = manifest.get("layers")
if not isinstance(layers, list):
    fail("manifest layers is not an array")
if len(layers) != 1:
    fail(f"expected exactly one image layer, found {len(layers)}")
layer_digest = require_sha256_descriptor(layers[0], "layer")

expected_config_blob = oci_root / "blobs" / "sha256" / config_digest.split(":", 1)[1]
if not expected_config_blob.is_file():
    fail(f"missing config blob: {expected_config_blob}")
layer_blob = oci_root / "blobs" / "sha256" / layer_digest.split(":", 1)[1]
if not layer_blob.is_file():
    fail(f"missing layer blob: {layer_blob}")

expected_arch = {"amd64": "amd64", "arm64": "arm64"}[publish_arch]
if config.get("architecture") != expected_arch:
    fail(f"config architecture mismatch: {config.get('architecture')} != {expected_arch}")
if config.get("os") != "linux":
    fail(f"config os mismatch: {config.get('os')} != linux")

rootfs = config.get("rootfs")
if not isinstance(rootfs, dict) or rootfs.get("type") != "layers":
    fail("config rootfs.type is not layers")
diff_ids = rootfs.get("diff_ids")
if not isinstance(diff_ids, list) or len(diff_ids) != len(layers):
    fail("config rootfs.diff_ids does not match manifest layer count")
for diff_id in diff_ids:
    if not isinstance(diff_id, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", diff_id):
        fail(f"invalid rootfs diff_id: {diff_id}")

image_config = config.get("config") or {}
labels = image_config.get("Labels")
if labels not in (None, {}):
    fail(f"unexpected image labels: {sorted(labels)}")

with tarfile.open(layer_blob, "r:*") as archive:
    members = set()
    baked_pins_text = None
    for member in archive.getmembers():
        name = member.name
        while name.startswith("./"):
            name = name[2:]
        name = name.lstrip("/")
        members.add(name)
        if name == "opt/sqlite3-lsio-mod/baked-pins.txt":
            handle = archive.extractfile(member)
            if handle is None:
                fail("baked-pins.txt is not a regular file")
            baked_pins_text = handle.read().decode("utf-8")

def require_member(path):
    normalized = path.strip("/")
    if normalized not in members:
        fail(f"missing layer path: /{normalized}")

for required in [
    "opt/sqlite3-lsio-mod/baked-pins.txt",
    "opt/sqlite3-lsio-mod/lib/arch.sh",
    "opt/sqlite3-lsio-mod/lib/logging.sh",
    "opt/sqlite3-lsio-mod/lib/manifest-parser.sh",
    "opt/sqlite3-lsio-mod/lib/selector.sh",
    "opt/sqlite3-lsio-mod/lib/sha.sh",
    "opt/sqlite3-lsio-mod/lib/swap.sh",
]:
    require_member(required)

runtime_arches = {
    "amd64": ["linux-x86_64-v2", "linux-x86_64-v3"],
    "arm64": ["linux-arm64"],
}[publish_arch]
if baked_pins_text is None:
    fail("missing baked-pins.txt content")

artifact_relpaths = set()
artifact_arches = set()
for line in baked_pins_text.splitlines():
    if not line or line.startswith("#"):
        continue
    fields = line.split("|")
    if fields[0] != "artifact":
        continue
    if len(fields) != 9:
        fail(f"malformed artifact row in baked-pins.txt: {line}")
    _, version, row_mod, _server_id, runtime_arch, _compat_group, relpath, _target_path, _sha = fields
    if version != "1":
        fail(f"unsupported artifact row version in baked-pins.txt: {version}")
    if row_mod != mod:
        fail(f"artifact row mod mismatch in baked-pins.txt: {row_mod} != {mod}")
    if runtime_arch not in runtime_arches:
        fail(f"artifact row arch does not belong to publish arch {publish_arch}: {runtime_arch}")
    artifact_arches.add(runtime_arch)
    artifact_relpaths.add(relpath)

for runtime_arch in runtime_arches:
    if runtime_arch not in artifact_arches:
        fail(f"missing artifact row for runtime arch: {runtime_arch}")
for relpath in artifact_relpaths:
    require_member(f"opt/sqlite3-lsio-mod/{relpath}")

if mod == "plex":
    require_member("opt/sqlite3-lsio-mod/lib/plex-patch.sh")
    require_member("opt/sqlite3-lsio-mod/pins/library-compat-groups.tsv")

if any(name.startswith("root-fs/") for name in members):
    fail("layer contains root-fs prefix instead of root filesystem paths")
if any(name.startswith("etc/cont-init.d/") for name in members):
    fail("layer contains legacy cont-init.d paths")

print(
    "skopeo OCI static check passed "
    f"mod={mod} publish_arch={publish_arch} layers=1 runtime_arches={','.join(runtime_arches)}"
)
PY
