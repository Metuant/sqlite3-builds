# Plex Pool-Patch Derivation

This document records the current derivation method and site data for the Plex
ConnectionPool pool-size patch. The runtime patch lowers each supported
ConnectionPool size immediate from 20 to 16 in `Plex Media Server` and
`Plex Media Scanner`.

## Method

Derivation starts from the RTTI name for
`__shared_ptr_emplace<ConnectionPool>`. The typeinfo reference leads to the
ConnectionPool vtable, the setup helper, and then the caller that passes the
pool size into the constructor path. The patch site is the caller-side load of
the pool-size immediate:

- amd64: `mov esi,#20`, patched to `mov esi,#16`.
- arm64: `movz w1,#20`, patched to `movz w1,#16`.

The file offset is derived from the runtime virtual address by subtracting the
binary's load delta:

| Arch | VA to file-offset delta |
|---|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `0x1000` |
| `linux-arm64` | `0x10000` |

Every patch tuple uses:

```text
label|offset|write_seek|original_hex|patched_hex
```

`offset` and `write_seek` are decimal file offsets. `label` is the file offset
in hex. `original_hex` and `patched_hex` are 16-byte contexts. The context must
be unique in the target binary.

## Baselines

The supported image tag is `lscr.io/linuxserver/plex:1.43.2`.

| Arch | Binary | Baseline SHA-256 |
|---|---|---|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Server` | `22861f0c26767eaa2ce0cfcf697bc14b1870d58079c1089c634a1f454b445597` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `8f12a65001a11953740e78f2f023aa785d5f3caf269da951b7d75b8b45c7785e` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Server` | `6f6aaac01e0a226310ee5475976d10e39721047fdfa6373249449e1a7eb0cd52` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `9ae3a009471f2411a57fb0c29bafd9e2c60592dba56d8282467a1a6c0e7b9332` |

## Patch Sites

| Arch | Binary | Tuple |
|---|---|---|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Server` | `0xb175e7|11630055|11630056|be140000004c89ff41b800000000506a|be100000004c89ff41b800000000506a` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x37d4b7|3658935|3658936|be140000004c89ff41b800000000506a|be100000004c89ff41b800000000506a` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x3997bf|3774399|3774400|be140000004889df41b80000000068d0|be100000004889df41b80000000068d0` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Server` | `0xa18fd4|10588116|10588116|81028052e00313aae4031f2ae7031f2a|01028052e00313aae4031f2ae7031f2a` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x33ad9c|3386780|3386780|81028052e00313aae4031f2ae7031f2a|01028052e00313aae4031f2ae7031f2a` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x3575f8|3503608|3503608|81028052e00315aae4031f2ae7031f2a|01028052e00315aae4031f2ae7031f2a` |

## Exclusions

These pool-size-2 sites are for a different database and must not be patched:

| Arch | Binary | File offset | VA |
|---|---|---:|---:|
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Server` | `0xaf660b` | `0xaf760b` |
| `linux-x86_64-v2` / `linux-x86_64-v3` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x372be9` | `0x373be9` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Server` | `0x9f7868` | `0xa07868` |
| `linux-arm64` | `/usr/lib/plexmediaserver/Plex Media Scanner` | `0x32f6d0` | `0x33f6d0` |

## Runtime Mechanism

The runtime patcher receives the target binary path, expected baseline SHA, and
one or more site tuples from the Phase 04 Plex pool-patch oneshot. It reads
exactly 16 bytes at each tuple offset. A binary is patchable only when every
site matches `original_hex` and the current binary SHA matches the `pool-pre`
baseline row. If every site already matches `patched_hex`, the patcher logs
`pool_patch event=already-patched` and skips the binary.

For each original site, the patcher computes `write_index` as
`write_seek - offset`, selects the one output byte from `patched_hex`, writes
that byte to a same-filesystem temporary copy with `dd conv=notrunc`, and
verifies the 16-byte context. It preserves the `.bundled.bak` backup, restores
owner and mode metadata on the temp copy, and atomically replaces the target.

Unknown, mixed, mismatched, or write-failed states warn and skip without
modifying the runtime target.
