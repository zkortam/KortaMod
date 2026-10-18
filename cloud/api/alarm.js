// Sets one of the Chromatic's alarms: time, repeat days (bit 0 Monday .. bit 6
// Sunday; 0 rings once) and a short label.
import { deviceCommand } from "./_lib.js";

const ALARMS = 4;

export const PUT = deviceCommand("alarm", b => {
  const index = b.index ?? 0, days = b.days ?? 127, label = b.label ?? "";
  const ok = Number.isInteger(index) && index >= 0 && index < ALARMS && typeof b.on === "boolean"
    && Number.isInteger(b.hour) && b.hour >= 0 && b.hour <= 23 && Number.isInteger(b.minute) && b.minute >= 0 && b.minute <= 59
    && Number.isInteger(days) && days >= 0 && days <= 127 && typeof label === "string" && label.length <= 24;
  return ok && { index, on: b.on, hour: b.hour, minute: b.minute, days, label };
}, { invalid: "Expected {index 0-3, on, hour 0-23, minute 0-59, days 0-127, label}." });
