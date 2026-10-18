// The Chromatic downloads a game it was asked to install. Only the device the
// game was sent to can fetch it, with the same credentials as its check-ins.
import { ID_RE, SECRET_RE, keys as k, store, sha256, sameHex, bearer, fail, guarded } from "../_lib.js";
import { gameKey } from "../games.js";

const GID_RE = /^[0-9a-f]{16}$/;

async function handle(request) {
  const id = request.headers.get("x-device-id") || "";
  const secret = bearer(request);
  if (!ID_RE.test(id) || !SECRET_RE.test(secret)) return fail(401, "bad device credentials");
  const db = store();
  const dev = (await db.hgetall(k.dev(id))) || {};
  if (!dev.secret || !sameHex(dev.secret, sha256(secret))) return fail(401, "wrong device secret");
  const gid = new URL(request.url).searchParams.get("gid") || "";
  if (!GID_RE.test(gid)) return fail(400, "bad game id");
  const raw = await db.get(gameKey(gid));
  let game = null;
  try { game = raw ? JSON.parse(raw) : null; } catch { game = null; }
  if (!game || game.id !== id || typeof game.source !== "string") return fail(404, "no such game");
  return new Response(game.source, {
    status: 200,
    headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff" },
  });
}

export const GET = guarded(handle);
