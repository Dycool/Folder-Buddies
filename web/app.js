import { argon2id } from "./vendor/noble/argon2.js";

(() => {
  "use strict";

  const BASE91 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
  const ROOM_LEN = 6;
  const LOOKUP_LEN = 2;          // public half: the Durable Object room name
  const ARGON = { t: 3, m: 65536, p: 1, dkLen: 32 }; // matches libsodium in the native app
  const BIN_MAGIC = 0x4642494e; // FBIN
  const CHUNK_SIZE = 64 * 1024;
  const ICE_TIMEOUT_MS = 8000;
  const STATS_INTERVAL_MS = 500;
  const DEFAULT_ICE = [{ urls: "stun:stun.l.google.com:19302" }];

  const $ = (id) => document.getElementById(id);
  const els = {
    tabs: [...document.querySelectorAll(".tab")],
    pages: [...document.querySelectorAll(".tabpage")],
    folderInput: $("folderInput"),
    browseFolder: $("browseFolder"),
    maxClients: $("maxClients"),
    shareToggle: $("shareToggle"),
    connectCode: $("connectCode"),
    copyAll: $("copyAll"),
    shareStatus: $("shareStatus"),
    connectInput: $("connectInput"),
    connectToggle: $("connectToggle"),
    connectStatus: $("connectStatus"),
    explorer: $("explorer"),
    currentPath: $("currentPath"),
    upButton: $("upButton"),
    fileRows: $("fileRows"),
    transfers: $("transfers"),
    statsLabel: $("statsLabel"),
    toast: $("toast"),
    turnstileMount: $("turnstileMount"),
  };

  const state = {
    rootHandle: null,
    code: "",          // full 6-char code
    lookup: "",        // public half (relay room)
    rootBase: null,    // HKDF base key derived from the secret half
    maxClients: 0,
    hostWs: null,
    clientWs: null,
    hostPeers: new Map(),
    clientPeer: null,
    dataChannel: null,
    currentPath: "/",
    pending: new Map(),
    downloads: new Map(),
    nextReqId: 1,
    turnstileWidgetId: null,
    turnstileResolver: null,
    toastTimer: 0,
    bytesServed: 0,
    bytesReceived: 0,
    lastServed: 0,
    lastReceived: 0,
  };

  const te = new TextEncoder();
  const td = new TextDecoder();

  function setShareStatus(text) { els.shareStatus.textContent = text; }
  function setConnectStatus(text) { els.connectStatus.textContent = text; }

  function toast(text) {
    els.toast.textContent = text;
    els.toast.classList.add("show");
    clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(() => els.toast.classList.remove("show"), 2400);
  }

  function clean(value) {
    return String(value || "").trim();
  }

  function config() {
    return window.FB_WEBAPP_CONFIG || {};
  }

  function workerUrl() {
    const value = clean(config().signalingUrl);
    if (!value) throw new Error("Worker URL is missing. Set FB_SIGNALING_URL in GitHub Actions variables.");
    return value.replace(/\/+$/, "");
  }

  function iceServers() {
    return Array.isArray(config().iceServers) ? config().iceServers : DEFAULT_ICE;
  }

  function looksLikeRoom(text) {
    return text.length === ROOM_LEN && [...text].every((c) => BASE91.includes(c));
  }

  function lookupOf(code) { return code.slice(0, LOOKUP_LEN); }
  function keyPartOf(code) { return code.slice(LOOKUP_LEN); }

  function randomBytes(n) {
    const b = new Uint8Array(n);
    crypto.getRandomValues(b);
    return b;
  }

  function randomRoom() {
    return [...randomBytes(ROOM_LEN)].map((b) => BASE91[b % BASE91.length]).join("");
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
  async function deriveRootBase(code) {
    const saltSeed = await sha256Bytes(te.encode("FolderBuddies-web-salt-v1\0" + lookupOf(code)));
    const root = argon2id(te.encode(keyPartOf(code)), saltSeed.slice(0, 16), ARGON);
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

  function shareLink(code) {
    return `${location.origin}${location.pathname}#r=${code}`;
  }

  function parseJoinInput(raw) {
    const text = clean(raw);
    const fromHash = (frag) => new URLSearchParams(frag).get("r") || "";
    try {
      const url = new URL(text);
      const r = fromHash(url.hash.slice(1));
      if (r) return r;
    } catch { /* not a URL */ }
    if (text.includes("#")) {
      const r = fromHash(text.split("#").pop());
      if (r) return r;
    }
    return text;
  }

  function applyHash() {
    if (!location.hash) return;
    const room = new URLSearchParams(location.hash.slice(1)).get("r") || "";
    if (looksLikeRoom(room)) {
      els.connectInput.value = room;
      selectTab("connect");
    }
  }

  async function turnstileToken() {
    const sitekey = clean(config().turnstileSiteKey);
    if (!sitekey) return "";
    await waitForTurnstile();
    if (state.turnstileWidgetId === null) {
      state.turnstileWidgetId = window.turnstile.render(els.turnstileMount, {
        sitekey,
        size: "invisible",
        callback: (token) => {
          if (state.turnstileResolver) {
            state.turnstileResolver.resolve(token || "");
            state.turnstileResolver = null;
          }
        },
        "error-callback": () => {
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
      const ws = new WebSocket(signalUrl(lookup, role, token));
      const timer = setTimeout(() => reject(new Error("Signaling timed out")), 12000);
      ws.onopen = () => { clearTimeout(timer); resolve(ws); };
      ws.onerror = () => { clearTimeout(timer); reject(new Error("Signaling failed")); };
      ws.onclose = () => {
        if (role === "host" && state.hostWs === ws) setShareStatus("Not sharing.");
        if (role === "client" && state.clientWs === ws) setConnectStatus("Disconnected.");
      };
      ws.onmessage = (event) => {
        try { onMessage(JSON.parse(event.data)); }
        catch { /* ignore malformed signaling */ }
      };
    });
  }

  // Connect as host, regenerating the code if the relay room is already taken.
  async function openHostSignal() {
    for (let attempt = 0; attempt < 12; ++attempt) {
      const code = randomRoom();
      const lookup = lookupOf(code);
      const token = await turnstileToken().catch(() => "");
      const ws = await openSignal("host", lookup, (msg) => handleHostSignal(msg).catch((err) => toast(err.message)), token);
      const claimed = await new Promise((resolve) => {
        const onReady = (event) => {
          let msg; try { msg = JSON.parse(event.data); } catch { return; }
          if (msg.kind === "ready") { cleanup(); resolve(true); }
          else if (msg.kind === "error" && msg.error === "host_already_connected") { cleanup(); resolve(false); }
        };
        const onClose = () => { cleanup(); resolve(false); };
        const cleanup = () => { ws.removeEventListener("message", onReady); ws.removeEventListener("close", onClose); };
        ws.addEventListener("message", onReady);
        ws.addEventListener("close", onClose);
      });
      if (claimed) return { code, lookup, ws };
      try { ws.close(); } catch { /* already closing */ }
    }
    throw new Error("Could not find a free room code");
  }

  async function sendEncryptedSignal(ws, peerId, payload) {
    const aad = `web-signal:${state.lookup}:${peerId}`;
    const ciphertext = await encryptJson(aad, payload);
    ws.send(JSON.stringify({ kind: "signal", peerId, ciphertext }));
  }

  function decryptSignal(peerId, ciphertext) {
    const aad = `web-signal:${state.lookup}:${peerId}`;
    return decryptJson(aad, ciphertext);
  }

  function makePeer(label) {
    const pc = new RTCPeerConnection({ iceServers: iceServers() });
    pc.onconnectionstatechange = () => {
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
    dc.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      let msg;
      try { msg = JSON.parse(event.data); } catch { return; }
      handleHostRequest(peerId, dc, msg).catch((e) => safeSendJson(dc, { t: "error", id: msg?.id || 0, message: e.message }));
    };
  }

  function setupClientChannel(dc) {
    state.dataChannel = dc;
    dc.binaryType = "arraybuffer";
    dc.onopen = () => {
      setConnectStatus("Connected.");
      els.explorer.hidden = false;
      listRemote("/").catch((e) => toast(e.message));
    };
    dc.onclose = () => setConnectStatus("Disconnected.");
    dc.onerror = () => toast("Connection error");
    dc.onmessage = (event) => {
      if (typeof event.data === "string") handleClientJson(event.data);
      else handleClientBinary(event.data);
    };
  }

  function safeSendJson(dc, obj) {
    if (dc.readyState === "open") dc.send(JSON.stringify(obj));
  }

  function nextId() {
    return state.nextReqId++ >>> 0;
  }

  function requestJson(dc, obj, timeoutMs = 20000) {
    const id = obj.id || nextId();
    obj.id = id;
    const p = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        state.pending.delete(id);
        reject(new Error("Request timed out"));
      }, timeoutMs);
      state.pending.set(id, { resolve, reject, timer });
    });
    safeSendJson(dc, obj);
    return p;
  }

  function handleClientJson(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }
    if (msg.t === "error") {
      const pending = state.pending.get(msg.id);
      if (pending) {
        clearTimeout(pending.timer);
        state.pending.delete(msg.id);
        pending.reject(new Error(msg.message || "Remote error"));
      }
      toast(msg.message || "Remote error");
      return;
    }
    if (msg.t === "listResult") {
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
    if (d.writer) {
      await d.writeQueue;
      await d.writer.close();
    } else {
      const blob = new Blob(d.chunks, { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = d.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 10000);
    }
    d.label.textContent = `${d.name} — done`;
    d.progress.value = d.progress.max;
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
      safeSendJson(dc, { t: "listResult", id: msg.id, path, entries });
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
    throw new Error("Unknown request");
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
    return normalizePath(`${base.replace(/\/$/, "")}/${name}`);
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

  async function fileHandleFor(path) {
    const handle = await handleForPath(path);
    if (handle.kind !== "file") throw new Error("Not a file");
    return handle;
  }

  async function createHostPeer(peerId) {
    const pc = makePeer(`Peer ${peerId}`);
    const dc = pc.createDataChannel("folderbuddies-files", { ordered: true });
    setupHostChannel(peerId, dc);
    state.hostPeers.set(peerId, { pc, dc });
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceComplete(pc);
    await sendEncryptedSignal(state.hostWs, peerId, { type: "offer", sdp: pc.localDescription });
  }

  async function handleHostSignal(msg) {
    if (msg.kind === "ready") {
      renderShareStatus();
      return;
    }
    if (msg.kind === "client-joined") {
      if (state.maxClients > 0 && state.hostPeers.size >= state.maxClients) {
        toast("Max clients reached; ignoring a new connection");
        return;
      }
      createHostPeer(msg.peerId).then(renderShareStatus).catch((e) => toast(e.message));
      return;
    }
    if (msg.kind === "client-left") {
      const peer = state.hostPeers.get(msg.peerId);
      if (peer) peer.pc.close();
      state.hostPeers.delete(msg.peerId);
      renderShareStatus();
      return;
    }
    if (msg.kind === "signal") {
      const signal = await decryptSignal(msg.peerId, msg.ciphertext);
      if (signal.type !== "answer") return;
      const peer = state.hostPeers.get(msg.peerId);
      if (peer) await peer.pc.setRemoteDescription(signal.sdp);
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
    if (msg.kind === "signal") {
      const peerId = state.clientPeer?.peerId || msg.peerId;
      const signal = await decryptSignal(peerId, msg.ciphertext);
      if (signal.type === "offer") {
        await acceptOffer(peerId, signal.sdp, async (answer) => {
          await sendEncryptedSignal(state.clientWs, peerId, { type: "answer", sdp: answer });
        });
      }
    }
  }

  async function acceptOffer(peerId, offerSdp, sendAnswer) {
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
    if (!state.hostWs) { setShareStatus("Not sharing."); return; }
    setShareStatus(`Sharing — ${state.hostPeers.size} client(s)`);
  }

  function setShareRunning(running) {
    els.shareToggle.textContent = running ? "Stop sharing" : "Start sharing";
    els.folderInput.disabled = running;
    els.browseFolder.disabled = running;
    els.maxClients.disabled = running;
    els.copyAll.disabled = !running;
    if (!running) {
      els.connectCode.value = "";
    }
  }

  async function pickFolder() {
    if (!window.showDirectoryPicker) throw new Error("Folder hosting needs Chromium or Edge");
    state.rootHandle = await window.showDirectoryPicker({ mode: "read" });
    els.folderInput.value = state.rootHandle.name;
  }

  async function startHost() {
    if (!state.rootHandle) await pickFolder();
    setShareStatus("Starting…");
    els.shareToggle.disabled = true;
    const maxClients = Number(els.maxClients.value);
    state.maxClients = Number.isFinite(maxClients) && maxClients > 0 ? Math.floor(maxClients) : 0;

    try {
      const { code, lookup, ws } = await openHostSignal();
      state.code = code;
      state.lookup = lookup;
      state.rootBase = await deriveRootBase(code);
      state.hostWs = ws;
      ws.onmessage = (event) => {
        try { handleHostSignal(JSON.parse(event.data)).catch((e) => toast(e.message)); }
        catch { /* ignore */ }
      };
    } catch (e) {
      state.code = "";
      state.lookup = "";
      state.rootBase = null;
      throw e;
    } finally {
      els.shareToggle.disabled = false;
    }

    els.connectCode.value = state.code;
    setShareRunning(true);
    renderShareStatus();
    toast("Sharing.");
  }

  async function stopHost() {
    if (state.hostWs) state.hostWs.close();
    for (const p of state.hostPeers.values()) p.pc.close();
    state.hostWs = null;
    state.hostPeers.clear();
    state.code = "";
    state.lookup = "";
    state.rootBase = null;
    setShareRunning(false);
    setShareStatus("Not sharing.");
    toast("Stopped sharing");
  }

  function copyShareDetails() {
    return ["Connect code:", els.connectCode.value, "", "Share link:", shareLink(els.connectCode.value)].join("\n");
  }

  function setConnected(connected) {
    els.connectToggle.textContent = connected ? "Disconnect" : "Connect & browse";
    els.connectInput.disabled = connected;
  }

  async function connect() {
    const code = parseJoinInput(els.connectInput.value);
    if (!looksLikeRoom(code)) throw new Error("Paste the 6-character room code or share link");

    setConnectStatus("Connecting…");
    state.code = code;
    state.lookup = lookupOf(code);
    state.rootBase = await deriveRootBase(code);
    const token = await turnstileToken();
    state.clientWs = await openSignal("client", state.lookup, (msg) => handleClientSignal(msg).catch((e) => toast(e.message)), token);
    setConnected(true);
  }

  function disconnectClient() {
    if (state.clientWs) state.clientWs.close();
    if (state.clientPeer?.pc) state.clientPeer.pc.close();
    state.clientWs = null;
    state.clientPeer = null;
    state.dataChannel = null;
    setConnected(false);
    els.explorer.hidden = true;
    els.fileRows.textContent = "";
    els.transfers.textContent = "";
    setConnectStatus("Not connected.");
  }

  function disconnectAll() {
    stopHost().catch(() => {});
    disconnectClient();
  }

  async function listRemote(path) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");
    const p = normalizePath(path);
    const res = await requestJson(state.dataChannel, { t: "list", path: p });
    state.currentPath = res.path || p;
    els.currentPath.textContent = state.currentPath;
    renderRows(res.entries || []);
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
      tr.className = e.kind === "directory" ? "dir" : "file";
      const name = document.createElement("td");
      name.className = "name";
      name.textContent = e.kind === "directory" ? `📁 ${e.name}` : `📄 ${e.name}`;
      if (e.kind === "directory") name.onclick = () => listRemote(e.path).catch((err) => toast(err.message));
      const size = document.createElement("td");
      size.textContent = e.kind === "file" ? formatSize(e.size || 0) : "—";
      const action = document.createElement("td");
      if (e.kind === "file") {
        const btn = document.createElement("button");
        btn.textContent = "Download";
        btn.onclick = () => downloadRemote(e).catch((err) => toast(err.message));
        action.appendChild(btn);
      }
      tr.append(name, size, action);
      els.fileRows.appendChild(tr);
    }
  }

  async function downloadRemote(entry) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");

    let writer = null;
    if (window.showSaveFilePicker) {
      const saveHandle = await window.showSaveFilePicker({ suggestedName: entry.name });
      writer = await saveHandle.createWritable();
    }

    const id = nextId();
    const transfer = document.createElement("div");
    transfer.className = "transfer";
    const label = document.createElement("div");
    label.textContent = `${entry.name} — starting`;
    const progress = document.createElement("progress");
    progress.max = entry.size || 1;
    progress.value = 0;
    transfer.append(label, progress);
    els.transfers.prepend(transfer);

    state.downloads.set(id, { name: entry.name, size: entry.size || 0, received: 0, chunks: [], writer, writeQueue: Promise.resolve(), label, progress });
    safeSendJson(state.dataChannel, { t: "download", id, path: entry.path });
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
    return navigator.clipboard.writeText(text);
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
      if (state.hostWs) stopHost().catch((e) => toast(e.message));
      else startHost().catch((e) => { els.shareToggle.disabled = false; toast(e.message); });
    };

    els.copyAll.onclick = () => copy(copyShareDetails()).then(() => toast("Copied")).catch((e) => toast(e.message));

    els.connectToggle.onclick = () => {
      if (state.clientWs || state.dataChannel) disconnectClient();
      else connect().catch((e) => toast(e.message));
    };

    els.upButton.onclick = () => {
      const parts = state.currentPath.split("/").filter(Boolean);
      parts.pop();
      listRemote("/" + parts.join("/")).catch((e) => toast(e.message));
    };

    window.addEventListener("beforeunload", disconnectAll);
  }

  function init() {
    applyHash();
    initEvents();
    setInterval(refreshStats, STATS_INTERVAL_MS);
  }

  init();
})();
