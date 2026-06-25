# Folder Buddies

Folder Buddies is a zero-knowledge, cross-platform P2P virtual filesystem. One
machine hosts a folder; another machine mounts it as a real disk/volume/path.
The data path stays direct P2P TCP and every filesystem byte after the handshake
is sealed with ChaCha20-Poly1305.

Performance and privacy are the priorities: Cloudflare Workers are used only as
blind signaling, never as a relay and never as plaintext storage.

---

## Zero-knowledge signaling

The host generates two separate secrets:

1. a **filesystem session secret** used by the direct P2P encrypted transport;
2. a **strong room password** used to encrypt/decrypt the rendezvous payload.

When Cloudflare signaling is configured, the host publishes:

- KV key: an **exactly 6-character Base91 room code**;
- KV value: an opaque encrypted Base91 payload;
- auth verifier: a password-derived verifier used for request authentication.

The encrypted payload contains the connection metadata the client needs:

- IPv4/IPv6 address;
- port;
- folder display name;
- filesystem session secret.

Cloudflare never receives the plaintext password, IP address, port, folder name,
or filesystem secret. The client performs one `GET /room?code=...` request and
sends a password-derived HMAC proof in `X-FB-Auth`; the Worker returns the opaque
payload only if the proof matches.

### Offline mode

If the Worker URL is not configured, the Worker is down, or the free-tier quota is
exhausted, the app automatically falls back to offline mode. The host prints a
long Base91 blob containing the same encrypted payload. The client auto-detects:

- **6 clean Base91 characters** → Cloudflare `GET /room`;
- **long Base91 string** → local offline decryption.

In both cases the client also needs the room password.

### Cloudflare endpoints

Only these endpoints exist:

- `POST /create`
- `GET /room?code=<room>`
- `DELETE /room?code=<room>`

Rooms expire passively through Cloudflare KV `expirationTtl = 30 days`. There is
no host-alive polling. `GET /room` and `DELETE /room` are rate-limited to 5
requests per minute per client/room pair.

---

## Mounting backend

- **Linux:** native kernel FUSE through `libfuse3`.
- **Windows:** native Microsoft **Projected File System / ProjFS**. On first
  mount, the app checks whether `Client-ProjFS` is enabled. If not, it asks for
  UAC elevation and runs:

  ```powershell
  dism /online /enable-feature /featurename:Client-ProjFS /all /norestart
  ```

  The Windows projection hydrates file data on demand from the P2P stream.
- **macOS:** FUSE-T preferred. Release builds should bundle the FUSE-T installer
  inside the `.app`, then codesign and notarize the final bundle.

---

## Building

Requirements on every platform: **CMake ≥ 3.21**, a **C++23** compiler, **Qt 6
Widgets + Network**. UPnP support is auto-enabled if **miniupnpc** is found.

### Linux

```sh
sudo apt-get install -y cmake ninja-build pkg-config qt6-base-dev \
    libfuse3-dev libminiupnpc-dev
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/folderbuddies
```

### Windows

Use Visual Studio 2022 with the Windows SDK and Qt 6. The app links against the
native Windows SDK `ProjectedFSLib` library.

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release `
  -DFB_SIGNALING_URL="https://folderbuddies-signaling.<your-subdomain>.workers.dev"
cmake --build build
```

### macOS

```sh
brew install cmake ninja qt6 miniupnpc
# Install FUSE-T for development, and pass -DFUSET_PKG for release packaging.
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DFUSET_PKG=/path/to/FUSE-T.pkg \
  -DFB_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
cmake --build build
```

Notarization is done after build with Apple tooling, for example `xcrun notarytool submit` followed by `xcrun stapler staple`.

### Cloudflare Worker

The Worker files live in [`cloudflare/`](cloudflare/). Deploy them with Wrangler,
create a KV namespace bound as `ROOMS`, then rebuild the app with:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DFB_SIGNALING_URL="https://folderbuddies-signaling.<your-subdomain>.workers.dev"
cmake --build build
```

The privacy-sensitive crypto is in the C++23 app. The deployable Cloudflare
Worker has a tiny JavaScript module because Cloudflare exposes HTTP/KV bindings
through the Worker runtime; it stores only opaque strings and performs HMAC
verification.

### Public repo / CI secret safety

This repo is designed to stay public. Do not commit Cloudflare account IDs, API tokens, or KV namespace IDs. Keep `cloudflare/wrangler.toml` with placeholders and configure the real values as GitHub repository secrets:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`
- `CF_KV_ROOMS_ID`
- `CF_KV_ROOMS_PREVIEW_ID`

The Cloudflare workflow creates a temporary ignored `cloudflare/wrangler.ci.toml` at runtime. The `Public Repo Safety` workflow fails CI if obvious secret files or real Cloudflare IDs are accidentally committed.

---

## CLI

```sh
# Host a folder. Prints either a 6-char room code or an offline blob, plus password.
folderbuddies host /path/to/folder [--lan] [--port N] [--max-clients N]

# Connect using a room code or offline blob.
folderbuddies connect "<room-code-or-offline-blob>" --password "<password>" \
  [--mount ~/FolderBuddies] [--conns 4]
```

The GUI exposes the same flow: share tab gives a connect code, password, and
offline fallback; connect tab accepts either the 6-character room code or the
long fallback blob plus password.

## CI / GitHub Actions

The patched workflows live in `.github/workflows/`:

- `build.yml` builds Linux, Windows, and macOS with C++23.
- `build-release.yml` creates a draft release from version tags like `v1.2.3`.
- `cloudflare-worker.yml` validates the Worker and can deploy it with Wrangler.

Optional repository configuration:

- `FB_SIGNALING_URL` repository variable: hardcodes your Worker endpoint into release builds.
- `CLOUDFLARE_ACCOUNT_ID` and `CLOUDFLARE_API_TOKEN` secrets: enable Worker deployment.
- macOS signing/notarization secrets:
  - `MACOS_CERTIFICATE_P12`
  - `MACOS_CERTIFICATE_PASSWORD`
  - `MACOS_CODESIGN_IDENTITY`
  - `APPLE_ID`
  - `APPLE_APP_SPECIFIC_PASSWORD`
  - `APPLE_TEAM_ID`

If the macOS secrets are absent, CI still builds an unsigned `.app` zip. If they are present, the workflow signs, submits with `notarytool`, staples, and then packages the notarized app.

## Webapp / GitHub Pages client

The repository now includes a static browser app in `web/`. It is designed for GitHub Pages and talks to the same Cloudflare Worker for zero-knowledge rendezvous.

Webapp capabilities:

- host a browser-selected folder with the File System Access API;
- generate a private share link containing either a 6-character room code or a huge encrypted offline fallback offer;
- connect from another browser and browse a dedicated remote file explorer;
- stream selected files over WebRTC DataChannel;
- avoid copying the host folder into cache. Directory entries are listed on demand and file bytes are read with `File.stream()` only when downloaded.

The browser app cannot create a real OS mount point. Use the native app for Linux FUSE, Windows ProjFS, and macOS FUSE-T.

To publish the webapp:

1. Set the public repository variable `FB_SIGNALING_URL` to your Worker URL.
2. Enable GitHub Pages with **GitHub Actions** as the source.
3. Run or push to trigger `.github/workflows/webapp.yml`.

No GitHub Pages secret is needed. The Worker URL is public by design; Cloudflare account IDs, API tokens, and KV namespace IDs must stay in GitHub Secrets only.
