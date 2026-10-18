// Routes /api/notes, /api/alarm, ... (see vercel.json) to their handlers.
import * as commands from "./_commands.js";
import { fail } from "./_lib.js";

const METHOD = { notes: "PUT", alarm: "PUT", clock: "PUT", settings: "PUT", message: "POST", timer: "POST", wifi: "POST" };

function route(request) {
  const url = new URL(request.url);
  const last = url.pathname.split("/").pop();
  const kind = METHOD[last] ? last : url.searchParams.get("kind");
  if (!METHOD[kind]) return fail(404, "Not found.");
  if (request.method !== METHOD[kind]) return fail(405, "Method not allowed.");
  return commands[kind](request);
}

export const PUT = route;
export const POST = route;
