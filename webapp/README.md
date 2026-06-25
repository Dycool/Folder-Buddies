# Folder Buddies Web

Static browser client for GitHub Pages. The UI has two tabs, **Host** and
**Connect**, plus a status bar showing live transfer rates.

- **Host** — pick a folder, optionally allow writes, optionally cap the number
  of clients, then press *Host*. The app claims a free room on the primary
  Cloudflare relay and shows the **connect code** (a short 6-char read-only code,
  or a stronger 16-char code when you allow writes).
- **Connect** — paste the connect code or share link, press *Connect & browse*,
  and the remote file explorer opens inline. Selected files stream over a WebRTC
  DataChannel.

The code is split into a public relay-room (lookup) half and a secret half that
never leaves the browser: read-only codes are 4-char lookup + 2-char (~13-bit)
secret, read-write codes are 8-char lookup + 8-char (~52-bit) secret. The browser
derives the WebRTC signaling-encryption key from the secret half with Argon2id
(vendored, audited `@noble/hashes` in `webapp/vendor/`),
so relays see only opaque encrypted offers/answers and never learn the addresses
exchanged. Note that the WebRTC DataChannel's own DTLS uses the browser's
classical key exchange, which is not post-quantum.

## Automatic signaling fallback

The browser tries signaling in this order:

1. **Cloudflare Worker WebSocket** — the normal room flow.
2. **Firebase Realtime Database** — optional automatic fallback if the public
   Firebase web config is present in `webapp/config.js` / GitHub Pages variables.
3. **Manual offline WebRTC offer/answer** — giant copy/paste codes, used only if
   both automatic signaling backends fail.

Firebase is only a signaling fallback. It stores transient encrypted signaling
messages under `webRooms/*`; file data still flows through the WebRTC
DataChannel. To enable it on GitHub Pages, set these public repository variables:

- `FIREBASE_API_KEY`
- `FIREBASE_AUTH_DOMAIN`
- `FIREBASE_DATABASE_URL`
- `FIREBASE_PROJECT_ID`
- `FIREBASE_APP_ID`

Minimum test-mode-ish Realtime Database rules for the fallback while developing:

```json
{
  "rules": {
    "webRooms": {
      ".read": true,
      ".write": true
    }
  }
}
```

Do not leave a production public database unbounded forever. Add TTL cleanup,
quota monitoring, and App Check/auth hardening before real public usage. See `../databases/firebase-fallback.md` for a full setup checklist and stricter rule example.

## Hosting model

Hosting does not pre-cache or copy the selected folder. The browser keeps the
user-granted `FileSystemDirectoryHandle`, lists directories on demand, and streams
requested file chunks only when downloaded.

The browser cannot create a real OS mount point. Use the native app for Linux
FUSE, Windows ProjFS, and macOS FUSE-T.
