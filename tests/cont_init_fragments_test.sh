#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

find lsio-mods -type f \( -name '*.sh' -o -name run \) -print -exec bash -n {} \;
bash -n tools/lsio-mod/stage-lsio-mod.sh

for script in \
  lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/run \
  lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-verify/run \
  lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-swap/run \
  lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-poolpatch/run \
  lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/run \
  lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-verify/run \
  lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-swap/run \
  lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-config/run
do
  first_line="$(sed -n '1p' "$script")"
  [ "$first_line" = '#!/usr/bin/with-contenv bash' ] || {
    echo "FATAL: wrong shebang in $script: $first_line" >&2
    exit 1
  }
done

assert_oneshot() {
  mod="$1"
  service="$2"
  dep="$3"
  root="lsio-mods/${mod}/root-fs/etc/s6-overlay/s6-rc.d"
  service_dir="${root}/${service}"
  [ "$(cat "${service_dir}/type")" = 'oneshot' ] || {
    echo "FATAL: wrong s6-rc type for ${mod}/${service}" >&2
    exit 1
  }
  [ "$(cat "${service_dir}/up")" = "/etc/s6-overlay/s6-rc.d/${service}/run" ] || {
    echo "FATAL: wrong s6-rc up command for ${mod}/${service}" >&2
    exit 1
  }
  [ -f "${service_dir}/dependencies.d/${dep}" ] || {
    echo "FATAL: missing dependency ${dep} for ${mod}/${service}" >&2
    exit 1
  }
  [ -f "${root}/user/contents.d/${service}" ] || {
    echo "FATAL: missing user contents marker for ${mod}/${service}" >&2
    exit 1
  }
}

assert_oneshot emby init-mod-sqlite3-preflight init-mods
assert_oneshot emby init-mod-sqlite3-verify init-mod-sqlite3-preflight
assert_oneshot emby init-mod-sqlite3-swap init-mod-sqlite3-verify
assert_oneshot emby init-mod-sqlite3-config init-mod-sqlite3-swap
[ -f lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mods-end/dependencies.d/init-mod-sqlite3-config ] || {
  echo "FATAL: missing emby init-mods-end dependency marker" >&2
  exit 1
}

assert_oneshot plex init-mod-sqlite3-preflight init-mods
assert_oneshot plex init-mod-sqlite3-verify init-mod-sqlite3-preflight
assert_oneshot plex init-mod-sqlite3-swap init-mod-sqlite3-verify
assert_oneshot plex init-mod-sqlite3-poolpatch init-mod-sqlite3-swap
[ -f lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mods-end/dependencies.d/init-mod-sqlite3-poolpatch ] || {
  echo "FATAL: missing plex init-mods-end dependency marker" >&2
  exit 1
}

if find lsio-mods -path '*/root-fs/etc/cont-init.d/*' -print -quit | grep -q .; then
  echo "FATAL: legacy cont-init.d phase script remains" >&2
  exit 1
fi

if grep -rEn '(^|[^A-Za-z0-9_])(PLEX_POOL_PATCH_|SQLITE3_LSIO_MOD_|TMPDIR|PUID|PGID)([^A-Za-z0-9_]|$)' lsio-mods; then
  echo "FATAL: mod runtime contains forbidden custom env-var surface" >&2
  exit 1
fi
grep -Fq 'require_all_or_warn awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname' lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/run
grep -Fq 'require_all_or_warn awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname' lsio-mods/emby/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-preflight/run
grep -Fq 'require_all_or_warn awk chmod chown cp dd grep mktemp mv od printf rm sha256sum stat tr uname' lsio-mods/plex/root-fs/etc/s6-overlay/s6-rc.d/init-mod-sqlite3-poolpatch/run
grep -Fq '| Common phases | `awk chmod chown cp grep mkdir mktemp mv rm sed sha256sum stat tr uname` |' docs/invariants/sqlite3-builds.md
grep -Fq '| Plex pool patch | `dd od printf` |' docs/invariants/sqlite3-builds.md
if grep -rEn '(>|mv |cp |rm |dd ).*libicu.*plex\.so\.69|libicu.*plex\.so\.69.*(>|mv |cp |rm |dd )' lsio-mods; then
  echo "FATAL: mod source writes Plex ICU runtime files" >&2
  exit 1
fi
if grep -rEn 'dd of="\$bin"|dd of="\$target"' lsio-mods; then
  echo "FATAL: mod source writes patched bytes directly into runtime targets" >&2
  exit 1
fi
if grep -rEn '(^|[^-])pins[.](txt|sh)' lsio-mods; then
  echo "FATAL: legacy runtime pin consumer remains" >&2
  exit 1
fi

grep -Fxq 'FROM scratch' lsio-mods/plex/Dockerfile
grep -Fxq 'COPY root-fs /' lsio-mods/plex/Dockerfile
grep -Fxq 'FROM scratch' lsio-mods/emby/Dockerfile
grep -Fxq 'COPY root-fs /' lsio-mods/emby/Dockerfile

printf 's6-rc init-mod fragment tests passed\n'
