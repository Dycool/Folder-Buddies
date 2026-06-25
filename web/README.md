# Folder Buddies Web

Static browser client for GitHub Pages. The UI mirrors the native desktop app:
a window with two tabs, **Share a folder** and **Connect to a share**, and a
status bar showing live transfer rates.

- **Share a folder** — pick a folder, optionally cap the number of clients, then
  press *Start sharing*. The app claims a free 6-character room on the relay and
  shows the **connect code** (and a shareable link). There is no password.
  *Copy all* copies the code and link.
- **Connect to a share** — paste the connect code (or share link), press
  *Connect & browse*, and the remote file explorer opens inline. Selected files
  stream over a WebRTC DataChannel.

The 6-character code is split into a public 2-char relay-room half and a secret
4-char half. The browser derives the WebRTC signaling-encryption key from the
secret half with Argon2id (vendored, audited `@noble/hashes` in `web/vendor/`),
so Cloudflare relays only opaque encrypted offers/answers and never learns the
addresses exchanged. Note that the WebRTC DataChannel's own DTLS uses the
browser's classical key exchange, which is not post-quantum.

Hosting does not pre-cache or copy the selected folder. The browser keeps the
user-granted `FileSystemDirectoryHandle`, lists directories on demand, and streams
requested file chunks only when downloaded.

The browser cannot create a real OS mount point. Use the native app for Linux
FUSE, Windows ProjFS, and macOS FUSE-T.
