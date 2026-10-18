// Shared by every function: storage, authentication, validation, replies.
//
// Storage is shaped for few operations, because the database bills per command:
//   km:d:{id}       hash   secret, seen, state, code, code_at, active_until, fast, pending, rotate, paired
//   km:q:{id}       list   commands waiting for the device
//   km:code:{code}  string device id, expires with the code
//   km:tok:{hash}   string device id for a browser token (only the hash is stored)
//   km:toks:{id}    set    token hashes of a device, for Forget phones
//   km:res:{cmd}    string a command's result, briefly
//   km:push:{id}    hash   token hash -> that browser's push subscription
// A device's routine check-in is one HGETALL and one HSET.
import { createHash, randomBytes, timingSafeEqual } from "node:crypto";
import { Redis } from "@upstash/redis";

export const ID_RE = /^[0-9a-f]{16}$/;        // device id, made by the device
export const SECRET_RE = /^[0-9a-f]{64}$/;    // device secret, made by the device
export const TOKEN_RE = /^[0-9a-f]{64}$/;     // browser token, made here
export const CODE_RE = /^\d{6}$/;
export const CODE_S = 600;
export const TOKEN_S = 180 * 24 * 3600;
export const MAX_BROWSERS = 8;
export const ACTIVE_MS = 60000;               // a browser looked within this: device polls fast
export const POLL_FAST_MS = 2000;            // a browser has the page open
export const POLL_PAIRED_MS = 20000;          // idle, with browsers paired
export const POLL_LONELY_MS = 240000;         // idle, nobody paired: only keep the code alive
export const ONLINE_MS = POLL_PAIRED_MS + 15000;

export const keys = {
  dev: id => `km:d:${id}`,
  queue: id => `km:q:${id}`,
  codeOwner: code => `km:code:${code}`,
  token: hash => `km:tok:${hash}`,
  tokens: id => `km:toks:${id}`,
  result: cmd => `km:res:${cmd}`,
  pairIp: ip => `km:pairip:${ip}`,
  push: id => `km:push:${id}`,
};
const k = keys;

/* In-memory stand-in with the same calls, for tests without a database. It also
 * counts commands, so tests can hold the per-request cost down. */
class MemoryStore {
  constructor() { this.m = new Map(); this.calls = 0; }
  #live(key) { const e = this.m.get(key); if (e && e.exp && e.exp <= Date.now()) { this.m.delete(key); return undefined; } return e; }
  #of(key, make) { let e = this.#live(key); if (!e || !(e.v instanceof make)) { e = { v: new make(), exp: 0 }; this.m.set(key, e); } return e; }
  async get(key) { this.calls++; const e = this.#live(key); return e && typeof e.v === "string" ? e.v : null; }
  async set(key, v, o = {}) {
    this.calls++;
    if (o.nx && this.#live(key)) return null;
    this.m.set(key, { v: String(v), exp: o.ex ? Date.now() + o.ex * 1000 : 0 });
    return "OK";
  }
  async del(...ks) { this.calls++; let n = 0; for (const key of ks) n += this.m.delete(key) ? 1 : 0; return n; }
  async incr(key) { this.calls++; const e = this.#live(key); const v = (e ? Number(e.v) : 0) + 1; this.m.set(key, { v: String(v), exp: e ? e.exp : 0 }); return v; }
  async expire(key, s) { this.calls++; const e = this.#live(key); if (!e) return 0; e.exp = Date.now() + s * 1000; return 1; }
  async hgetall(key) { this.calls++; const e = this.#live(key); return e && e.v instanceof Map && e.v.size ? Object.fromEntries(e.v) : null; }
  async hset(key, obj) { this.calls++; const h = this.#of(key, Map).v; let n = 0; for (const [f, v] of Object.entries(obj)) { if (!h.has(f)) n++; h.set(f, String(v)); } return n; }
  async hdel(key, ...fs) { this.calls++; const e = this.#live(key); if (!e || !(e.v instanceof Map)) return 0; let n = 0; for (const f of fs) n += e.v.delete(f) ? 1 : 0; return n; }
  async hincrby(key, f, by) { this.calls++; const h = this.#of(key, Map).v; const v = Number(h.get(f) || 0) + by; h.set(f, String(v)); return v; }
  async sadd(key, ...ms) { this.calls++; const s = this.#of(key, Set).v; let n = 0; for (const x of ms) if (!s.has(x)) { s.add(x); n++; } return n; }
  async srem(key, ...ms) { this.calls++; const s = this.#of(key, Set).v; let n = 0; for (const x of ms) n += s.delete(x) ? 1 : 0; return n; }
  async smembers(key) { this.calls++; const e = this.#live(key); return e && e.v instanceof Set ? [...e.v] : []; }
  async rpush(key, ...vs) { this.calls++; const e = this.#of(key, Array); e.v.push(...vs.map(String)); return e.v.length; }
  async lrange(key, a, b) { this.calls++; const e = this.#live(key); if (!e || !Array.isArray(e.v)) return []; return e.v.slice(a, b < 0 ? e.v.length + b + 1 : b + 1); }
  async ltrim(key, a, b) { this.calls++; const e = this.#live(key); if (!e || !Array.isArray(e.v)) return "OK"; e.v = e.v.slice(a, b < 0 ? e.v.length + b + 1 : b + 1); return "OK"; }
}

let db = null;
export function store() {
  if (db) return db;
  const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
  const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
  if (url && token) {
    db = new Redis({ url, token, automaticDeserialization: false });
    /* Without deserialization HGETALL comes back as [field, value, ...]; every caller
     * here expects an object of strings, like the memory store returns. */
    const raw = db.hgetall.bind(db);
    db.hgetall = async key => hashReply(await raw(key));
  }
  else if (process.env.KORTAMOD_MEMORY_STORE === "1") db = new MemoryStore();
  else throw new Error("No database configured");
  return db;
}
export function hashReply(r) {
  if (!Array.isArray(r)) return r && Object.keys(r).length ? r : null;
  if (!r.length) return null;
  const o = {};
  for (let i = 0; i + 1 < r.length; i += 2) o[r[i]] = String(r[i + 1]);
  return o;
}
export function useMemoryStore() { db = new MemoryStore(); return db; }

export const sha256 = s => createHash("sha256").update(s).digest("hex");
export const randomHex = bytes => randomBytes(bytes).toString("hex");
export function sameHex(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  return timingSafeEqual(Buffer.from(a), Buffer.from(b));
}

export function json(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff" },
  });
}
export const fail = (status, error) => json(status, { error });

export function bearer(request) {
  const h = request.headers.get("authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7).trim() : "";
}

/* Reads a JSON object no larger than `limit` bytes; null if absent or invalid. */
export async function readJson(request, limit) {
  const text = await request.text();
  if (!text || text.length > limit) return null;
  try { const v = JSON.parse(text); return v && typeof v === "object" && !Array.isArray(v) ? v : null; }
  catch { return null; }
}

/* The device a browser token belongs to, with that device's record, or null.
 * renew: extend the pairing (sliding expiry); done on page load and on writes. */
export async function browserDevice(request, { renew = false } = {}) {
  const token = bearer(request);
  if (!TOKEN_RE.test(token)) return null;
  const hash = sha256(token);
  const db = store();
  const id = await db.get(k.token(hash));
  if (!id || !ID_RE.test(id)) return null;
  if (renew) await db.expire(k.token(hash), TOKEN_S);
  const dev = (await db.hgetall(k.dev(id))) || {};
  return { id, tokenHash: hash, dev };
}

export function deviceStatus(dev, now = Date.now()) {
  const seen = Number(dev.seen || 0);
  return {
    online: now - seen < ONLINE_MS,
    awake: now - seen < POLL_FAST_MS + 3000 && Number(dev.fast || 0) === 1,   /* checking in fast right now */
    seen,
  };
}

/* Marks a browser as watching, so the device switches to fast polling. Written at
 * most every 20 s to keep commands down. */
export async function markActive(id, dev, now = Date.now()) {
  if (Number(dev.active_until || 0) - now > ACTIVE_MS - 20000) return;
  await store().hset(k.dev(id), { active_until: now + ACTIVE_MS });
}

/* Queues a command and waits for the device's answer. The wait covers a device
 * that is still idle: it learns about the browser at its next check-in. */
export async function command(id, dev, kind, payload, waitMs) {
  const db = store();
  const now = Date.now();
  const { awake } = deviceStatus(dev, now);
  const limit = waitMs ?? (awake ? 9000 : POLL_PAIRED_MS + 9000);
  const end = now + limit;
  /* exp: the device refuses the command after this (server time), so it is never
   * applied after the browser has been told it failed. */
  const cmd = { id: randomHex(8), kind, t: now, exp: end - 1500, ...payload };
  await db.rpush(k.queue(id), JSON.stringify(cmd));
  await db.hset(k.dev(id), { pending: 1, active_until: now + ACTIVE_MS });
  await new Promise(res => setTimeout(res, Math.min(awake ? 500 : 1500, limit)));
  while (Date.now() < end) {
    const r = await db.get(k.result(cmd.id));
    if (r !== null) return Number(r);
    await new Promise(res => setTimeout(res, 600));
  }
  return null;
}

export function replyFor(result) {
  if (result === null) return fail(504, "Your Chromatic didn't answer. Check that Phone link is on and it's on Wi-Fi.");
  if (result === 1) return fail(409, "Changed on the Chromatic meanwhile. Reload first.");
  if (result === 2) return fail(423, "The Chromatic is busy with that right now.");
  if (result === 4) return fail(504, "The request expired before your Chromatic got it. Try again.");
  if (result !== 0) return fail(400, "The Chromatic refused that value.");
  return json(200, { ok: true });
}

/* Every handler runs inside this: an unexpected failure (database down or not yet
 * configured) becomes a clear 503 instead of a crash page. */
export function guarded(handler) {
  return async request => {
    try { return await handler(request); }
    catch (e) { console.error(e); return fail(503, "KortaMod's service is unavailable right now. Try again shortly."); }
  };
}

/* Shared shape of the browser write endpoints: auth, validate, online, queue, answer. */
export function deviceCommand(kind, validate, opts = {}) {
  return guarded(async request => {
    const who = await browserDevice(request, { renew: true });
    if (!who) return fail(401, "Not paired.");
    const body = await readJson(request, opts.limit || 2048);
    const payload = body && validate(body);
    if (!payload) return fail(400, opts.invalid || "Invalid request.");
    if (!deviceStatus(who.dev).online) return fail(503, "Your Chromatic is offline. Turn on Phone link and Wi-Fi.");
    const result = await command(who.id, who.dev, kind, payload);
    if (result === 2 && opts.busy) return fail(423, opts.busy);
    return replyFor(result);
  });
}
