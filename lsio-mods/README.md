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
but never writes them. On amd64 and arm64, the Plex patch phase applies
ConnectionPool SITE writes to `Plex Media Server` and `Plex Media Scanner`.
For PMS, it folds those SITE writes and the length-preserving 84-byte OLD-to-NEW
SQLite source-id write into one temp-copy operation and one atomic replace; the
scanner remains pool-only.

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

For troubleshooting, check container stdout for stable swap event names such as
`event=installed`, `event=skip-already-current`, and
`event=unknown-target-sha`.

The Plex patch phase uses two event namespaces:

| Namespace | Coverage | Emitted events |
|---|---|---|
| `pool_patch` | ConnectionPool SITE operations for PMS and the scanner. | `event=missing-backup`, `event=bad-backup-sha`, `event=missing-binary`, `event=no-sites`, `event=already-patched`, `event=unknown-target-state`, `event=pre-sha-mismatch`, `event=backup-create-failed`, `event=existing-backup-untrusted`, `event=invalid-site`, `event=verify-failed`, `event=write-failed`, `event=patched` |
| `plex_patch` | The combined PMS operation, including source-id validation and the source-id fold. | `event=missing-binary`, `event=no-sites`, `event=invalid-source-id-length`, `event=already-patched`, `event=conflicting-source-id-state`, `event=unknown-source-id-state`, `event=source-id-match-count`, `event=invalid-source-id-offset`, `event=unknown-target-state`, `event=pre-sha-mismatch`, `event=post-sha-mismatch`, `event=missing-pristine-backup`, `event=backup-create-failed`, `event=existing-backup-untrusted`, `event=invalid-site`, `event=source-id-write-failed`, `event=source-id-readback-failed`, `event=verify-failed`, `event=write-failed`, `event=atomic-replace-failed`, `event=source-id-patched` |

## Removal

Remove the mod by deleting `DOCKER_MODS` and recreating the container.
The mod preserves a sibling `.bundled.bak` when it creates one and does not
delete it on removal.
