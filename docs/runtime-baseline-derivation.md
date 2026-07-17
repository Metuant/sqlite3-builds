# Runtime Baseline Derivation

## Purpose and Scope

Derive or verify the SHA-256 values recorded in `pins/runtime-baselines.tsv`
from published OCI images without a Docker daemon. Use `skopeo` to copy the
image into a local OCI dir transport, extract the exact in-image files, and
hash the extracted bytes.

This covers:

- Plex curated `detect` rows for `plex_pms:pristine`, `plex_pms:patched`,
  `plex_scanner:pristine`, and `plex_scanner:patched`.
- Emby `detect` rows for `emby_deps` and `emby_dll`.
- `pre target_sqlite` rows for Plex and Emby.
- Plex `pre plex_icu_linked:<soname>` rows and matching `icu-runtime` rows.
- `pins/emby-detector-evidence.tsv` rows mirroring every Emby `detect` and
  `pre target_sqlite` baseline tuple, with `image_ref` from
  `pins/runtime-support.tsv`, `file_path` and `sha256` from
  `pins/runtime-baselines.tsv`, and `source` set to
  `pins/runtime-baselines.tsv`.

Arch mapping:

- `--override-arch amd64` feeds tracked `linux-x86_64-v2` and
  `linux-x86_64-v3`.
- `--override-arch arm64` feeds tracked `linux-arm64`.

Recorded values live in `pins/runtime-baselines.tsv`. Plex patch pool-site tuple
inputs live in `pins/plex-patch-pool-sites.tsv`. Site derivation, baselines,
and exclusions live in `docs/plex-pool-patch-derivation.md`.

## Prerequisites

Require:

- `bash`
- `skopeo`
- `sha256sum` or `shasum -a 256`
- `python3`
- `tar`
- `od`
- `dd`
- `awk`
- `grep`
- `mktemp`

## Extraction

Copy the published image into a fresh dir transport:

```bash
copy_dir="$(mktemp -d)"
image_ref='ghcr.io/linuxserver/plex:1.43.2'
skopeo_arch='amd64'

skopeo copy \
  --override-os linux \
  --override-arch "$skopeo_arch" \
  "docker://$image_ref" \
  "dir:$copy_dir"
```

Use these helpers for the rest of the workflow:

```bash
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

manifest_layers() {
  python3 - "$1" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    data = json.load(handle)

layers = data.get("layers")
if not isinstance(layers, list):
    raise SystemExit("manifest.json missing layers array")

for layer in layers:
    digest = layer["digest"]
    print(digest.split(":", 1)[1] if ":" in digest else digest)
PY
}

blob_mode() {
  local blob_path="$1"
  local magic

  magic="$(od -An -tx1 -N2 "$blob_path" | tr -d ' \n')"
  if [ "$magic" = "1f8b" ]; then
    printf 'gzip\n'
  else
    printf 'plain\n'
  fi
}

resolve_oci_member() {
  local copy_dir="$1"
  local target_path="$2"
  local digest
  local blob_path
  local mode
  local listing_path
  local raw_member
  local winner=''

  while IFS= read -r digest; do
    blob_path="${copy_dir}/${digest}"
    mode="$(blob_mode "$blob_path")"
    listing_path="$(mktemp)"

    if [ "$mode" = "gzip" ]; then
      tar -tzf "$blob_path" >"$listing_path"
    else
      tar -tf "$blob_path" >"$listing_path"
    fi

    raw_member="$(python3 - "$listing_path" "$target_path" <<'PY'
import sys

listing_path, target_path = sys.argv[1:]
matches = []

with open(listing_path, "r", encoding="utf-8", errors="surrogateescape") as src:
    for line in src:
        member = line.rstrip("\n")
        normalized = member[2:] if member.startswith("./") else member
        if not normalized.startswith("/"):
            normalized = "/" + normalized
        if normalized == target_path:
            matches.append(member)

if matches:
    print(matches[-1])
PY
)"
    rm -f "$listing_path"

    if [ -n "$raw_member" ]; then
      winner="${blob_path}"$'\t'"${mode}"$'\t'"${raw_member}"
    fi
  done < <(manifest_layers "${copy_dir}/manifest.json")

  if [ -z "$winner" ]; then
    printf 'missing member for %s\n' "$target_path" >&2
    return 1
  fi

  printf '%s\n' "$winner"
}

extract_oci_path() {
  local copy_dir="$1"
  local target_path="$2"
  local output_path="$3"
  local resolved
  local blob_path
  local mode
  local raw_member

  resolved="$(resolve_oci_member "$copy_dir" "$target_path")" || return 1
  blob_path="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $1}')"
  mode="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $2}')"
  raw_member="$(printf '%s\n' "$resolved" | awk -F '\t' '{print $3}')"

  if [ "$mode" = "gzip" ]; then
    tar -xzOf "$blob_path" "$raw_member" >"$output_path"
  else
    tar -xOf "$blob_path" "$raw_member" >"$output_path"
  fi
}

lookup_runtime_sha() {
  local kind="$1"
  local mod="$2"
  local server_id="$3"
  local arch="$4"
  local path_role="$5"
  local file_path="$6"

  awk -F '\t' \
    -v kind="$kind" \
    -v mod="$mod" \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v path_role="$path_role" \
    -v file_path="$file_path" \
    '
      $1 == kind &&
      $2 == mod &&
      $3 == server_id &&
      $4 == arch &&
      $5 == path_role &&
      $8 == file_path {
        print $12
      }
    ' pins/runtime-baselines.tsv
}

lookup_icu_runtime_sha() {
  local compat_group="$1"
  local arch="$2"
  local soname="$3"

  awk -F '\t' \
    -v compat_group="$compat_group" \
    -v arch="$arch" \
    -v soname="$soname" \
    '
      $1 == "icu-runtime" &&
      $4 == arch &&
      $9 == compat_group &&
      $10 == "linked" &&
      $11 == soname {
        print $12
      }
    ' pins/runtime-baselines.tsv
}

lookup_plex_baseline_sha() {
  local server_id="$1"
  local arch="$2"
  local binary_path="$3"

  awk -F '\t' \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v binary_path="$binary_path" \
    '
      NR > 1 &&
      $1 == server_id &&
      $2 == arch &&
      $3 == binary_path {
        print $4
        exit
      }
    ' pins/plex-patch-pool-sites.tsv
}

write_plex_site_file() {
  local server_id="$1"
  local arch="$2"
  local binary_path="$3"
  local output_path="$4"

  awk -F '\t' \
    -v server_id="$server_id" \
    -v arch="$arch" \
    -v binary_path="$binary_path" \
    '
      NR > 1 &&
      $1 == server_id &&
      $2 == arch &&
      $3 == binary_path {
        printf "%s|%s|%s|%s|%s\n", $5, $6, $7, $8, $9
      }
    ' pins/plex-patch-pool-sites.tsv >"$output_path"

  grep -q . "$output_path"
}
```

Match the exact in-image path from the tracked row. Do not match fragments. The
Plex paths contain spaces, tar members may be prefixed with `./`, and overlay
layers can replace an earlier file. The last matching layer in
`manifest.json` wins.

Examples:

```bash
extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/Plex Media Server' \
  "$copy_dir/Plex Media Server"

extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/lib/libsqlite3.so' \
  "$copy_dir/libsqlite3.so"

extract_oci_path \
  "$copy_dir" \
  '/usr/lib/plexmediaserver/lib/libicuucplex.so.69' \
  "$copy_dir/libicuucplex.so.69"
```

Use the same helper for Emby paths such as:

- `/app/emby/system/EmbyServer.deps.json`
- `/app/emby/system/EmbyServer.dll`
- `/app/emby/lib/libsqlite3.so.3.49.2`

## Pristine Cross-Check

Hash the extracted file before trusting it:

```bash
actual_sha="$(sha256_of "$copy_dir/Plex Media Server")"
expected_sha="$(lookup_runtime_sha \
  'detect' \
  'plex' \
  'plex-1.43.2' \
  'linux-x86_64-v2' \
  'plex_pms:pristine' \
  '/usr/lib/plexmediaserver/Plex Media Server')"

if [ "$actual_sha" != "$expected_sha" ]; then
  printf 'STOP: pristine mismatch expected=%s actual=%s\n' \
    "$expected_sha" \
    "$actual_sha" >&2
  exit 1
fi
```

Rules:

- For any `pre target_sqlite` row, the extracted file SHA must equal the
  recorded `sha256`.
- For any Plex `pre plex_icu_linked:<soname>` row, the extracted file SHA must
  equal both the `pre` row and the matching `icu-runtime` row for that
  `compat_group`, `arch`, and `soname`.
- For any Plex pristine detector binary, the extracted file SHA must equal the
  matching `detect ...:pristine` row and the `baseline_sha256` recorded in
  `pins/plex-patch-pool-sites.tsv`.
- On any mismatch, STOP. Do not trust the image, do not derive a patched
  detector SHA, and do not update tracked pins from that source.

## Plex Patched Detector Derivation

Only derive a patched Plex detector SHA from a pristine binary that already
passed the cross-check above. Pull the real site tuples from
`pins/plex-patch-pool-sites.tsv`. For site derivation, baselines, exclusions,
and runtime semantics, use `docs/plex-pool-patch-derivation.md`. For the shared
runtime patch core, use
`lsio-mods/shared/cont-init-fragments/plex-patch.sh`. Do not copy those
tables into this doc.

Both PMS and Scanner patched-detector derivations apply only their pool-site
tuples. The PMS source id remains OLD in the derived output. The resulting
`plex_pms:patched` and `plex_scanner:patched` SHAs are pool-patched SHAs; neither
derivation includes the OLD-to-NEW source-id edit.

Build the tuple file for one binary:

```bash
server_id='plex-1.43.2'
arch_key='linux-x86_64-v2'
binary_path='/usr/lib/plexmediaserver/Plex Media Server'
site_file="$copy_dir/pms.sites"

write_plex_site_file "$server_id" "$arch_key" "$binary_path" "$site_file" || {
  printf 'STOP: missing pool-site tuples for %s %s\n' "$arch_key" "$binary_path" >&2
  exit 1
}
```

Apply the tuples with an independent Python byte loop:

```bash
patch_plex_detector_python() {
  local pristine_path="$1"
  local site_file="$2"
  local patched_path="$3"

  python3 - "$pristine_path" "$site_file" "$patched_path" <<'PY'
import pathlib
import sys

pristine_path = pathlib.Path(sys.argv[1])
site_file = pathlib.Path(sys.argv[2])
patched_path = pathlib.Path(sys.argv[3])

patched = bytearray(pristine_path.read_bytes())

for raw_line in site_file.read_text(encoding="utf-8").splitlines():
    if not raw_line:
        continue
    label, offset, write_seek, original_hex, patched_hex = raw_line.split("|")
    offset = int(offset)
    write_seek = int(write_seek)
    write_index = write_seek - offset
    if write_index < 0 or write_index > 15:
        raise SystemExit(f"STOP: invalid write_index for {label}: {write_index}")

    byte_start = write_index * 2
    byte_end = byte_start + 2
    if (
        original_hex[:byte_start] != patched_hex[:byte_start] or
        original_hex[byte_end:] != patched_hex[byte_end:] or
        original_hex[byte_start:byte_end] == patched_hex[byte_start:byte_end]
    ):
        raise SystemExit(f"STOP: tuple is not a single-byte patch at {label}")

    observed = bytes(patched[offset:offset + 16]).hex()
    if observed != original_hex:
        raise SystemExit(f"STOP: original_hex mismatch at {label}")

    patched[write_seek] = int(patched_hex[byte_start:byte_end], 16)
    observed_patched = bytes(patched[offset:offset + 16]).hex()
    if observed_patched != patched_hex:
        raise SystemExit(f"STOP: patched_hex mismatch at {label}")

patched_path.write_bytes(patched)
PY
}

patch_plex_detector_python \
  "$copy_dir/Plex Media Server" \
  "$site_file" \
  "$copy_dir/Plex Media Server.patched.A"

patched_sha_a="$(sha256_of "$copy_dir/Plex Media Server.patched.A")"
printf '%s\n' "$patched_sha_a"
```

That model is fixed:

- `write_index = write_seek - offset`
- `write_index` must be in `0..15`
- assert the 16-byte window equals `original_hex`
- write exactly one byte from `patched_hex`
- assert the 16-byte window equals `patched_hex`
- do not inspect or modify source-id bytes for either binary

The resulting SHA-256 is the pool-patched detector SHA for that binary. For PMS,
the hashed output still contains the OLD source id.

## Emby Detectors

Emby detector derivation has no patch step. Extract and hash these two files:

- `/app/emby/system/EmbyServer.deps.json`
- `/app/emby/system/EmbyServer.dll`

Example:

```bash
extract_oci_path \
  "$copy_dir" \
  '/app/emby/system/EmbyServer.deps.json' \
  "$copy_dir/EmbyServer.deps.json"

extract_oci_path \
  "$copy_dir" \
  '/app/emby/system/EmbyServer.dll' \
  "$copy_dir/EmbyServer.dll"

deps_sha="$(sha256_of "$copy_dir/EmbyServer.deps.json")"
dll_sha="$(sha256_of "$copy_dir/EmbyServer.dll")"
printf 'deps=%s\n' "$deps_sha"
printf 'dll=%s\n' "$dll_sha"
```

Compare those SHAs to the matching `detect` rows in `pins/runtime-baselines.tsv`.
Use the same direct extraction-and-hash check for Emby `pre target_sqlite`.
`/app/emby/lib/libsqlite3.so.*` is byte-identical across the current supported
versions, so `EmbyServer.deps.json` and `EmbyServer.dll` are the version
discriminants.

## A/B Independent Cross-Derivation

Do not sign off a new Plex patched detector SHA from one implementation alone.
Run two independent derivations and require identical output:

- Lane A: the Python byte loop above.
- Lane B: the shared shell patch core in
  `lsio-mods/shared/cont-init-fragments/plex-patch.sh`.

Example Lane B:

```bash
. lsio-mods/shared/cont-init-fragments/plex-patch.sh

expected_pre_sha="$(lookup_plex_baseline_sha "$server_id" "$arch_key" "$binary_path")"
mapfile -t site_strings <"$site_file"

actual_pre_sha="$(sha256_of "$copy_dir/Plex Media Server")"
if [ "$actual_pre_sha" != "$expected_pre_sha" ]; then
  printf 'STOP: shell lane pristine mismatch expected=%s actual=%s\n' \
    "$expected_pre_sha" \
    "$actual_pre_sha" >&2
  exit 1
fi

plex_pool_patch_populate_binary_tmp \
  "$copy_dir/Plex Media Server.patched.B" \
  "$copy_dir/Plex Media Server" \
  "$expected_pre_sha" \
  "${site_strings[@]}" || {
    printf 'STOP: shell patch lane failed\n' >&2
    exit 1
  }

patched_sha_b="$(sha256_of "$copy_dir/Plex Media Server.patched.B")"

if [ "$patched_sha_a" != "$patched_sha_b" ]; then
  printf 'STOP: A/B mismatch A=%s B=%s\n' "$patched_sha_a" "$patched_sha_b" >&2
  exit 1
fi
```

For Scanner Lane B, use the same `plex_pool_patch_populate_binary_tmp` helper
with the Scanner pristine path, expected pristine SHA, and Scanner site tuples.

Before sign-off, also require equality with the recorded
`detect ...:patched` row for the selected role. Both `plex_pms:patched` and
`plex_scanner:patched` must hash the pool-only output; the PMS output still
contains the OLD source id. If the two lanes disagree, or either lane disagrees
with the recorded value, STOP. Pool-site derivation itself is separate
human-review-gated work: the M5 pool-review.

## PMS Source-ID-Patched Detector Derivation

`plex_pms:source-id-patched` is computed during manifest rendering. It is not a
row in `pins/runtime-baselines.tsv`. This role hashes the exact PMS bytes with
every pool site post-patch and the one 84-byte source id changed from OLD to
NEW. Its detector path is the same PMS path used by `plex_pms:pristine` and
`plex_pms:patched`.

`tools/ci/mod-bake-smoke.sh` pulls each supported full
`ghcr.io/linuxserver/plex:<version>` image and exports the PMS path from that
image to a host scratch file before invoking the renderer. The renderer accepts
that file through `--plex-pms-pristine SERVER_ID:ARCH:PATH`. The LSIO base image
alone is not used as the derivation source.

The renderer applies these gates in order:

1. Require exactly one pristine PMS input for every rendered server/arch tuple.
2. Hash the complete input and require equality with the curated
   `plex_pms:pristine` SHA.
3. Require the pool-site baseline SHA to bind to that same pristine detector and
   require the existing two-review site gates.
4. Read the 84-byte OLD id from `pins/library-compat-groups.tsv` and decode the
   84-byte NEW id from canonical `SQLITE_SOURCE_ID` in `pins/versions.env`;
   require one OLD and zero NEW matches in the pristine input.
5. Call `plex_patch_populate_pms_tmp` so the build-time output uses the same pool
   and source-id byte transform as runtime.
6. Require zero OLD and one NEW match in the output, hash the complete output,
   and emit that SHA as the exact `plex_pms:source-id-patched` detector row.

The full pristine SHA check occurs before the shared transform. A computed SHA
therefore proves which exact bytes the current shared runtime transform produces
from the pinned pristine input, reviewed pool sites, and pinned NEW id. It does
not independently prove the semantic correctness of the reviewed offsets, the
publisher identity behind the pulled image, runtime ownership restoration, or
atomic replacement; the curated anchors, review evidence, runtime tests, and CI
smoke own those separate claims.

The A/B procedure below is an optional independent audit of the transform. It is
not a curation step and its final SHA is not written to a pin file.

Read the source ids from their runtime authorities and require exact lengths:

```bash
source_id_old="$(awk -F '\t' '
  NR == 1 {
    for (i = 1; i <= NF; i++) if ($i == "sqlite_source_id_guard") guard_col = i
    next
  }
  $2 == "plex" { source_id = $guard_col; plex_rows++ }
  END {
    if (guard_col > 0 && plex_rows == 1 && length(source_id) == 84) print source_id
    else exit 1
  }
' pins/library-compat-groups.tsv)" || {
  printf 'STOP: invalid OLD Plex source id\n' >&2
  exit 1
}

source_id_new_encoded="$(awk -F= '
  $1 == "SQLITE_SOURCE_ID" {
    value = substr($0, index($0, "=") + 1)
    count++
  }
  END {
    if (count == 1 && value != "") print value
    else exit 1
  }
' pins/versions.env)" || {
  printf 'STOP: missing or ambiguous NEW Plex source-id pin\n' >&2
  exit 1
}
[[ "$source_id_new_encoded" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}%20[0-9]{2}:[0-9]{2}:[0-9]{2}%20[0-9a-f]{64}$ ]] || {
  printf 'STOP: malformed NEW Plex source-id pin\n' >&2
  exit 1
}
source_id_new="${source_id_new_encoded//%20/ }"
[ "${#source_id_new}" -eq 84 ] || {
  printf 'STOP: invalid NEW Plex source id\n' >&2
  exit 1
}
```

Lane A starts from the independently derived pool-only PMS output and performs
one checked length-preserving replacement:

```bash
python3 - \
  "$copy_dir/Plex Media Server.patched.A" \
  "$source_id_old" \
  "$source_id_new" \
  "$copy_dir/Plex Media Server.source-id-patched.A" <<'PY'
import pathlib
import sys

pool_post_path = pathlib.Path(sys.argv[1])
source_id_old = sys.argv[2].encode("ascii")
source_id_new = sys.argv[3].encode("ascii")
output_path = pathlib.Path(sys.argv[4])

if len(source_id_old) != 84 or len(source_id_new) != 84:
    raise SystemExit("STOP: source ids must both be 84 bytes")

patched = bytearray(pool_post_path.read_bytes())
if patched.count(source_id_old) != 1:
    raise SystemExit("STOP: pool-patched PMS must contain exactly one OLD source id")
if patched.count(source_id_new) != 0:
    raise SystemExit("STOP: pool-patched PMS already contains NEW source id")

source_id_offset = patched.index(source_id_old)
patched[source_id_offset:source_id_offset + 84] = source_id_new
if patched.count(source_id_old) != 0 or patched.count(source_id_new) != 1:
    raise SystemExit("STOP: source-id replacement postcondition failed")

output_path.write_bytes(patched)
PY
```

Lane B starts again from the pristine PMS and uses the shared runtime
temp-population function to apply the pool sites and NEW source id together:

```bash
. lsio-mods/shared/cont-init-fragments/plex-patch.sh

old_matches="$(LC_ALL=C grep -aobF "$source_id_old" \
  "$copy_dir/Plex Media Server" 2>/dev/null || true)"
old_count="$(printf '%s\n' "$old_matches" | awk 'NF { count++ } END { print count + 0 }')"
[ "$old_count" -eq 1 ] || {
  printf 'STOP: pristine PMS must contain exactly one OLD source id\n' >&2
  exit 1
}
source_id_offset="${old_matches%%:*}"
mapfile -t site_strings <"$site_file"

plex_patch_populate_pms_tmp \
  "$copy_dir/Plex Media Server.source-id-patched.B" \
  "$copy_dir/Plex Media Server" \
  "$source_id_offset" \
  "$source_id_new" \
  "${site_strings[@]}" || {
    printf 'STOP: shell PMS source-id-patched lane failed\n' >&2
    exit 1
  }

source_id_patched_sha_a="$(sha256_of \
  "$copy_dir/Plex Media Server.source-id-patched.A")"
source_id_patched_sha_b="$(sha256_of \
  "$copy_dir/Plex Media Server.source-id-patched.B")"
[ "$source_id_patched_sha_a" = "$source_id_patched_sha_b" ] || {
  printf 'STOP: PMS source-id-patched A/B mismatch A=%s B=%s\n' \
    "$source_id_patched_sha_a" "$source_id_patched_sha_b" >&2
  exit 1
}
[ "$source_id_patched_sha_a" != "$patched_sha_a" ] || {
  printf 'STOP: PMS final SHA unexpectedly equals pool-only SHA\n' >&2
  exit 1
}
```

For an independent audit, require both lanes to match the renderer-emitted SHA.
Do not record that SHA in `pins/runtime-baselines.tsv`; the next render recomputes
it from the verified image bytes.

The required production render-input set is:

| Server ID | Arch | Renderer input |
|---|---|---|
| `plex-1.43.1` | `linux-x86_64-v2` | `--plex-pms-pristine` |
| `plex-1.43.1` | `linux-x86_64-v3` | `--plex-pms-pristine` |
| `plex-1.43.1` | `linux-arm64` | `--plex-pms-pristine` |
| `plex-1.43.2` | `linux-x86_64-v2` | `--plex-pms-pristine` |
| `plex-1.43.2` | `linux-x86_64-v3` | `--plex-pms-pristine` |
| `plex-1.43.2` | `linux-arm64` | `--plex-pms-pristine` |

These six pristine PMS inputs are supplied from the two pulled full Plex images
by the amd64 and arm64 bake-smoke jobs. The renderer fails closed when any input
is absent or does not match its curated pristine baseline. Test fixtures create
real byte inputs and independently construct the expected final bytes; they do
not curate the derived role.

At runtime, `plex_patch_apply_pms` accepts either pristine pool bytes verified
against `expected_pre_sha` or pool-patched bytes verified against
`expected_pool_post_sha` with a verified pristine backup. One
`sqlite3_mod_atomic_replace` call applies or re-verifies the pool-site writes
and writes and reads back the NEW id on the same temporary copy before
replacing PMS once. The selector accepts the resulting exact SHA only through
`plex_pms:source-id-patched`.

## No-Fabrication

Never invent, approximate, or backfill a SHA from memory.

- If a required curated `detect`, `pre`, `icu-runtime`, or `pool-site` source value is
  missing, STOP and report the missing row.
- If the extracted file does not match the recorded pristine baseline, STOP and
  report the mismatch.
- If a pool-only SHA is not reproduced byte-for-byte by both derivation lanes,
  STOP and report the mismatch.
- Never add a real, guessed, or placeholder `plex_pms:source-id-patched` SHA to
  `pins/runtime-baselines.tsv`; manifest rendering computes it from verified
  pristine bytes.

Recorded values live in `pins/runtime-baselines.tsv`,
`pins/plex-patch-pool-sites.tsv`, and
`docs/plex-pool-patch-derivation.md`.
