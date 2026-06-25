# Building from source

Folder Buddies uses **CMake ≥ 3.21** with **Ninja** as the preferred generator. Every platform needs a **C++23** compiler and **Qt 6** (Widgets + Network).

---

## Prerequisites

| Dependency | Required | Notes |
|---|---|---|
| **CMake ≥ 3.21** | Yes | |
| **C++23 compiler** | Yes | GCC 14+, Clang 18+, MSVC 2022 17.12+ |
| **Qt 6** (Widgets + Network) | Yes | |
| **libsodium** | Yes | Argon2id key derivation |
| **FUSE / ProjFS** | Platform | Linux: libfuse3; macOS: FUSE-T; Windows: native |
| **miniupnpc** | Optional | UPnP port mapping |
| **libdatachannel** | Optional | WebRTC compatibility transport |

---

## Linux

```sh
sudo apt-get install -y cmake ninja-build pkg-config qt6-base-dev \
    libfuse3-dev libminiupnpc-dev libsodium-dev

cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/folderbuddies
```

The AppImage is built by CI. To reproduce locally, install `linuxdeploy` and `linuxdeploy-plugin-qt`, then:

```sh
DESTDIR=AppDir cmake --install build --prefix /usr
./linuxdeploy-x86_64.AppImage --appdir AppDir \
  -d client/src/folderbuddies.desktop -i client/src/icons/icon.png \
  --plugin qt --output appimage
```

---

## Windows

Use **Visual Studio 2022** with the Windows SDK and **Ninja**. vcpkg is the recommended package manager.

```pwsh
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install qtbase[widgets]:x64-windows-static miniupnpc:x64-windows-static libsodium:x64-windows-static

cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=path\to\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
```

The Windows backend uses the native **Projected File System (ProjFS)** which ships with the Windows SDK — no extra libraries needed.

---

## macOS

```sh
brew install cmake ninja qt6 miniupnpc libsodium

cmake -G Ninja -S client -B build -DCMAKE_BUILD_TYPE=Release \
  -DFUSET_PKG=/path/to/FUSE-T.pkg
cmake --build build
```

macOS uses **FUSE-T** (preferred) or macFUSE for the mount backend. FUSE-T is auto-detected if installed via Homebrew (`brew install --cask fuse-t`).

### Notarization

```sh
xcrun notarytool submit folderbuddies-macos-notary.zip \
  --apple-id "$APPLE_ID" --password "$APPLE_APP_SPECIFIC_PASSWORD" \
  --team-id "$APPLE_TEAM_ID" --wait
xcrun stapler staple build/folderbuddies.app
```

---

## Cloudflare Worker validation (optional)

The Worker has a standalone CMake project for CI validation:

```sh
cmake -S databases/cloudflare -B build/cloudflare-core \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23
cmake --build build/cloudflare-core
```

---

## CMake options

| Option | Default | Description |
|---|---|---|
| `-DFB_SIGNALING_URL=<url>` | — | Hardcode your Worker endpoint into the build |
| `-DFB_FIREBASE_DATABASE_URL=<url>` | — | Hardcode Firebase fallback endpoint |
| `-DFUSET_PKG=<path>` | — | Bundle a FUSE-T installer inside the macOS `.app` |
| `-DFB_CODESIGN_IDENTITY=<name>` | — | Developer ID identity for macOS codesigning |
| `-DFB_BUILD_TESTS=ON` | ON | Build the crypto + signaling self-tests |
| `-DFB_HAVE_LIBDATACHANNEL` | auto | Enable WebRTC compatibility transport |

---

## Running tests

```sh
cmake --build build --target fb_crypto_selftest
ctest --test-dir build --output-on-failure
```

Two tests are included:
- **crypto_selftest** — AEAD encrypt/decrypt against RFC 8439 vectors.
- **signaling_selftest** — Room code and offline blob round-trips.
