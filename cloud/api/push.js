// Notifications for this browser: GET the server's public key, POST a push
// subscription, DELETE to stop.
import { keys as k, store, browserDevice, readJson, json, fail, guarded } from "./_lib.js";

function valid(sub) {
  if (!sub || typeof sub !== "object" || typeof sub.endpoint !== "string" || !sub.keys) return null;
  const { endpoint, keys } = sub;
  if (!/^https:\/\//.test(endpoint) || endpoint.length > 1024) return null;
  if (typeof keys.p256dh !== "string" || typeof keys.auth !== "string" || keys.p256dh.length > 200 || keys.auth.length > 100) return null;
  return { endpoint, keys: { p256dh: keys.p256dh, auth: keys.auth } };
}

async function get() {
  const key = process.env.VAPID_PUBLIC_KEY;
  return key ? json(200, { key }) : fail(503, "Notifications aren't set up on this server.");
}

async function post(request) {
  const who = await browserDevice(request);
  if (!who) return fail(401, "Not paired.");
  const body = await readJson(request, 2048);
  const sub = valid(body && body.subscription);
  if (!sub) return fail(400, "Expected a push subscription.");
  const db = store();
  await db.hset(k.push(who.id), { [who.tokenHash]: JSON.stringify(sub) });
  if (who.dev.push !== "1") await db.hset(k.dev(who.id), { push: 1 });
  return json(200, { ok: true });
}

async function remove(request) {
  const who = await browserDevice(request);
  if (!who) return fail(401, "Not paired.");
  await store().hdel(k.push(who.id), who.tokenHash);
  return json(200, { ok: true });
}

export const GET = guarded(get);
export const POST = guarded(post);
export const DELETE = guarded(remove);
