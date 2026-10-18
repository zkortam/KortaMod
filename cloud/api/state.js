// What the paired Chromatic last reported. Reading marks a browser as watching,
// so the device switches to checking in every second. ?load=1 (once per page load)
// also renews the pairing.
import { browserDevice, deviceStatus, markActive, json, fail, guarded } from "./_lib.js";

async function handle(request) {
  const renew = new URL(request.url).searchParams.get("load") === "1";
  const who = await browserDevice(request, { renew });
  if (!who) return fail(401, "Not paired.");
  const now = Date.now();
  await markActive(who.id, who.dev, now);
  const { online, awake, seen } = deviceStatus(who.dev, now);
  let state = null;
  try { state = who.dev.state ? JSON.parse(who.dev.state) : null; } catch { state = null; }
  return json(200, { online, awake, seen, now, state });
}

export const GET = guarded(handle);
