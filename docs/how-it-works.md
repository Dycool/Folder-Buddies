# How it works

Folder Buddies is a zero-knowledge P2P virtual filesystem. The host shares a folder; the client mounts it as a real OS volume. Every byte is end-to-end encrypted with ChaCha20-Poly1305. Cloudflare Workers are used only as blind signaling — they never see your data.

---

## Architecture overview

```
┌──────────────────┐   TCP (ChaCha20-Poly1305)    ┌──────────────────┐
│      Host        │◄────────────────────────────►│     Client       │
│                  │                              │                  │
│     Server       │                              │    RamCache      │
│  (posix I/O)     │                              │  (block cache)   │
│                  │                              │                  │
└──────────────────┘                              └────────┬─────────┘
                                                           │
                                                ┌──────────┴──────────┐
                                                │     Mount layer     │
                                                │   FUSE / ProjFS     │
                                                │   → real volume     │
                                                └─────────────────────┘
```

The host runs a TCP server that serves files from a single folder. The client opens multiple TCP streams, wraps them in a secure channel, and exposes a remote filesystem. The mount layer turns that into a real OS volume — FUSE on Linux/macOS, ProjFS on Windows.

A **RamCache** sits between the mount and the transport, caching metadata, directory listings, and raw data blocks with LRU eviction and sequential read-ahead.

---

## Security model

### End-to-end encryption

All data is encrypted with **ChaCha20-Poly1305** (RFC 8439) — an AEAD cipher that provides both confidentiality and authentication. Per connection, two directional 256-bit keys are derived from the handshake, and each message uses a strictly incrementing 96-bit counter nonce so replay is impossible.

```
handshake → HMAC-SHA256 → master secret
  ├── client→server key
  └── server→client key
```

### The share code

The host never types a password. The connection metadata (IP, port, folder name, 256-bit data-path secret) is sealed into a share code in one of two forms:

**Room code (Cloudflare)** — The code comes in two tiers, told apart purely by total length, and is split into a public lookup half (the Cloudflare KV key) and a secret half that never leaves the client:
- **Read-only (default): 6 chars** = 4-char lookup + 2-char (~13-bit) secret half.
- **Read-write: 16 chars** = 8-char lookup + 8-char (~52-bit) secret half. The host issues this stronger tier automatically whenever it grants write access, since tampering is the higher-stakes capability.

The KV value holds the metadata encrypted under `Argon2id(secret half)`. Cloudflare only stores the lookup key and an opaque blob — it never receives the IP, port, folder name, or secret half. (Because the read-only secret is only ~13 bits, a read-only share is decryptable by anyone who can dump the stored record; use read-write or the offline blob for content that must survive a server compromise.)

If Cloudflare is unreachable or rate-limited, the client transparently falls back to **Firebase Realtime Database** using the same record format, then to the self-contained offline blob.

**Offline blob (self-contained)** — A long Base91 string embedding its own 256-bit key. No server needed at all. Used when Cloudflare is unavailable or when `--secure-hash` is set.

```
read-only 6-char code ──┬── lookup 4 chars → KV key
                        └── secret 2 chars ┐
read-write 16-char code ┬── lookup 8 chars → KV key
                        └── secret 8 chars ┴→ Argon2id → wrap key
                                                  │
                                    decrypts blob key → decrypts token

Offline blob ─── Base91 decode ─── extract blob key ─── decrypt token
```

### libsodium / Argon2id

The secret half of the room code protects the wrapped blob key with **Argon2id** (3 iterations, 64 MiB memory, 16-byte salt). For the read-write tier this makes brute-forcing the ~52-bit secret half expensive while keeping the online path fast for legitimate clients; the read-only tier's 2-char (~13-bit) secret is intentionally light (see the tradeoff note above). The webapp uses the same parameters via `@noble/hashes` so both sides derive identical wrap keys.

---

## Wire protocol

Every connection begins with a plaintext handshake, then all subsequent messages are sealed.

### Handshake

```
Client                          Server
  │                               │
  ├── OP_HELLO ──────────────────►│
  │                               │
  │◄── OP_CHALLENGE (nonceS) ─────┤
  │                               │
  ├── OP_AUTH (nonceC, proof) ───►│
  │                               │
  │◄── OP_AUTH_OK ────────────────┤
  │                               │
```

1. Client sends `OP_HELLO` with the protocol version.
2. Server replies with `OP_CHALLENGE` containing a random nonce.
3. Client proves knowledge of the secret by sending `OP_AUTH` with its own nonce and `HMAC-SHA256(key, nonceC + nonceS)`.
4. Server verifies and replies `OP_AUTH_OK`. Both sides now derive session keys and activate the `SecureChannel`.

### Framing

After activation, every message is framed as:

```
[4 bytes length][AEAD ciphertext: header + payload + 16-byte tag]
```

The header inside the ciphertext:

| Field | Size | Description |
|---|---|---|
| `magic` | 4 bytes | Always `0x46424459` ("FBDY") |
| `op` | 2 bytes | Operation code (see below) |
| `status` | 2 bytes | 0 = success, positive errno on error |
| `req_id` | 8 bytes | Correlates request/response pairs |
| `length` | 4 bytes | Payload length |

### Operation codes

| Code | Operation | Description |
|---|---|---|
| 1 | `OP_HELLO` | Handshake init |
| 2 | `OP_CHALLENGE` | Server nonce |
| 3 | `OP_AUTH` | Client auth proof |
| 4 | `OP_AUTH_OK` | Auth accepted |
| 10 | `OP_GETATTR` | File/directory stat |
| 11 | `OP_READDIR` | List directory |
| 12 | `OP_OPEN` | Open file handle |
| 13 | `OP_READ` | Read file bytes |
| 14 | `OP_WRITE` | Write file bytes |
| 15-27 | `OP_CREATE`, `OP_MKDIR`, etc. | Mutation operations |

### Socket tuning

All data sockets use `TCP_NODELAY` (no Nagle) and 4 MiB send/receive buffers for low-latency bulk transfer on high-BDP networks.

---

## Mount backends

### Linux — kernel FUSE

Uses **libfuse3** (`FUSE_USE_VERSION 31`) with the high-level API. Multi-threaded with `fuse_loop_mt`. Kernel writeback cache and parallel directory operations are enabled when supported. Mounts prefer the normal desktop volume locations (`/media/$USER`, `/run/media/$USER`) and fall back to `$HOME/FolderBuddies`. If the user unmounts/ejects the volume, the FUSE loop exits and Folder Buddies disconnects.

### Windows — ProjFS

Uses the native **Projected File System** shipped with Windows. The Provider pattern callbacks hydrate files on demand: placeholders appear immediately with metadata, content is fetched only when an application reads it. If ProjFS is disabled, the app requests UAC elevation and runs `dism /online /enable-feature /featurename:Client-ProjFS /all /norestart`.

The projected root is exposed as a drive letter (`Z:` through `D:`). If that drive mapping is removed/ejected, Folder Buddies stops the ProjFS provider and disconnects.

### macOS — FUSE-T

macOS uses a **FUSE 3** provider; release builds currently install FUSE-T and use only its FUSE3 API. Mounts prefer `/Volumes/<folder-name>` and fall back to `$HOME/FolderBuddies` when the user cannot create `/Volumes` entries. The client verifies that macOS actually exposed the path as a mounted volume before reporting success, and Finder/OS unmounts disconnect the session.

---

## Cloudflare Worker

The Worker at `databases/cloudflare/worker.mjs` exposes exactly three REST endpoints:

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/create` | Store an opaque sealed record (salt, wrapped key, payload) |
| `GET` | `/room?code=<lookup>` | Fetch the sealed record by the public lookup half |
| `DELETE` | `/room?code=<lookup>` | Delete (requires `X-FB-Owner` credential matching the owner token) |

It also serves:
- `GET /robots.txt` — blocks all crawlers including AI bots.
- `GET /.well-known/security.txt` — security contact and disclosure policy.
- WebSocket upgrades to `WEB_ROOMS` Durable Object for the webapp.

Rate limiting is 5 requests/min per client IP and 30 requests/min per room. Records expire after 30 days via KV `expirationTtl`.

The Worker never sees the unencrypted payload. The native client's `SignalingClient` class talks to these endpoints over HTTPS.

---

## RamCache

The in-memory cache sits between the OS mount and the TCP transport:

| Cache | TTL | Max entries |
|---|---|---|
| Metadata (`getattr`) | 2 s | 8,192 |
| Directory listings | 5 s | 1,024 |
| Negative lookups (ENOENT) | 1 s | 4,096 |
| Data blocks (LRU) | — | Auto-sized from system RAM |

Block cache size is auto-selected from detected RAM (256 MiB on machines ≤ 4 GiB, up to 4 GiB on > 32 GiB). Sequential read-ahead ramps up exponentially to a 32 MiB window. A pool of 2–8 prefetch threads feeds the cache ahead of the application's read cursor. All writes are strict write-through — the host must acknowledge before the write returns to the application.

---

## WebRTC compatibility transport

The native TCP path is always preferred, but **libdatachannel** provides a WebRTC compatibility layer for native↔browser interoperability:

| Path | Transport |
|---|---|
| Native host → native client | Direct TCP (ChaCha20-Poly1305) |
| Browser host → browser client | WebRTC (DTLS) via Cloudflare Durable Objects |
| Native ↔ browser | WebRTC compatibility via libdatachannel |

The feature is auto-detected at CMake configure time. If libdatachannel is found, `FB_HAVE_LIBDATACHANNEL` is set and the compatibility host/client are compiled in. The CLI prints WebRTC availability on startup.

Signaling for compatibility mode reuses the same Cloudflare Worker WebSocket rooms. When the native host detects a WebRTC-compatible connect code (prefixed `FBS2:`, `FBW2O:`, or `FBW2A:`), it falls back to WebRTC data channels instead of TCP.

**Privacy note:** WebRTC data channels encrypt file traffic with DTLS, but compatibility-mode SDP/ICE signaling metadata passes through the signaling backend as plaintext so browser and native can interoperate.

---

## Cross-platform file I/O

File handles on the host side use positioned `pread`/`pwrite` (or `OVERLAPPED` on Windows) so concurrent threads on the same file never interfere. Open flags (`O_RDONLY`, `O_WRONLY`, etc.) are translated to portable wire values so a Linux client can mount a Windows host's folder and vice versa.
