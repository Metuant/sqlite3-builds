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

The Plex mod detects the running server from the server-owned `Plex Media
Server` and `Plex Media Scanner` detector files, selects the matching manifest
rows, verifies the staged artifact and runtime baselines, then replaces
`/usr/lib/plexmediaserver/lib/libsqlite3.so`. It verifies Plex ICU runtime files
but never writes them. On amd64 and arm64, it also pool-patches `Plex Media
Server` and `Plex Media Scanner`.

## Emby

Set:

```text
DOCKER_MODS=ghcr.io/<namespace>/linuxserver-mod-sqlite3-emby:<YYYY.MM.DD-rN>
```

The Emby mod detects the running server from the server-owned
`EmbyServer.deps.json` and `EmbyServer.dll` detector files, selects the matching
manifest rows, verifies the staged artifact and runtime baselines, then replaces
the target path from the selected artifact row.

## Version Drift

Running an older mod tag against a newer LSIO base image may warn and skip the
swap. Bump the mod tag to the matching repository release tag.

For troubleshooting, check container stdout for stable event names such as `event=installed`,
`event=skip-already-current`, `event=unknown-target-sha`,
`pool_patch event=patched`, and `pool_patch event=already-patched`.

## Removal

Remove the mod by deleting `DOCKER_MODS` and recreating the container.
The mod preserves a sibling `.bundled.bak` when it creates one and does not
delete it on removal.
