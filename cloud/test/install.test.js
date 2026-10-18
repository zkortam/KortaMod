// The browser installer's plan, run against a simulated Chromatic flash.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import {
  install, restore, toStock, loadStock, loadPackage, md5Hex, sha256Hex, layoutOf, appVersion, InstallError,
  FLASH_BYTES, APP_OFFSET, APP_BYTES, PART_OFFSET, OTADATA, GAMES, KORTA_AREA,
} from "../public/install-core.js";

const FW = new URL("../public/fw/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("install.json", FW), "utf8"));
const fetchBytes = async path => new Uint8Array(readFileSync(new URL("." + path.replace(/^\/fw/, ""), FW)));

/* An app header like the stock one, with a chosen version string. */
function appWithVersion(app, version) {
  const out = app.slice(0, 0x1000);
  out.fill(0, 0x30, 0x50);
  out.set(new TextEncoder().encode(version), 0x30);
  return out;
}
function flashImage(pkg, { layout = "stock", version = "0.13.4" } = {}) {
  const img = new Uint8Array(FLASH_BYTES).fill(0xff);
  img[0x1000] = 0xe9;
  img.set(layout === "stock" ? pkg.tables.stock : pkg.tables.korta, PART_OFFSET);
  for (let i = 0x9000; i < 0xf000; i++) img[i] = (i * 31) & 0xff;   /* NVS: the user's settings */
  img.set(appWithVersion(pkg.app, version), APP_OFFSET);
  if (layout === "korta") for (let i = GAMES[0]; i < GAMES[0] + 4096; i++) img[i] = 0x42;  /* an installed game */
  return img;
}
/* A Chromatic: flash bytes, an operation log, optional faults. */
function chromatic(img, { mac = "84:1f:e8:e6:b7:f8", corrupt = false } = {}) {
  const log = [];
  return {
    flash: img, log,
    async mac() { log.push("mac"); return mac; },
    async read(addr, size, progress) { log.push(`read ${addr.toString(16)} ${size}`); progress && progress(1); return img.slice(addr, addr + size); },
    async write(files, progress) {
      for (const f of files) {
        log.push(`write ${f.address.toString(16)} ${f.data.length}`);
        img.set(f.data, f.address);
        if (corrupt) img[f.address] ^= 1;
      }
      progress && progress(1);
    },
    async md5(addr, size) { log.push(`md5 ${addr.toString(16)}`); return md5Hex(img.subarray(addr, addr + size)); },
    async reset() { log.push("reset"); },
  };
}

test("md5 and sha256 match Node's", async () => {
  for (const n of [0, 1, 55, 56, 63, 64, 65, 1000, 70001]) {
    const data = new Uint8Array(n).map((_, i) => (i * 7 + n) & 255);
    assert.equal(md5Hex(data), createHash("md5").update(data).digest("hex"), `md5 of ${n} bytes`);
    assert.equal(await sha256Hex(data), createHash("sha256").update(data).digest("hex"));
  }
});

test("the published firmware matches its manifest and is a KortaMod build", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  assert.match(pkg.version, /^0\.13\.4-korta\d+$/);
  assert.equal(pkg.version, manifest.version);
  const bad = structuredClone(manifest);
  bad.app.sha256 = "0".repeat(64);
  await assert.rejects(loadPackage(fetchBytes, bad), e => e.code === "package");
});

test("stock Chromatic: backup first, then the new layout and the app; settings untouched", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const img = flashImage(pkg);
  const nvs = img.slice(0x9000, 0xf000);
  const dev = chromatic(img);
  const phases = [];
  let saved = null;
  const result = await install(dev, pkg, {
    on: { phase: p => phases.push(p) },
    save: async b => { saved = b; dev.log.push("saved"); },
    now: new Date(2026, 9, 18, 9, 5, 7),
  });
  assert.deepEqual(phases, ["check", "backup", "install", "verify", "done"]);
  assert.equal(saved.name, "chromatic-mcu-20261018-090507");
  assert.equal(saved.image.length, FLASH_BYTES);
  assert.deepEqual(saved.record, { mac: "84:1f:e8:e6:b7:f8", sha256: createHash("sha256").update(saved.image).digest("hex") });
  const firstWrite = dev.log.findIndex(l => l.startsWith("write"));
  assert.ok(dev.log.indexOf("saved") >= 0 && dev.log.indexOf("saved") < firstWrite, "the backup is saved before anything is written");
  assert.deepEqual(dev.log.filter(l => l.startsWith("write")), [
    `write ${OTADATA[0].toString(16)} ${OTADATA[1]}`, `write ${GAMES[0].toString(16)} ${GAMES[1]}`,
    `write ${PART_OFFSET.toString(16)} 3072`, `write ${APP_OFFSET.toString(16)} ${pkg.app.length}`,
  ]);
  assert.equal(layoutOf(img.subarray(PART_OFFSET), pkg.tables), "korta");
  assert.equal(appVersion(img.subarray(APP_OFFSET)), pkg.version);
  assert.deepEqual(img.slice(0x9000, 0xf000), nvs, "NVS (settings, Wi-Fi) kept");
  assert.ok(img.subarray(GAMES[0], GAMES[0] + GAMES[1]).every(b => b === 0xff));
  assert.equal(dev.log.at(-1), "reset");
  assert.equal(result.layout, "stock");
  assert.equal(result.from, "0.13.4");
});

test("already KortaMod: only otadata and the app; installed games stay", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const img = flashImage(pkg, { layout: "korta", version: "0.13.4-korta7" });
  const dev = chromatic(img);
  await install(dev, pkg, { save: async () => {} });
  assert.deepEqual(dev.log.filter(l => l.startsWith("write")),
    [`write ${OTADATA[0].toString(16)} ${OTADATA[1]}`, `write ${APP_OFFSET.toString(16)} ${pkg.app.length}`]);
  assert.equal(img[GAMES[0]], 0x42);
});

test("refuses an untested version, a different layout or no bootloader, before backing up or writing", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  for (const [setup, code] of [
    [img => img.set(appWithVersion(pkg.app, "0.13.5"), APP_OFFSET), "version"],
    [img => { img[PART_OFFSET + 40] ^= 0xff; }, "layout"],
    [img => { img[0x1000] = 0; }, "bootloader"],
    [img => { img[APP_OFFSET] = 0; }, "no-app"],
  ]) {
    const img = flashImage(pkg);
    setup(img);
    const dev = chromatic(img);
    let saved = false;
    await assert.rejects(install(dev, pkg, { save: async () => { saved = true; } }), e => e instanceof InstallError && e.code === code && e.written === false);
    assert.ok(!saved && !dev.log.some(l => l.startsWith("write")), code);
  }
  const img = flashImage(pkg, { version: "0.13.5" });
  await assert.rejects(install(chromatic(img), pkg, { save: async () => {} }), e => e.version === "0.13.5");
});

test("a failed backup save stops before any write", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const dev = chromatic(flashImage(pkg));
  await assert.rejects(install(dev, pkg, { save: async () => { throw new Error("disk full"); } }), e => e.written === false);
  assert.ok(!dev.log.some(l => l.startsWith("write")));
});

test("a bad write is caught by verification", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const dev = chromatic(flashImage(pkg), { corrupt: true });
  await assert.rejects(install(dev, pkg, { save: async () => {} }), e => e.code === "verify" && e.written === true);
  assert.ok(!dev.log.includes("reset"));
});

test("restore puts back this Chromatic's own backup only", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const original = flashImage(pkg);
  const record = { mac: "84:1f:e8:e6:b7:f8", sha256: createHash("sha256").update(original).digest("hex") };
  const now = flashImage(pkg, { layout: "korta", version: "0.13.4-korta8" });
  await assert.rejects(restore(chromatic(now.slice(), { mac: "aa:bb:cc:dd:ee:ff" }), original, record, pkg), e => e.code === "restore-mac");
  await assert.rejects(restore(chromatic(now.slice()), original, { ...record, sha256: "0".repeat(64) }, pkg), e => e.code === "restore-sum");
  await assert.rejects(restore(chromatic(now.slice()), original.subarray(0, 1024), record, pkg), e => e.code === "restore-size");
  await assert.rejects(restore(chromatic(now.slice()), original, null, pkg), e => e.code === "restore-record");
  const dev = chromatic(now);
  await restore(dev, original, record, pkg);
  assert.deepEqual(dev.log.filter(l => l.startsWith("write")), [`write 0 ${FLASH_BYTES}`]);
  assert.equal(md5Hex(now), md5Hex(original));
});

test("an install cut off mid-write can be finished in the same session", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const img = flashImage(pkg, { layout: "korta", version: "0.13.4" });
  img.fill(0xff, APP_OFFSET, APP_OFFSET + 0x8000);   /* the app's start was erased */
  await assert.rejects(install(chromatic(img.slice()), pkg, { save: async () => {} }), e => e.code === "no-app");
  const dev = chromatic(img);
  const r = await install(dev, pkg, { save: async () => {}, resume: true });
  assert.equal(r.from, "interrupted install");
  assert.equal(appVersion(img.subarray(APP_OFFSET)), pkg.version);
  /* Resume never accepts a foreign layout or a missing bootloader. */
  const other = flashImage(pkg);
  other[PART_OFFSET + 40] ^= 0xff;
  await assert.rejects(install(chromatic(other), pkg, { save: async () => {}, resume: true }), e => e.code === "layout");
});

test("return to stock: official app and layout, KortaMod areas cleared, settings kept, backup first", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const stockApp = await loadStock(fetchBytes, manifest, pkg.tables);
  assert.equal(appVersion(stockApp), "0.13.4");
  const img = flashImage(pkg, { layout: "korta", version: "0.13.4-korta8" });
  const nvs = img.slice(0x9000, 0xf000);
  const dev = chromatic(img);
  let saved = false;
  const r = await toStock(dev, pkg, stockApp, { save: async () => { saved = true; dev.log.push("saved"); } });
  assert.ok(saved && dev.log.indexOf("saved") < dev.log.findIndex(l => l.startsWith("write")));
  assert.deepEqual(dev.log.filter(l => l.startsWith("write")), [
    `write ${KORTA_AREA[0].toString(16)} ${KORTA_AREA[1]}`, `write ${PART_OFFSET.toString(16)} 3072`, `write ${APP_OFFSET.toString(16)} ${APP_BYTES}`,
  ]);
  assert.equal(layoutOf(img.subarray(PART_OFFSET), pkg.tables), "stock");
  assert.equal(appVersion(img.subarray(APP_OFFSET)), "0.13.4");
  assert.ok(img.subarray(APP_OFFSET + stockApp.length, APP_OFFSET + APP_BYTES).every(b => b === 0xff), "nothing of KortaMod past the official app");
  assert.ok(img.subarray(KORTA_AREA[0], KORTA_AREA[0] + KORTA_AREA[1]).every(b => b === 0xff), "updates and games areas empty");
  assert.deepEqual(img.slice(0x9000, 0xf000), nvs);
  assert.equal(r.from, "0.13.4-korta8");
  const tampered = structuredClone(manifest);
  tampered.official.app_sha256 = "1".repeat(64);
  await assert.rejects(loadStock(fetchBytes, tampered, pkg.tables), e => e.code === "package");
});

test("inspect decides install, update, current or newer without writing", async () => {
  const { inspect } = await import("../public/install-core.js");
  const pkg = await loadPackage(fetchBytes, manifest);
  const n = Number(/korta(\d+)$/.exec(pkg.version)[1]);
  for (const [version, layout, action] of [
    ["0.13.4", "stock", "install"],
    [`0.13.4-korta${n - 1}`, "stock", "update"],
    [`0.13.4-korta${n - 1}`, "korta", "update"],
    [pkg.version, "korta", "current"],
    [`0.13.4-korta${n + 1}`, "korta", "newer"],
  ]) {
    const dev = chromatic(flashImage(pkg, { layout, version }));
    const r = await inspect(dev, pkg);
    assert.equal(r.action, action, `${version} on ${layout}`);
    assert.equal(r.version, version);
    assert.equal(r.layout, layout);
    assert.equal(r.target, pkg.version);
    assert.ok(!dev.log.some(l => l.startsWith("write")) && !dev.log.includes("mac"), "inspect only reads the head");
  }
  const odd = flashImage(pkg, { version: "0.13.5" });
  await assert.rejects(inspect(chromatic(odd), pkg), e => e.code === "version" && e.version === "0.13.5");
});

test("update of a KortaMod-layout Chromatic: no repartition; backup is optional", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const img = flashImage(pkg, { layout: "korta", version: "0.13.4-korta7" });
  const table = img.slice(PART_OFFSET, PART_OFFSET + 0x1000);
  const dev = chromatic(img);
  let saved = false;
  const r = await install(dev, pkg, { backup: false, save: async () => { saved = true; } });
  assert.equal(saved, false, "no backup when the person turned it off");
  assert.ok(!dev.log.some(l => l === `read 0 ${FLASH_BYTES}`), "no 4 MiB read");
  assert.ok(!dev.log.includes("mac"));
  assert.deepEqual(dev.log.filter(l => l.startsWith("write")),
    [`write ${OTADATA[0].toString(16)} ${OTADATA[1]}`, `write ${APP_OFFSET.toString(16)} ${pkg.app.length}`]);
  assert.deepEqual(img.slice(PART_OFFSET, PART_OFFSET + 0x1000), table, "partition table untouched");
  assert.equal(img[GAMES[0]], 0x42, "installed games kept");
  assert.equal(r.backup, null);
  assert.equal(appVersion(img.subarray(APP_OFFSET)), pkg.version);
});

test("update of a stock-layout KortaMod migrates the layout; stock never skips its backup", async () => {
  const pkg = await loadPackage(fetchBytes, manifest);
  const old = flashImage(pkg, { layout: "stock", version: "0.13.4-korta7" });
  const dev = chromatic(old);
  await install(dev, pkg, { backup: true, save: async () => {} });
  assert.ok(dev.log.includes(`write ${PART_OFFSET.toString(16)} 3072`), "KortaMod layout added");
  assert.equal(layoutOf(old.subarray(PART_OFFSET), pkg.tables), "korta");
  const stock = chromatic(flashImage(pkg));
  let saved = false;
  await install(stock, pkg, { backup: false, save: async () => { saved = true; } });
  assert.ok(saved, "backup: false is ignored for ModRetro's firmware");
});
