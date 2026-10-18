// Firmware manifest endpoint.
import { test } from "node:test";
import assert from "node:assert/strict";
import { GET, setManifestSource, valid, build } from "../api/firmware.js";

const good = {
  version: "0.13.4-korta8", size: 900000, sha256: "a".repeat(64),
  signature: "MEUCIQ" + "A".repeat(60) + "==", url: "https://kortamod.app/fw/0.13.4-korta8.bin", notes: "Games",
};
const get = q => GET(new Request("https://kortamod.app/api/firmware" + (q || "")));

test("versions compare by build number", () => {
  assert.equal(build("0.13.4-korta12"), 12);
  assert.equal(build("0.13.4"), -1);
  assert.ok(build("0.13.4-korta10") > build("0.13.4-korta9"));
});

test("a valid manifest is served and says whether it is newer", async () => {
  setManifestSource(async () => good);
  let r = await get("?current=0.13.4-korta7");
  assert.equal(r.status, 200);
  const body = await r.json();
  assert.equal(body.newer, true);
  assert.equal(body.sha256, good.sha256);
  assert.equal((await (await get("?current=0.13.4-korta8")).json()).newer, false);
  assert.equal((await (await get("?current=0.13.4")).json()).newer, false, "stock firmware isn't offered OTA");
  assert.equal((await (await get()).json()).newer, undefined);
});

test("missing or malformed manifests are not served", async () => {
  setManifestSource(async () => null);
  assert.equal((await get()).status, 404);
  for (const bad of [
    { ...good, url: "http://x/fw.bin" },
    { ...good, size: 0x100001 },
    { ...good, sha256: "zz" },
    { ...good, version: "1.0" },
    { ...good, signature: "<script>" },
  ]) {
    assert.equal(valid(bad), false);
    setManifestSource(async () => bad);
    assert.equal((await get()).status, 404);
  }
  setManifestSource(async () => { throw new Error("down"); });
  assert.equal((await get()).status, 404);
});

test("update queues the published firmware for an older Chromatic", async () => {
  const { useMemoryStore, keys } = await import("../api/_lib.js");
  const { POST: sync } = await import("../api/device/sync.js");
  const { POST: pair } = await import("../api/pair.js");
  const { POST: update } = await import("../api/update.js");
  useMemoryStore();
  const ID = "00000000000000ab", SECRET = "c".repeat(64);
  const dev = (body) => sync(new Request("http://x/api/device/sync", { method: "POST",
    headers: { "x-device-id": ID, authorization: "Bearer " + SECRET, "content-type": "application/json" }, body: JSON.stringify(body) }));
  await dev({ code: "135790", state: { device: { mcu: "v0.13.4-korta7" } } });
  const t = await (await pair(new Request("http://x/api/pair", { method: "POST", headers: { "content-type": "application/json", "x-forwarded-for": "7.7.7.7" }, body: JSON.stringify({ code: "135790" }) }))).json();
  const call = () => update(new Request("http://x/api/update", { method: "POST", headers: { authorization: "Bearer " + t.token } }));
  setManifestSource(async () => ({ version: "0.13.4-korta7", size: 900000, sha256: "a".repeat(64), signature: "A".repeat(96), url: "https://kortamod.app/fw/0.13.4-korta7.bin" }));
  assert.equal((await call()).status, 409, "same version");
  setManifestSource(async () => ({ version: "0.13.4-korta8", size: 900000, sha256: "a".repeat(64), signature: "A".repeat(96), url: "https://kortamod.app/fw/0.13.4-korta8.bin" }));
  /* The device answers 5: no OTA layout yet. */
  const pending = call();
  await new Promise(r => setTimeout(r, 300));
  const reply = await (await dev({ code: "135790" })).json();
  const cmd = reply.commands.find(c => c.kind === "update");
  assert.ok(cmd && cmd.url.endsWith("korta8.bin") && cmd.size === 900000);
  await dev({ code: "135790", results: [{ id: cmd.id, result: 5 }] });
  const r = await pending;
  assert.equal(r.status, 409);
  assert.match((await r.json()).error, /installer/);
  assert.ok(keys);
});
