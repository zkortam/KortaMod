// Moves the Chromatic to another Wi-Fi network. If it cannot join, it returns to
// the network it was on, so this can never strand it offline. The password only
// passes through the short-lived command queue.
import { deviceCommand } from "./_lib.js";

const bytes = s => new TextEncoder().encode(s).length;

export const POST = deviceCommand("wifi", b => {
  const ssid = typeof b.ssid === "string" ? b.ssid : "";
  const password = typeof b.password === "string" ? b.password : "";
  const ok = ssid && bytes(ssid) <= 32 && (!password || (bytes(password) >= 8 && bytes(password) <= 63));
  return ok && { ssid, password };
}, { limit: 512, invalid: "Network name up to 32 characters; password empty or 8-63 characters." });
