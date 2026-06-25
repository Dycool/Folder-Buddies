# Folder Buddies Web

This is the no-install browser version of Folder Buddies. It is meant to be hosted as a static site on GitHub Pages and uses the Cloudflare Worker only for encrypted WebRTC signaling.

## What it can do

- Host a user-selected folder from the browser with `showDirectoryPicker` / `FileSystemDirectoryHandle`.
- Connect to a 6-character room link through the Cloudflare Worker WebSocket signaling path.
- Use a huge offline fallback link with manual answer copy/paste when Cloudflare is unavailable.
- Browse the remote folder in a dedicated web file explorer.
- Download files on demand over WebRTC DataChannel.

## What it intentionally does not do

- It does not mount a real OS filesystem. Use the native app for FUSE / ProjFS / FUSE-T.
- It does not relay file bytes through Cloudflare or GitHub Pages.
- It does not copy or cache the selected host folder. The host enumerates directories on request and streams each selected file with `File.stream()`.

## Browser notes

Hosting requires the File System Access API, so Chromium/Edge are the best targets. Browsing/downloading as a client works in more browsers, but saving very large files is best when `showSaveFilePicker` is available because chunks can be streamed straight to disk instead of being accumulated in memory as a Blob fallback.

## Link formats

Cloud room links use the URL fragment so the code/password are not sent to GitHub Pages:

```text
https://<user>.github.io/<repo>/#r=<6-char-room>&p=<password>
```

Offline fallback links also use the fragment and carry an encrypted WebRTC offer:

```text
https://<user>.github.io/<repo>/#o=<huge-encrypted-offer>&p=<password>
```

The client creates an encrypted answer blob and sends it back to the host manually.
