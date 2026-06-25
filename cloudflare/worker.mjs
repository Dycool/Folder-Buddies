// Folder Buddies Cloudflare Worker
// Public API surface stays intentionally tiny:
//   POST   /create
//   GET    /room?code=<6-char Base91 room>
//   DELETE /room?code=<6-char Base91 room>
//
// Native app mode uses KV for long-lived encrypted rendezvous blobs.
// Webapp mode uses GET /room as a WebSocket upgrade into a Durable Object for
// short-lived encrypted WebRTC signaling. File bytes never touch Cloudflare.

const BASE91 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
const ROOM_RE = new RegExp(`^[${BASE91.replace(/[\\^$.*+?()[\]{}|\-]/g, "\\$&")}] {6}$`.replace(" ", ""));
const ROOM_TTL_SECONDS = 30 * 24 * 60 * 60;
const RATE_LIMIT_TTL_SECONDS = 60;
const RATE_LIMIT_MAX = 5;
const MAX_SIGNAL_BYTES = 96 * 1024;
const MAX_TURNSTILE_TOKEN_BYTES = 4096;

const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "POST, GET, DELETE, OPTIONS",
  "access-control-allow-headers": "content-type, x-fb-auth",
  "access-control-max-age": "86400",
};

function json(body, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      ...CORS,
      ...extraHeaders,
    },
  });
}

function clientKey(request) {
  return request.headers.get("CF-Connecting-IP") || request.headers.get("X-Forwarded-For") || "unknown";
}

function validRoom(room) {
  return typeof room === "string" && ROOM_RE.test(room);
}

async function rateLimit(env, request, method, room) {
  const key = `rl:${method}:${clientKey(request)}:${room}`;
  const current = parseInt((await env.ROOMS.get(key)) || "0", 10);
  if (current >= RATE_LIMIT_MAX) return false;
  await env.ROOMS.put(key, String(current + 1), { expirationTtl: RATE_LIMIT_TTL_SECONDS });
  return true;
}

async function readRoom(env, room) {
  const raw = await env.ROOMS.get(room, { type: "json", cacheTtl: 30 });
  if (!raw || typeof raw !== "object") return null;
  if (typeof raw.auth !== "string" || typeof raw.payload !== "string") return null;
  return raw;
}

async function hmacBase91(keyBase91, message) {
  const rawKey = base91Decode(keyBase91);
  const key = await crypto.subtle.importKey("raw", rawKey, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(message));
  return base91Encode(new Uint8Array(sig));
}

function timingSafeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; ++i) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

async function verifyRequestAuth(request, stored, method, room) {
  const got = request.headers.get("X-FB-Auth") || "";
  const msg = `${method}\n${room}\nFB-worker-auth-proof-v1`;
  const expect = await hmacBase91(stored.auth, msg);
  return timingSafeEqual(got, expect);
}


async function verifyTurnstile(request, env, token) {
  // Turnstile is intentionally optional so native clients keep working when the
  // secret is not configured. When TURNSTILE_SECRET_KEY exists, browser
  // WebSocket signaling must provide a fresh token.
  if (!env.TURNSTILE_SECRET_KEY) return { ok: true, disabled: true };
  if (typeof token !== "string" || token.length < 20) {
    return { ok: false, status: 403, error: "turnstile_required" };
  }
  if (token.length > MAX_TURNSTILE_TOKEN_BYTES) {
    return { ok: false, status: 400, error: "turnstile_token_too_large" };
  }
  const form = new FormData();
  form.append("secret", env.TURNSTILE_SECRET_KEY);
  form.append("response", token);
  const ip = request.headers.get("CF-Connecting-IP");
  if (ip) form.append("remoteip", ip);

  let result;
  try {
    const res = await fetch("https://challenges.cloudflare.com/turnstile/v0/siteverify", {
      method: "POST",
      body: form,
    });
    result = await res.json();
  } catch {
    return { ok: false, status: 503, error: "turnstile_unavailable" };
  }
  if (!result?.success) {
    return { ok: false, status: 403, error: "turnstile_failed", codes: result?.["error-codes"] || [] };
  }
  return { ok: true };
}

async function create(request, env) {
  let body;
  try { body = await request.json(); } catch { return json({ error: "invalid_json" }, 400); }
  const room = body?.room;
  const auth = body?.auth;
  const payload = body?.payload;
  const ttl = Math.min(Math.max(Number(body?.ttl || ROOM_TTL_SECONDS), 60), ROOM_TTL_SECONDS);
  if (!validRoom(room)) return json({ error: "room_must_be_exactly_6_base91_chars" }, 400);
  if (typeof auth !== "string" || auth.length < 20) return json({ error: "bad_auth_verifier" }, 400);
  if (typeof payload !== "string" || payload.length < 40) return json({ error: "bad_payload" }, 400);
  if (body?.turnstileToken) {
    const turnstile = await verifyTurnstile(request, env, body.turnstileToken);
    if (!turnstile.ok) return json({ error: turnstile.error, codes: turnstile.codes || [] }, turnstile.status || 403);
  }

  // KV has no compare-and-swap. This avoids normal collisions; hosts retry on 409.
  if (await env.ROOMS.get(room)) return json({ error: "room_exists" }, 409);
  await env.ROOMS.put(room, JSON.stringify({ auth, payload, createdAt: Date.now() }), {
    expirationTtl: ttl,
  });
  return json({ ok: true, ttl }, 201);
}

async function getRoom(request, env, room) {
  if (!(await rateLimit(env, request, "GET", room))) return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  const stored = await readRoom(env, room);
  if (!stored) return json({ error: "not_found" }, 404);
  if (!(await verifyRequestAuth(request, stored, "GET", room))) return json({ error: "unauthorized" }, 401);
  return json({ payload: stored.payload });
}

async function deleteRoom(request, env, room) {
  if (!(await rateLimit(env, request, "DELETE", room))) return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  const stored = await readRoom(env, room);
  if (!stored) return json({ ok: true }, 404);
  if (!(await verifyRequestAuth(request, stored, "DELETE", room))) return json({ error: "unauthorized" }, 401);
  await env.ROOMS.delete(room);
  return json({ ok: true });
}

async function webSocketRoom(request, env, room) {
  if (!(await rateLimit(env, request, "GET", room))) {
    return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  }
  const url = new URL(request.url);
  const turnstile = await verifyTurnstile(request, env, url.searchParams.get("turnstile") || "");
  if (!turnstile.ok) return json({ error: turnstile.error, codes: turnstile.codes || [] }, turnstile.status || 403);

  const id = env.WEB_ROOMS.idFromName(room);
  return env.WEB_ROOMS.get(id).fetch(request);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
    if (request.method === "POST" && url.pathname === "/create") return create(request, env);
    if ((request.method === "GET" || request.method === "DELETE") && url.pathname === "/room") {
      const room = url.searchParams.get("code") || "";
      if (!validRoom(room)) return json({ error: "room_must_be_exactly_6_base91_chars" }, 400);
      if (request.method === "GET" && request.headers.get("Upgrade") === "websocket") {
        return webSocketRoom(request, env, room);
      }
      return request.method === "GET" ? getRoom(request, env, room) : deleteRoom(request, env, room);
    }
    return json({ error: "not_found" }, 404);
  },
};

export class WebSignalingRoom {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.host = null;
    this.clients = new Map();
  }

  async fetch(request) {
    const url = new URL(request.url);
    const room = url.searchParams.get("code") || "";
    const role = url.searchParams.get("role") || "";
    if (!validRoom(room)) return json({ error: "room_must_be_exactly_6_base91_chars" }, 400);
    if (request.headers.get("Upgrade") !== "websocket") return json({ error: "websocket_required" }, 426);
    if (role !== "host" && role !== "client") return json({ error: "bad_role" }, 400);

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.accept();

    if (role === "host") this.attachHost(server, room);
    else this.attachClient(server, room);

    return new Response(null, { status: 101, webSocket: client });
  }

  attachHost(ws, room) {
    if (this.host) {
      ws.send(JSON.stringify({ kind: "error", error: "host_already_connected" }));
      ws.close(4409, "host already connected");
      return;
    }
    this.host = ws;
    ws.send(JSON.stringify({ kind: "ready", role: "host", room }));
    for (const peerId of this.clients.keys()) {
      ws.send(JSON.stringify({ kind: "client-joined", peerId }));
    }
    ws.addEventListener("message", (event) => this.onHostMessage(ws, event.data));
    ws.addEventListener("close", () => this.detachHost(ws));
    ws.addEventListener("error", () => this.detachHost(ws));
  }

  attachClient(ws, room) {
    const peerId = crypto.randomUUID();
    this.clients.set(peerId, ws);
    ws.send(JSON.stringify({ kind: "ready", role: "client", room, peerId }));
    if (this.host) this.host.send(JSON.stringify({ kind: "client-joined", peerId }));
    ws.addEventListener("message", (event) => this.onClientMessage(peerId, ws, event.data));
    ws.addEventListener("close", () => this.detachClient(peerId, ws));
    ws.addEventListener("error", () => this.detachClient(peerId, ws));
  }

  detachHost(ws) {
    if (this.host !== ws) return;
    this.host = null;
    for (const client of this.clients.values()) {
      safeSend(client, { kind: "host-left" });
    }
  }

  detachClient(peerId, ws) {
    if (this.clients.get(peerId) !== ws) return;
    this.clients.delete(peerId);
    if (this.host) safeSend(this.host, { kind: "client-left", peerId });
  }

  onHostMessage(ws, data) {
    if (this.host !== ws) return;
    const msg = parseSignal(data);
    if (!msg) return ws.close(4400, "bad message");
    const client = this.clients.get(msg.peerId);
    if (client) safeSend(client, { kind: "signal", peerId: msg.peerId, ciphertext: msg.ciphertext });
  }

  onClientMessage(peerId, ws, data) {
    if (this.clients.get(peerId) !== ws) return;
    const msg = parseSignal(data);
    if (!msg || msg.peerId !== peerId) return ws.close(4400, "bad message");
    if (this.host) safeSend(this.host, { kind: "signal", peerId, ciphertext: msg.ciphertext });
  }
}

function parseSignal(data) {
  if (typeof data !== "string" || data.length > MAX_SIGNAL_BYTES) return null;
  let msg;
  try { msg = JSON.parse(data); } catch { return null; }
  if (msg?.kind !== "signal") return null;
  if (typeof msg.peerId !== "string" || msg.peerId.length < 8 || msg.peerId.length > 80) return null;
  if (typeof msg.ciphertext !== "string" || msg.ciphertext.length < 20 || msg.ciphertext.length > MAX_SIGNAL_BYTES) return null;
  return msg;
}

function safeSend(ws, msg) {
  try { ws.send(JSON.stringify(msg)); } catch { /* peer is gone */ }
}

// Same Base91 alphabet as the C++23 app and webapp.
function base91Decode(text) {
  const table = new Map([...BASE91].map((c, i) => [c, i]));
  const out = [];
  let v = -1, b = 0, n = 0;
  for (const ch of text) {
    if (/\s/.test(ch)) continue;
    const c = table.get(ch);
    if (c === undefined) throw new Error("invalid_base91");
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
