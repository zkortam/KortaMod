// Replaces the Chromatic's notes at a revision (409 if it changed meanwhile). The
// first `pinned` notes are pinned.
import { deviceCommand } from "./_lib.js";

const SLOTS = 16;

export const PUT = deviceCommand("notes", b =>
  Number.isInteger(b.rev) && b.rev >= 0 && b.rev <= 65535 && Array.isArray(b.items) && b.items.length <= SLOTS
  && b.items.every(t => typeof t === "string" && t.length <= 400)
  && Number.isInteger(b.pinned ?? 0) && (b.pinned ?? 0) >= 0 && (b.pinned ?? 0) <= b.items.length
  && { rev: b.rev, items: b.items, pinned: b.pinned ?? 0 },
  { limit: 16384, invalid: "Expected {rev, items, pinned} with at most 16 text notes.", busy: "A note is being edited on the Chromatic." });
