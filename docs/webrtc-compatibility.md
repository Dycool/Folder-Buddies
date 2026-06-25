# WebRTC compatibility transport

Folder Buddies keeps the native path as the fast path:

1. Native host + native client: direct TCP is tried first.
2. If a native client cannot use the native TCP share, it can fall back to the WebRTC compatibility transport when Folder Buddies is built with libdatachannel.
3. Browser host + browser client: regular browser WebRTC stays unchanged.
4. Browser host + native client, and native host + browser client: WebRTC compatibility uses the same browser data-channel protocol so the two app types can interoperate.

This transport is intentionally a compatibility layer, not a replacement for the native protocol. It is slower than native TCP for filesystem mounts because reads may need to cache full files and writes are uploaded when the native file handle closes.

## Build dependency

The feature is compiled automatically when CMake can find libdatachannel:

```bash
cmake -S . -B build -DFB_SIGNALING_URL="https://your-worker.workers.dev"
cmake --build build
```

If CMake prints `WebRTC compatibility: disabled`, install libdatachannel through your package manager, vcpkg, Conan, or a system CMake package, then configure again.

## Privacy note

WebRTC data channels encrypt file traffic with DTLS. In the current compatibility implementation, SDP/ICE signaling for native↔browser compatibility is sent as plain signaling payloads through the configured signaling backend so browser and native can interoperate without a second crypto implementation. That exposes connection metadata such as ICE candidates to the signaling backend in compatibility mode.

The normal native↔native TCP protocol and browser↔browser encrypted signaling paths are unchanged. A future hardening step should add the same sealed signaling format to the native WebRTC compatibility path.

## Current fallback order

For native TCP rooms, the existing order remains:

1. Cloudflare room lookup/publish.
2. Firebase Realtime Database room lookup/publish, when configured.
3. Long offline code.

For the new native↔browser WebRTC compatibility transport, this first implementation uses the Cloudflare WebSocket signaling room. Firebase-backed WebRTC compatibility signaling is a separate follow-up because native needs a realtime listener/polling relay equivalent to the browser Firebase relay.
