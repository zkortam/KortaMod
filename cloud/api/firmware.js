// The newest signed KortaMod firmware, for updates over the air. The manifest is
// a static file written by scripts/sign_firmware.py (public/fw/latest.json); the
// image sits next to it. ?current=<version> adds whether that version is older.
import { json, fail, guarded } from "./_lib.js";

const VERSION_RE = /^0\.13\.4-korta(\d+)$/;
const HEX64 = /^[0-9a-f]{64}$/;
const B64 = /^[A-Za-z0-9+/]{40,120}={0,2}$/;

/* Tests replace how the manifest is read. */
let source = async request => {
  const r = await fetch(new URL("/fw/latest.json", request.url), { cache: "no-store" });
  return r.ok ? r.json() : null;
};
export function setManifestSource(fn) { source = fn; }

/* The published manifest if it is well formed, else null. */
export async function latest(request) {
  try { const m = await source(request); return valid(m) ? m : null; } catch { return null; }
}

export function build(n) { const m = VERSION_RE.exec(n || ""); return m ? Number(m[1]) : -1; }

export function valid(m) {
  return !!m && VERSION_RE.test(m.version) && Number.isInteger(m.size) && m.size > 0 && m.size <= 0x100000
    && HEX64.test(m.sha256) && typeof m.signature === "string" && B64.test(m.signature)
    && typeof m.url === "string" && m.url.startsWith("https://") && m.url.length < 160;
}

async function handle(request) {
  let manifest = null;
  try { manifest = await source(request); } catch { manifest = null; }
  if (!valid(manifest)) return fail(404, "No firmware update is published.");
  const { version, size, sha256, signature, url } = manifest;
  const out = { version, size, sha256, signature, url, notes: typeof manifest.notes === "string" ? manifest.notes : "" };
  const current = new URL(request.url).searchParams.get("current");
  if (current !== null) out.newer = build(version) > build(current) && build(current) >= 0;
  return json(200, out);
}

export const GET = guarded(handle);
