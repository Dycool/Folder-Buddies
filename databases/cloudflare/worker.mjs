// Folder Buddies Cloudflare Worker
//   POST   /create                       store an opaque sealed record
//   GET    /room?code=<lookup>           fetch the sealed record
//   DELETE /room?code=<lookup>           delete (X-FB-Owner credential)
//
// The KV key is only the public "lookup" half of the share code; the secret
// half never reaches Cloudflare, so the stored record is undecryptable here.
// Webapp mode reuses GET /room as a WebSocket upgrade into a Durable Object for
// encrypted WebRTC signaling. File bytes never touch Cloudflare.

const BASE91 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
// Public lookup half of a connect code: 4 chars (read-only tier) or 8 (read-write).
const LOOKUP_LENS = [4, 8];
const BASE91_CLASS = `[${BASE91.replace(/[\\^$.*+?()[\]{}|\-]/g, "\\$&")}]`;
const ROOM_RE = new RegExp(`^(?:${LOOKUP_LENS.map((n) => `${BASE91_CLASS}{${n}}`).join("|")})$`);
const ROOM_TTL_SECONDS = 30 * 24 * 60 * 60;
const RATE_LIMIT_TTL_SECONDS = 60;
const RATE_LIMIT_MAX = 5;
const RATE_LIMIT_ROOM_MAX = 30;
const MAX_FIELD_BYTES = 8 * 1024;
const MAX_CREATE_BODY_BYTES = 64 * 1024;
const MAX_SIGNAL_BYTES = 96 * 1024;
const MAX_TURNSTILE_TOKEN_BYTES = 4096;
const MAX_WS_MESSAGES_PER_SOCKET = 250;

const SECURITY_CONTACT = "https://github.com/Dycool/Folder-Buddies/security/advisories/new";
const SECURITY_POLICY = "https://github.com/Dycool/Folder-Buddies/blob/main/docs/SECURITY.md";

const AI_BOT_RE = /(GPTBot|ChatGPT-User|OAI-SearchBot|CCBot|ClaudeBot|Claude-Web|anthropic-ai|Google-Extended|Applebot-Extended|PerplexityBot|Perplexity-User|Amazonbot|Bytespider|Bytedance|Diffbot|FacebookBot|Meta-ExternalAgent|ImagesiftBot|Omgili|YouBot|cohere-ai|Timpibot|PetalBot|DataForSeoBot|magpie-crawler|Scrapy|AI2Bot|Webzio)/i;

const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "POST, GET, DELETE, OPTIONS",
  "access-control-allow-headers": "content-type, x-fb-owner",
  "access-control-max-age": "86400",
};

function allowedOrigins(env) {
  return String(env.ALLOWED_ORIGINS || "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
}

function allowedOrigin(request, env) {
  const origin = request.headers.get("Origin");
  if (!origin) return true; // native clients and curl have no Origin header.
  const allowed = allowedOrigins(env);
  if (!allowed.length) return true; // allow all until configured.
  return allowed.includes(origin);
}

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

function text(body, status = 200) {
  return new Response(body, {
    status,
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "public, max-age=86400", ...CORS },
  });
}

function isAiBot(request) {
  return AI_BOT_RE.test(request.headers.get("User-Agent") || "");
}

function robotsTxt() {
  return [
    "User-agent: *",
    "Disallow: /",
    "",
    "# AI and training crawlers are not permitted.",
    "User-agent: GPTBot",
    "Disallow: /",
    "User-agent: ChatGPT-User",
    "Disallow: /",
    "User-agent: CCBot",
    "Disallow: /",
    "User-agent: ClaudeBot",
    "Disallow: /",
    "User-agent: Google-Extended",
    "Disallow: /",
    "User-agent: Applebot-Extended",
    "Disallow: /",
    "User-agent: PerplexityBot",
    "Disallow: /",
    "User-agent: Bytespider",
    "Disallow: /",
    "User-agent: Amazonbot",
    "Disallow: /",
    "User-agent: Meta-ExternalAgent",
    "Disallow: /",
    "",
  ].join("\n");
}

function securityTxt() {
  const expires = new Date(Date.now() + 365 * 24 * 60 * 60 * 1000).toISOString().replace(/\.\d+Z$/, "Z");
  return [
    `Contact: ${SECURITY_CONTACT}`,
    `Expires: ${expires}`,
    `Policy: ${SECURITY_POLICY}`,
    "Preferred-Languages: en",
    "",
  ].join("\n");
}

function clientKey(request) {
  return request.headers.get("CF-Connecting-IP") || request.headers.get("X-Forwarded-For") || "unknown";
}

function validRoom(room) {
  return typeof room === "string" && ROOM_RE.test(room);
}

async function kvRateLimit(env, key, max) {
  const current = parseInt((await env.ROOMS.get(key)) || "0", 10);
  if (current >= max) return false;
  await env.ROOMS.put(key, String(current + 1), { expirationTtl: RATE_LIMIT_TTL_SECONDS });
  return true;
}

async function rateLimit(env, request, method, room = "") {
  const ip = clientKey(request);
  if (!(await kvRateLimit(env, `rl:ip:${method}:${ip}`, RATE_LIMIT_MAX))) return false;
  if (room && !(await kvRateLimit(env, `rl:room:${method}:${room}`, RATE_LIMIT_ROOM_MAX))) return false;
  return true;
}

async function readRoom(env, room) {
  const raw = await env.ROOMS.get(room, { type: "json", cacheTtl: 30 });
  if (!raw || typeof raw !== "object") return null;
  if (typeof raw.salt !== "string" || typeof raw.wrapped !== "string" ||
      typeof raw.payload !== "string" || typeof raw.owner !== "string") return null;
  return raw;
}

function validField(s, min = 4) {
  return typeof s === "string" && s.length >= min && s.length <= MAX_FIELD_BYTES;
}

function timingSafeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; ++i) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

async function verifyTurnstile(request, env, token) {
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

function envFlag(value) {
  return /^(1|true|yes|on)$/i.test(String(value || "").trim());
}

async function verifyWebSocketTurnstile(request, env, token) {
  // WebSocket signaling must work for native clients too. Native clients cannot
  // solve browser Turnstile challenges, and privacy/DNS filters can also block
  // challenges.cloudflare.com in browsers. Keep Turnstile opt-in for WS rooms;
  // rate limits and message caps remain the default protection.
  const required = envFlag(env.REQUIRE_TURNSTILE_FOR_WEBSOCKET);
  if (!env.TURNSTILE_SECRET_KEY) return { ok: true, disabled: true };
  if (!required && !token) return { ok: true, skipped: true };
  return verifyTurnstile(request, env, token);
}

async function create(request, env) {
  const contentLength = Number(request.headers.get("content-length") || "0");
  if (contentLength > MAX_CREATE_BODY_BYTES) return json({ error: "body_too_large" }, 413);
  let body;
  try { body = await request.json(); } catch { return json({ error: "invalid_json" }, 400); }
  const lookup = body?.lookup;
  const salt = body?.salt;
  const wrapped = body?.wrapped;
  const payload = body?.payload;
  const owner = body?.owner;
  const ttl = Math.min(Math.max(Number(body?.ttl || ROOM_TTL_SECONDS), 60), ROOM_TTL_SECONDS);
  if (!validRoom(lookup)) return json({ error: "bad_lookup" }, 400);
  if (!(await rateLimit(env, request, "POST", lookup))) return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  if (!validField(salt) || !validField(wrapped) || !validField(payload) || !validField(owner)) {
    return json({ error: "bad_record" }, 400);
  }
  if (body?.turnstileToken) {
    const turnstile = await verifyTurnstile(request, env, body.turnstileToken);
    if (!turnstile.ok) return json({ error: turnstile.error, codes: turnstile.codes || [] }, turnstile.status || 403);
  }

  if (await env.ROOMS.get(lookup)) return json({ error: "room_exists" }, 409);
  await env.ROOMS.put(lookup, JSON.stringify({ salt, wrapped, payload, owner, createdAt: Date.now() }), {
    expirationTtl: ttl,
  });
  return json({ ok: true, ttl }, 201);
}

async function getRoom(request, env, room) {
  if (!(await rateLimit(env, request, "GET", room))) return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  const stored = await readRoom(env, room);
  if (!stored) return json({ error: "not_found" }, 404);
  return json({ salt: stored.salt, wrapped: stored.wrapped, payload: stored.payload });
}

async function deleteRoom(request, env, room) {
  if (!(await rateLimit(env, request, "DELETE", room))) return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  const stored = await readRoom(env, room);
  if (!stored) return json({ ok: true }, 404);
  if (!timingSafeEqual(request.headers.get("X-FB-Owner") || "", stored.owner)) {
    return json({ error: "unauthorized" }, 401);
  }
  await env.ROOMS.delete(room);
  return json({ ok: true });
}

async function webSocketRoom(request, env, room) {
  if (!(await rateLimit(env, request, "GET", room))) {
    return json({ error: "rate_limited", waitSeconds: 60 }, 429);
  }
  const url = new URL(request.url);
  const turnstile = await verifyWebSocketTurnstile(request, env, url.searchParams.get("turnstile") || "");
  if (!turnstile.ok) return json({ error: turnstile.error, codes: turnstile.codes || [] }, turnstile.status || 403);

  const id = env.WEB_ROOMS.idFromName(room);
  return env.WEB_ROOMS.get(id).fetch(request);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
    if (request.method === "GET" && url.pathname === "/robots.txt") return text(robotsTxt());
    if (request.method === "GET" && (url.pathname === "/.well-known/security.txt" || url.pathname === "/security.txt")) {
      return text(securityTxt());
    }
    if (isAiBot(request)) return json({ error: "ai_bots_not_allowed" }, 403);
    if (!allowedOrigin(request, env)) return json({ error: "bad_origin" }, 403);
    if (request.method === "POST" && url.pathname === "/create") return create(request, env);
    if ((request.method === "GET" || request.method === "DELETE") && url.pathname === "/room") {
      const room = url.searchParams.get("code") || "";
      if (!validRoom(room)) return json({ error: "bad_lookup" }, 400);
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
    this.messageCounts = new WeakMap();
  }

  async fetch(request) {
    const url = new URL(request.url);
    const room = url.searchParams.get("code") || "";
    const role = url.searchParams.get("role") || "";
    if (!validRoom(room)) return json({ error: "bad_lookup" }, 400);
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

  bumpMessage(ws) {
    const next = (this.messageCounts.get(ws) || 0) + 1;
    this.messageCounts.set(ws, next);
    if (next > MAX_WS_MESSAGES_PER_SOCKET) {
      ws.close(4429, "too many messages");
      return false;
    }
    return true;
  }

  onHostMessage(ws, data) {
    if (this.host !== ws) return;
    if (!this.bumpMessage(ws)) return;
    const msg = parseSignal(data);
    if (!msg) return ws.close(4400, "bad message");
    const client = this.clients.get(msg.peerId);
    if (client) safeSend(client, { kind: "signal", peerId: msg.peerId, ciphertext: msg.ciphertext });
  }

  onClientMessage(peerId, ws, data) {
    if (this.clients.get(peerId) !== ws) return;
    if (!this.bumpMessage(ws)) return;
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


