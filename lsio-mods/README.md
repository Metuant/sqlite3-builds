# sqlite3 LSIO Docker Mods

This repository publishes two LSIO Docker mods:

- `ghcr.io/<namespace>/linuxserver-mod-sqlite3-plex:<tag>`
- `ghcr.io/<namespace>/linuxserver-mod-sqlite3-emby:<tag>`

Tags mirror repository CalVer release tags exactly. There is no `latest` tag.

## Plex

Set:

```text
DOCKER_MODS=ghcr.io/<namespace>/linuxserver-mod-sqlite3-plex:<YYYY.MM.DD-rN>
```

The Plex mod replaces `/usr/lib/plexmediaserver/lib/libsqlite3.so` when the
target file's runtime SHA matches the baked baseline row. It verifies Plex ICU
runtime files but never writes them. On amd64, it also pool-patches `Plex Media
Server` and `Plex Media Scanner`. On arm64, the SQLite swap is supported and
the pool patch logs a deferred warning.

## Emby

Set:

```text
DOCKER_MODS=ghcr.io/<namespace>/linuxserver-mod-sqlite3-emby:<YYYY.MM.DD-rN>
```

The Emby mod replaces `/app/emby/lib/libsqlite3.so.3.49.2` when the target
file's runtime SHA matches the baked baseline row.

## Version Drift

Running an older mod tag against a newer LSIO base image may warn and skip the
swap. Bump the mod tag to the matching repository release tag.

For troubleshooting, check container stdout for stable event names such as `event=installed`,
`event=skip-already-current`, `event=unknown-target-sha`,
`event=pool-patch-deferred`, and `pool_patch event=patched`.

## Removal

Remove the mod by deleting `DOCKER_MODS` and recreating the container.
The mod preserves a sibling `.bundled.bak` when it creates one and does not
delete it on removal.
