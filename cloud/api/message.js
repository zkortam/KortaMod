// Shows a message on the Chromatic, or with find=true flashes it to locate it.
import { deviceCommand } from "./_lib.js";

export const POST = deviceCommand("message", b =>
  typeof b.text === "string" && b.text.length <= 400 && (b.find === undefined || typeof b.find === "boolean")
  && (b.text.trim() || b.find === true) && { text: b.text, find: b.find === true },
  { invalid: "Expected {text, find}.", busy: "An alarm or timer is ringing on the Chromatic." });
