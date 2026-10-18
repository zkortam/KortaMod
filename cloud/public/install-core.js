// What the browser installer does, independent of the page and of the serial
// library, so it can be tested with a simulated Chromatic. It mirrors
// release/kortamod/installer.py step for step:
//   check   bootloader, partition layout (stock or KortaMod) and app version
//   backup  the whole 4 MiB flash, saved on this computer before any write
//   install clear otadata; from stock also clear the games area and write the
//           KortaMod partition table; write the app (settings in NVS stay)
//   verify  every written region by MD5, then restart
// A device is an object with mac(), read(addr, size, progress), write(files,
// progress), md5(addr, size) and reset().

export const FLASH_BYTES = 4 * 1024 * 1024;
export const APP_OFFSET = 0x10000;
export const APP_BYTES = 1024 * 1024;
export const PART_OFFSET = 0x8000;
export const OTADATA = [0x110000, 0x2000];
export const GAMES = [0x220000, 0x80000];
export const KORTA_AREA = [0x110000, 0x190000];   /* otadata, the update slot and games: unused by stock */
export const USB_FILTER = { usbVendorId: 0x374e, usbProductId: 0x0101 };
const VERSION_RE = /^0\.13\.4(-korta[3-9]|-korta[1-9][0-9])?$/;

/** Errors the page explains. `code` picks the message; `written` says whether flash changed. */
export class InstallError extends Error {
  constructor(code, message, detail = {}) {
    super(message);
    this.code = code;
    Object.assign(this, detail);
  }
}

export function appVersion(data) {
  if (!data || data.length < 0x50 || data[0] !== 0xe9 || data[0x20] !== 0x32 || data[0x21] !== 0x54
      || data[0x22] !== 0xcd || data[0x23] !== 0xab) {
    throw new InstallError("no-app", "No compatible ESP32 application at 0x10000");
  }
  const raw = data.subarray(0x30, 0x50);
  const end = raw.indexOf(0);
  return new TextDecoder("ascii").decode(end < 0 ? raw : raw.subarray(0, end));
}

function startsWith(data, prefix) {
  if (!data || data.length < prefix.length) return false;
  for (let i = 0; i < prefix.length; i++) if (data[i] !== prefix[i]) return false;
  return true;
}

/** "stock" or "korta" for the partition table read at 0x8000. */
export function layoutOf(part, tables) {
  if (startsWith(part, tables.stock)) return "stock";
  if (startsWith(part, tables.korta)) return "korta";
  throw new InstallError("layout", "Partition layout differs from stock Chromatic v4.2");
}

export function checkImage(boot, appHead, part, tables, { brokenApp = false } = {}) {
  if (!boot || boot.length < 0x20 || boot[0] !== 0xe9) {
    throw new InstallError("bootloader", "The connected MCU does not have the expected ESP32 bootloader");
  }
  const layout = layoutOf(part, tables);
  /* After an install cut off mid-write the app is partial; finishing it is the fix. */
  if (brokenApp) {
    try { appVersion(appHead); } catch { return { version: "interrupted install", layout }; }
  }
  const version = appVersion(appHead);
  if (!VERSION_RE.test(version)) throw new InstallError("version", `MCU version ${version} is untested`, { version });
  return { version, layout };
}

export async function sha256Hex(data) {
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, "0")).join("");
}

/* MD5, to compare with the flasher's own checksum of what it wrote. */
const S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
const K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
export function md5Hex(data) {
  const n = data.length, padded = new Uint8Array(((n + 8) >>> 6) + 1 << 6);
  padded.set(data);
  padded[n] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (n * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(n / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = view.getUint32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((F << S[i]) | (F >>> (32 - S[i])))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  return [a0, b0, c0, d0].map(w => [0, 8, 16, 24].map(s => ((w >>> s) & 255).toString(16).padStart(2, "0")).join("")).join("");
}

/** Fetches the firmware named in the manifest and checks every SHA-256. */
export async function loadPackage(fetchBytes, manifest) {
  const files = {};
  for (const key of ["app", "stock", "korta"]) {
    const entry = manifest && manifest[key];
    if (!entry || !entry.path || !/^[0-9a-f]{64}$/.test(entry.sha256 || "")) throw new InstallError("package", "The firmware list is damaged");
    const data = await fetchBytes(entry.path);
    if (data.length !== entry.size || (await sha256Hex(data)) !== entry.sha256) {
      throw new InstallError("package", `Firmware file ${entry.path} is damaged`);
    }
    files[key] = data;
  }
  if (files.app.length < 0x50 || files.app.length > APP_BYTES) throw new InstallError("package", "The app does not fit the factory partition");
  const version = appVersion(files.app);
  if (manifest.version && version !== manifest.version) throw new InstallError("package", "The firmware list and the app disagree");
  return { app: files.app, tables: { stock: files.stock, korta: files.korta }, version };
}

export function backupName(now = new Date()) {
  const p = n => String(n).padStart(2, "0");
  return `chromatic-mcu-${now.getFullYear()}${p(now.getMonth() + 1)}${p(now.getDate())}-${p(now.getHours())}${p(now.getMinutes())}${p(now.getSeconds())}`;
}

const blank = size => new Uint8Array(size).fill(0xff);

/** The N of 0.13.4-kortaN; 0 for ModRetro's own firmware. */
export function buildOf(version) {
  const m = /-korta(\d+)$/.exec(version || "");
  return m ? Number(m[1]) : 0;
}

/* Reads what the Chromatic runs now: bootloader, partition table, app header. */
async function readHead(device, pkg, progress, resume) {
  const head = await device.read(0x1000, 0x10000, f => progress(f));
  return checkImage(head.subarray(0, 0x1000), head.subarray(APP_OFFSET - 0x1000, APP_OFFSET),
    head.subarray(PART_OFFSET - 0x1000, PART_OFFSET), pkg.tables, { brokenApp: resume });
}

/**
 * What this page should offer, decided without writing anything:
 * "install" (ModRetro's firmware), "update" (an older KortaMod), "current"
 * (this build) or "newer" (a later build than the one hosted here). Unknown
 * versions and layouts throw, like the installer.
 */
export async function inspect(device, pkg, { resume = false } = {}) {
  return guarded(async () => {
    const found = await readHead(device, pkg, () => {}, resume);
    const have = buildOf(found.version), want = buildOf(pkg.version);
    const action = resume || have === 0 ? "install" : have < want ? "update" : have === want ? "current" : "newer";
    return { ...found, action, target: pkg.version };
  });
}

/* Check and back up: the shared start of every change. Nothing is written here.
 * Without `backup` (an update the person chose not to back up) the 4 MiB read is skipped. */
async function prepare(device, pkg, { phase, progress, save, now, resume, backup = true }) {
  phase("check");
  const found = await readHead(device, pkg, progress, resume);
  /* Skipping is only for updating a KortaMod: a first install always backs up. */
  if (!backup && buildOf(found.version) > 0) return { found, layout: found.layout, name: null, mac: null };

  phase("backup");
  const mac = (await device.mac()).toLowerCase();
  const image = await device.read(0, FLASH_BYTES, f => progress(f));
  if (!image || image.length !== FLASH_BYTES) throw new InstallError("backup", "Backup was incomplete; nothing has been written");
  const { layout } = checkImage(image.subarray(0x1000, 0x2000), image.subarray(APP_OFFSET, APP_OFFSET + 0x1000),
    image.subarray(PART_OFFSET, PART_OFFSET + 0x1000), pkg.tables, { brokenApp: resume });
  const record = { mac, sha256: await sha256Hex(image) };
  const name = backupName(now);
  if (!save) throw new InstallError("backup", "No place to save the backup");
  await save({ name, image, record });
  return { found, layout, name, mac };
}
/* Write, then check every region by MD5 before restarting. */
async function writeAndVerify(device, files, { phase, progress }) {
  phase("install");
  await device.write(files, f => progress(f));
  phase("verify");
  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    if ((await device.md5(f.address, f.data.length)) !== md5Hex(f.data)) {
      throw new InstallError("verify", "What was written does not match; run it again");
    }
    progress((i + 1) / files.length);
  }
  await device.reset();
  phase("done");
}
/* Runs `body`, marking errors with whether flash had been written. */
async function guarded(body) {
  const state = { written: false };
  try {
    return await body(state);
  } catch (error) {
    if (error instanceof InstallError) { error.written = state.written; throw error; }
    throw new InstallError("device", error && error.message ? error.message : String(error), { written: state.written, cause: error });
  }
}
function hooks(on = {}) { return { phase: on.phase || (() => {}), progress: on.progress || (() => {}) }; }

/**
 * Installs KortaMod. `on.phase(name)` gets check, backup, install, verify, done;
 * `on.progress(fraction)` 0..1 within a phase; `save({name, image, record})` must
 * store the backup (and resolve) before anything is written. `resume`: this page
 * cut off an earlier install mid-write, so a partial app is expected and accepted.
 * `backup: false` skips the backup when updating a KortaMod (never from stock).
 * The plan follows the layout: from stock, add the KortaMod partitions; on the
 * KortaMod layout, only otadata and the app (so updates never repartition).
 */
export async function install(device, pkg, { on, save, now, resume = false, backup = true } = {}) {
  const h = hooks(on);
  return guarded(async state => {
    const { found, layout, name, mac } = await prepare(device, pkg, { ...h, save, now, resume, backup });
    /* otadata first, so the bootloader runs the app written here, not an update
     * left in the staging slot. From stock, the games area starts empty. */
    const files = [{ address: OTADATA[0], data: blank(OTADATA[1]) }];
    if (layout === "stock") {
      files.push({ address: GAMES[0], data: blank(GAMES[1]) });
      files.push({ address: PART_OFFSET, data: pkg.tables.korta });
    }
    files.push({ address: APP_OFFSET, data: pkg.app });
    state.written = true;
    await writeAndVerify(device, files, h);
    return { from: found.version, to: pkg.version, layout, backup: name, mac };
  });
}

/** ModRetro's official app (from its v4.2 release image), checked against pinned hashes. */
export async function loadStock(fetchBytes, manifest, tables) {
  const entry = manifest && manifest.official;
  if (!entry || !/^[0-9a-f]{64}$/.test(entry.sha256 || "") || !/^[0-9a-f]{64}$/.test(entry.app_sha256 || "")) {
    throw new InstallError("package", "The official firmware isn't available here");
  }
  const image = await fetchBytes(entry.path);
  if (image.length !== entry.size || (await sha256Hex(image)) !== entry.sha256) throw new InstallError("package", "The official firmware download is damaged");
  if (!startsWith(image.subarray(PART_OFFSET), tables.stock)) throw new InstallError("package", "The official firmware uses an unexpected layout");
  const app = image.slice(APP_OFFSET);
  if ((await sha256Hex(app)) !== entry.app_sha256 || appVersion(app) !== "0.13.4") throw new InstallError("package", "The official firmware app is damaged");
  return app;
}

/**
 * Back to ModRetro's official firmware and partition layout, like the installer's
 * --stock: KortaMod's areas and the whole app partition are cleared, then the
 * stock table and the official app are written. Settings (NVS) stay.
 */
export async function toStock(device, pkg, stockApp, { on, save, now, resume = false } = {}) {
  const h = hooks(on);
  return guarded(async state => {
    const { found, name, mac } = await prepare(device, pkg, { ...h, save, now, resume });
    const app = blank(APP_BYTES);
    app.set(stockApp);
    const files = [
      { address: KORTA_AREA[0], data: blank(KORTA_AREA[1]) },
      { address: PART_OFFSET, data: pkg.tables.stock },
      { address: APP_OFFSET, data: app },
    ];
    state.written = true;
    await writeAndVerify(device, files, h);
    return { from: found.version, to: "0.13.4", backup: name, mac };
  });
}

/** Puts back a full backup made for this same Chromatic (its .bin and .json). */
export async function restore(device, image, record, pkg, { on } = {}) {
  const h = hooks(on);
  return guarded(async state => {
    h.phase("check");
    if (!image || image.length !== FLASH_BYTES) throw new InstallError("restore-size", "Choose the complete 4 MiB backup (.bin)");
    if (!record || typeof record !== "object" || typeof record.sha256 !== "string" || typeof record.mac !== "string") {
      throw new InstallError("restore-record", "Choose the backup's .json file too");
    }
    if ((await sha256Hex(image)) !== record.sha256) throw new InstallError("restore-sum", "This .json doesn't belong to that backup");
    if ((await device.mac()).toLowerCase() !== record.mac.toLowerCase()) {
      throw new InstallError("restore-mac", "This backup belongs to a different Chromatic");
    }
    checkImage(image.subarray(0x1000, 0x2000), image.subarray(APP_OFFSET, APP_OFFSET + 0x1000),
      image.subarray(PART_OFFSET, PART_OFFSET + 0x1000), pkg.tables);
    state.written = true;
    await writeAndVerify(device, [{ address: 0, data: image }], h);
  });
}
