// Sets, starts, stops or resets the Chromatic's timer.
import { deviceCommand } from "./_lib.js";

export const POST = deviceCommand("timer", b =>
  ["set", "start", "stop", "reset"].includes(b.action)
  && (b.action !== "set" || (typeof b.countdown === "boolean"
    && (!b.countdown || (Number.isInteger(b.seconds) && b.seconds >= 5 && b.seconds <= 10800))))
  && { action: b.action, countdown: !!b.countdown, seconds: b.seconds || 0 },
  { limit: 256, invalid: "Expected {action, countdown, seconds 5-10800}.", busy: "Stop the timer before changing it." });
