// Revokes this browser's token.
import { keys as k, store, browserDevice, json, fail, guarded } from "./_lib.js";

async function handle(request) {
  const who = await browserDevice(request);
  if (!who) return fail(401, "Not paired.");
  const db = store();
  await db.del(k.token(who.tokenHash));
  if (who.dev.push === "1") await db.hdel(k.push(who.id), who.tokenHash);
  if (await db.srem(k.tokens(who.id), who.tokenHash)) await db.hincrby(k.dev(who.id), "paired", -1);
  return json(200, { ok: true });
}

export const POST = guarded(handle);
