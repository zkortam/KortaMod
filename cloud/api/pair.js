// Exchanges the code shown on a Chromatic for a browser token. Codes work once.
import {
  CODE_RE, ID_RE, TOKEN_S, MAX_BROWSERS, ACTIVE_MS, keys as k,
  store, sha256, randomHex, readJson, json, fail, guarded,
} from "./_lib.js";

const TRIES_PER_IP = 20;      // per 10 minutes
const WINDOW_S = 600;

/* The paired count drifts when tokens lapse after 180 days unused; recount (and
 * drop lapsed tokens) only when it looks full. */
async function livePairings(db, id) {
  const hashes = await db.smembers(k.tokens(id));
  const dead = [];
  for (const h of hashes) if (!(await db.get(k.token(h)))) dead.push(h);
  if (dead.length) await db.srem(k.tokens(id), ...dead);
  const live = hashes.length - dead.length;
  await db.hset(k.dev(id), { paired: live });
  return live;
}

async function handle(request) {
  const db = store();
  const ip = (request.headers.get("x-forwarded-for") || "local").split(",")[0].trim();
  const tries = await db.incr(k.pairIp(ip));
  if (tries === 1) await db.expire(k.pairIp(ip), WINDOW_S);
  if (tries > TRIES_PER_IP) return fail(429, "Too many attempts. Wait 10 minutes.");
  const body = await readJson(request, 256);
  const code = body && typeof body.code === "string" ? body.code.replace(/\D/g, "") : "";
  if (!CODE_RE.test(code)) return fail(400, "The code has 6 digits.");
  const id = await db.get(k.codeOwner(code));
  if (!id || !ID_RE.test(id)) return fail(403, "Wrong or expired code. Check the Phone screen on your Chromatic.");
  const dev = (await db.hgetall(k.dev(id))) || {};
  if (Number(dev.paired || 0) >= MAX_BROWSERS && await livePairings(db, id) >= MAX_BROWSERS) {
    return fail(409, "This Chromatic already has 8 browsers. Choose Forget phones on it first.");
  }
  const token = randomHex(32);
  const hash = sha256(token);
  await db.set(k.token(hash), id, { ex: TOKEN_S });
  await db.sadd(k.tokens(id), hash);
  /* Single use: retire the code, have the device show a fresh one, and wake it. */
  await db.del(k.codeOwner(code));
  await db.hset(k.dev(id), { rotate: 1, code: "", active_until: Date.now() + ACTIVE_MS });
  await db.hincrby(k.dev(id), "paired", 1);
  return json(200, { token });
}

export const POST = guarded(handle);
