// The Chromatic's side of the relay: it reports its state and pairing code and
// collects browser commands. The site tells it how soon to come back: every
// second while a browser is watching, every 20 s when browsers are paired, every
// 4 minutes when none are. The first call from a device registers it.
import {
  ID_RE, SECRET_RE, CODE_RE, CODE_S, POLL_FAST_MS, POLL_PAIRED_MS, POLL_LONELY_MS, keys as k,
  store, sha256, sameHex, bearer, readJson, json, fail, guarded,
} from "../_lib.js";
import { events, notify } from "../_push.js";

const STATE_MAX = 12288;
const COMMANDS_PER_SYNC = 8;
const RESULT_RE = /^[0-9a-f]{16}$/;
const CODE_REFRESH_MS = 240000;

/* Devices first registered before the single-record layout: keep their secret and
 * paired browsers. Returns the paired count, or null if the secret doesn't match. */
async function migrate(db, id, hash) {
  const old = await db.get(`km:dev:${id}:secret`);
  if (old && !sameHex(old, hash)) return null;
  if (!old) return 0;
  const hashes = await db.smembers(`km:dev:${id}:tokens`);
  let live = 0;
  for (const h of hashes) if (await db.get(k.token(h)) && await db.sadd(k.tokens(id), h)) live++;
  await db.del(`km:dev:${id}:secret`, `km:dev:${id}:tokens`, `km:dev:${id}:state`, `km:dev:${id}:seen`,
    `km:dev:${id}:code`, `km:dev:${id}:active`, `km:dev:${id}:rotate`);
  return live;
}

async function handle(request) {
  const id = request.headers.get("x-device-id") || "";
  const secret = bearer(request);
  if (!ID_RE.test(id) || !SECRET_RE.test(secret)) return fail(401, "bad device credentials");
  const db = store();
  const now = Date.now();
  const dev = (await db.hgetall(k.dev(id))) || {};
  const hash = sha256(secret);
  /* First contact registers the device's own random secret. */
  if (dev.secret && !sameHex(dev.secret, hash)) return fail(401, "wrong device secret");
  const body = await readJson(request, STATE_MAX + 2048);
  if (!body) return fail(400, "expected a JSON object");

  const update = { seen: now };
  let reported = null;
  if (dev.v !== "2") {
    const moved = await migrate(db, id, hash);
    if (moved === null) return fail(401, "wrong device secret");
    update.secret = hash;
    update.v = 2;
    update.paired = Number(dev.paired || 0) + moved;
    dev.paired = update.paired;
  }
  if (body.state && typeof body.state === "object") {
    const text = JSON.stringify(body.state);
    if (text.length <= STATE_MAX) { update.state = text; reported = body.state; }
  }

  if (Array.isArray(body.results)) {
    for (const r of body.results.slice(0, 16)) {
      if (r && RESULT_RE.test(r.id) && Number.isInteger(r.result)) await db.set(k.result(r.id), String(r.result), { ex: 120 });
    }
  }

  let paired = Number(dev.paired || 0);
  if (body.forget === true) {
    const hashes = await db.smembers(k.tokens(id));
    if (hashes.length) await db.del(...hashes.map(h => k.token(h)), k.tokens(id));
    await db.del(k.push(id));
    paired = 0;
    update.paired = 0;
    update.push = 0;
  }

  /* Pairing code: claimed when it changes and refreshed before it expires. A code
   * another device holds, or one a browser just used, makes the device pick anew. */
  let rotate = dev.rotate === "1";
  if (rotate) update.rotate = 0;
  else if (typeof body.code === "string" && CODE_RE.test(body.code)
           && (body.code !== dev.code || now - Number(dev.code_at || 0) > CODE_REFRESH_MS)) {
    const owner = await db.get(k.codeOwner(body.code));
    if (owner && owner !== id) rotate = true;
    else {
      await db.set(k.codeOwner(body.code), id, { ex: CODE_S });
      if (dev.code && dev.code !== body.code && now - Number(dev.code_at || 0) < CODE_S * 1000) await db.del(k.codeOwner(dev.code));
      update.code = body.code;
      update.code_at = now;
    }
  }

  /* Clear the flag before reading the queue: a browser sets it after pushing, so a
   * command added meanwhile always leaves the flag set for the next check-in. */
  const commands = [];
  if (dev.pending === "1") {
    await db.hset(k.dev(id), { pending: 0 });
    const raw = await db.lrange(k.queue(id), 0, COMMANDS_PER_SYNC - 1);
    if (raw.length) await db.ltrim(k.queue(id), raw.length, -1);
    if (raw.length === COMMANDS_PER_SYNC) update.pending = 1;
    for (const item of raw) {
      try { commands.push(typeof item === "string" ? JSON.parse(item) : item); } catch { /* skip */ }
    }
  }

  /* Notifications never hold up or fail a check-in. */
  if (reported && dev.push === "1" && body.forget !== true) {
    let before = null;
    try { before = dev.state ? JSON.parse(dev.state) : null; } catch { before = null; }
    const list = events(before, reported);
    if (list.length) await Promise.race([notify(id, list).catch(() => 0), new Promise(r => setTimeout(r, 3000))]);
  }

  const fast = Number(dev.active_until || 0) > now;
  update.fast = fast ? 1 : 0;
  await db.hset(k.dev(id), update);
  return json(200, {
    now,
    commands,
    paired,
    active: fast,
    rotate_code: rotate,
    poll_ms: fast ? POLL_FAST_MS : paired > 0 ? POLL_PAIRED_MS : POLL_LONELY_MS,
  });
}

export const POST = guarded(handle);
