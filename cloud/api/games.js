// Games on the Chromatic. POST stores a game's source for an hour and asks the
// device to download and install it; DELETE asks it to remove one. The device
// fetches the source itself (api/device/game.js), so the command stays small.
import {
  store, sha256, randomHex, browserDevice, deviceStatus, command, replyFor, readJson, json, fail, guarded,
} from "./_lib.js";

export const SOURCE_MAX = 16000;   // bytes, the device's limit
export const NAME_MAX = 12;
export const SLOTS = 32;
export const GAME_S = 3600;
export const gameKey = gid => `km:game:${gid}`;

const bytes = s => new TextEncoder().encode(s).length;

function gameReply(result) {
  if (result === 2) return fail(423, "A game is running on the Chromatic.");
  if (result === 3) return fail(400, "The Chromatic couldn't store that game (too big or no free slot).");
  if (result === 5) return fail(409, "Run the KortaMod installer once to enable games on this Chromatic.");
  return replyFor(result);
}

async function paired(request) {
  const who = await browserDevice(request, { renew: true });
  if (!who) return { error: fail(401, "Not paired.") };
  return { who };
}

async function install(request) {
  const { who, error } = await paired(request);
  if (error) return error;
  const body = await readJson(request, 6 * SOURCE_MAX + 1024);
  const name = body && typeof body.name === "string" ? body.name.trim() : "";
  const source = body && typeof body.source === "string" ? body.source : "";
  if (!name || name.length > NAME_MAX) return fail(400, "Name the game in 1 to 12 characters.");
  if (!source.trim()) return fail(400, "The game has no code.");
  const size = bytes(source);
  if (size > SOURCE_MAX) return fail(400, "The game is too long (16,000 bytes at most).");
  if (!deviceStatus(who.dev).online) return fail(503, "Your Chromatic is offline. Turn on Phone link and Wi-Fi.");
  const gid = randomHex(8);
  /* Kept with the device it was sent to: no other device can download it. */
  await store().set(gameKey(gid), JSON.stringify({ id: who.id, source }), { ex: GAME_S });
  const result = await command(who.id, who.dev, "game", { op: "install", gid, name, size, sha256: sha256(source) });
  return gameReply(result);
}

async function remove(request) {
  const { who, error } = await paired(request);
  if (error) return error;
  const body = await readJson(request, 256);
  const slot = body && body.slot;
  if (!Number.isInteger(slot) || slot < 0 || slot >= SLOTS) return fail(400, "Expected {slot 0-31}.");
  if (!deviceStatus(who.dev).online) return fail(503, "Your Chromatic is offline. Turn on Phone link and Wi-Fi.");
  return gameReply(await command(who.id, who.dev, "game", { op: "delete", slot }));
}

export const POST = guarded(install);
export const DELETE = guarded(remove);
