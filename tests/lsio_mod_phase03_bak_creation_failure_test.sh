#!/usr/bin/env bash
set -euo pipefail

tmp_parent="${TMPDIR:-/tmp}"
tmp_root="$(mktemp -d "${tmp_parent%/}/sqlite3-phase03.XXXXXX" 2>/dev/null || mktemp -d /tmp/sqlite3-phase03.XXXXXX)"
cleanup() {
  chmod -R u+w "$tmp_root" 2>/dev/null || true
  rm -rf "$tmp_root"
}
trap cleanup EXIT

fixture="$tmp_root/fixture"
lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
artifact_dir="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64/icu69"
runtime_root="$fixture/usr/lib/plexmediaserver"
runtime_lib_dir="$runtime_root/lib"
pms_path="$runtime_root/Plex Media Server"
scanner_path="$runtime_root/Plex Media Scanner"
detector_sha_root="$tmp_root/detector-sha"
mkdir -p "$lib_root" "$artifact_dir" "$runtime_lib_dir" "$detector_sha_root"

cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
cp lsio-mods/shared/cont-init-fragments/manifest-parser.sh "$lib_root/manifest-parser.sh"
cp lsio-mods/shared/cont-init-fragments/selector.sh "$lib_root/selector.sh"
cp lsio-mods/shared/cont-init-fragments/atomic-write.sh "$lib_root/atomic-write.sh"
cp lsio-mods/shared/cont-init-fragments/swap.sh "$lib_root/swap.sh"

cat > "$artifact_dir/libsqlite3.so" <<'EOF_ARTIFACT'
sqlite-current
EOF_ARTIFACT
cat > "$runtime_lib_dir/libsqlite3.so" <<'EOF_BASELINE'
sqlite-baseline
EOF_BASELINE
cat > "$pms_path" <<'EOF_PMS'
plex pms pristine
EOF_PMS
cat > "$scanner_path" <<'EOF_SCANNER'
plex scanner pristine
EOF_SCANNER
cat > "$detector_sha_root/plex-pms-patched" <<'EOF_PMS_PATCHED'
plex pms patched
EOF_PMS_PATCHED
cat > "$detector_sha_root/plex-pms-source-id-patched" <<'EOF_PMS_SOURCE_ID_PATCHED'
plex pms source-id patched
EOF_PMS_SOURCE_ID_PATCHED
cat > "$detector_sha_root/plex-scanner-patched" <<'EOF_SCANNER_PATCHED'
plex scanner patched
EOF_SCANNER_PATCHED
for so in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
  printf 'icu-runtime-%s\n' "$so" > "$runtime_lib_dir/$so"
done

current_sha="$(sha256sum "$artifact_dir/libsqlite3.so" | awk '{print $1}')"
baseline_sha="$(sha256sum "$runtime_lib_dir/libsqlite3.so" | awk '{print $1}')"
pms_pristine_sha="$(sha256sum "$pms_path" | awk '{print $1}')"
scanner_pristine_sha="$(sha256sum "$scanner_path" | awk '{print $1}')"
pms_patched_sha="$(sha256sum "$detector_sha_root/plex-pms-patched" | awk '{print $1}')"
pms_source_id_patched_sha="$(sha256sum "$detector_sha_root/plex-pms-source-id-patched" | awk '{print $1}')"
scanner_patched_sha="$(sha256sum "$detector_sha_root/plex-scanner-patched" | awk '{print $1}')"
icu_uc_sha="$(sha256sum "$runtime_lib_dir/libicuucplex.so.69" | awk '{print $1}')"
icu_i18n_sha="$(sha256sum "$runtime_lib_dir/libicui18nplex.so.69" | awk '{print $1}')"
icu_data_sha="$(sha256sum "$runtime_lib_dir/libicudataplex.so.69" | awk '{print $1}')"

cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
meta|3|release_tag|2026.05.28-r1|generated_at|2026-05-28T00:00:00Z
detect|1|plex|plex-fixture|linux-arm64|plex_pms:pristine|$pms_path|$pms_pristine_sha
detect|1|plex|plex-fixture|linux-arm64|plex_pms:patched|$pms_path|$pms_patched_sha
detect|1|plex|plex-fixture|linux-arm64|plex_pms:source-id-patched|$pms_path|$pms_source_id_patched_sha
detect|1|plex|plex-fixture|linux-arm64|plex_scanner:pristine|$scanner_path|$scanner_pristine_sha
detect|1|plex|plex-fixture|linux-arm64|plex_scanner:patched|$scanner_path|$scanner_patched_sha
artifact|1|plex|plex-fixture|linux-arm64|icu69|artifacts/linux-arm64/icu69/libsqlite3.so|${fixture}/usr/lib/plexmediaserver/lib/libsqlite3.so|$current_sha
pre|2|plex|plex-fixture|linux-arm64|target_sqlite|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libsqlite3.so|$baseline_sha
pre|2|plex|plex-fixture|linux-arm64|plex_icu_linked:libicuucplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicuucplex.so.69|$icu_uc_sha
pre|2|plex|plex-fixture|linux-arm64|plex_icu_linked:libicui18nplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicui18nplex.so.69|$icu_i18n_sha
pre|2|plex|plex-fixture|linux-arm64|plex_icu_linked:libicudataplex.so.69|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicudataplex.so.69|$icu_data_sha
pool-site|1|plex|plex-fixture|linux-arm64|$pms_path|pool-open-flags|0|0|00112233445566778899aabbccddeeff|ffeeddccbbaa99887766554433221100
pool-site|1|plex|plex-fixture|linux-arm64|$scanner_path|pool-open-flags|8|15|11112222333344445555666677778888|88887777666655554444333322221111
EOF_PINS

script_copy="$tmp_root/init-mod-sqlite3-swap-run"
cp lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-swap/run "$script_copy"
python3 - "$script_copy" "$fixture" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
fixture = sys.argv[2]
text = script_path.read_text()
text = text.replace("/opt/sqlite3-lsio-mod", f"{fixture}/opt/sqlite3-lsio-mod")
text = text.replace("/usr/lib/plexmediaserver/lib/", f"{fixture}/usr/lib/plexmediaserver/lib/")
script_path.write_text(text)
PY
chmod +x "$script_copy"

mkdir -p "$tmp_root/bin"
cat > "$tmp_root/bin/uname" <<'EOF_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
EOF_UNAME
chmod +x "$tmp_root/bin/uname"

before_sha="$(sha256sum "$runtime_lib_dir/libsqlite3.so" | awk '{print $1}')"
chmod 0555 "$runtime_lib_dir"
set +e
PATH="$tmp_root/bin:$PATH" bash "$script_copy" > "$tmp_root/phase82.log" 2>&1
rc=$?
set -e
chmod 0755 "$runtime_lib_dir"
after_sha="$(sha256sum "$runtime_lib_dir/libsqlite3.so" | awk '{print $1}')"

[ "$rc" -eq 0 ] || {
  echo "FATAL: expected exit 0, got $rc" >&2
  exit 1
}
[ "$before_sha" = "$baseline_sha" ] || {
  echo "FATAL: fixture target did not start at bundled baseline SHA" >&2
  exit 1
}
[ "$after_sha" = "$before_sha" ] || {
  echo "FATAL: target SHA changed after backup creation failure" >&2
  exit 1
}
grep -Eq 'event=(bak-create-failed|bak-write-failed)' "$tmp_root/phase82.log" || {
  echo "FATAL: expected backup creation warning in phase82 log" >&2
  cat "$tmp_root/phase82.log" >&2
  exit 1
}
if find "$runtime_lib_dir" -maxdepth 1 -name 'libsqlite3.so.sqlite3-mod.*' -print -quit | grep -q .; then
  echo "FATAL: temp install file was written on backup creation failure" >&2
  exit 1
fi

printf 'phase03 backup creation failure test passed\n'
