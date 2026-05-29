#!/usr/bin/env bash
set -euo pipefail

find lsio-mods -type f \( -name '*.sh' -o -name '80-sqlite3-mod-*' -o -name '81-sqlite3-mod-*' -o -name '82-sqlite3-mod-*' -o -name '83-sqlite3-mod-*' \) -print -exec bash -n {} \;
bash -n tools/stage-lsio-mod.sh

for script in \
  lsio-mods/plex/root-fs/etc/cont-init.d/80-sqlite3-mod-preflight \
  lsio-mods/plex/root-fs/etc/cont-init.d/81-sqlite3-mod-verify \
  lsio-mods/plex/root-fs/etc/cont-init.d/82-sqlite3-mod-swap \
  lsio-mods/plex/root-fs/etc/cont-init.d/83-sqlite3-mod-pool-patch \
  lsio-mods/emby/root-fs/etc/cont-init.d/80-sqlite3-mod-preflight \
  lsio-mods/emby/root-fs/etc/cont-init.d/81-sqlite3-mod-verify \
  lsio-mods/emby/root-fs/etc/cont-init.d/82-sqlite3-mod-swap
do
  first_line="$(sed -n '1p' "$script")"
  [ "$first_line" = '#!/usr/bin/with-contenv bash' ] || {
    echo "FATAL: wrong shebang in $script: $first_line" >&2
    exit 1
  }
done

if rg -n '(^|[^A-Za-z0-9_])(PLEX_POOL_PATCH_|SQLITE3_LSIO_MOD_|TMPDIR|PUID|PGID)([^A-Za-z0-9_]|$)' lsio-mods; then
  echo "FATAL: mod runtime contains forbidden custom env-var surface" >&2
  exit 1
fi
if rg -n '(>|mv |cp |rm |dd ).*libicu.*plex\.so\.69|libicu.*plex\.so\.69.*(>|mv |cp |rm |dd )' lsio-mods; then
  echo "FATAL: mod source writes Plex ICU runtime files" >&2
  exit 1
fi
if rg -n '(^|[^-])pins[.](txt|sh)' lsio-mods; then
  echo "FATAL: legacy runtime pin consumer remains" >&2
  exit 1
fi

grep -Fxq 'FROM scratch' lsio-mods/plex/Dockerfile
grep -Fxq 'COPY root-fs /' lsio-mods/plex/Dockerfile
grep -Fxq 'FROM scratch' lsio-mods/emby/Dockerfile
grep -Fxq 'COPY root-fs /' lsio-mods/emby/Dockerfile

printf 'cont-init fragment tests passed\n'
