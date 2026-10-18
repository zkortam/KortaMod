// Sets the Chromatic's clock and time zone. The server's clock is the reference,
// not the browser's, which may be wrong; only the zone comes from the browser.
import { deviceCommand } from "./_lib.js";

export const PUT = deviceCommand("clock", b =>
  Number.isInteger(b.zone_minutes) && b.zone_minutes >= -720 && b.zone_minutes <= 840
  && { epoch: Math.floor(Date.now() / 1000), zone_minutes: b.zone_minutes },
  { invalid: "Expected {zone_minutes}." });
