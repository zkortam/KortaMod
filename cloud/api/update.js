// Installs the newest signed firmware on the paired Chromatic. The device checks
// the image's SHA-256 and signature itself, so this only says where it is.
import { browserDevice, deviceStatus, command, replyFor, fail, guarded } from "./_lib.js";
import { latest, build } from "./firmware.js";

async function handle(request) {
  const who = await browserDevice(request, { renew: true });
  if (!who) return fail(401, "Not paired.");
  if (!deviceStatus(who.dev).online) return fail(503, "Your Chromatic is offline. Turn on Phone link and Wi-Fi.");
  const m = await latest(request);
  if (!m) return fail(404, "No firmware update is published.");
  let current = "";
  try { current = (JSON.parse(who.dev.state || "{}").device || {}).mcu || ""; } catch { current = ""; }
  if (build(m.version) <= build(current.replace(/^v/, ""))) return fail(409, "Already up to date.");
  const result = await command(who.id, who.dev, "update", { url: m.url, size: m.size, sha256: m.sha256, signature: m.signature });
  if (result === 5) return fail(409, "Run the KortaMod installer once over USB to enable updates from the site.");
  if (result === 2) return fail(423, "Charge your Chromatic (30% or plugged in) and try again.");
  return replyFor(result);
}

export const POST = guarded(handle);
