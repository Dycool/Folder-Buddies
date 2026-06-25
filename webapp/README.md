# Folder Buddies Web

Static browser client for GitHub Pages. The UI has two tabs, **Host** and
**Connect**, plus a status bar showing live transfer rates.

- **Host** — pick a folder, optionally allow writes, optionally cap the number
  of clients, then press *Host*. The app claims a free 6-character room on the
  primary Cloudflare relay and shows the **connect code**.
- **Connect** — paste the connect code or share link, press *Connect & browse*,
  and the remote file explorer opens inline. Selected files stream over a WebRTC
  DataChannel.

The 6-character code is split into a public 2-char relay-room half and a secret
4-char half. The browser derives the WebRTC signaling-encryption key from the
secret half with Argon2id (vendored, audited `@noble/hashes` in `webapp/vendor/`),
so relays see only opaque encrypted offers/answers and never learn the addresses
exchanged. Note that the WebRTC DataChannel's own DTLS uses the browser's
classical key exchange, which is not post-quantum.

## Automatic signaling fallback

The browser tries signaling in this order:

1. **Cloudflare Worker WebSocket** — the normal 6-character room flow.
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
