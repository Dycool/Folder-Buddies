<p align="center">
  <img src="client/src/icons/icon.png" alt="Icon" width="128" height="128">
</p>

# Folder Buddies

**Zero-knowledge, cross-platform P2P virtual filesystem — host a folder on one machine, mount it as a real volume on another.**

🔒 **End-to-end encrypted** — Every filesystem byte after the handshake is sealed with ChaCha20-Poly1305.

👥 **No password to type** — Share a short room code or a self-contained offline blob. The client splits, fetches, and unwraps everything locally.

🌐 **Works everywhere** — Native Linux FUSE, Windows ProjFS, macOS FUSE-T, and a browser-based webapp for quick access.

⚡ **Direct P2P TCP** — Cloudflare Workers are used as the primary blind signaling relay, with Firebase as an automatic fallback. The data path stays peer-to-peer.

🔄 **Automatic fallback** — If Cloudflare is rate-limited or unreachable, the client transparently falls back to Firebase Realtime Database, then to the self-contained offline blob.

🖥️ **CLI + GUI** — Both interfaces expose the same host/connect flow.

> **Pre-compiled Binaries Available!**
> You can download the desktop client for Windows, macOS, and Linux directly from the **[Releases](https://github.com/Dycool/Folder-Buddies/releases)** page.
>
> The webapp is published via GitHub Pages — no client install needed for browser-to-browser access.

---

## 🚀 Quick Start

**1. 🔑 Host a folder**
```
folderbuddies host /path/to/folder [--lan] [--port N]
```
This prints either a **room code** (Cloudflare) or a **long offline blob**. Share one with the client. Read-only shares use a short 6-char code; enabling writes (`--write`) issues a stronger 16-char code.

**2. 🔗 Connect using just that code**
```
folderbuddies connect "<room-code-or-offline-blob>" [--mount ~/FolderBuddies] [--conns 4]
```
No password, no account. The client auto-detects the format and fetches the sealed metadata.

**3. 🌐 Or use the webapp**
Open the GitHub Pages URL, host a folder from your browser with the File System Access API, and share the link. Other browsers can browse, preview, and download files over WebRTC.

---

## 🔐 How it works

The host seals connection metadata (IP, port, folder name, 256-bit session secret) with ChaCha20-Poly1305 under a fresh random key. The client never types a password:

| Method | How it works |
|---|---|
| **Room code** | Two tiers by access: read-only **6-char** (4-char lookup + ~13-bit secret) and read-write **16-char** (8-char lookup + ~52-bit secret). The public lookup half → Cloudflare KV; the secret half never leaves the client. The KV value stores metadata wrapped under `Argon2id(secret half)`. |
| **Offline blob** | Self-contained Base91 string with its own 256-bit key. No server needed. |

Cloudflare stores only the lookup half and an opaque encrypted record — never the IP, port, folder name, data-path secret, or secret half of the code.

---

## 🖥️ Mounting backends

| Platform | Backend |
|---|---|
| **Linux** | Native kernel FUSE via `libfuse3`; unmount/eject disconnects the session |
| **Windows** | Native Projected File System (ProjFS), exposed as a drive letter; drive removal disconnects the session |
| **macOS** | FUSE3 provider (FUSE-T supported through its FUSE 3 API); Finder/OS unmount disconnects the session |

---

## 🔨 Building

Requirements: **CMake ≥ 3.21**, a **C++23** compiler, **Qt 6 Widgets + Network**. UPnP is auto-enabled if `miniupnpc` is found.

**Linux**
```
sudo apt-get install -y cmake ninja-build pkg-config qt6-base-dev libfuse3-dev libminiupnpc-dev
cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/folderbuddies
```

**Windows**
```
cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release `
  -DFB_SIGNALING_URL="https://folderbuddies-signaling.<your-subdomain>.workers.dev"
cmake --build build
```

**macOS**
```
brew install cmake ninja qt6 miniupnpc
cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release \
  -DFB_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
cmake --build build
```
Notarize after build with `xcrun notarytool submit && xcrun stapler staple`.

**Cloudflare Worker**
```
cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release \
  -DFB_SIGNALING_URL="https://folderbuddies-signaling.<your-subdomain>.workers.dev"
cmake --build build
```
The Worker files live in `databases/cloudflare/`. Deploy with Wrangler and create a KV namespace bound as `ROOMS`.

---

## 📚 Documentation

Detailed guides are in the `docs/` folder:

* **[Building from Source](docs/building.md)** — Prerequisites, platform-specific build commands, CMake options, and running tests.
* **[How It Works](docs/how-it-works.md)** — Architecture, security model, wire protocol, mount backends, Cloudflare Worker, and caching.


---

## 🔗 References

| Component | Source |
|---|---|
| **Desktop clients** | [Qt 6 Widgets](https://doc.qt.io/qt-6/qtwidgets-index.html) / TCP sockets |
| **Signaling** | [Cloudflare Workers](https://workers.cloudflare.com/) + KV |
| **Cryptography** | [ChaCha20-Poly1305](https://datatracker.ietf.org/doc/html/rfc8439) / [Argon2id](https://datatracker.ietf.org/doc/html/rfc9106) |
| **Filesystem (Linux)** | [libfuse3](https://github.com/libfuse/libfuse) / kernel FUSE |
| **Filesystem (Windows)** | [ProjFS](https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system) |
| **Filesystem (macOS)** | FUSE3 provider / [FUSE-T](https://github.com/macos-fuse-t/fuse-t) |
| **Webapp** | WebRTC / File System Access API |
| **Protocol** | Custom P2P TCP with per-session ChaCha20-Poly1305 sealing |

---

## 🐛 Reporting Issues

Found a bug or have a feature request? Open an issue **[here](https://github.com/Dycool/Folder-Buddies/issues)** with as much detail as possible (OS, reproduction steps, relevant logs).

---

## 📄 License

See the repository license for details.
