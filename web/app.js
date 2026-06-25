(() => {
  "use strict";

  const BASE91 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
  const ROOM_LEN = 6;
  const BIN_MAGIC = 0x4642494e; // "FBIN"
  const CHUNK_SIZE = 64 * 1024;
  const ICE_TIMEOUT_MS = 8000;

  const $ = (id) => document.getElementById(id);
  const els = {
    workerUrl: $("workerUrl"),
    iceServers: $("iceServers"),
    turnstileBox: $("turnstileBox"),
    turnstileWidget: $("turnstileWidget"),
    turnstileStatus: $("turnstileStatus"),
    connectionState: $("connectionState"),
    pickFolder: $("pickFolder"),
    hostCloud: $("hostCloud"),
    hostOffline: $("hostOffline"),
    stopHost: $("stopHost"),
    hostFolder: $("hostFolder"),
    hostRoom: $("hostRoom"),
    hostPassword: $("hostPassword"),
    shareLink: $("shareLink"),
    copyShare: $("copyShare"),
    copyPassword: $("copyPassword"),
    offlineAnswer: $("offlineAnswer"),
    acceptOfflineAnswer: $("acceptOfflineAnswer"),
    joinCode: $("joinCode"),
    joinPassword: $("joinPassword"),
    joinCloud: $("joinCloud"),
    makeOfflineAnswer: $("makeOfflineAnswer"),
    disconnect: $("disconnect"),
    offlineAnswerOut: $("offlineAnswerOut"),
    copyOfflineAnswer: $("copyOfflineAnswer"),
    explorer: $("explorer"),
    remoteStatus: $("remoteStatus"),
    currentPath: $("currentPath"),
    fileRows: $("fileRows"),
    upButton: $("upButton"),
    transfers: $("transfers"),
    log: $("log"),
  };

  const state = {
    rootHandle: null,
    rootName: "",
    password: "",
    room: "",
    hostWs: null,
    clientWs: null,
    hostPeers: new Map(), // peerId -> { pc, dc }
    clientPeer: null,
    dataChannel: null,
    currentPath: "/",
    pending: new Map(),
    nextReqId: 1,
    downloads: new Map(),
    offlineHostPeer: null,
    turnstileWidgetId: null,
    turnstileToken: "",
  };

  function log(message, cls = "") {
    const line = document.createElement("div");
    if (cls) line.className = cls;
    line.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;
    els.log.appendChild(line);
    els.log.scrollTop = els.log.scrollHeight;
  }

  function setStatus(text) {
    els.connectionState.textContent = text;
  }

  function cleanInput(value) {
    return String(value || "").trim();
  }

  function encodeHash(params) {
    const sp = new URLSearchParams();
    for (const [k, v] of Object.entries(params)) if (v) sp.set(k, v);
    return `${location.origin}${location.pathname}#${sp.toString()}`;
  }

  function parseSharedInput(raw) {
    const text = cleanInput(raw);
    try {
      const url = new URL(text);
      if (url.hash) {
        const sp = new URLSearchParams(url.hash.slice(1));
        return {
          room: sp.get("r") || "",
          offlineOffer: sp.get("o") || "",
          password: sp.get("p") || "",
        };
      }
    } catch { /* not a URL */ }
    if (text.includes("#")) {
      const sp = new URLSearchParams(text.split("#").pop());
      return {
        room: sp.get("r") || "",
        offlineOffer: sp.get("o") || "",
        password: sp.get("p") || "",
      };
    }
    return looksLikeRoom(text) ? { room: text, offlineOffer: "", password: "" }
                               : { room: "", offlineOffer: text, password: "" };
  }

  function applyHashToJoinForm() {
    if (!location.hash) return;
    const sp = new URLSearchParams(location.hash.slice(1));
    const room = sp.get("r") || "";
    const offlineOffer = sp.get("o") || "";
    const password = sp.get("p") || "";
    if (room || offlineOffer) els.joinCode.value = room || offlineOffer;
    if (password) els.joinPassword.value = password;
  }

  function looksLikeRoom(text) {
    return text.length === ROOM_LEN && [...text].every((c) => BASE91.includes(c));
  }

  function randomBytes(n) {
    const b = new Uint8Array(n);
    crypto.getRandomValues(b);
    return b;
  }

  function randomRoom() {
    return [...randomBytes(ROOM_LEN)].map((b) => BASE91[b % BASE91.length]).join("");
  }

  function randomPassword() {
    return base91Encode(randomBytes(32));
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
      if (c === undefined) throw new Error("Invalid Base91 text");
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

  function concatBytes(...parts) {
    const size = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array(size);
    let off = 0;
    for (const p of parts) { out.set(p, off); off += p.length; }
    return out;
  }

  const te = new TextEncoder();
  const td = new TextDecoder();

  async function deriveAesKey(password, salt, aad) {
    const root = await crypto.subtle.digest("SHA-256", te.encode(`FolderBuddies-web-root-v1\0${password}`));
    const baseKey = await crypto.subtle.importKey("raw", root, "HKDF", false, ["deriveKey"]);
    return crypto.subtle.deriveKey(
      { name: "HKDF", hash: "SHA-256", salt, info: te.encode(aad) },
      baseKey,
      { name: "AES-GCM", length: 256 },
      false,
      ["encrypt", "decrypt"]
    );
  }

  async function encryptJson(password, aad, obj) {
    const salt = randomBytes(16);
    const iv = randomBytes(12);
    const key = await deriveAesKey(password, salt, aad);
    const plain = te.encode(JSON.stringify(obj));
    const ct = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: te.encode(aad) }, key, plain));
    return base91Encode(concatBytes(te.encode("FBW1"), salt, iv, ct));
  }

  async function decryptJson(password, aad, blob) {
    const data = base91Decode(blob);
    if (data.length < 4 + 16 + 12 + 16 || td.decode(data.slice(0, 4)) !== "FBW1") {
      throw new Error("Not a Folder Buddies Web encrypted blob");
    }
    const salt = data.slice(4, 20);
    const iv = data.slice(20, 32);
    const ct = data.slice(32);
    const key = await deriveAesKey(password, salt, aad);
    const plain = await crypto.subtle.decrypt({ name: "AES-GCM", iv, additionalData: te.encode(aad) }, key, ct);
    return JSON.parse(td.decode(plain));
  }

  function getWorkerUrl() {
    const value = cleanInput(els.workerUrl.value || window.FB_WEBAPP_CONFIG?.signalingUrl || "");
    if (!value) throw new Error("Set your Cloudflare Worker URL first.");
    return value.replace(/\/+$/, "");
  }

  function getTurnstileSiteKey() {
    return cleanInput(window.FB_WEBAPP_CONFIG?.turnstileSiteKey || "");
  }

  function initTurnstile() {
    const siteKey = getTurnstileSiteKey();
    if (!siteKey) {
      els.turnstileBox.hidden = true;
      return;
    }
    els.turnstileBox.hidden = false;
    els.turnstileStatus.textContent = "Loading Cloudflare Turnstile…";

    const render = () => {
      if (!window.turnstile) {
        setTimeout(render, 150);
        return;
      }
      if (state.turnstileWidgetId !== null) return;
      state.turnstileWidgetId = window.turnstile.render(els.turnstileWidget, {
        sitekey: siteKey,
        callback: (token) => {
          state.turnstileToken = token || "";
          els.turnstileStatus.textContent = state.turnstileToken
            ? "Browser check ready for the next cloud connection."
            : "Browser check did not return a token.";
        },
        "expired-callback": () => {
          state.turnstileToken = "";
          els.turnstileStatus.textContent = "Browser check expired. Please complete it again.";
        },
        "error-callback": () => {
          state.turnstileToken = "";
          els.turnstileStatus.textContent = "Turnstile failed to load. Offline fallback still works.";
        },
      });
    };
    render();
  }

  function resetTurnstile() {
    const siteKey = getTurnstileSiteKey();
    if (!siteKey || !window.turnstile || state.turnstileWidgetId === null) return;
    state.turnstileToken = "";
    els.turnstileStatus.textContent = "Refreshing browser check…";
    try { window.turnstile.reset(state.turnstileWidgetId); }
    catch { els.turnstileStatus.textContent = "Refresh the page to run Turnstile again."; }
  }

  async function takeTurnstileToken() {
    if (!getTurnstileSiteKey()) return "";
    if (!state.turnstileToken) {
      throw new Error("Complete the Turnstile browser check before using a cloud room.");
    }
    const token = state.turnstileToken;
    state.turnstileToken = "";
    // Turnstile tokens are short-lived and validated server-side, so consume one
    // per WebSocket connection and immediately ask the widget for the next one.
    setTimeout(resetTurnstile, 0);
    return token;
  }

  function makeWsUrl(room, role, turnstileToken = "") {
    const u = new URL(getWorkerUrl());
    u.protocol = u.protocol === "https:" ? "wss:" : "ws:";
    u.pathname = "/room";
    u.search = "";
    u.searchParams.set("code", room);
    u.searchParams.set("role", role);
    u.searchParams.set("web", "1");
    if (turnstileToken) u.searchParams.set("turnstile", turnstileToken);
    return u;
  }

  function parseIceServers() {
    try {
      const value = JSON.parse(els.iceServers.value || "[]");
      if (!Array.isArray(value)) throw new Error("ICE config must be an array");
      return value;
    } catch (e) {
      throw new Error(`Bad ICE servers JSON: ${e.message}`);
    }
  }

  function makePeerConnection(label) {
    const pc = new RTCPeerConnection({ iceServers: parseIceServers() });
    pc.onconnectionstatechange = () => log(`${label}: WebRTC ${pc.connectionState}`);
    pc.oniceconnectionstatechange = () => log(`${label}: ICE ${pc.iceConnectionState}`);
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

  async function openSignal(role, room, onMessage, turnstileToken = "") {
    const ws = new WebSocket(makeWsUrl(room, role, turnstileToken));
    ws.onopen = () => log(`${role} signaling connected for room ${room}`, "ok");
    ws.onclose = (event) => log(`${role} signaling closed (${event.code || "no code"})`);
    ws.onerror = () => log(`${role} signaling error`, "err");
    ws.onmessage = (event) => {
      try { onMessage(JSON.parse(event.data)); }
      catch { log("Bad signaling message ignored", "err"); }
    };
    return ws;
  }

  async function sendEncryptedSignal(ws, room, peerId, payload) {
    const aad = `web-signal:${room}:${peerId}`;
    const ciphertext = await encryptJson(state.password, aad, payload);
    ws.send(JSON.stringify({ kind: "signal", peerId, ciphertext }));
  }

  async function decryptSignal(room, peerId, ciphertext) {
    const aad = `web-signal:${room}:${peerId}`;
    return decryptJson(state.password, aad, ciphertext);
  }

  function setupHostDataChannel(peerId, dc) {
    dc.binaryType = "arraybuffer";
    dc.onopen = () => log(`Peer ${peerId} data channel open`, "ok");
    dc.onclose = () => log(`Peer ${peerId} data channel closed`);
    dc.onerror = () => log(`Peer ${peerId} data channel error`, "err");
    dc.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      let msg;
      try { msg = JSON.parse(event.data); } catch { return; }
      handleHostRequest(peerId, dc, msg).catch((e) => {
        safeSendJson(dc, { t: "error", id: msg?.id || 0, message: e.message });
      });
    };
  }

  function setupClientDataChannel(dc) {
    state.dataChannel = dc;
    dc.binaryType = "arraybuffer";
    dc.onopen = () => {
      log("Data channel open", "ok");
      setStatus("Connected");
      els.disconnect.disabled = false;
      els.explorer.hidden = false;
      els.remoteStatus.textContent = "Connected. Browsing remote folder.";
      listRemote("/").catch((e) => log(e.message, "err"));
    };
    dc.onclose = () => {
      log("Data channel closed");
      setStatus("Disconnected");
      els.remoteStatus.textContent = "Disconnected.";
    };
    dc.onerror = () => log("Data channel error", "err");
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
      log(msg.message || "Remote error", "err");
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
      if (d) {
        d.size = Number(msg.size || 0);
        d.received = 0;
        d.label.textContent = `${msg.name} — 0 / ${formatSize(d.size)}`;
        d.progress.max = d.size || 1;
        d.progress.value = 0;
      }
      return;
    }
    if (msg.t === "fileEnd") {
      finishDownload(msg.id).catch((e) => log(e.message, "err"));
    }
  }

  function handleClientBinary(buf) {
    const view = new DataView(buf);
    if (view.byteLength < 8 || view.getUint32(0, false) !== BIN_MAGIC) return;
    const id = view.getUint32(4, false);
    const d = state.downloads.get(id);
    if (!d) return;
    const chunk = new Uint8Array(buf, 8);
    d.received += chunk.byteLength;
    d.progress.value = d.received;
    d.label.textContent = `${d.name} — ${formatSize(d.received)} / ${formatSize(d.size || d.received)}`;
    if (d.writer) {
      d.writeQueue = d.writeQueue.then(() => d.writer.write(chunk));
      d.writeQueue.catch((e) => log(`Write failed: ${e.message}`, "err"));
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
    log(`Downloaded ${d.name}`, "ok");
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
          } catch { /* permission might have changed */ }
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
      log(`Served ${path} to ${peerId}`);
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
          const chunk = value.slice(off, Math.min(off + CHUNK_SIZE, value.byteLength));
          await sendBinaryChunk(dc, id, chunk);
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
    const pc = makePeerConnection(`host ${peerId}`);
    const dc = pc.createDataChannel("folderbuddies-files", { ordered: true });
    setupHostDataChannel(peerId, dc);
    state.hostPeers.set(peerId, { pc, dc });
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceComplete(pc);
    await sendEncryptedSignal(state.hostWs, state.room, peerId, { type: "offer", sdp: pc.localDescription });
    log(`Offer sent to ${peerId}`);
  }

  async function handleHostSignal(msg) {
    if (msg.kind === "ready") {
      setStatus("Hosting");
      return;
    }
    if (msg.kind === "client-joined") {
      log(`Client joined: ${msg.peerId}`);
      createHostPeer(msg.peerId).catch((e) => log(e.message, "err"));
      return;
    }
    if (msg.kind === "client-left") {
      const peer = state.hostPeers.get(msg.peerId);
      if (peer) peer.pc.close();
      state.hostPeers.delete(msg.peerId);
      log(`Client left: ${msg.peerId}`);
      return;
    }
    if (msg.kind === "signal") {
      const signal = await decryptSignal(state.room, msg.peerId, msg.ciphertext);
      if (signal.type === "answer") {
        const peer = state.hostPeers.get(msg.peerId);
        if (!peer) return;
        await peer.pc.setRemoteDescription(signal.sdp);
        log(`Answer accepted from ${msg.peerId}`, "ok");
      }
    }
  }

  async function handleClientSignal(msg) {
    if (msg.kind === "ready") {
      state.clientPeer = { peerId: msg.peerId, pc: null };
      setStatus("Waiting for host");
      log(`Joined room ${msg.room}; waiting for encrypted offer`);
      return;
    }
    if (msg.kind === "host-left") {
      log("Host left the room", "err");
      setStatus("Host left");
      return;
    }
    if (msg.kind === "signal") {
      const peerId = state.clientPeer?.peerId || msg.peerId;
      const signal = await decryptSignal(state.room, peerId, msg.ciphertext);
      if (signal.type === "offer") {
        await acceptOfferAndAnswer(peerId, signal.sdp, async (answer) => {
          await sendEncryptedSignal(state.clientWs, state.room, peerId, { type: "answer", sdp: answer });
        });
      }
    }
  }

  async function acceptOfferAndAnswer(peerId, offerSdp, sendAnswer) {
    const pc = makePeerConnection(`client ${peerId}`);
    state.clientPeer = { peerId, pc };
    pc.ondatachannel = (event) => setupClientDataChannel(event.channel);
    await pc.setRemoteDescription(offerSdp);
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    await waitForIceComplete(pc);
    await sendAnswer(pc.localDescription);
    log("Encrypted answer sent", "ok");
  }

  async function startCloudHost() {
    if (!state.rootHandle) throw new Error("Choose a folder first");
    state.password = randomPassword();
    state.room = randomRoom();
    const turnstileToken = await takeTurnstileToken();
    state.hostWs = await openSignal("host", state.room, (msg) => handleHostSignal(msg).catch((e) => log(e.message, "err")), turnstileToken);
    els.hostRoom.textContent = state.room;
    els.hostPassword.textContent = state.password;
    els.shareLink.value = encodeHash({ r: state.room, p: state.password });
    els.copyShare.disabled = false;
    els.copyPassword.disabled = false;
    els.stopHost.disabled = false;
    els.acceptOfflineAnswer.disabled = true;
    log("Cloud room link created. Share the link with the client.", "ok");
  }

  async function startOfflineHost() {
    if (!state.rootHandle) throw new Error("Choose a folder first");
    state.password = randomPassword();
    state.room = "offline";
    const pc = makePeerConnection("offline host");
    const dc = pc.createDataChannel("folderbuddies-files", { ordered: true });
    setupHostDataChannel("offline", dc);
    state.offlineHostPeer = pc;
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceComplete(pc);
    const blob = await encryptJson(state.password, "web-offline-offer", { type: "offer", sdp: pc.localDescription });
    els.hostRoom.textContent = "offline";
    els.hostPassword.textContent = state.password;
    els.shareLink.value = encodeHash({ o: blob, p: state.password });
    els.copyShare.disabled = false;
    els.copyPassword.disabled = false;
    els.stopHost.disabled = false;
    els.acceptOfflineAnswer.disabled = false;
    setStatus("Offline offer ready");
    log("Huge offline offer link created. The client must send an offline answer back.", "ok");
  }

  async function acceptOfflineAnswer() {
    if (!state.offlineHostPeer) throw new Error("No offline host session is waiting");
    const blob = cleanInput(els.offlineAnswer.value);
    const answer = await decryptJson(state.password, "web-offline-answer", blob);
    if (answer.type !== "answer") throw new Error("Offline blob is not an answer");
    await state.offlineHostPeer.setRemoteDescription(answer.sdp);
    setStatus("Offline connected");
    log("Offline answer accepted", "ok");
  }

  async function joinCloud() {
    const parsed = parseSharedInput(els.joinCode.value);
    const password = cleanInput(els.joinPassword.value || parsed.password);
    if (!password) throw new Error("Password is required");
    state.password = password;
    state.room = parsed.room || cleanInput(els.joinCode.value);
    if (!looksLikeRoom(state.room)) {
      if (parsed.offlineOffer) return makeOfflineAnswer();
      throw new Error("Paste a 6-character room code/link, or use the offline answer button for huge fallback links.");
    }
    const turnstileToken = await takeTurnstileToken();
    state.clientWs = await openSignal("client", state.room, (msg) => handleClientSignal(msg).catch((e) => log(e.message, "err")), turnstileToken);
    els.disconnect.disabled = false;
    setStatus("Connecting");
  }

  async function makeOfflineAnswer() {
    const parsed = parseSharedInput(els.joinCode.value);
    const password = cleanInput(els.joinPassword.value || parsed.password);
    const offerBlob = parsed.offlineOffer || cleanInput(els.joinCode.value);
    if (!password) throw new Error("Password is required");
    state.password = password;
    const offer = await decryptJson(password, "web-offline-offer", offerBlob);
    if (offer.type !== "offer") throw new Error("Offline blob is not an offer");
    await acceptOfferAndAnswer("offline", offer.sdp, async (answerSdp) => {
      const answerBlob = await encryptJson(password, "web-offline-answer", { type: "answer", sdp: answerSdp });
      els.offlineAnswerOut.value = answerBlob;
      els.copyOfflineAnswer.disabled = false;
      log("Offline answer created. Send it back to the host.", "ok");
    });
    els.disconnect.disabled = false;
    setStatus("Offline answer ready");
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
      tr.innerHTML = `<td colspan="5" class="muted">Empty folder</td>`;
      els.fileRows.appendChild(tr);
      return;
    }
    for (const e of entries) {
      const tr = document.createElement("tr");
      tr.className = e.kind === "directory" ? "dir" : "file";
      const name = document.createElement("td");
      name.className = "name";
      name.textContent = e.kind === "directory" ? `📁 ${e.name}` : `📄 ${e.name}`;
      if (e.kind === "directory") name.onclick = () => listRemote(e.path).catch((err) => log(err.message, "err"));
      const kind = document.createElement("td");
      kind.textContent = e.kind;
      const size = document.createElement("td");
      size.textContent = e.kind === "file" ? formatSize(e.size || 0) : "—";
      const mtime = document.createElement("td");
      mtime.textContent = e.mtime ? new Date(e.mtime).toLocaleString() : "—";
      const action = document.createElement("td");
      if (e.kind === "file") {
        const btn = document.createElement("button");
        btn.textContent = "Download";
        btn.onclick = () => downloadRemote(e).catch((err) => log(err.message, "err"));
        action.appendChild(btn);
      }
      tr.append(name, kind, size, mtime, action);
      els.fileRows.appendChild(tr);
    }
  }

  async function downloadRemote(entry) {
    if (!state.dataChannel || state.dataChannel.readyState !== "open") throw new Error("Not connected");
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

    let writer = null;
    if (window.showSaveFilePicker) {
      const saveHandle = await window.showSaveFilePicker({ suggestedName: entry.name });
      writer = await saveHandle.createWritable();
    }
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

  function disconnectAll() {
    if (state.hostWs) state.hostWs.close();
    if (state.clientWs) state.clientWs.close();
    for (const p of state.hostPeers.values()) p.pc.close();
    if (state.clientPeer?.pc) state.clientPeer.pc.close();
    if (state.offlineHostPeer) state.offlineHostPeer.close();
    state.hostWs = null;
    state.clientWs = null;
    state.hostPeers.clear();
    state.clientPeer = null;
    state.dataChannel = null;
    state.offlineHostPeer = null;
    els.stopHost.disabled = true;
    els.disconnect.disabled = true;
    els.acceptOfflineAnswer.disabled = true;
    setStatus("Idle");
    log("Disconnected");
  }

  async function pickFolder() {
    if (!window.showDirectoryPicker) {
      throw new Error("This browser cannot host folders yet. Use Chromium/Edge, or the native app.");
    }
    state.rootHandle = await window.showDirectoryPicker({ mode: "read" });
    state.rootName = state.rootHandle.name || "selected folder";
    els.hostFolder.textContent = state.rootName;
    els.hostCloud.disabled = false;
    els.hostOffline.disabled = false;
    log(`Selected folder: ${state.rootName}`, "ok");
  }

  function copy(text) {
    return navigator.clipboard.writeText(text);
  }

  function initEvents() {
    els.pickFolder.onclick = () => pickFolder().catch((e) => log(e.message, "err"));
    els.hostCloud.onclick = () => startCloudHost().catch((e) => log(e.message, "err"));
    els.hostOffline.onclick = () => startOfflineHost().catch((e) => log(e.message, "err"));
    els.stopHost.onclick = disconnectAll;
    els.copyShare.onclick = () => copy(els.shareLink.value).then(() => log("Share link copied", "ok")).catch((e) => log(e.message, "err"));
    els.copyPassword.onclick = () => copy(state.password).then(() => log("Password copied", "ok")).catch((e) => log(e.message, "err"));
    els.acceptOfflineAnswer.onclick = () => acceptOfflineAnswer().catch((e) => log(e.message, "err"));
    els.joinCloud.onclick = () => joinCloud().catch((e) => log(e.message, "err"));
    els.makeOfflineAnswer.onclick = () => makeOfflineAnswer().catch((e) => log(e.message, "err"));
    els.disconnect.onclick = disconnectAll;
    els.copyOfflineAnswer.onclick = () => copy(els.offlineAnswerOut.value).then(() => log("Offline answer copied", "ok")).catch((e) => log(e.message, "err"));
    els.upButton.onclick = () => {
      const parts = state.currentPath.split("/").filter(Boolean);
      parts.pop();
      listRemote("/" + parts.join("/")).catch((e) => log(e.message, "err"));
    };
  }

  function init() {
    els.workerUrl.value = window.FB_WEBAPP_CONFIG?.signalingUrl || "";
    applyHashToJoinForm();
    initEvents();
    initTurnstile();
    log("Ready. Host mode streams files on demand; it does not pre-cache the folder.");
  }

  init();
})();
