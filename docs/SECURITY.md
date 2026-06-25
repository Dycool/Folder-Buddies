# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | ✅        |
| < 1.0   | ❌        |

Security fixes are released against the latest `1.0.x` line. Always run the most
recent release from the [Releases](https://github.com/Dycool/Folder-Buddies/releases)
page.

## Reporting a vulnerability

**Please do not open a public issue for security reports.**

Report privately through GitHub Security Advisories:
**https://github.com/Dycool/Folder-Buddies/security/advisories/new**

Include, where possible:

- Affected component (desktop client, webapp, Cloudflare Worker, Firebase rules).
- Version / commit and platform (Linux / macOS / Windows / browser).
- Reproduction steps and impact.
- Any proof-of-concept.

We aim to acknowledge reports within 7 days and to ship a fix or mitigation for
confirmed high-severity issues as quickly as practical. Please allow a reasonable
disclosure window before publishing details.

## Security model (summary)

- **Transport.** After the handshake, every filesystem message on the P2P link is
  sealed with ChaCha20-Poly1305 (RFC 8439) using per-connection directional keys
  and a strictly increasing counter nonce. Authentication is an HMAC-SHA-256
  challenge-response over both peers' random nonces; the same secret derives the
  session keys, so a passive eavesdropper without the code can neither connect nor
  read traffic.
- **Blind signaling.** Cloudflare only ever stores the public *lookup* half of a
  share code plus an opaque record. The secret half never reaches the server; the
  record is wrapped under `Argon2id(secret half)` (64 MiB, t=3). Cloudflare cannot
  decrypt the IP, port, folder name, or data-path secret. Firebase fallback and
  the offline blob carry the same opaque, client-sealed payloads.
- **Path confinement.** The host server refuses `..` path components and resolves
  every request under the shared root. Shares are read-only unless the host
  explicitly enables writes.
- **No telemetry / no accounts.** No passwords are typed and no credentials are
  stored server-side beyond the opaque room record, which expires (default 30 days).

See [how-it-works.md](how-it-works.md) for the full protocol and architecture.

## Known limitations / hardening tradeoffs

These are deliberate design tradeoffs, documented so you can choose appropriately:

- **Short 10-character codes have a ~52-bit secret.** The secret half is 8 Base91
  characters, hardened with Argon2id and bounded by the room TTL. The public
  lookup half is only 2 characters and therefore enumerable, so the secret half is
  what resists an offline brute-force. At ~52 bits behind Argon2id this is a hard
  target, but **for the strongest protection use the native client's "Secure hash
  code" option or the self-contained offline blob**, which carry a full 256-bit
  secret.
- **Folder name during the native handshake.** The connecting client sends only a
  SHA-256 of the share's folder name in the initial (pre-encryption) `HELLO`
  message — never the literal name — so a passive observer cannot read it off the
  wire. A guessable name could still be confirmed by hashing a candidate, so treat
  highly sensitive folder names accordingly. All file contents and metadata after
  the handshake are encrypted.
- **Trust in the host's own content.** A host shares its own folder; symlinks that
  the host itself placed inside the share are followed on the host. Only share
  folders whose contents you trust.

## Scope

In scope: the desktop client, the webapp, the Cloudflare Worker, and the Firebase
security rules in this repository. Out of scope: vulnerabilities in third-party
dependencies (report those upstream), and attacks that require the host to share a
folder containing attacker-controlled content the host does not trust.
