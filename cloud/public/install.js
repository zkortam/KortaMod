// The browser installer page: Web Serial + esptool-js drive the steps in
// install-core.js (the same plan as the downloadable installer).
import { ESPLoader, Transport } from "/vendor/esptool-js-0.7.0.mjs";
import { install, inspect, restore, toStock, loadPackage, loadStock, InstallError, USB_FILTER, FLASH_BYTES } from "/install-core.js";

const $ = id => document.getElementById(id);
const VIEWS = ["vStart", "vNoUsb", "vDevice", "vWork", "vDone", "vError", "vRestore", "vStock"];
const STEPS = ["check", "backup", "install", "verify"];
/* Share of the whole bar each phase takes (the backup reads all 4 MiB; an
 * update without a backup gives its share to writing). */
const WEIGHT = { check: 0.03, backup: 0.47, install: 0.38, verify: 0.12 };
const BAUD = 921600;
let log = [];
let lastBackup = null;     /* {name, image, record}: offered again if a download was blocked */
let interrupted = false;   /* a write was cut off in this session: Install may finish it */

/* Views with pills under the card: the start view offers Restore and Stock;
 * the side flows offer Back; working, done and device views offer nothing. */
const PILLS = { vStart: "pillsStart", vDevice: "pillsBack", vRestore: "pillsBack", vStock: "pillsBack", vError: "pillsBack" };
function show(id) {
  for (const v of VIEWS) $(v).hidden = v !== id;
  $("pillsStart").hidden = PILLS[id] !== "pillsStart";
  $("pillsBack").hidden = PILLS[id] !== "pillsBack";
  const first = $(id).querySelector("h1");
  if (first) { first.tabIndex = -1; first.focus({ preventScroll: true }); }
}
function note(line) {
  log.push(line);
  if (log.length > 400) log = log.slice(-300);
}

/* ---------- progress: one word and a percentage over one bar ---------- */
let current = "check";
const WORD = { check: "Checking", backup: "Backing up", install: "Installing", verify: "Verifying" };
const skipped = s => $("steps").querySelector(`[data-step="${s}"]`).hidden;
function setPhase(name) {
  current = name;
  setProgress(0);
}
function setProgress(f) {
  f = Math.max(0, Math.min(1, f || 0));
  const sum = STEPS.reduce((n, s) => n + (skipped(s) ? 0 : WEIGHT[s]), 0);
  let done = 0;
  for (const s of STEPS) { if (s === current) break; if (!skipped(s)) done += WEIGHT[s]; }
  const total = current === "done" ? 1 : (done + (WEIGHT[current] || 0) * f) / sum;
  $("barFill").style.width = (total * 100).toFixed(1) + "%";
  $("bar").setAttribute("aria-valuenow", String(Math.round(total * 100)));
  const word = current === "install" ? $("stepInstall").textContent : WORD[current];
  if (word) $("workSub").textContent = `${word} · ${Math.round(total * 100)}%`;
}

/* ---------- the Chromatic over Web Serial ---------- */
async function connect() {
  let port;
  try { port = await navigator.serial.requestPort({ filters: [USB_FILTER] }); }
  catch (e) { if (e && e.name === "NotFoundError") return null; throw e; }
  const transport = new Transport(port, false);
  const terminal = { clean() {}, writeLine: s => note(s), write: s => note(s) };
  const loader = new ESPLoader({ transport, baudrate: BAUD, romBaudrate: 115200, terminal });
  try {
    await loader.main();
  } catch (e) {
    try { await transport.disconnect(); } catch {}
    const busy = /open|in use|busy|access/i.test(String(e && (e.message || e.name)));
    throw new InstallError(busy ? "busy" : "connect", String(e && e.message || e), { cause: e });
  }
  if (!loader.chip || loader.chip.CHIP_NAME !== "ESP32") {
    try { await transport.disconnect(); } catch {}
    throw new InstallError("wrong-device", "Not an ESP32: " + (loader.chip && loader.chip.CHIP_NAME));
  }
  const device = {
    mac: () => loader.chip.readMac(loader),
    read: (addr, size, progress) => loader.readFlash(addr, size, (_, got, total) => progress && progress(got / total)),
    write: (files, progress) => {
      const total = files.reduce((n, f) => n + f.data.length, 0);
      const before = files.map((_, i) => files.slice(0, i).reduce((n, f) => n + f.data.length, 0));
      return loader.writeFlash({
        fileArray: files.map(f => ({ data: f.data, address: f.address })),
        flashMode: "keep", flashFreq: "keep", flashSize: "keep", eraseAll: false, compress: true,
        reportProgress: (i, written, size) => progress && progress((before[i] + written * (files[i].data.length / (size || 1))) / total),
      });
    },
    md5: (addr, size) => loader.flashMd5sum(addr, size),
    reset: () => loader.after("hard_reset"),
  };
  return { port, transport, device };
}
async function release(link) {
  if (link) { try { await link.transport.disconnect(); } catch {} }
}

/* After the restart: set the clock like the installer does. Best effort; the
 * phone link sets it too once paired. */
async function syncClock(port) {
  const minutes = -new Date().getTimezoneOffset();
  if (minutes < -720 || minutes > 840) return false;
  try {
    await port.open({ baudRate: 115200 });
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  } catch { return false; }
  const reader = port.readable.getReader(), writer = port.writable.getWriter();
  const dec = new TextDecoder();
  let text = "", ok = false;
  const readUntil = async (re, ms) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const r = await Promise.race([reader.read(), new Promise(res => setTimeout(() => res(null), 400))]);
      if (r && r.value) { text += dec.decode(r.value); if (text.length > 16384) text = text.slice(-8192); }
      if (r && r.done) return false;
      if (re.test(text)) return true;
    }
    return false;
  };
  try {
    if (await readUntil(/mcu>/, 15000)) {
      const stamp = Math.floor(Date.now() / 1000);
      await writer.write(new TextEncoder().encode(`korta time ${stamp} ${minutes}\r\n`));
      await new Promise(r => setTimeout(r, 300));
      text = "";
      await writer.write(new TextEncoder().encode("korta\r\n"));
      ok = await readUntil(new RegExp(`korta time \\d+ known 1 zone_minutes \\+?${minutes}\\b`), 5000);
    }
  } catch {}
  try { await reader.cancel(); } catch {}
  reader.releaseLock();
  writer.releaseLock();
  try { await port.close(); } catch {}
  return ok;
}

/* ---------- the backup: two downloads, never uploaded ---------- */
function download(name, bytes, type) {
  const url = URL.createObjectURL(new Blob([bytes], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}
function saveBackup(b) {
  download(b.name + ".bin", b.image, "application/octet-stream");
  download(b.name + ".json", JSON.stringify(b.record), "application/json");
}
async function saveFirst(b) {
  lastBackup = b;
  saveBackup(b);
  /* Give the browser a moment to take the files before anything is written. */
  await new Promise(r => setTimeout(r, 1500));
}

/* ---------- firmware ---------- */
let pkgPromise = null;
async function fetchBytes(path) {
  const f = await fetch(path, { cache: "no-store" });
  if (!f.ok) throw new InstallError("package", "Could not download " + path);
  return new Uint8Array(await f.arrayBuffer());
}
async function manifest() {
  const r = await fetch("/fw/install.json", { cache: "no-store" });
  if (!r.ok) throw new InstallError("package", "The firmware list could not be loaded");
  return r.json();
}
function firmware() {
  if (!pkgPromise) {
    pkgPromise = manifest().then(m => loadPackage(fetchBytes, m));
    pkgPromise.catch(() => { pkgPromise = null; });
  }
  return pkgPromise;
}

/* ---------- errors, in plain words ---------- */
function explain(e, mode) {
  const code = e && e.code;
  const written = !!(e && e.written);
  const unchanged = "Nothing was changed.";
  switch (code) {
    case "connect": return ["Couldn't connect", "Replug it with a data cable and turn it on."];
    case "busy": return ["Port in use", "Close other apps using your Chromatic."];
    case "wrong-device": return ["Not a Chromatic", "Choose your Chromatic in the list."];
    case "version": return ["Not supported yet", `It runs ${e.version}. ${unchanged}`];
    case "layout": return ["Unknown layout", unchanged];
    case "bootloader": case "no-app": return ["Not a Chromatic v4.2", unchanged];
    case "backup": return ["Backup didn't finish", unchanged];
    case "package": return ["Couldn't download", "Check your connection."];
    case "verify": return ["Check failed", "Try again to finish."];
    case "restore-size": case "restore-record": case "restore-sum": case "restore-mac": return ["Can't restore that", e.message + "."];
    default:
      if (!written) return ["Disconnected", "Nothing changed. Plug it back in."];
      return ["Disconnected while writing", "Plug it back in and try again to finish."];
  }
}
function fail(e, mode) {
  note("error: " + (e && (e.stack || e.message) || e));
  if (e && e.written && e.code !== "verify") interrupted = mode !== "restore";
  const [title, sub] = explain(e, mode);
  $("errTitle").textContent = title;
  $("errSub").textContent = sub;
  $("errRestore").hidden = !(e && e.written);
  $("retryBtn").textContent = mode === "restore" ? "Restore again" : "Try again";
  $("retryBtn").onclick = mode === "restore" ? () => show("vRestore") : mode === "stock" ? runStock : runConnect;
  backupLine($("errFine"));
  $("errLog").textContent = log.slice(-60).join("\n");
  show("vError");
}
function backupLine(el) {
  if (!lastBackup) { el.hidden = true; return; }
  el.hidden = false;
  el.textContent = "";
  el.append(`Backup: ${lastBackup.name} · keep it private · `);
  const again = document.createElement("button");
  again.textContent = "Save again";
  again.onclick = () => saveBackup(lastBackup);
  el.append(again);
}

/* ---------- connect, see what it runs, install or update ---------- */
let link = null;        /* the open connection while the device view is shown */
let found = null;       /* inspect() result */
const short = v => v === "0.13.4" ? "ModRetro firmware 0.13.4" : v === "interrupted install" ? "Unfinished install"
  : "KortaMod " + v.replace(/^0\.13\.4-/, "");
const tag = v => v.replace(/^0\.13\.4-/, "");

function renderDevice(r) {
  $("devNow").textContent = short(r.version);
  const target = tag(r.target);
  const up = r.action === "current" || r.action === "newer";
  $("devTitle").textContent = r.action === "install" ? (r.version === "interrupted install" ? "Finish installing" : "Ready to install")
    : r.action === "update" ? "Update available" : "Up to date";
  $("devSub").hidden = r.action !== "newer";
  $("devSub").textContent = r.action === "newer" ? `Newer than ${target}.` : "";
  $("backupRow").hidden = r.action !== "update";
  $("backupBox").checked = true;
  $("goBtn").hidden = up;
  $("goBtn").textContent = r.action === "update" ? `Update to ${target}` : `Install ${target}`;
  $("goNote").hidden = r.action !== "install";
  $("devPair").hidden = !up;
  $("reinstallBtn").hidden = !up;
  $("reinstallBtn").textContent = "Reinstall";
  show("vDevice");
}

async function runConnect() {
  log = [];
  $("connectBtn").classList.add("busy");
  try {
    const pkg = await firmware();
    const m = await manifest();
    link = await connect();
    if (!link) return;   /* the picker was closed */
    found = await inspect(link.device, pkg, { resume: interrupted });
    note("found " + JSON.stringify(found));
    renderDevice(found);
  } catch (e) {
    await release(link);
    link = null;
    fail(e, "install");
  } finally {
    $("connectBtn").classList.remove("busy");
  }
}

async function runChange() {
  const updating = found && found.action !== "install";
  const keepBackup = !updating || $("backupBox").checked;
  try {
    const pkg = await firmware();
    $("workTitle").textContent = updating ? "Updating" : "Installing";
    $("stepInstall").textContent = updating ? "Updating" : "Installing";
    $("steps").querySelector('[data-step="backup"]').hidden = !keepBackup;
    show("vWork");
    setPhase("check");
    const result = await install(link.device, pkg, {
      on: { phase: setPhase, progress: setProgress }, save: saveFirst, resume: interrupted, backup: keepBackup,
    });
    interrupted = false;
    const port = link.port;
    await release(link);
    link = null;
    $("workSub").textContent = "Restarting";
    note("clock sync: " + await syncClock(port));
    $("doneTitle").textContent = updating ? "Updated" : "Installed";
    $("doneSub").textContent = updating ? `KortaMod ${tag(result.to)}` : "On your Chromatic: Menu, then Home › Phone.";
    $("doneNext").hidden = updating;
    $("pairNow").hidden = false;
    $("pairNow").textContent = "Pair";
    backupLine($("doneFine"));
    show("vDone");
  } catch (e) {
    await release(link);
    link = null;
    fail(e, "install");
  } finally {
    $("steps").querySelector('[data-step="backup"]').hidden = false;
  }
}

/* ---------- back to stock ---------- */
async function runStock() {
  log = [];
  let link = null;
  $("stockBtn").classList.add("busy");
  try {
    const pkg = await firmware();
    const official = await loadStock(fetchBytes, await manifest(), pkg.tables);
    link = await connect();
    if (!link) return;
    $("workTitle").textContent = "Returning to stock";
    $("stepInstall").textContent = "Writing";
    show("vWork");
    setPhase("check");
    await toStock(link.device, pkg, official, { on: { phase: setPhase, progress: setProgress }, save: saveFirst, resume: interrupted });
    interrupted = false;
    await release(link);
    $("doneTitle").textContent = "Back to stock";
    $("doneSub").textContent = "Update it with ModRetro's tool from here on.";
    $("doneNext").hidden = true;
    $("pairNow").hidden = true;
    backupLine($("doneFine"));
    show("vDone");
  } catch (e) {
    await release(link);
    fail(e, "stock");
  } finally {
    $("stockBtn").classList.remove("busy");
  }
}

/* ---------- restore ---------- */
let chosen = { bin: null, json: null };
function paintFiles() {
  const set = (el, file, empty) => {
    el.classList.toggle("ok", !!file);
    el.querySelector("use").setAttribute("href", file ? "#i-check-circle-fill" : "#i-download-simple");
    el.querySelector("span").textContent = file ? file.name : empty;
  };
  set($("fileBin"), chosen.bin, ".bin");
  set($("fileJson"), chosen.json, ".json");
  $("restoreBtn").disabled = !(chosen.bin && chosen.json);
}
$("pick").addEventListener("change", () => {
  for (const f of $("pick").files) {
    if (/\.bin$/i.test(f.name)) chosen.bin = f;
    else if (/\.json$/i.test(f.name)) chosen.json = f;
  }
  $("pick").value = "";
  paintFiles();
});
async function runRestore() {
  log = [];
  let link = null;
  try {
    if (chosen.bin.size !== FLASH_BYTES) throw new InstallError("restore-size", "That .bin isn't a complete 4 MiB backup");
    let record;
    try { record = JSON.parse(await chosen.json.text()); } catch { throw new InstallError("restore-record", "That .json is damaged"); }
    const image = new Uint8Array(await chosen.bin.arrayBuffer());
    const pkg = await firmware();
    link = await connect();
    if (!link) return;
    $("workTitle").textContent = "Restoring";
    $("stepInstall").textContent = "Restoring";
    show("vWork");
    setPhase("check");
    await restore(link.device, image, record, pkg, { on: { phase: setPhase, progress: setProgress } });
    await release(link);
    $("doneTitle").textContent = "Restored";
    $("doneSub").textContent = "Back to how it was.";
    $("doneNext").hidden = true;
    $("pairNow").hidden = true;
    $("doneFine").hidden = true;
    show("vDone");
  } catch (e) {
    await release(link);
    fail(e, "restore");
  }
}

/* ---------- wiring ---------- */
$("connectBtn").onclick = runConnect;
$("goBtn").onclick = runChange;
$("reinstallBtn").onclick = runChange;
$("backBtn").onclick = async () => { await release(link); link = null; show("vStart"); };
$("restoreLink").onclick = () => show("vRestore");
$("stockLink").onclick = () => show("vStock");
$("stockBtn").onclick = runStock;
$("errRestore").onclick = () => show("vRestore");
$("restoreBtn").onclick = runRestore;

/* Previews of each state, for checking the design without a device. */
function preview(state) {
  const fake = { name: "chromatic-mcu-20261018-091507", image: new Uint8Array(1), record: {} };
  if (state === "work") { show("vWork"); setPhase("backup"); setProgress(0.42); return; }
  if (state === "done") { lastBackup = fake; backupLine($("doneFine")); show("vDone"); return; }
  if (state === "error") { lastBackup = fake; fail(Object.assign(new InstallError("version", "x"), { version: "0.13.5", written: false }), "install"); return; }
  if (state === "lost") { lastBackup = fake; fail(Object.assign(new InstallError("device", "The device has been lost."), { written: true }), "install"); return; }
  if (state === "nousb") { show("vNoUsb"); return; }
  if (state === "install") { renderDevice({ version: "0.13.4", target: "0.13.4-korta8", action: "install" }); return; }
  if (state === "update") { renderDevice({ version: "0.13.4-korta7", target: "0.13.4-korta8", action: "update" }); return; }
  if (state === "current") { renderDevice({ version: "0.13.4-korta8", target: "0.13.4-korta8", action: "current" }); return; }
  if (state === "stock") { show("vStock"); return; }
  if (state === "restore") { chosen = { bin: { name: "chromatic-mcu-20261017-232208.bin" }, json: null }; paintFiles(); show("vRestore"); return; }
  show("vStart");
}
const state = new URLSearchParams(location.search).get("preview");
if (state) preview(state);
else if (!("serial" in navigator)) show("vNoUsb");
else { show("vStart"); firmware().catch(() => {}); }
/* "Latest: korta8 · 18 Oct 2026" under the button. */
manifest().then(m => {
  const when = m.date ? new Date(m.date + "T12:00:00").toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" }) : "";
  $("latest").textContent = `Latest ${tag(m.version)}`;
  $("latest").title = when;
  $("latest").hidden = false;
}).catch(() => {});
