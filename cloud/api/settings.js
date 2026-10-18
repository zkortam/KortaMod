// Changes one Chromatic setting, with the same effect as its Settings rows.
import { deviceCommand } from "./_lib.js";

const RANGES = {
  brightness: [1, 16], low_batt: [0, 2], player: [1, 8], palette: [0, 14],
  night: "bool", flow: "bool", hour12: "bool", silent: "bool", blend: "bool",
  cc_lcd: "bool", cc_usb: "bool", transit: "bool", dpad4: "bool",
};

export const PUT = deviceCommand("setting", b => {
  const range = RANGES[b.key];
  const ok = range && (range === "bool" ? typeof b.value === "boolean"
    : Number.isInteger(b.value) && b.value >= range[0] && b.value <= range[1]);
  return ok && { key: b.key, value: b.value };
}, { limit: 256, invalid: "Unknown setting or value out of range." });
