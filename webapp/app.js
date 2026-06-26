import { argon2id } from "./vendor/noble/argon2.js";

(() => {
  "use strict";

  const BASE91 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
  // Connect codes come in two tiers, told apart purely by total length:
  //   read-only (default): 4-char lookup + 2-char secret half  ( 6 total)
  //   read-write:          8-char lookup + 8-char secret half  (16 total)
  // The host issues the long tier exactly when it grants write access. The lookup
  // is the public Durable Object room name; the secret half never reaches Cloudflare.
  const SHORT_LOOKUP_LEN = 4, SHORT_CODE_LEN = 6;
  const LONG_LOOKUP_LEN = 8, LONG_CODE_LEN = 16;
  const ARGON = { t: 3, m: 65536, p: 1, dkLen: 32 }; // matches libsodium in the native app
  const BIN_MAGIC = 0x4642494e; // FBIN
  const CHUNK_SIZE = 64 * 1024;
  const ICE_TIMEOUT_MS = 8000;
  const CLIENT_FALLBACK_DELAY_MS = 11000;
  const STATS_INTERVAL_MS = 500;
  const DEFAULT_ICE = [{ urls: "stun:stun.l.google.com:19302" }];
  const TEXT_PREVIEW_MAX = 4 * 1024 * 1024;
  const WEB_OFFLINE_OFFER_PREFIX = "FBW2O:";
  const WEB_OFFLINE_ANSWER_PREFIX = "FBW2A:";
  const FIREBASE_SDK_VERSION = "10.12.5";

  const $ = (id) => document.getElementById(id);
  const els = {
    tabs: [...document.querySelectorAll(".tab")],
    pages: [...document.querySelectorAll(".tabpage")],
    folderInput: $("folderInput"),
    browseFolder: $("browseFolder"),
    allowWrites: $("allowWrites"),
    shareToggle: $("shareToggle"),
    connectCode: $("connectCode"),
    copyAll: $("copyAll"),
    copyShareLink: $("copyShareLink"),
    shareStatus: $("shareStatus"),
    offlineAnswerRow: $("offlineAnswerRow"),
    offlineAnswerInput: $("offlineAnswerInput"),
    applyOfflineAnswer: $("applyOfflineAnswer"),
    connectInput: $("connectInput"),
    offlineAnswerOutRow: $("offlineAnswerOutRow"),
    offlineAnswerOut: $("offlineAnswerOut"),
    copyOfflineAnswer: $("copyOfflineAnswer"),
    connectToggle: $("connectToggle"),
    connectStatus: $("connectStatus"),
    explorer: $("explorer"),
    currentPath: $("currentPath"),
    upButton: $("upButton"),
    shareFolderButton: $("shareFolderButton"),
    uploadButton: $("uploadButton"),
    uploadInput: $("uploadInput"),
    uploadFolderButton: $("uploadFolderButton"),
    folderUploadInput: $("folderUploadInput"),
    dropzone: $("dropzone"),
    fileRows: $("fileRows"),
    transfers: $("transfers"),
    statsLabel: $("statsLabel"),
    toast: $("toast"),
    turnstileMount: $("turnstileMount"),
    previewPanel: $("previewPanel"),
    previewTitle: $("previewTitle"),
    previewBody: $("previewBody"),
    closePreview: $("closePreview"),
    savePreview: $("savePreview"),
    unsupportedBrowser: $("unsupportedBrowser"),
  };

  const state = {
    rootHandle: null,
    code: "",          // full connect code (6 read-only / 16 read-write)
    lookup: "",        // public half (relay room)
    secureSecret: "",  // always "" (reserved; the secure-hash long-code option was removed)
    connectToken: "",  // what the host shows/copies to clients
    pendingConnectToken: "", // hidden token loaded from URL hashes/direct links
    rootBase: null,    // HKDF base key derived from the secret half of the code
    allowWrites: false,
    clientCanWrite: false,
    directFilePath: "",
    directFileConsumed: false,
    directFolderPath: "",
    directFolderConsumed: false,
    focusPath: "",
    lastSignalCloseReason: "",
    lastPeerState: "",
    hostWs: null,
    clientWs: null,
    hostPeers: new Map(),
    plainSignalPeers: new Set(),
    manualHost: null,
    manualClient: false,
    clientPeer: null,
    dataChannel: null,
    currentPath: "/",
    refreshTimer: null,
    renderedSig: "",
    renderedPath: "",
    pending: new Map(),
    downloads: new Map(),
    uploads: new Map(),
    clientFallbackTimer: 0,
    nextReqId: 1,
    turnstileWidgetId: null,
    turnstileResolver: null,
    toastTimer: 0,
    bytesServed: 0,
    bytesReceived: 0,
    lastServed: 0,
    lastReceived: 0,
  };

  // ---- RAM-only client cache ------------------------------------------------
  // Makes folder browsing and small previews feel instant without ever touching
  // persistent browser storage. Everything here is plain JS memory: it vanishes
  // on reload, tab close, or disconnect. No IndexedDB / CacheStorage / OPFS /
  // localStorage / service-worker caching of remote file contents — ever.
  const CACHE = (() => {
    const DIR_TTL = 5000, META_TTL = 2000, NEG_TTL = 1000;
    const DIR_MAX = 256, META_MAX = 4096, NEG_MAX = 1024;
    const MiB = 1024 * 1024;
    const TEXT_LIMIT = 4 * MiB, IMAGE_LIMIT = 16 * MiB, PDF_LIMIT = 32 * MiB;

    // Conservative automatic budget; browsers kill tabs that grow unbounded.
    function computeBudget() {
      const dm = navigator.deviceMemory;
      let b = 64 * MiB;
      if (typeof dm === "number" && dm > 0) {
        if (dm <= 4) b = 32 * MiB;
        else if (dm >= 16) b = 128 * MiB;
        else b = 64 * MiB;
      }
      return Math.min(b, 256 * MiB);
    }
    const PREVIEW_BUDGET = computeBudget();

    const dir = new Map();     // path -> { msg, at }
    const meta = new Map();    // path -> { size, mtime, at }
    const neg = new Map();     // path -> at
    const preview = new Map(); // key -> { blob, bytes, mime } (insertion order = LRU)
    const inflight = new Map();// dedup key -> Promise
    let previewBytes = 0;
    const stats = {
      dirHit: 0, dirMiss: 0, metaHit: 0, metaMiss: 0, negHit: 0,
      previewHit: 0, previewMiss: 0, dedup: 0, evictions: 0, bytesFromCache: 0,
    };

    const now = () => Date.now();
    const fresh = (at, ttl) => now() - at <= ttl;
    function bound(map, max) { while (map.size > max) map.delete(map.keys().next().value); }

    // Share an identical in-flight request instead of sending a duplicate.
    function dedup(key, fn) {
      const existing = inflight.get(key);
      if (existing) { stats.dedup++; return existing; }
      const p = Promise.resolve().then(fn).finally(() => inflight.delete(key));
      inflight.set(key, p);
      return p;
    }

    function dirGet(path) {
      const e = dir.get(path);
      if (e && fresh(e.at, DIR_TTL)) { stats.dirHit++; return e.msg; }
      if (e) dir.delete(path);
      stats.dirMiss++;
      return null;
    }
    function dirSet(path, msg) {
      dir.set(path, { msg, at: now() });
      bound(dir, DIR_MAX);
      for (const entry of msg.entries || []) {
        if (entry.kind === "file") {
          meta.set(entry.path, { size: entry.size, mtime: entry.mtime, at: now() });
        }
        neg.delete(entry.path);
      }
      bound(meta, META_MAX);
    }
    function dirInvalidate(path) { dir.delete(normalizePath(path)); }

    function metaGet(path) {
      const e = meta.get(path);
      if (e && fresh(e.at, META_TTL)) { stats.metaHit++; return e; }
      if (e) meta.delete(path);
      stats.metaMiss++;
      return null;
    }
    function negGet(path) {
      const at = neg.get(path);
      if (at && fresh(at, NEG_TTL)) { stats.negHit++; return true; }
      if (at) neg.delete(path);
      return false;
    }

    function previewKey(entry) {
      return `${entry.path} ${entry.size ?? 0} ${entry.mtime ?? 0}`;
    }
    function previewGet(entry) {
      const key = previewKey(entry);
      const v = preview.get(key);
      if (!v) { stats.previewMiss++; return null; }
      preview.delete(key); preview.set(key, v); // touch -> most recently used
      stats.previewHit++; stats.bytesFromCache += v.bytes;
      return v.blob;
    }
    function previewSet(entry, blob, mime) {
      const size = blob.size;
      const limit = mime.startsWith("image/") ? IMAGE_LIMIT
        : mime === "application/pdf" ? PDF_LIMIT
        : (mime.startsWith("audio/") || mime.startsWith("video/")) ? 0
        : TEXT_LIMIT;
      if (limit === 0 || size > limit || size > PREVIEW_BUDGET) return; // stream big/AV instead
      const key = previewKey(entry);
      const prev = preview.get(key);
      if (prev) { previewBytes -= prev.bytes; preview.delete(key); }
      preview.set(key, { blob, bytes: size, mime });
      previewBytes += size;
      while (previewBytes > PREVIEW_BUDGET) {
        const oldest = preview.keys().next().value;
        if (oldest === undefined || oldest === key) break;
        previewBytes -= preview.get(oldest).bytes;
        preview.delete(oldest);
        stats.evictions++;
      }
    }
    function previewInvalidate(path) {
      for (const key of [...preview.keys()]) {
        if (key.startsWith(`${path} `)) { previewBytes -= preview.get(key).bytes; preview.delete(key); }
      }
    }

    // Mutation invalidation (only after the host confirms the operation).
    function afterWrite(path) {
      const p = normalizePath(path);
      meta.delete(p); neg.delete(p);
      previewInvalidate(p);
      dirInvalidate(parentPath(p));
    }
    function afterDelete(path) {
      const p = normalizePath(path);
      meta.delete(p);
      previewInvalidate(p);
      neg.set(p, now()); bound(neg, NEG_MAX);
      dirInvalidate(p);            // in case it was a directory
      dirInvalidate(parentPath(p));
    }

    function clear() {
      dir.clear(); meta.clear(); neg.clear(); preview.clear(); inflight.clear();
      previewBytes = 0;
    }

    function snapshot() {
      return { ...stats, budgetMiB: PREVIEW_BUDGET / MiB, previewBytes, dirEntries: dir.size, previewItems: preview.size };
    }

    return { dedup, dirGet, dirSet, dirInvalidate, metaGet, negGet,
             previewGet, previewSet, previewInvalidate, afterWrite, afterDelete, clear, snapshot };
  })();

  const te = new TextEncoder();
  const td = new TextDecoder();
  let firebaseSdkPromise = null;

  function setShareStatus(text) { els.shareStatus.textContent = text; }
  function setConnectStatus(text) { els.connectStatus.textContent = text; }

  function toast(text) {
    els.toast.textContent = text;
    els.toast.classList.add("show");
    clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(() => els.toast.classList.remove("show"), 2400);
  }

  // The File System Access pickers (showDirectoryPicker / showSaveFilePicker)
  // reject with an AbortError when the user simply dismisses the dialog. That is
  // a normal cancellation, not a failure, so callers swallow it instead of
  // surfacing a confusing "The user aborted a request." toast.
  function isAbortError(e) {
    return e && e.name === "AbortError";
  }

  function clean(value) {
    return String(value || "").trim();
  }

  function signalCloseReason(provider, event) {
    const label = provider === "firebase" ? "Firebase signaling" : "Cloudflare signaling";
    const code = Number(event?.code || 0);
    const reason = clean(event?.reason);
    if (code && reason) return `${label} closed (${code}: ${reason})`;
    if (code) return `${label} closed (${code})`;
    if (reason) return `${label} closed (${reason})`;
    return `${label} closed`;
  }

  function clientDataChannelOpen() {
    return state.dataChannel?.readyState === "open";
  }

  function clientHasOfferOrChannel() {
    return !!(state.dataChannel || state.clientPeer?.pc);
  }

  function clearClientFallbackTimer() {
    clearTimeout(state.clientFallbackTimer);
    state.clientFallbackTimer = 0;
  }

  function describeClientDisconnect(base) {
    const parts = [base];
    const peerState = state.lastPeerState || state.clientPeer?.pc?.connectionState || "";
    if (peerState) parts.push(`peer: ${peerState}`);
    if (state.lastSignalCloseReason) parts.push(`last relay: ${state.lastSignalCloseReason}`);
    return parts.join("; ");
  }

  function setClientDisconnected(reason) {
    const detail = clean(reason) || "connection closed";
    setConnectStatus(`Disconnected: ${detail}.`);
    try { console.warn("Folder Buddies disconnected:", detail); } catch { /* ignore */ }
  }

  function config() {
    return window.FB_WEBAPP_CONFIG || {};
  }

  function workerUrl() {
    const value = clean(config().signalingUrl);
    if (!value) throw new Error("Worker URL is missing. Set FB_SIGNALING_URL in GitHub Actions variables.");
    return value.replace(/\/+$/, "");
  }

  function siteUrl() {
    const base = clean(config().siteUrl) || "https://dycool.github.io/Folder-Buddies/";
    return base.endsWith("/") ? base : base + "/";
  }

  function iceServers() {
    return Array.isArray(config().iceServers) ? config().iceServers : DEFAULT_ICE;
  }

  function firebaseConfig() {
    const raw = config().firebase || config().firebaseSignaling || {};
    return raw && typeof raw === "object" ? raw : {};
  }

  function firebaseEnabled() {
    const f = firebaseConfig();
    return !!(f.apiKey && f.databaseURL && f.projectId);
  }

  function unsupportedBrowserReasons() {
    const missing = [];
    const c = globalThis.crypto;
    if (!c || typeof c.getRandomValues !== "function" || !c.subtle) missing.push("Web Crypto");
    if (!globalThis.RTCPeerConnection) missing.push("WebRTC");
    if (!globalThis.WebSocket) missing.push("WebSockets");
    if (!globalThis.TextEncoder || !globalThis.TextDecoder) missing.push("text encoding");
    if (!globalThis.Blob || !globalThis.URL || typeof URL.createObjectURL !== "function") missing.push("downloads");
    if (!globalThis.URLSearchParams) missing.push("share links");
    return missing;
  }

  function supportsHosting() {
    return typeof window.showDirectoryPicker === "function";
  }

  function supportsFolderUpload() {
    const input = document.createElement("input");
    return typeof window.showDirectoryPicker === "function" || "webkitdirectory" in input;
  }

  function blockUnsupportedBrowser(missing) {
    document.querySelector(".window").hidden = true;
    if (els.unsupportedBrowser) {
      const detail = els.unsupportedBrowser.querySelector("[data-unsupported-detail]");
      if (detail && missing.length) detail.textContent = `Missing: ${missing.join(", ")}.`;
      els.unsupportedBrowser.hidden = false;
    }
  }

  function disableHostTab() {
    const tab = document.querySelector('[data-tab="share"]');
    const page = document.querySelector('[data-page="share"]');
    if (tab) {
      tab.hidden = true;
      tab.disabled = true;
    }
    if (page) page.hidden = true;
    selectTab("connect");
  }

  function firebaseRoomPath(lookup) {
    const safe = btoa(lookup).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
    return `webRooms/${safe}`;
  }

  const FIREBASE_ROOM_MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;

  function firebaseCreatedAtMs(room) {
    const raw = room?.host?.createdAt ?? room?.createdAt;
    if (typeof raw !== "number" || !Number.isFinite(raw) || raw <= 0) return 0;

    // Backward compat:
    // seconds timestamps are around 1,700,000,000
    // ms timestamps are around 1,700,000,000,000
    return raw < 20_000_000_000 ? raw * 1000 : raw;
  }

  function firebaseRoomExpired(room) {
    const createdAt = firebaseCreatedAtMs(room);
    return createdAt <= 0 || createdAt < Date.now() - FIREBASE_ROOM_MAX_AGE_MS;
  }

  async function firebaseDeleteIfExpired(sdk, path, room) {
    if (!firebaseRoomExpired(room)) return false;
    try {
      await sdk.remove(sdk.ref(sdk.db, path));
    } catch {
      // Best-effort cleanup only.
    }
    return true;
  }

  function looksLikeRoom(text) {
    return (text.length === SHORT_CODE_LEN || text.length === LONG_CODE_LEN) &&
      [...text].every((c) => BASE91.includes(c));
  }

  function lookupLenOf(code) { return code.length === LONG_CODE_LEN ? LONG_LOOKUP_LEN : SHORT_LOOKUP_LEN; }
  function lookupOf(code) { return code.slice(0, lookupLenOf(code)); }
  function keyPartOf(code) { return code.slice(lookupLenOf(code)); }

  function randomBytes(n) {
    const b = new Uint8Array(n);
    crypto.getRandomValues(b);
    return b;
  }

  function randomRoom(longCode = false) {
    const n = longCode ? LONG_CODE_LEN : SHORT_CODE_LEN;
    return [...randomBytes(n)].map((b) => BASE91[b % BASE91.length]).join("");
  }

  function concatBytes(...parts) {
    const size = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array(size);
    let off = 0;
    for (const p of parts) { out.set(p, off); off += p.length; }
    return out;
  }

  async function sha256Bytes(bytes) {
    return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  }

  // Derive an HKDF base key from the secret half of the code via Argon2id. The
  // salt is fixed per public half so host and client agree without exchanging it.
  // The secret half never reaches Cloudflare, so the relayed signaling stays blind.
  async function deriveRootBase(code, secureSecret = "") {
    const secret = secureSecret || keyPartOf(code);
    const saltSeed = await sha256Bytes(te.encode("FolderBuddies-web-salt-v1\0" + lookupOf(code)));
    const root = argon2id(te.encode(secret), saltSeed.slice(0, 16), ARGON);
    return crypto.subtle.importKey("raw", root, "HKDF", false, ["deriveKey"]);
  }

  async function deriveAesKey(base, salt, aad) {
    return crypto.subtle.deriveKey(
      { name: "HKDF", hash: "SHA-256", salt, info: te.encode(aad) },
      base,
      { name: "AES-GCM", length: 256 },
      false,
      ["encrypt", "decrypt"]
    );
  }

  function base91Encode(data) {
    let out = "", b = 0, n = 0;
    for (const byte of data) {
      b |= byte << n;
      n += 8;
      if (n > 13) {
        let v = b & 8191;
        if (v > 88) { b >>= 13; n -= 13; }
        else { v = b & 16383; b >>= 14; n -= 14; }
        out += BASE91[v % 91] + BASE91[Math.floor(v / 91)];
      }
    }
    if (n) {
      out += BASE91[b % 91];
      if (n > 7 || b > 90) out += BASE91[Math.floor(b / 91)];
    }
    return out;
  }

  function base91Decode(text) {
    const table = new Map([...BASE91].map((c, i) => [c, i]));
    const out = [];
    let v = -1, b = 0, n = 0;
    for (const ch of text) {
      if (/\s/.test(ch)) continue;
      const c = table.get(ch);
      if (c === undefined) throw new Error("Invalid encrypted text");
      if (v < 0) v = c;
      else {
        v += c * 91;
        b |= v << n;
        n += (v & 8191) > 88 ? 13 : 14;
        do { out.push(b & 255); b >>= 8; n -= 8; } while (n > 7);
        v = -1;
      }
    }
    if (v >= 0) out.push((b | (v << n)) & 255);
    return new Uint8Array(out);
  }


  function encodeWebOffline(prefix, obj) {
    return prefix + base91Encode(te.encode(JSON.stringify(obj)));
  }

  function decodeWebOffline(prefix, text) {
    const cleaned = clean(text);
    if (!cleaned.startsWith(prefix)) throw new Error("Invalid offline web code");
    try {
      return JSON.parse(td.decode(base91Decode(cleaned.slice(prefix.length))));
    } catch (e) {
      throw new Error(`Invalid offline web code: ${e.message}`);
    }
  }

  function resolveConnectToken(raw, options = {}) {
    const codeOrToken = parseJoinInput(raw, options);
    if (looksLikeRoom(codeOrToken)) return { code: codeOrToken, lookup: lookupOf(codeOrToken), secureSecret: "", token: codeOrToken };
    return { code: codeOrToken, lookup: "", secureSecret: "", token: codeOrToken };
  }

  function isWebOfflineOffer(text) {
    return clean(text).startsWith(WEB_OFFLINE_OFFER_PREFIX);
  }

  function isWebOfflineAnswer(text) {
    return clean(text).startsWith(WEB_OFFLINE_ANSWER_PREFIX);
  }

  async function encryptJson(aad, obj) {
    const salt = randomBytes(16);
    const iv = randomBytes(12);
    const key = await deriveAesKey(state.rootBase, salt, aad);
    const plain = te.encode(JSON.stringify(obj));
    const ct = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: te.encode(aad) }, key, plain));
    return base91Encode(concatBytes(te.encode("FBW1"), salt, iv, ct));
  }

  async function decryptJson(aad, blob) {
    const data = base91Decode(blob);
    if (data.length < 48 || td.decode(data.slice(0, 4)) !== "FBW1") throw new Error("Invalid encrypted signal");
    const salt = data.slice(4, 20);
    const iv = data.slice(20, 32);
    const ct = data.slice(32);
    const key = await deriveAesKey(state.rootBase, salt, aad);
    const plain = await crypto.subtle.decrypt({ name: "AES-GCM", iv, additionalData: te.encode(aad) }, key, ct);
    return JSON.parse(td.decode(plain));
  }

  function shareLink(code, path = "", kind = "file") {
    const token = clean(code);
    if (!token) throw new Error("No share code available");
    const params = new URLSearchParams();
    params.set(isWebOfflineOffer(token) ? "o" : "r", token);
    if (path) params.set(kind === "folder" ? "p" : "f", normalizePath(path));
    return `${siteUrl()}#${params.toString()}`;
  }

  function activeShareToken() {
    return state.connectToken || state.code;
  }

  function hostShareLink() {
    return shareLink(clean(els.connectCode?.value) || activeShareToken());
  }

  function clientShareLink(path = state.currentPath || "/", kind = "folder") {
    return shareLink(activeShareToken(), path, kind);
  }

  function hashFragmentFrom(text) {
    const hashAt = String(text || "").indexOf("#");
    return hashAt >= 0 ? String(text).slice(hashAt + 1) : "";
  }

  function currentHashFragment() {
    return hashFragmentFrom(location.href) || (location.hash ? location.hash.slice(1) : "");
  }

  function shareParamsFromFragment(frag) {
    const params = new URLSearchParams(frag);
    return {
      room: params.get("r") || "",
      offer: params.get("o") || "",
      file: params.get("f") || "",
      folder: params.get("p") || params.get("d") || "",
    };
  }

  function parseJoinInput(raw, options = {}) {
    const text = clean(raw);
    const fromHash = (frag) => {
      const { room, offer, file, folder } = shareParamsFromFragment(frag);
      const directFile = file ? normalizePath(file) : "";
      const directFolder = folder ? normalizePath(folder) : "";
      state.directFilePath = directFile && directFile !== "/" ? directFile : "";
      state.directFolderPath = !state.directFilePath && directFolder ? directFolder : "";
      state.directFileConsumed = false;
      state.directFolderConsumed = false;
      state.focusPath = state.directFilePath;
      return offer || room;
    };
    try {
      const url = new URL(text);
      const r = fromHash(hashFragmentFrom(url.href) || url.hash.slice(1));
      if (r) return r;
    } catch { /* not a URL */ }
    const hashAt = text.indexOf("#");
    if (hashAt >= 0) {
      const frag = text.slice(hashAt + 1);
      const params = shareParamsFromFragment(frag);
      if (params.room || params.offer) {
        const r = fromHash(frag);
        if (r) return r;
      }
    }
    if (!options.preserveDirect) {
      state.directFilePath = "";
      state.directFileConsumed = false;
      state.directFolderPath = "";
      state.directFolderConsumed = false;
      state.focusPath = "";
    }
    return text;
  }

  function applyHash() {
    const frag = currentHashFragment();
    if (!frag) return false;
    const { room, offer, file, folder } = shareParamsFromFragment(frag);
    const token = isWebOfflineOffer(offer) ? offer : room;
    if (isWebOfflineOffer(token) || looksLikeRoom(token)) {
      state.pendingConnectToken = token;
      const directFile = file ? normalizePath(file) : "";
      const directFolder = folder ? normalizePath(folder) : "";
      state.directFilePath = directFile && directFile !== "/" ? directFile : "";
      state.directFolderPath = !state.directFilePath && directFolder ? directFolder : "";
      state.directFileConsumed = false;
      state.directFolderConsumed = false;
      state.focusPath = state.directFilePath;
      els.connectInput.value = token;
      selectTab("connect");
      return true;
    }
    return false;
  }

  async function turnstileToken() {
    if (config().turnstileRequiredForWebSocket !== true) return "";
    const sitekey = clean(config().turnstileSiteKey);
    if (!sitekey) return "";
    await waitForTurnstile();
    if (state.turnstileWidgetId === null) {
      state.turnstileWidgetId = window.turnstile.render(els.turnstileMount, {
        sitekey,
        execution: "execute",
        appearance: "interaction-only",
        callback: (token) => {
          els.turnstileMount.classList.remove("is-interactive");
          if (state.turnstileResolver) {
            state.turnstileResolver.resolve(token || "");
            state.turnstileResolver = null;
          }
        },
        "before-interactive-callback": () => {
          // Safari (ITP/Private Relay) often forces an interactive challenge.
          // Bring the otherwise-hidden widget on-screen so it can be solved.
          els.turnstileMount.classList.add("is-interactive");
        },
        "after-interactive-callback": () => {
          els.turnstileMount.classList.remove("is-interactive");
        },
        "error-callback": () => {
          els.turnstileMount.classList.remove("is-interactive");
          if (state.turnstileResolver) {
            state.turnstileResolver.reject(new Error("Browser check failed"));
            state.turnstileResolver = null;
          }
        },
        "expired-callback": () => {},
      });
    }
    return new Promise((resolve, reject) => {
      state.turnstileResolver = { resolve, reject };
      try { window.turnstile.execute(state.turnstileWidgetId); }
      catch (e) { state.turnstileResolver = null; reject(e); }
      setTimeout(() => {
        if (state.turnstileResolver) {
          state.turnstileResolver = null;
          els.turnstileMount.classList.remove("is-interactive");
          reject(new Error("Browser check timed out"));
        }
      }, 15000);
    });
  }

  function waitForTurnstile() {
    if (window.turnstile) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const start = Date.now();
      const tick = () => {
        if (window.turnstile) return resolve();
        if (Date.now() - start > 10000) return reject(new Error("Turnstile did not load"));
        setTimeout(tick, 150);
      };
      tick();
    });
  }

  function signalUrl(lookup, role, token = "") {
    const u = new URL(workerUrl());
    u.protocol = u.protocol === "https:" ? "wss:" : "ws:";
    u.pathname = "/room";
    u.search = "";
    u.searchParams.set("code", lookup);
    u.searchParams.set("role", role);
    u.searchParams.set("web", "1");
    if (token) u.searchParams.set("turnstile", token);
    return u;
  }

  function openSignal(role, lookup, onMessage, token = "") {
    return new Promise((resolve, reject) => {
      let settled = false;
      const ws = new WebSocket(signalUrl(lookup, role, token));
      ws.provider = "cloudflare";
      ws._fbReady = false;
      ws._fbSignalError = "";
      ws._fbClosed = false;
      const fail = (message) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        try { ws.close(); } catch { /* ignore */ }
        reject(new Error(message));
      };
      const timer = setTimeout(() => fail("Cloudflare signaling timed out"), 12000);
      ws.onopen = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        resolve(ws);
      };
      ws.onerror = () => fail("Cloudflare signaling failed");
      ws.onclose = (event) => {
        ws._fbClosed = true;
        ws._fbCloseEvent = event;
        if (!settled) fail("Cloudflare signaling closed before it was ready");
        if (role === "host" && state.hostWs === ws) setShareStatus("Not hosting.");
        if (role === "client" && state.clientWs === ws) {
          state.lastSignalCloseReason = signalCloseReason(ws.provider, event);
          if (clientDataChannelOpen()) {
            setConnectStatus(`Connected. ${state.lastSignalCloseReason}; file session still active.`);
          } else {
            setClientDisconnected(state.lastSignalCloseReason);
          }
        }
      };
      ws.onmessage = (event) => {
        let msg;
        try { msg = JSON.parse(event.data); }
        catch { return; }
        if (msg.kind === "ready") ws._fbReady = true;
        if (msg.kind === "error") ws._fbSignalError = msg.error || "signaling_error";
        try { onMessage(msg); }
        catch { /* ignore malformed signaling */ }
      };
    });
  }

  function waitForCloudflareHostClaim(ws) {
    if (ws._fbReady) return Promise.resolve(true);
    if (ws._fbSignalError === "host_already_connected" || ws._fbClosed) return Promise.resolve(false);
    return new Promise((resolve) => {
      const done = (ok) => {
        clearTimeout(timer);
        ws.removeEventListener("message", onMessage);
        ws.removeEventListener("close", onClose);
        resolve(ok);
      };
      const onMessage = (event) => {
        let msg; try { msg = JSON.parse(event.data); } catch { return; }
        if (msg.kind === "ready") done(true);
        else if (msg.kind === "error" && msg.error === "host_already_connected") done(false);
      };
      const onClose = () => done(false);
      const timer = setTimeout(() => done(!ws._fbSignalError && !ws._fbClosed), 1500);
      ws.addEventListener("message", onMessage);
      ws.addEventListener("close", onClose);
    });
  }

  function makeSignalRelay(provider, sendFn, closeFn) {
    const messageListeners = new Set();
    const closeListeners = new Set();
    const relay = {
      provider,
      readyState: 1,
      onmessage: null,
      onclose: null,
      send(data) {
        if (relay.readyState !== 1) throw new Error(`${provider} signaling is closed`);
        return sendFn(data);
      },
      close() {
        if (relay.readyState === 3) return;
        relay.readyState = 3;
        try { closeFn?.(); } catch { /* ignore cleanup errors */ }
        relay._emitClose();
      },
      addEventListener(type, fn) {
        if (type === "message") messageListeners.add(fn);
        if (type === "close") closeListeners.add(fn);
      },
      removeEventListener(type, fn) {
        if (type === "message") messageListeners.delete(fn);
        if (type === "close") closeListeners.delete(fn);
      },
      _emit(obj) {
        if (relay.readyState === 3) return;
        const ev = { data: JSON.stringify(obj) };
        try { relay.onmessage?.(ev); } catch { /* listener owns its errors */ }
        for (const fn of [...messageListeners]) {
          try { fn(ev); } catch { /* listener owns its errors */ }
        }
      },
      _emitClose() {
        const ev = {};
        try { relay.onclose?.(ev); } catch { /* listener owns its errors */ }
        for (const fn of [...closeListeners]) {
          try { fn(ev); } catch { /* listener owns its errors */ }
        }
      },
    };
    return relay;
  }

  async function firebaseSdk() {
    if (!firebaseSdkPromise) {
      firebaseSdkPromise = Promise.all([
        import(`https://www.gstatic.com/firebasejs/${FIREBASE_SDK_VERSION}/firebase-app.js`),
        import(`https://www.gstatic.com/firebasejs/${FIREBASE_SDK_VERSION}/firebase-database.js`),
      ]).then(([appMod, dbMod]) => {
        const cfg = firebaseConfig();
        const appName = "folder-buddies-fallback";
        const app = appMod.getApps().find((a) => a.name === appName) || appMod.initializeApp(cfg, appName);
        const db = dbMod.getDatabase(app);
        return { ...dbMod, db };
      });
    }
    return firebaseSdkPromise;
  }

  function parseRelaySignal(data) {
    let msg;
    try { msg = JSON.parse(data); } catch { throw new Error("Bad signaling message"); }
    if (msg?.kind !== "signal" || typeof msg.peerId !== "string" || typeof msg.ciphertext !== "string") {
      throw new Error("Bad signaling message");
    }
    return msg;
  }

  async function pushFirebaseSignal(sdk, path, value) {
    await sdk.set(sdk.push(sdk.ref(sdk.db, path)), { ...value, at: sdk.serverTimestamp() });
  }

  async function openFirebaseHostSignal(preferredCode = "") {
    if (!firebaseEnabled()) throw new Error("Firebase fallback is not configured");
    const sdk = await firebaseSdk();

    const attempts = preferredCode ? 1 : 12;
    for (let attempt = 0; attempt < attempts; ++attempt) {
      const code = preferredCode || randomRoom(state.allowWrites);
      const lookup = lookupOf(code);
      const path = firebaseRoomPath(lookup);
      const roomRef = sdk.ref(sdk.db, path);

      const existingSnap = await sdk.get(roomRef);
      if (existingSnap.exists()) {
        const existingRoom = existingSnap.val();
        if (await firebaseDeleteIfExpired(sdk, path, existingRoom)) {
        }
      }

      const host = { createdAt: Date.now() };
      const claim = await sdk.runTransaction(roomRef, (current) => {
        if (current !== null) return;
        return { v: 1, host };
      }, { applyLocally: false });
      if (!claim.committed) continue;

      const unsubs = [];
      let closed = false;
      const cleanup = () => {
        if (closed) return;
        closed = true;
        for (const un of unsubs.splice(0)) {
          try { un(); } catch { /* ignore */ }
        }
        sdk.remove(roomRef).catch(() => {});
      };
      sdk.onDisconnect(roomRef).remove().catch(() => {});

      const relay = makeSignalRelay("firebase", async (data) => {
        const msg = parseRelaySignal(data);
        await pushFirebaseSignal(sdk, `${path}/signalsToClient/${msg.peerId}`, { ciphertext: msg.ciphertext });
      }, cleanup);

      unsubs.push(sdk.onChildAdded(sdk.ref(sdk.db, `${path}/clients`), (snap) => {
        const peerId = snap.key;
        if (peerId) relay._emit({ kind: "client-joined", peerId });
      }));
      unsubs.push(sdk.onChildAdded(sdk.ref(sdk.db, `${path}/signalsToHost`), (snap) => {
        const v = snap.val() || {};
        if (typeof v.peerId === "string" && typeof v.ciphertext === "string") {
          relay._emit({ kind: "signal", peerId: v.peerId, ciphertext: v.ciphertext });
        }
        sdk.remove(snap.ref).catch(() => {});
      }));
      setTimeout(() => relay._emit({ kind: "ready", role: "host", room: lookup }), 0);
      return { code, lookup, ws: relay, provider: "firebase" };
    }
    throw new Error("Firebase fallback could not find a free room code");
  }

  function taggedError(message, code) {
    const err = new Error(message);
    err.code = code;
    return err;
  }

  async function openFirebaseClientSignal(lookup) {
    if (!firebaseEnabled()) throw new Error("Firebase fallback is not configured");
    const sdk = await firebaseSdk();
    const path = firebaseRoomPath(lookup);
    const roomRef = sdk.ref(sdk.db, path);
    const snap = await sdk.get(roomRef);
    if (!snap.exists()) throw taggedError("Host not found", "room_not_found");

    const room = snap.val();
    if (firebaseRoomExpired(room)) {
      await firebaseDeleteIfExpired(sdk, path, room);
      throw taggedError("Host not found", "room_not_found");
    }

    const peerId = crypto.randomUUID();
    const clientRef = sdk.ref(sdk.db, `${path}/clients/${peerId}`);
    const unsubs = [];
    let closed = false;
    const cleanup = () => {
      if (closed) return;
      closed = true;
      for (const un of unsubs.splice(0)) {
        try { un(); } catch { /* ignore */ }
      }
      sdk.remove(clientRef).catch(() => {});
    };

    const relay = makeSignalRelay("firebase", async (data) => {
      const msg = parseRelaySignal(data);
      await pushFirebaseSignal(sdk, `${path}/signalsToHost`, { peerId: msg.peerId, ciphertext: msg.ciphertext });
    }, cleanup);

    unsubs.push(sdk.onChildAdded(sdk.ref(sdk.db, `${path}/signalsToClient/${peerId}`), (snap2) => {
      const v = snap2.val() || {};
      if (typeof v.ciphertext === "string") relay._emit({ kind: "signal", peerId, ciphertext: v.ciphertext });
      sdk.remove(snap2.ref).catch(() => {});
    }));
    unsubs.push(sdk.onValue(roomRef, (roomSnap) => {
      if (!roomSnap.exists()) {
        relay._emit({ kind: "host-left" });
        relay.close();
      }
    }));

    await sdk.set(clientRef, { joinedAt: sdk.serverTimestamp() });
    sdk.onDisconnect(clientRef).remove().catch(() => {});
    setTimeout(() => relay._emit({ kind: "ready", role: "client", room: lookup, peerId }), 0);
    return relay;
  }

  // Connect as host, regenerating the code if the relay room is already taken.
  async function openCloudflareHostSignal(preferredCode = "") {
    const attempts = preferredCode ? 1 : 12;
    for (let attempt = 0; attempt < attempts; ++attempt) {
      const code = preferredCode || randomRoom(state.allowWrites);
      const lookup = lookupOf(code);
      const token = await turnstileToken();
      const ws = await openSignal("host", lookup, (msg) => handleHostSignal(msg).catch((err) => toast(err.message)), token);
      const claimed = await waitForCloudflareHostClaim(ws);
      if (claimed) return { code, lookup, ws, provider: "cloudflare" };
      try { ws.close(); } catch { /* already closing */ }
    }
    throw new Error("Could not find a free room code");
  }

  async function openHostSignal(preferredCode = "") {
    let cloudError = "";
    try {
      return await openCloudflareHostSignal(preferredCode);
    } catch (e) {
      cloudError = e?.message || String(e);
    }

    try {
      const out = await openFirebaseHostSignal(preferredCode);
      toast(`Cloudflare failed; using Firebase fallback. ${cloudError}`);
      return out;
    } catch (e) {
      const firebaseError = e?.message || String(e);
      throw new Error(`Cloudflare failed (${cloudError}); Firebase fallback failed (${firebaseError})`);
    }
  }


  function base64UrlEncodeBytes(bytes) {
    let bin = "";
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
  }

  function base64UrlDecodeBytes(text) {
    let s = String(text || "").replace(/-/g, "+").replace(/_/g, "/");
    while (s.length % 4) s += "=";
    const bin = atob(s);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; ++i) out[i] = bin.charCodeAt(i);
    return out;
  }

  function encodePlainSignal(payload) {
    return "plain:" + base64UrlEncodeBytes(te.encode(JSON.stringify(payload)));
  }

  function decodePlainSignal(ciphertext) {
    if (typeof ciphertext !== "string" || !ciphertext.startsWith("plain:")) return null;
    const obj = JSON.parse(td.decode(base64UrlDecodeBytes(ciphertext.slice(6))));
    obj.__plain = true;
    return obj;
  }

  async function sendPlainSignal(ws, peerId, payload) {
    await ws.send(JSON.stringify({ kind: "signal", peerId, ciphertext: encodePlainSignal(payload) }));
  }

  async function sendEncryptedSignal(ws, peerId, payload) {
    if (state.plainSignalPeers.has(peerId)) return sendPlainSignal(ws, peerId, payload);
    const aad = `web-signal:${state.lookup}:${peerId}`;
    const ciphertext = await encryptJson(aad, payload);
    await ws.send(JSON.stringify({ kind: "signal", peerId, ciphertext }));
  }

  function decryptSignal(peerId, ciphertext) {
    const plain = decodePlainSignal(ciphertext);
    if (plain) {
      state.plainSignalPeers.add(peerId);
      return plain;
    }
    const aad = `web-signal:${state.lookup}:${peerId}`;
    return decryptJson(aad, ciphertext);
  }

  function makePeer(label) {
    const pc = new RTCPeerConnection({ iceServers: iceServers() });
    pc.onconnectionstatechange = () => {
      if (pc === state.clientPeer?.pc) {
        state.lastPeerState = pc.connectionState || "";
        if (pc.connectionState === "disconnected") {
          setConnectStatus("Connection interrupted; trying to recover…");
        } else if (pc.connectionState === "failed") {
          setClientDisconnected(describeClientDisconnect("peer connection failed"));
        } else if (pc.connectionState === "connected") {
          setConnectStatus("Connected.");
        }
      }
      if (pc.connectionState === "failed") toast(`${label} connection failed`);
    };
    return pc;
  }

  function waitForIceComplete(pc, timeoutMs = ICE_TIMEOUT_MS) {
    if (pc.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve) => {
      const done = () => {
        clearTimeout(timer);
        pc.removeEventListener("icegatheringstatechange", onChange);
        resolve();
      };
      const onChange = () => { if (pc.iceGatheringState === "complete") done(); };
      const timer = setTimeout(done, timeoutMs);
      pc.addEventListener("icegatheringstatechange", onChange);
    });
  }

  function setupHostChannel(peerId, dc) {
    dc.binaryType = "arraybuffer";
    dc.onopen = () => renderShareStatus();
    dc.onclose = () => {
      const peer = state.hostPeers.get(peerId);
      if (peer?.dc === dc) {
        peer.pc.close();
        state.hostPeers.delete(peerId);
      }
      renderShareStatus();
    };
    dc.onmessage = (event) => {
      if (typeof event.data !== "string") {
        handleHostBinary(dc, event.data);
        return;
      }
      let msg;
      try { msg = JSON.parse(event.data); } catch { return; }
      handleHostRequest(peerId, dc, msg).catch((e) => safeSendJson(dc, { t: "error", id: msg?.id || 0, message: e.message }));
    };
  }

  function setupClientChannel(dc) {
    state.dataChannel = dc;
    dc.binaryType = "arraybuffer";
    dc.onopen = () => {
      clearClientFallbackTimer();
      setConnectStatus("Connected.");
      CACHE.clear(); // fresh session: never carry cache across connections
      els.explorer.hidden = false;
      if (state.directFilePath && !state.directFileConsumed) {
        state.directFileConsumed = true;
        const filePath = state.directFilePath;
        state.focusPath = filePath;
        setConnectStatus("Connected. Opening file location…");
        listRemote(parentPath(filePath))
          .then(() => {
            setConnectStatus("Connected. Downloading…");
            return downloadRemote({ name: basename(filePath), path: filePath, kind: "file" }, { browserDownload: true, direct: true });
          })
          .then(() => toast("Download started"))
          .catch((e) => toast(e.message));
      } else if (state.directFolderPath && !state.directFolderConsumed) {
        state.directFolderConsumed = true;
        const folderPath = state.directFolderPath;
        listRemote(folderPath).then(() => {
          setConnectStatus("Connected.");
          toast("Folder opened");
        }).catch((e) => toast(e.message));
      } else {
        listRemote("/").catch((e) => toast(e.message));
      }
      clearInterval(state.refreshTimer);
      state.refreshTimer = setInterval(() => { pollCurrentDir().catch(() => {}); }, 4000);
    };
    dc.onclose = () => {
      clearClientFallbackTimer();
      clearInterval(state.refreshTimer);
      state.refreshTimer = null;
      if (state.dataChannel !== dc) return;
      const relay = state.clientWs;
      const pc = state.clientPeer?.pc;
      if (!state.lastPeerState && pc?.connectionState) state.lastPeerState = pc.connectionState;
      state.dataChannel = null;
      state.clientWs = null;
      state.clientPeer = null;
      try { relay?.close?.(); } catch { /* ignore */ }
      try { pc?.close?.(); } catch { /* ignore */ }
      setConnected(false);
      setClientDisconnected(describeClientDisconnect("data channel closed"));
    };
    dc.onerror = () => {
      toast("Connection error");
      if (state.dataChannel === dc) setClientDisconnected(describeClientDisconnect("data channel error"));
    };
    dc.onmessage = (event) => {
      if (typeof event.data === "string") handleClientJson(event.data);
      else handleClientBinary(event.data);
    };
  }

  function safeSendJson(dc, obj) {
    if (dc.readyState === "open") dc.send(JSON.stringify(obj));
  }

  function broadcastFsChanged(path, exceptPeerId) {
    for (const [pid, peer] of state.hostPeers) {
      if (pid === exceptPeerId) continue;
      safeSendJson(peer.dc, { t: "fsChanged", path: normalizePath(path || "/") });
    }
  }

  function nextId() {
    return state.nextReqId++ >>> 0;
  }

  function expectResponse(id, timeoutMs = 20000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        state.pending.delete(id);
        reject(new Error("Request timed out"));
      }, timeoutMs);
      state.pending.set(id, { resolve, reject, timer });
    });
  }

  function requestJson(dc, obj, timeoutMs = 20000) {
    const id = obj.id || nextId();
    obj.id = id;
    const p = expectResponse(id, timeoutMs);
    safeSendJson(dc, obj);
    return p;
  }

  function handleClientJson(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }
    if (msg.t === "fsChanged") {
      const changed = normalizePath(msg.path || "/");
      if (changed === state.currentPath || parentPath(changed) === state.currentPath) {
        CACHE.dirInvalidate(state.currentPath);
        pollCurrentDir().catch(() => {});
      }
      return;
    }
    if (msg.t === "error") {
      const pending = state.pending.get(msg.id);
      if (pending) {
        clearTimeout(pending.timer);
        state.pending.delete(msg.id);
        pending.reject(new Error(msg.message || "Remote error"));
      }
      const download = state.downloads.get(msg.id);
      if (download?.reject) {
        state.downloads.delete(msg.id);
        download.reject(new Error(msg.message || "Remote error"));
      }
      toast(msg.message || "Remote error");
      return;
    }
    if (msg.t === "listResult") {
      state.clientCanWrite = !!msg.write;
      const pending = state.pending.get(msg.id);
      if (pending) {
        clearTimeout(pending.timer);
        state.pending.delete(msg.id);
        pending.resolve(msg);
      }
      return;
    }
    if (msg.t === "ok" || msg.t === "uploadReady") {
      const pending = state.pending.get(msg.id);
      if (pending) {
        clearTimeout(pending.timer);
        state.pending.delete(msg.id);
        pending.resolve(msg);
      }
      return;
    }
    if (msg.t === "fileStart") {
      const d = state.downloads.get(msg.id);
      if (!d) return;
      d.size = Number(msg.size || 0);
      d.received = 0;
      d.label.textContent = `${msg.name} — 0 / ${formatSize(d.size)}`;
      d.progress.max = d.size || 1;
      d.progress.value = 0;
      return;
    }
    if (msg.t === "fileEnd") finishDownload(msg.id).catch((e) => toast(e.message));
  }

  function handleClientBinary(buf) {
    const view = new DataView(buf);
    if (view.byteLength < 8 || view.getUint32(0, false) !== BIN_MAGIC) return;
    const id = view.getUint32(4, false);
    const d = state.downloads.get(id);
    if (!d) return;
    const chunk = new Uint8Array(buf, 8);
    d.received += chunk.byteLength;
    state.bytesReceived += chunk.byteLength;
    d.progress.value = d.received;
    d.label.textContent = `${d.name} — ${formatSize(d.received)} / ${formatSize(d.size || d.received)}`;
    if (d.writer) {
      d.writeQueue = d.writeQueue.then(() => d.writer.write(chunk));
      d.writeQueue.catch((e) => toast(`Write failed: ${e.message}`));
    } else {
      d.chunks.push(chunk.slice());
    }
  }

  async function finishDownload(id) {
    const d = state.downloads.get(id);
    if (!d) return;
    state.downloads.delete(id);
    try {
      if (d.writer) {
        await d.writeQueue;
        await d.writer.close();
      } else {
        const blob = new Blob(d.chunks, { type: d.mime || "application/octet-stream" });
        if (d.mode === "blob") {
          d.resolve?.(blob);
        } else {
          const url = URL.createObjectURL(blob);
          const a = document.createElement("a");
          a.href = url;
          a.download = d.name;
          a.click();
          setTimeout(() => URL.revokeObjectURL(url), 10000);
        }
      }
      d.label.textContent = `${d.name} — done`;
      d.progress.value = d.progress.max;
      if (d.mode === "direct") setConnectStatus("Download complete.");
    } catch (e) {
      d.reject?.(e);
      throw e;
    }
  }

  async function handleHostRequest(peerId, dc, msg) {
    if (!state.rootHandle) throw new Error("No folder selected");
    if (msg.t === "list") {
      const path = normalizePath(msg.path || "/");
      const dir = await directoryHandleFor(path);
      const entries = [];
      for await (const [name, handle] of dir.entries()) {
        const entry = { name, path: joinPath(path, name), kind: handle.kind, size: null, mtime: null };
        if (handle.kind === "file") {
          try {
            const file = await handle.getFile();
            entry.size = file.size;
            entry.mtime = file.lastModified;
          } catch { /* permission can change */ }
        }
        entries.push(entry);
      }
      entries.sort((a, b) => (a.kind === b.kind ? a.name.localeCompare(b.name) : a.kind === "directory" ? -1 : 1));
      safeSendJson(dc, { t: "listResult", id: msg.id, path, entries, write: !!state.allowWrites });
      return;
    }
    if (msg.t === "download") {
      const path = normalizePath(msg.path || "/");
      const fileHandle = await fileHandleFor(path);
      const file = await fileHandle.getFile();
      safeSendJson(dc, { t: "fileStart", id: msg.id, name: file.name, size: file.size, mtime: file.lastModified });
      await streamFile(dc, msg.id, file);
      safeSendJson(dc, { t: "fileEnd", id: msg.id });
      return;
    }
    if (msg.t === "uploadStart") {
      await assertHostWritable();
      const path = normalizePath(msg.path || "/");
      if (path === "/") throw new Error("Bad upload path");
      const parent = await ensureDirectoryHandle(parentPath(path));
      const name = safeEntryName(basename(path));
      const fileHandle = await parent.getFileHandle(name, { create: true });
      const writer = await fileHandle.createWritable();
      state.uploads.set(msg.id, { writer, writeQueue: Promise.resolve(), received: 0, size: Number(msg.size || 0), name, path });
      safeSendJson(dc, { t: "uploadReady", id: msg.id });
      return;
    }
    if (msg.t === "mkdir") {
      await assertHostWritable();
      const path = normalizePath(msg.path || "/");
      await ensureDirectoryHandle(path);
      safeSendJson(dc, { t: "ok", id: msg.id });
      broadcastFsChanged(path, peerId);
      return;
    }
    if (msg.t === "uploadEnd") {
      await assertHostWritable();
      const upload = state.uploads.get(msg.id);
      if (!upload) throw new Error("Upload not found");
      await upload.writeQueue;
      await upload.writer.close();
      state.uploads.delete(msg.id);
      safeSendJson(dc, { t: "ok", id: msg.id });
      broadcastFsChanged(upload.path || "/", peerId);
      return;
    }
    if (msg.t === "delete") {
      await assertHostWritable();
      const path = normalizePath(msg.path || "/");
      if (path === "/") throw new Error("Cannot delete the shared folder root");
      const parent = await directoryHandleFor(parentPath(path));
      await parent.removeEntry(basename(path), { recursive: true });
      safeSendJson(dc, { t: "ok", id: msg.id });
      broadcastFsChanged(path, peerId);
      return;
    }
    throw new Error("Unknown request");
  }

  function handleHostBinary(dc, buf) {
    const view = new DataView(buf);
    if (view.byteLength < 8 || view.getUint32(0, false) !== BIN_MAGIC) return;
    const id = view.getUint32(4, false);
    const upload = state.uploads.get(id);
    if (!upload) return;
    const chunk = new Uint8Array(buf, 8);
    upload.received += chunk.byteLength;
    upload.writeQueue = upload.writeQueue.then(() => upload.writer.write(chunk));
    upload.writeQueue.catch((e) => {
      state.uploads.delete(id);
      safeSendJson(dc, { t: "error", id, message: `Upload failed: ${e.message}` });
    });
  }

  async function streamFile(dc, id, file) {
    const reader = file.stream().getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (let off = 0; off < value.byteLength; off += CHUNK_SIZE) {
          await sendBinaryChunk(dc, id, value.slice(off, Math.min(off + CHUNK_SIZE, value.byteLength)));
        }
      }
    } finally {
      reader.releaseLock();
    }
  }

  async function sendBinaryChunk(dc, id, chunk) {
    while (dc.bufferedAmount > 8 * 1024 * 1024) {
      await new Promise((resolve) => {
        dc.bufferedAmountLowThreshold = 2 * 1024 * 1024;
        dc.onbufferedamountlow = resolve;
        setTimeout(resolve, 250);
      });
    }
    const out = new Uint8Array(8 + chunk.byteLength);
    const view = new DataView(out.buffer);
    view.setUint32(0, BIN_MAGIC, false);
    view.setUint32(4, id >>> 0, false);
    out.set(chunk, 8);
    dc.send(out);
    state.bytesServed += chunk.byteLength;
  }

  function normalizePath(path) {
    const parts = String(path || "/").split(/[\\/]+/).filter(Boolean);
    const safe = [];
    for (const p of parts) {
      if (p === ".") continue;
      if (p === "..") safe.pop();
      else safe.push(p);
    }
    return "/" + safe.join("/");
  }

  function joinPath(base, name) {
    return normalizePath(`${base.replace(/\/$/, "")}/${safeEntryName(name)}`);
  }

  function safeRelativePath(path) {
    const safe = [];
    for (const part of String(path || "").split(/[\\/]+/).filter(Boolean)) {
      if (part === ".") continue;
      if (part === "..") safe.pop();
      else safe.push(safeEntryName(part));
    }
    return safe.join("/");
  }

  function joinSafeRelativePath(base, path) {
    const rel = safeRelativePath(path);
    return rel ? normalizePath(`${base.replace(/\/$/, "")}/${rel}`) : normalizePath(base);
  }

  function parentPath(path) {
    const parts = normalizePath(path).split("/").filter(Boolean);
    parts.pop();
    return "/" + parts.join("/");
  }

  function basename(path) {
    return normalizePath(path).split("/").filter(Boolean).pop() || "";
  }

  function safeEntryName(name) {
    const cleaned = String(name || "").replace(/[\/\0]/g, "").trim();
    return cleaned || "unnamed";
  }

  async function assertHostWritable() {
    if (!state.allowWrites) throw new Error("The host has writes disabled");
    if (!state.rootHandle) throw new Error("No folder selected");
    if (state.rootHandle.queryPermission && state.rootHandle.requestPermission) {
      const opts = { mode: "readwrite" };
      let status = await state.rootHandle.queryPermission(opts);
      if (status !== "granted") status = await state.rootHandle.requestPermission(opts);
      if (status !== "granted") throw new Error("Write permission was not granted by the host browser");
    }
  }

  async function handleForPath(path) {
    const parts = normalizePath(path).split("/").filter(Boolean);
    let handle = state.rootHandle;
    for (const part of parts) {
      if (handle.kind !== "directory") throw new Error("Path is not a directory");
      handle = await handle.getDirectoryHandle(part).catch(async () => handle.getFileHandle(part));
    }
    return handle;
  }

  async function directoryHandleFor(path) {
    const handle = await handleForPath(path);
    if (handle.kind !== "directory") throw new Error("Not a directory");
    return handle;
  }

  async function ensureDirectoryHandle(path) {
    let handle = state.rootHandle;
    for (const part of normalizePath(path).split("/").filter(Boolean)) {
      if (handle.kind !== "directory") throw new Error("Path is not a directory");
      handle = await handle.getDirectoryHandle(safeEntryName(part), { create: true });
    }
    return handle;
  }

  async function fileHandleFor(path) {
    const handle = await handleForPath(path);
    if (handle.kind !== "file") throw new Error("Not a file");
    return handle;
  }

  async function createHostPeer(peerId, plainSignal = false) {
    if (plainSignal) state.plainSignalPeers.add(peerId);
    const pc = makePeer(`Peer ${peerId}`);
    const dc = pc.createDataChannel("folderbuddies-files", { ordered: true });
    setupHostChannel(peerId, dc);
    state.hostPeers.set(peerId, { pc, dc });
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceComplete(pc);
    await sendEncryptedSignal(state.hostWs, peerId, { type: "offer", sdp: pc.localDescription });
  }


  async function createManualHostOffer() {
    const peerId = "manual";
    const pc = makePeer("Offline web client");
    const dc = pc.createDataChannel("folderbuddies-files", { ordered: true });
    setupHostChannel(peerId, dc);
    state.hostPeers.set(peerId, { pc, dc });
    state.manualHost = { peerId, pc, dc };
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceComplete(pc, Math.max(ICE_TIMEOUT_MS, 12000));
    return encodeWebOffline(WEB_OFFLINE_OFFER_PREFIX, {
      v: 1,
      type: "web-offline-offer",
      code: state.code,
      lookup: state.lookup,
      write: !!state.allowWrites,
      secret: state.secureSecret || "",
      sdp: pc.localDescription,
    });
  }

  async function applyManualAnswer(text) {
    const answer = decodeWebOffline(WEB_OFFLINE_ANSWER_PREFIX, text);
    if (answer.v !== 1 || answer.type !== "web-offline-answer") throw new Error("Invalid offline answer");
    if (answer.code !== state.code) throw new Error("This answer belongs to a different offline offer");
    const peerId = answer.peerId || "manual";
    const peer = state.hostPeers.get(peerId);
    if (!peer) throw new Error("No waiting offline peer found");
    await peer.pc.setRemoteDescription(answer.sdp);
    renderShareStatus();
    toast("Offline answer applied");
  }

  async function handleHostSignal(msg) {
    if (msg.kind === "ready") {
      renderShareStatus();
      return;
    }
    if (msg.kind === "client-joined") {
      // Native compatibility clients announce themselves with a plain compat-hello
      // first. Wait briefly so we can answer with plain SDP; browser clients get
      // the normal encrypted signaling path after the grace period.
      setTimeout(() => {
        if (!state.hostPeers.has(msg.peerId)) {
          createHostPeer(msg.peerId, state.plainSignalPeers.has(msg.peerId)).then(renderShareStatus).catch((e) => toast(e.message));
        }
      }, 350);
      return;
    }
    if (msg.kind === "client-left") {
      const peer = state.hostPeers.get(msg.peerId);
      if (peer) peer.pc.close();
      state.hostPeers.delete(msg.peerId);
      state.plainSignalPeers.delete(msg.peerId);
      renderShareStatus();
      return;
    }
    if (msg.kind === "signal") {
      const signal = await decryptSignal(msg.peerId, msg.ciphertext);
      if (signal.type === "compat-hello") {
        state.plainSignalPeers.add(msg.peerId);
        if (!state.hostPeers.has(msg.peerId)) createHostPeer(msg.peerId, true).then(renderShareStatus).catch((e) => toast(e.message));
        return;
      }
      const peer = state.hostPeers.get(msg.peerId);
      if (!peer) return;
      if (signal.type === "answer") await peer.pc.setRemoteDescription(signal.sdp);
      else if (signal.type === "candidate" && signal.candidate) await peer.pc.addIceCandidate(signal.candidate);
    }
  }

  async function handleClientSignal(msg) {
    if (msg.kind === "ready") {
      state.clientPeer = { peerId: msg.peerId, pc: null };
      setConnectStatus("Waiting for host…");
      return;
    }
    if (msg.kind === "host-left") {
      toast("Host stopped sharing");
      setConnectStatus("Host stopped sharing.");
      return;
    }
    if (msg.kind === "error") {
      const reason = msg.error === "room_full"
        ? "The host's client limit is full."
        : "The host rejected the connection.";
      toast(reason);
      setConnectStatus(reason);
      return;
    }
    if (msg.kind === "signal") {
      const peerId = state.clientPeer?.peerId || msg.peerId;
      const signal = await decryptSignal(peerId, msg.ciphertext);
      if (signal.type === "offer") {
        await acceptOffer(peerId, signal.sdp, async (answer) => {
          if (signal.__plain) await sendPlainSignal(state.clientWs, peerId, { type: "answer", sdp: answer });
          else await sendEncryptedSignal(state.clientWs, peerId, { type: "answer", sdp: answer });
        });
      } else if (signal.type === "candidate" && signal.candidate && state.clientPeer?.pc) {
        await state.clientPeer.pc.addIceCandidate(signal.candidate);
      }
    }
  }

  async function acceptOffer(peerId, offerSdp, sendAnswer) {
    clearClientFallbackTimer();
    const pc = makePeer("Remote folder");
    state.clientPeer = { peerId, pc };
    pc.ondatachannel = (event) => setupClientChannel(event.channel);
    await pc.setRemoteDescription(offerSdp);
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    await waitForIceComplete(pc);
    await sendAnswer(pc.localDescription);
  }

  function renderShareStatus() {
    if (!state.hostWs && !state.manualHost) { setShareStatus("Not hosting."); return; }
    if (state.manualHost) {
      const dc = state.manualHost.dc;
      setShareStatus(dc?.readyState === "open" ? "Sharing offline — 1 client connected" : "Offline offer ready — paste the client's answer");
      return;
    }
    setShareStatus(`${state.hostWs?.provider === "firebase" ? "Hosting through Firebase fallback" : "Hosting"} — ${state.hostPeers.size} client(s)`);
  }

  function setShareRunning(running) {
    els.shareToggle.textContent = running ? "Stop hosting" : "Host";
    els.folderInput.disabled = running;
    els.browseFolder.disabled = running;
    if (els.allowWrites) els.allowWrites.disabled = running;
    els.copyAll.disabled = !running;
    els.copyShareLink.disabled = !running;
    if (els.offlineAnswerRow) els.offlineAnswerRow.hidden = !(running && state.manualHost);
    if (!running) {
      els.connectCode.value = "";
      if (els.offlineAnswerInput) els.offlineAnswerInput.value = "";
      if (els.offlineAnswerRow) els.offlineAnswerRow.hidden = true;
    }
  }

  function wantsWriteAccess() {
    return !!els.allowWrites?.checked;
  }

  // Returns true if a folder was chosen, false if the user cancelled the dialog.
  async function pickFolder() {
    if (!supportsHosting()) throw new Error("Hosting is not supported in this browser");
    let handle;
    try {
      handle = await window.showDirectoryPicker({ mode: wantsWriteAccess() ? "readwrite" : "read" });
    } catch (e) {
      if (isAbortError(e)) return false; // user dismissed the picker — not an error
      throw e;
    }
    state.rootHandle = handle;
    els.folderInput.value = state.rootHandle.name;
    return true;
  }

  async function startHost() {
    if (!supportsHosting()) throw new Error("Hosting is not supported in this browser");
    if (isMobile()) throw new Error("Hosting is not available on mobile browsers");
    if (!state.rootHandle && !(await pickFolder())) return; // user cancelled the folder chooser
    setShareStatus("Starting…");
    els.shareToggle.disabled = true;
    state.allowWrites = wantsWriteAccess();
    if (state.allowWrites) await assertHostWritable();

    try {
      const { code, lookup, ws } = await openHostSignal("");
      state.code = code;
      state.lookup = lookup;
      state.secureSecret = "";
      state.connectToken = code;
      state.rootBase = await deriveRootBase(code);
      state.hostWs = ws;
      ws.onmessage = (event) => {
        try { handleHostSignal(JSON.parse(event.data)).catch((e) => toast(e.message)); }
        catch { /* ignore */ }
      };
      els.connectCode.value = state.connectToken;
      toast(state.hostWs?.provider === "firebase" ? "Hosting through Firebase fallback." : "Hosting.");
    } catch (e) {
      const cloudError = e.message;
      state.code = randomRoom(state.allowWrites);
      state.lookup = lookupOf(state.code);
      state.secureSecret = "";
      state.connectToken = state.code;
      state.rootBase = await deriveRootBase(state.code);
      state.hostWs = null;
      const offerCode = await createManualHostOffer();
      state.connectToken = offerCode;
      els.connectCode.value = offerCode;
      toast(`Cloudflare/Firebase failed; using offline web code. ${cloudError}`);
    } finally {
      els.shareToggle.disabled = false;
    }

    setShareRunning(true);
    renderShareStatus();
  }

  async function stopHost() {
    if (state.hostWs) state.hostWs.close();
    for (const p of state.hostPeers.values()) p.pc.close();
    state.hostWs = null;
    state.manualHost = null;
    state.hostPeers.clear();
    state.plainSignalPeers.clear();
    for (const upload of state.uploads.values()) {
      try { upload.writer.close(); } catch { /* ignore */ }
    }
    state.uploads.clear();
    state.allowWrites = false;
    state.code = "";
    state.lookup = "";
    state.secureSecret = "";
    state.connectToken = "";
    state.rootBase = null;
    setShareRunning(false);
    setShareStatus("Not hosting.");
    toast("Stopped hosting");
  }

  function copyShareDetails() {
    return els.connectCode.value;
  }

  function setConnected(connected) {
    els.connectToggle.textContent = connected ? "Disconnect" : "Connect";
    els.connectInput.disabled = connected;
  }

  function hasClientSession() {
    return !!(state.clientWs || state.dataChannel || state.clientPeer || state.manualClient);
  }

  function handleClientSignalClose(relay, event) {
    if (state.clientWs !== relay) return;
    state.lastSignalCloseReason = signalCloseReason(relay.provider, event);
    if (clientDataChannelOpen()) {
      setConnectStatus(`Connected. ${state.lastSignalCloseReason}; file session still active.`);
    } else {
      setClientDisconnected(state.lastSignalCloseReason);
    }
  }

  function attachFirebaseClientSignal(relay) {
    state.clientWs = relay;
    relay.onmessage = (event) => {
      try { handleClientSignal(JSON.parse(event.data)).catch((err) => toast(err.message)); }
      catch { /* ignore */ }
    };
    relay.onclose = (event) => handleClientSignalClose(relay, event);
    return relay;
  }

  function scheduleCloudflareClientFallback(cloudWs) {
    clearClientFallbackTimer();
    state.clientFallbackTimer = setTimeout(async () => {
      state.clientFallbackTimer = 0;
      if (state.clientWs !== cloudWs || clientHasOfferOrChannel()) return;
      setConnectStatus("Still waiting for host. Trying Firebase fallback…");
      let relay;
      try {
        relay = await openFirebaseClientSignal(state.lookup);
      } catch {
        if (state.clientWs === cloudWs && !clientHasOfferOrChannel()) setConnectStatus("Waiting for host…");
        return;
      }
      if (state.clientWs !== cloudWs || clientHasOfferOrChannel()) {
        try { relay.close(); } catch { /* ignore */ }
        return;
      }
      state.clientWs = null;
      try { cloudWs.close(); } catch { /* ignore */ }
      attachFirebaseClientSignal(relay);
      setConnectStatus("Waiting for host through Firebase fallback…");
      toast("Cloudflare had no host; using Firebase fallback.");
    }, CLIENT_FALLBACK_DELAY_MS);
  }

  async function connect() {
    const rawInput = clean(els.connectInput.value);
    const pendingToken = clean(state.pendingConnectToken);
    const raw = rawInput || pendingToken;
    if (!raw) throw new Error("Enter a connect code or open a share link.");
    if (isWebOfflineOffer(raw)) return connectManualOffer(raw);

    const usingPreloadedToken = !!pendingToken && (!rawInput || rawInput === pendingToken);
    const resolved = resolveConnectToken(raw, { preserveDirect: usingPreloadedToken });
    if (isWebOfflineOffer(resolved.token)) return connectManualOffer(resolved.token);
    if (!looksLikeRoom(resolved.code)) throw new Error("The browser client accepts 6- or 16-character web room codes/share links or FBW2O offline web offer codes. Native IP/port blobs are for the native app.");

    state.lastSignalCloseReason = "";
    state.lastPeerState = "";
    clearClientFallbackTimer();
    setConnectStatus("Connecting…");
    state.code = resolved.code;
    state.lookup = resolved.lookup;
    state.secureSecret = resolved.secureSecret || "";
    state.connectToken = resolved.token;
    state.rootBase = await deriveRootBase(state.code, state.secureSecret);
    let cloudError = "";
    try {
      const token = await turnstileToken();
      state.clientWs = await openSignal("client", state.lookup, (msg) => handleClientSignal(msg).catch((e) => toast(e.message)), token);
      scheduleCloudflareClientFallback(state.clientWs);
    } catch (e) {
      cloudError = e?.message || String(e);
      try {
        attachFirebaseClientSignal(await openFirebaseClientSignal(state.lookup));
        toast(`Cloudflare failed; using Firebase fallback. ${cloudError}`);
      } catch (fb) {
        if (fb?.code === "room_not_found") throw new Error("Host not found");
        if (fb?.code === "room_full") throw new Error("The host's client limit is full.");
        throw new Error(cloudError || fb?.message || "Couldn't connect");
      }
    }
    setConnected(true);
  }

  async function connectManualOffer(text) {
    const offer = decodeWebOffline(WEB_OFFLINE_OFFER_PREFIX, text);
    if (offer.v !== 1 || offer.type !== "web-offline-offer" || !looksLikeRoom(offer.code)) throw new Error("Invalid offline web offer");
    state.lastSignalCloseReason = "";
    state.lastPeerState = "";
    setConnectStatus("Creating offline answer…");
    state.manualClient = true;
    state.code = offer.code;
    state.lookup = offer.lookup || lookupOf(offer.code);
    state.secureSecret = typeof offer.secret === "string" ? offer.secret : "";
    state.connectToken = clean(text);
    state.rootBase = await deriveRootBase(state.code, state.secureSecret);
    await acceptOffer("manual", offer.sdp, async (answer) => {
      const answerCode = encodeWebOffline(WEB_OFFLINE_ANSWER_PREFIX, {
        v: 1,
        type: "web-offline-answer",
        code: state.code,
        peerId: "manual",
        sdp: answer,
      });
      if (els.offlineAnswerOut) els.offlineAnswerOut.value = answerCode;
      if (els.offlineAnswerOutRow) els.offlineAnswerOutRow.hidden = false;
    });
    setConnected(true);
    setConnectStatus("Offline answer ready — send it back to the host, then wait for connection…");
  }

  function disconnectClient() {
    clearClientFallbackTimer();
    clearInterval(state.refreshTimer);
    state.refreshTimer = null;
    state.renderedSig = "";
    state.renderedPath = "";
    if (state.clientWs) state.clientWs.close();
    if (state.clientPeer?.pc) state.clientPeer.pc.close();
    state.clientWs = null;
    state.manualClient = false;
    state.clientPeer = null;
    state.dataChannel = null;
    state.clientCanWrite = false;
    state.directFilePath = "";
    state.directFileConsumed = false;
    state.directFolderPath = "";
    state.directFolderConsumed = false;
    state.focusPath = "";
    state.lastSignalCloseReason = "";
    state.lastPeerState = "";
    CACHE.clear(); // drop all cached listings/previews on disconnect
    setConnected(false);
    els.explorer.hidden = true;
    els.fileRows.textContent = "";
    els.transfers.textContent = "";
    if (els.previewPanel) els.previewPanel.hidden = true;
    if (els.offlineAnswerOutRow) els.offlineAnswerOutRow.hidden = true;
    if (els.offlineAnswerOut) els.offlineAnswerOut.value = "";
    setConnectStatus("Not connected.");
  }

  function disconnectAll() {
    stopHost().catch(() => {});
    disconnectClient();
  }

  async function listRemote(path) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");
    const p = normalizePath(path);
    // Serve a fresh cached listing instantly; otherwise fetch (de-duping
    // simultaneous identical requests) and cache the result.
    let res = CACHE.dirGet(p);
    if (!res) {
      res = await CACHE.dedup(`list:${p}`, () => requestJson(state.dataChannel, { t: "list", path: p }));
      CACHE.dirSet(p, res);
    }
    state.clientCanWrite = !!res.write; // also seeded from cached listings
    state.currentPath = res.path || p;
    renderCurrentPath(state.currentPath);
    if (els.upButton) els.upButton.disabled = state.currentPath === "/";
    if (els.shareFolderButton) els.shareFolderButton.disabled = !activeShareToken();
    renderWriteUi();
    renderRows(res.entries || []);
    state.renderedPath = state.currentPath;
    state.renderedSig = listSignature(res.entries || []);
  }

  function listSignature(entries) {
    return entries
      .map((e) => `${e.kind}:${e.name}:${e.size ?? ""}:${e.mtime ?? ""}`)
      .sort()
      .join("|");
  }

  async function pollCurrentDir() {
    const dc = state.dataChannel;
    if (!dc || dc.readyState !== "open" || els.explorer.hidden) return;
    const p = state.currentPath;
    let res;
    try {
      res = await requestJson(dc, { t: "list", path: p });
    } catch { return; }
    if (state.currentPath !== p || state.dataChannel !== dc) return;
    CACHE.dirSet(p, res);
    const sig = listSignature(res.entries || []);
    if (p === state.renderedPath && sig === state.renderedSig) return;
    state.clientCanWrite = !!res.write;
    renderWriteUi();
    renderRows(res.entries || []);
    state.renderedPath = p;
    state.renderedSig = sig;
  }

  function renderCurrentPath(path) {
    const p = normalizePath(path);
    els.currentPath.textContent = "";
    els.currentPath.title = p;

    const root = document.createElement("button");
    root.type = "button";
    root.textContent = "/";
    root.title = "/";
    root.onclick = () => listRemote("/").catch((e) => toast(e.message));
    els.currentPath.appendChild(root);

    let acc = "";
    for (const part of p.split("/").filter(Boolean)) {
      acc = `${acc}/${part}`;
      const sep = document.createElement("span");
      sep.className = "pathsep";
      sep.textContent = ">";
      els.currentPath.appendChild(sep);

      const btn = document.createElement("button");
      btn.type = "button";
      btn.textContent = part;
      btn.title = acc;
      const target = acc;
      btn.onclick = () => listRemote(target).catch((e) => toast(e.message));
      els.currentPath.appendChild(btn);
    }
  }

  function renderRows(entries) {
    els.fileRows.textContent = "";
    if (!entries.length) {
      const tr = document.createElement("tr");
      const td = document.createElement("td");
      td.colSpan = 3;
      td.className = "muted";
      td.textContent = "Empty folder";
      tr.appendChild(td);
      els.fileRows.appendChild(tr);
      return;
    }
    for (const e of entries) {
      const tr = document.createElement("tr");
      const rowPath = normalizePath(e.path || e.name);
      const focused = !!state.focusPath && rowPath === state.focusPath;
      tr.className = `${e.kind === "directory" ? "dir" : "file"}${focused ? " target" : ""}`;
      if (focused) tr.setAttribute("aria-current", "true");
      const name = document.createElement("td");
      name.className = "name";
      name.textContent = e.kind === "directory" ? `📁 ${e.name}` : `📄 ${e.name}`;
      name.title = e.path || e.name;
      name.onclick = () => previewRemote(e).catch((err) => toast(err.message));
      const size = document.createElement("td");
      size.textContent = e.kind === "file" ? formatSize(e.size || 0) : "—";
      const action = document.createElement("td");
      action.className = "actions";
      if (e.kind === "file") {
        const btn = document.createElement("button");
        btn.textContent = "Download";
        btn.onclick = () => downloadRemote(e).catch((err) => toast(err.message));
        action.appendChild(btn);
      }
      const share = document.createElement("button");
      share.textContent = "Share link";
      share.onclick = () => copy(clientShareLink(e.path, e.kind === "directory" ? "folder" : "file"))
        .then(() => toast(e.kind === "directory" ? "Folder link copied" : "File link copied"))
        .catch((err) => toast(err.message));
      action.appendChild(share);
      if (state.clientCanWrite) {
        const del = document.createElement("button");
        del.textContent = "Delete";
        del.className = "danger";
        del.onclick = () => deleteRemote(e).catch((err) => toast(err.message));
        action.appendChild(del);
      }
      tr.append(name, size, action);
      els.fileRows.appendChild(tr);
      if (focused && typeof tr.scrollIntoView === "function") {
        setTimeout(() => tr.scrollIntoView({ block: "nearest" }), 0);
      }
    }
  }

  function mimeForName(name) {
    const ext = (name.split(".").pop() || "").toLowerCase();
    const map = {
      txt: "text/plain", log: "text/plain", md: "text/markdown", csv: "text/csv",
      json: "application/json", xml: "application/xml", yaml: "text/yaml", yml: "text/yaml",
      js: "text/javascript", mjs: "text/javascript", cjs: "text/javascript", ts: "text/typescript",
      css: "text/css", html: "text/html", htm: "text/html", py: "text/x-python", cpp: "text/x-c++src",
      c: "text/x-csrc", h: "text/x-chdr", hpp: "text/x-c++hdr", java: "text/x-java-source",
      rs: "text/rust", go: "text/x-go", sh: "text/x-shellscript", ps1: "text/x-powershell",
      pdf: "application/pdf", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif",
      webp: "image/webp", bmp: "image/bmp", svg: "image/svg+xml", mp3: "audio/mpeg", wav: "audio/wav",
      ogg: "audio/ogg", flac: "audio/flac", mp4: "video/mp4", webm: "video/webm", mov: "video/quicktime"
    };
    return map[ext] || "application/octet-stream";
  }

  function isTextPreview(name, mime) {
    const ext = (name.split(".").pop() || "").toLowerCase();
    return mime.startsWith("text/") || ["json", "xml", "yaml", "yml", "js", "mjs", "cjs", "ts", "css", "html", "htm", "py", "cpp", "c", "h", "hpp", "java", "rs", "go", "sh", "ps1", "toml", "ini", "env"].includes(ext);
  }

  function canPreview(name, mime) {
    return isTextPreview(name, mime) || mime.startsWith("image/") || mime.startsWith("audio/") || mime.startsWith("video/") || mime === "application/pdf";
  }

  function makeTransfer(entry, verb, id = nextId()) {
    const transfer = document.createElement("div");
    transfer.className = "transfer";
    const label = document.createElement("div");
    label.textContent = `${entry.name} — ${verb}`;
    const progress = document.createElement("progress");
    progress.max = entry.size || 1;
    progress.value = 0;
    transfer.append(label, progress);
    els.transfers.textContent = "";
    els.transfers.appendChild(transfer);
    return { id, label, progress };
  }

  async function downloadRemote(entry, options = {}) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");

    let writer = null;
    if (!options.browserDownload && window.showSaveFilePicker) {
      try {
        const saveHandle = await window.showSaveFilePicker({ suggestedName: entry.name });
        writer = await saveHandle.createWritable();
      } catch (e) {
        if (isAbortError(e)) return; // user cancelled the save dialog — abort silently
        writer = null; // direct links may not have user activation; fall back to browser download
      }
    }

    const { id, label, progress } = makeTransfer(entry, "starting");
    state.downloads.set(id, { name: entry.name, size: entry.size || 0, received: 0, chunks: [], writer, writeQueue: Promise.resolve(), label, progress, mime: mimeForName(entry.name), mode: options.direct ? "direct" : "download" });
    safeSendJson(state.dataChannel, { t: "download", id, path: entry.path });
  }

  function fetchRemoteBlob(entry) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") return Promise.reject(new Error("Not connected"));
    const { id, label, progress } = makeTransfer(entry, "opening");
    const mime = mimeForName(entry.name);
    const promise = new Promise((resolve, reject) => {
      state.downloads.set(id, { name: entry.name, size: entry.size || 0, received: 0, chunks: [], writer: null, writeQueue: Promise.resolve(), label, progress, mime, mode: "blob", resolve, reject });
    });
    safeSendJson(state.dataChannel, { t: "download", id, path: entry.path });
    return promise;
  }

  async function previewRemote(entry) {
    if (entry.kind === "directory") return listRemote(entry.path);
    const mime = mimeForName(entry.name);
    if (!canPreview(entry.name, mime)) {
      toast("No browser preview for this file type; downloading instead");
      return downloadRemote(entry);
    }
    if (isTextPreview(entry.name, mime) && Number(entry.size || 0) > TEXT_PREVIEW_MAX) {
      toast("Text file is too large for the built-in editor; downloading instead");
      return downloadRemote(entry);
    }
    // Reuse a small cached preview blob (keyed by path+size+mtime) if present;
    // otherwise fetch once (de-duping concurrent opens) and cache when it fits.
    let blob = CACHE.previewGet(entry);
    if (!blob) {
      blob = await CACHE.dedup(`preview:${entry.path} ${entry.size ?? 0} ${entry.mtime ?? 0}`,
                               () => fetchRemoteBlob(entry));
      CACHE.previewSet(entry, blob, mime);
    }
    await renderPreview(entry, blob, mime);
  }

  async function renderPreview(entry, blob, mime) {
    els.previewBody.textContent = "";
    els.previewTitle.textContent = entry.path;
    els.savePreview.hidden = true;
    els.savePreview.onclick = null;
    let objectUrl = null;

    const cleanupUrl = () => {
      if (objectUrl) URL.revokeObjectURL(objectUrl);
      objectUrl = null;
    };
    els.closePreview.onclick = () => {
      cleanupUrl();
      els.previewPanel.hidden = true;
      els.previewBody.textContent = "";
    };

    if (isTextPreview(entry.name, mime)) {
      const textarea = document.createElement("textarea");
      textarea.className = "text-editor";
      textarea.spellcheck = false;
      textarea.readOnly = !state.clientCanWrite;
      textarea.setAttribute("aria-readonly", textarea.readOnly ? "true" : "false");
      textarea.value = await blob.text();
      els.previewBody.appendChild(textarea);
      if (state.clientCanWrite) {
        els.savePreview.hidden = false;
        els.savePreview.onclick = async () => {
          const file = new File([textarea.value], entry.name, { type: mime || "text/plain" });
          await uploadRemoteFile(file, entry.path);
          await listRemote(parentPath(entry.path));
          toast("Saved");
        };
      }
    } else if (mime.startsWith("image/")) {
      objectUrl = URL.createObjectURL(blob);
      const img = document.createElement("img");
      img.className = "preview-media";
      img.alt = entry.name;
      img.src = objectUrl;
      els.previewBody.appendChild(img);
    } else if (mime.startsWith("audio/")) {
      objectUrl = URL.createObjectURL(blob);
      const audio = document.createElement("audio");
      audio.controls = true;
      audio.src = objectUrl;
      els.previewBody.appendChild(audio);
    } else if (mime.startsWith("video/")) {
      objectUrl = URL.createObjectURL(blob);
      const video = document.createElement("video");
      video.className = "preview-media";
      video.controls = true;
      video.src = objectUrl;
      els.previewBody.appendChild(video);
    } else if (mime === "application/pdf") {
      objectUrl = URL.createObjectURL(blob);
      const frame = document.createElement("iframe");
      frame.className = "preview-frame";
      frame.src = objectUrl;
      els.previewBody.appendChild(frame);
    } else {
      els.previewPanel.hidden = true;
      return downloadRemote(entry);
    }
    els.previewPanel.hidden = false;
  }

  async function uploadFiles(files) {
    if (!state.clientCanWrite) throw new Error("The host has writes disabled");
    const list = [...files].filter((f) => f && f.size !== undefined);
    for (const file of list) await uploadRemoteFile(file);
    if (list.length) await listRemote(state.currentPath);
  }

  async function uploadPickedFolder() {
    if (!state.clientCanWrite) throw new Error("The host has writes disabled");
    if (window.showDirectoryPicker) {
      let handle;
      try {
        handle = await window.showDirectoryPicker({ mode: "read" });
      } catch (e) {
        if (isAbortError(e)) return; // user dismissed the picker — not an error
        throw e;
      }
      await uploadDirectoryHandle(handle);
      await listRemote(state.currentPath);
      toast("Folder uploaded");
      return;
    }
    if (els.folderUploadInput && "webkitdirectory" in els.folderUploadInput) {
      els.folderUploadInput.click();
      return;
    }
    throw new Error("Folder upload is not supported in this browser");
  }

  async function uploadDirectoryHandle(handle, targetPath = "") {
    if (handle.kind !== "directory") throw new Error("Choose a folder to upload");
    const rootPath = targetPath || joinPath(state.currentPath, handle.name || "folder");
    await createRemoteDirectory(rootPath);
    for await (const [name, child] of handle.entries()) {
      const childPath = joinSafeRelativePath(rootPath, name);
      if (child.kind === "directory") {
        await uploadDirectoryHandle(child, childPath);
      } else if (child.kind === "file") {
        await uploadRemoteFile(await child.getFile(), childPath);
      }
    }
  }

  async function uploadDirectoryFiles(files) {
    if (!state.clientCanWrite) throw new Error("The host has writes disabled");
    const list = [...files].filter((f) => f && f.size !== undefined);
    for (const file of list) {
      const rel = safeRelativePath(file.webkitRelativePath || file.name);
      if (!rel) continue;
      await uploadRemoteFile(file, joinSafeRelativePath(state.currentPath, rel));
    }
    if (list.length) {
      await listRemote(state.currentPath);
      toast("Folder uploaded");
    }
  }

  async function createRemoteDirectory(path) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");
    const p = normalizePath(path);
    await requestJson(state.dataChannel, { t: "mkdir", path: p }, 30000);
    CACHE.afterWrite(p);
  }

  async function uploadRemoteFile(file, targetPath = "") {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");
    const name = safeEntryName(file.name);
    const path = targetPath ? normalizePath(targetPath) : joinPath(state.currentPath, name);
    const { id, label, progress } = makeTransfer({ name, size: file.size }, "uploading");

    const ready = expectResponse(id, 30000);
    safeSendJson(state.dataChannel, { t: "uploadStart", id, path, size: file.size, mtime: file.lastModified });
    await ready;

    const reader = file.stream().getReader();
    let sent = 0;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (let off = 0; off < value.byteLength; off += CHUNK_SIZE) {
          const chunk = value.slice(off, Math.min(off + CHUNK_SIZE, value.byteLength));
          await sendBinaryChunk(state.dataChannel, id, chunk);
          sent += chunk.byteLength;
          progress.value = sent;
          label.textContent = `${name} — ${formatSize(sent)} / ${formatSize(file.size || sent)}`;
        }
      }
    } finally {
      reader.releaseLock();
    }

    const done = expectResponse(id, Math.max(30000, Math.ceil((file.size || 0) / 1024)));
    safeSendJson(state.dataChannel, { t: "uploadEnd", id });
    await done;
    // Host confirmed: only now drop the stale parent listing, metadata, and any
    // cached preview for the overwritten/created path.
    CACHE.afterWrite(path);
    label.textContent = `${name} — uploaded`;
    progress.value = progress.max;
  }

  async function deleteRemote(entry) {
    if (!state.clientCanWrite) throw new Error("The host has writes disabled");
    if (!confirm(`Delete ${entry.name}?`)) return;
    await requestJson(state.dataChannel, { t: "delete", path: entry.path }, 30000);
    // Host confirmed the delete: drop cached listing/metadata/preview before re-list.
    CACHE.afterDelete(entry.path);
    await listRemote(entry.kind === "directory" ? state.currentPath : parentPath(entry.path));
    toast("Deleted");
  }

  function renderWriteUi() {
    if (els.uploadButton) els.uploadButton.disabled = !state.clientCanWrite;
    if (els.uploadFolderButton) els.uploadFolderButton.disabled = !state.clientCanWrite || !supportsFolderUpload();
    if (els.dropzone) {
      els.dropzone.hidden = !state.clientCanWrite;
      els.dropzone.textContent = state.clientCanWrite ? "Drop files here to upload to this folder" : "";
    }
    const editor = els.previewBody?.querySelector?.(".text-editor");
    if (editor) {
      editor.readOnly = !state.clientCanWrite;
      editor.setAttribute("aria-readonly", editor.readOnly ? "true" : "false");
      if (!state.clientCanWrite && els.savePreview) {
        els.savePreview.hidden = true;
        els.savePreview.onclick = null;
      }
    }
  }

  function formatSize(bytes) {
    let n = Number(bytes || 0);
    const units = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; ++i; }
    return `${n.toFixed(i === 0 ? 0 : 1)} ${units[i]}`;
  }

  function humanRate(bytesPerSec) {
    const u = ["B/s", "KB/s", "MB/s", "GB/s"];
    let i = 0;
    while (bytesPerSec >= 1024 && i < u.length - 1) { bytesPerSec /= 1024; ++i; }
    return `${bytesPerSec.toFixed(1)} ${u[i]}`;
  }

  function refreshStats() {
    const scale = 1000 / STATS_INTERVAL_MS;
    const parts = [];
    if (state.hostWs) {
      parts.push(`Serve ↑${humanRate((state.bytesServed - state.lastServed) * scale)}`);
      state.lastServed = state.bytesServed;
    }
    if (state.dataChannel) {
      parts.push(`Mount ↓${humanRate((state.bytesReceived - state.lastReceived) * scale)}`);
      state.lastReceived = state.bytesReceived;
    }
    els.statsLabel.textContent = parts.length ? parts.join("   |   ") : "Idle";
  }

  function copy(text) {
    if (navigator.clipboard?.writeText) return navigator.clipboard.writeText(text);
    const textarea = document.createElement("textarea");
    textarea.value = text;
    textarea.setAttribute("readonly", "");
    textarea.style.position = "fixed";
    textarea.style.left = "-9999px";
    document.body.appendChild(textarea);
    textarea.select();
    try {
      if (!document.execCommand("copy")) throw new Error("Clipboard copy failed");
      return Promise.resolve();
    } catch (e) {
      return Promise.reject(e);
    } finally {
      textarea.remove();
    }
  }

  function selectTab(name) {
    for (const tab of els.tabs) {
      const active = tab.dataset.tab === name;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", active ? "true" : "false");
    }
    for (const page of els.pages) page.hidden = page.dataset.page !== name;
  }

  function initEvents() {
    for (const tab of els.tabs) tab.onclick = () => selectTab(tab.dataset.tab);

    els.browseFolder.onclick = () => pickFolder().catch((e) => toast(e.message));

    els.shareToggle.onclick = () => {
      if (state.hostWs || state.manualHost) stopHost().catch((e) => toast(e.message));
      else startHost().catch((e) => { els.shareToggle.disabled = false; toast(e.message); });
    };

    els.copyAll.onclick = () => copy(copyShareDetails()).then(() => toast("Code copied")).catch((e) => toast(e.message));
    els.copyShareLink.onclick = () => copy(hostShareLink()).then(() => toast("Share link copied")).catch((e) => toast(e.message));

    els.connectInput.oninput = () => {
      state.pendingConnectToken = "";
      state.directFilePath = "";
      state.directFileConsumed = false;
      state.directFolderPath = "";
      state.directFolderConsumed = false;
      state.focusPath = "";
    };

    if (els.applyOfflineAnswer) {
      els.applyOfflineAnswer.onclick = () => applyManualAnswer(els.offlineAnswerInput.value).catch((e) => toast(e.message));
    }

    if (els.copyOfflineAnswer) {
      els.copyOfflineAnswer.onclick = () => copy(els.offlineAnswerOut.value).then(() => toast("Answer copied")).catch((e) => toast(e.message));
    }

    els.connectToggle.onclick = () => {
      if (state.clientWs || state.dataChannel || state.clientPeer || state.manualClient) disconnectClient();
      else connect().catch((e) => toast(e.message));
    };

    els.upButton.onclick = () => {
      const parts = state.currentPath.split("/").filter(Boolean);
      parts.pop();
      listRemote("/" + parts.join("/")).catch((e) => toast(e.message));
    };

    if (els.shareFolderButton) {
      els.shareFolderButton.onclick = () => {
        const link = clientShareLink(state.currentPath || "/", "folder");
        copy(link).then(() => toast("Folder link copied")).catch((e) => toast(e.message));
      };
    }

    if (els.uploadButton && els.uploadInput) {
      els.uploadButton.onclick = () => els.uploadInput.click();
      els.uploadInput.onchange = () => {
        uploadFiles(els.uploadInput.files).catch((e) => toast(e.message));
        els.uploadInput.value = "";
      };
    }

    if (els.uploadFolderButton) {
      els.uploadFolderButton.onclick = () => uploadPickedFolder().catch((e) => toast(e.message));
    }

    if (els.folderUploadInput) {
      els.folderUploadInput.onchange = () => {
        uploadDirectoryFiles(els.folderUploadInput.files).catch((e) => toast(e.message));
        els.folderUploadInput.value = "";
      };
    }

    if (els.dropzone) {
      els.dropzone.ondragover = (e) => {
        if (!state.clientCanWrite) return;
        e.preventDefault();
        els.dropzone.classList.add("dragover");
      };
      els.dropzone.ondragleave = () => els.dropzone.classList.remove("dragover");
      els.dropzone.ondrop = (e) => {
        if (!state.clientCanWrite) return;
        e.preventDefault();
        els.dropzone.classList.remove("dragover");
        uploadFiles(e.dataTransfer.files).catch((err) => toast(err.message));
      };
    }

    if (els.closePreview) els.closePreview.onclick = () => { els.previewPanel.hidden = true; els.previewBody.textContent = ""; };

    window.addEventListener("beforeunload", disconnectAll);
    window.addEventListener("hashchange", openSharedHash);
    window.addEventListener("pageshow", (event) => {
      if (event.persisted) openSharedHash();
    });
  }

  function isMobile() {
    return /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent)
      || (navigator.maxTouchPoints > 1 && window.innerWidth < 800);
  }

  function openSharedHash() {
    if (!applyHash() || hasClientSession()) return;
    setConnectStatus("Opening shared link…");
    connect().catch((e) => {
      setConnectStatus("Not connected.");
      toast(e.message);
    });
  }

  function init() {
    const missing = unsupportedBrowserReasons();
    if (missing.length) {
      blockUnsupportedBrowser(missing);
      return;
    }
    if (isMobile() || !supportsHosting()) disableHostTab();
    initEvents();
    openSharedHash();
    setInterval(refreshStats, STATS_INTERVAL_MS);
    // Hidden devtools-only diagnostic (not a UI setting): console -> fbCacheStats()
    try { window.fbCacheStats = () => CACHE.snapshot(); } catch { /* ignore */ }
  }

  init();
})();
