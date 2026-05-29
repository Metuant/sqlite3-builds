#!/usr/bin/env bash
set -euo pipefail

tmp_root="$(mktemp -d)"
cleanup() {
  chmod -R u+w "$tmp_root" 2>/dev/null || true
  rm -rf "$tmp_root"
}
trap cleanup EXIT

fixture="$tmp_root/fixture"
lib_root="$fixture/opt/sqlite3-lsio-mod/lib"
artifact_dir="$fixture/opt/sqlite3-lsio-mod/artifacts/linux-arm64"
runtime_lib_dir="$fixture/usr/lib/plexmediaserver/lib"
mkdir -p "$lib_root" "$artifact_dir" "$runtime_lib_dir"

cp lsio-mods/shared/cont-init-fragments/logging.sh "$lib_root/logging.sh"
cp lsio-mods/shared/cont-init-fragments/sha.sh "$lib_root/sha.sh"
cp lsio-mods/shared/cont-init-fragments/arch.sh "$lib_root/arch.sh"
cp lsio-mods/shared/cont-init-fragments/swap.sh "$lib_root/swap.sh"

cat > "$artifact_dir/libsqlite3.so" <<'EOF_ARTIFACT'
sqlite-current
EOF_ARTIFACT
cat > "$runtime_lib_dir/libsqlite3.so" <<'EOF_BASELINE'
sqlite-baseline
EOF_BASELINE
for so in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
  printf 'icu-runtime-%s\n' "$so" > "$runtime_lib_dir/$so"
done

current_sha="$(sha256sum "$artifact_dir/libsqlite3.so" | awk '{print $1}')"
baseline_sha="$(sha256sum "$runtime_lib_dir/libsqlite3.so" | awk '{print $1}')"
icu_uc_sha="$(sha256sum "$runtime_lib_dir/libicuucplex.so.69" | awk '{print $1}')"
icu_i18n_sha="$(sha256sum "$runtime_lib_dir/libicui18nplex.so.69" | awk '{print $1}')"
icu_data_sha="$(sha256sum "$runtime_lib_dir/libicudataplex.so.69" | awk '{print $1}')"

cat > "$fixture/opt/sqlite3-lsio-mod/baked-pins.txt" <<EOF_PINS
version|2|release_tag|2026.05.27-r3
current|1|plex|linux-arm64|sqlite-fixture-library-plex-linux-arm64.so|${fixture}/usr/lib/plexmediaserver/lib/libsqlite3.so|$current_sha
pre|1|plex|linux-arm64|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libsqlite3.so|runtime|$baseline_sha
pre|1|plex|linux-arm64|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicuucplex.so.69|runtime|$icu_uc_sha
pre|1|plex|linux-arm64|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicui18nplex.so.69|runtime|$icu_i18n_sha
pre|1|plex|linux-arm64|lscr.io/linuxserver/plex:fixture|sha256:fixture|${fixture}/usr/lib/plexmediaserver/lib/libicudataplex.so.69|runtime|$icu_data_sha
EOF_PINS

script_copy="$tmp_root/82-sqlite3-mod-swap"
cp lsio-mods/plex/root-fs/etc/cont-init.d/82-sqlite3-mod-swap "$script_copy"
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
