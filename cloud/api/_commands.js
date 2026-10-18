// The browser commands that only queue something for the Chromatic. They share
// one function (api/command.js, routed by vercel.json) to stay within the plan's
// function count; each keeps its own URL.
import { deviceCommand } from "./_lib.js";

// ---- notes (PUT)
// Replaces the Chromatic's notes at a revision (409 if it changed meanwhile). The
// first `pinned` notes are pinned.

const SLOTS = 16;

export const notes = deviceCommand("notes", b =>
  Number.isInteger(b.rev) && b.rev >= 0 && b.rev <= 65535 && Array.isArray(b.items) && b.items.length <= SLOTS
  && b.items.every(t => typeof t === "string" && t.length <= 400)
  && Number.isInteger(b.pinned ?? 0) && (b.pinned ?? 0) >= 0 && (b.pinned ?? 0) <= b.items.length
  && { rev: b.rev, items: b.items, pinned: b.pinned ?? 0 },
  { limit: 16384, invalid: "Expected {rev, items, pinned} with at most 16 text notes.", busy: "A note is being edited on the Chromatic." });

// ---- alarm (PUT)
// Sets one of the Chromatic's alarms: time, repeat days (bit 0 Monday .. bit 6
// Sunday; 0 rings once) and a short label.

const ALARMS = 4;

export const alarm = deviceCommand("alarm", b => {
  const index = b.index ?? 0, days = b.days ?? 127, label = b.label ?? "";
  const ok = Number.isInteger(index) && index >= 0 && index < ALARMS && typeof b.on === "boolean"
    && Number.isInteger(b.hour) && b.hour >= 0 && b.hour <= 23 && Number.isInteger(b.minute) && b.minute >= 0 && b.minute <= 59
    && Number.isInteger(days) && days >= 0 && days <= 127 && typeof label === "string" && label.length <= 24;
  return ok && { index, on: b.on, hour: b.hour, minute: b.minute, days, label };
}, { invalid: "Expected {index 0-3, on, hour 0-23, minute 0-59, days 0-127, label}." });

// ---- clock (PUT)
// Sets the Chromatic's clock and time zone. The server's clock is the reference,
// not the browser's, which may be wrong; only the zone comes from the browser.

export const clock = deviceCommand("clock", b =>
  Number.isInteger(b.zone_minutes) && b.zone_minutes >= -720 && b.zone_minutes <= 840
  && { epoch: Math.floor(Date.now() / 1000), zone_minutes: b.zone_minutes },
  { invalid: "Expected {zone_minutes}." });

// ---- settings (PUT)
// Changes one Chromatic setting, with the same effect as its Settings rows.

const RANGES = {
  brightness: [1, 16], low_batt: [0, 2], player: [1, 8], palette: [0, 14],
  night: "bool", flow: "bool", hour12: "bool", silent: "bool", blend: "bool",
  cc_lcd: "bool", cc_usb: "bool", transit: "bool", dpad4: "bool",
};

export const settings = deviceCommand("setting", b => {
  const range = RANGES[b.key];
  const ok = range && (range === "bool" ? typeof b.value === "boolean"
    : Number.isInteger(b.value) && b.value >= range[0] && b.value <= range[1]);
  return ok && { key: b.key, value: b.value };
}, { limit: 256, invalid: "Unknown setting or value out of range." });

// ---- message (POST)
// Shows a message on the Chromatic, or with find=true flashes it to locate it.

export const message = deviceCommand("message", b =>
  typeof b.text === "string" && b.text.length <= 400 && (b.find === undefined || typeof b.find === "boolean")
  && (b.text.trim() || b.find === true) && { text: b.text, find: b.find === true },
  { invalid: "Expected {text, find}.", busy: "An alarm or timer is ringing on the Chromatic." });

// ---- timer (POST)
// Sets, starts, stops or resets the Chromatic's timer.

export const timer = deviceCommand("timer", b =>
  ["set", "start", "stop", "reset"].includes(b.action)
  && (b.action !== "set" || (typeof b.countdown === "boolean"
    && (!b.countdown || (Number.isInteger(b.seconds) && b.seconds >= 5 && b.seconds <= 10800))))
  && { action: b.action, countdown: !!b.countdown, seconds: b.seconds || 0 },
  { limit: 256, invalid: "Expected {action, countdown, seconds 5-10800}.", busy: "Stop the timer before changing it." });

// ---- wifi (POST)
// Moves the Chromatic to another Wi-Fi network. If it cannot join, it returns to
// the network it was on, so this can never strand it offline. The password only
// passes through the short-lived command queue.

const bytes = s => new TextEncoder().encode(s).length;

export const wifi = deviceCommand("wifi", b => {
  const ssid = typeof b.ssid === "string" ? b.ssid : "";
  const password = typeof b.password === "string" ? b.password : "";
  const ok = ssid && bytes(ssid) <= 32 && (!password || (bytes(password) >= 8 && bytes(password) <= 63));
  return ok && { ssid, password };
}, { limit: 512, invalid: "Network name up to 32 characters; password empty or 8-63 characters." });
