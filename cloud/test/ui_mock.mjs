// Front-end behaviour against a mocked API, no Chromatic needed:
//   node ui_mock.mjs <dir containing node_modules/puppeteer-core> [screenshot-dir]
// Serves cloud/public with the production CSP, plays a paired Chromatic and
// checks that every control sends the right request and the page reacts.
import http from "node:http";
import { readFileSync, existsSync, statSync } from "node:fs";
import { join, extname, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";

const [modules, shots] = process.argv.slice(2);
const puppeteer = createRequire(join(modules, "x.js"))("puppeteer-core");
const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "public");
const csp = JSON.parse(readFileSync(join(ROOT, "..", "vercel.json"), "utf8")).headers[0].headers.find(h => h.key === "Content-Security-Policy").value;
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm", ".png": "image/png", ".svg": "image/svg+xml", ".webmanifest": "application/manifest+json" };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (p === "/") p = "/index.html";
  const f = join(ROOT, p);
  if (!f.startsWith(ROOT) || !existsSync(f) || statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { "Content-Type": TYPES[extname(f)] || "application/octet-stream", "Content-Security-Policy": csp });
  res.end(readFileSync(f));
}).listen(0);
const base = "http://127.0.0.1:" + server.address().port;

let failures = 0;
const check = (ok, what) => { console.log((ok ? "  ok   " : "  FAIL ") + what); if (!ok) failures++; };
const settle = (ms = 400) => new Promise(r => setTimeout(r, ms));

const state = {
  notes: { rev: 1, slots: 16, capacity: 200, pinned: 1, items: ["Pinned one", "Second"] },
  alarms: [0, 1, 2, 3].map(i => ({ on: i === 0, hour: 7 + i, minute: 0, days: 31, label: i ? "" : "Work" })),
  clock: { known: true, epoch: Math.floor(Date.now() / 1000), zone_minutes: 60, hour12: false },
  timer: { countdown: true, running: false, done: false, set_s: 300, ms: 300000 },
  settings: { brightness: 8, night: false, flow: true, hour12: false, silent: false, blend: false, cc_lcd: false, cc_usb: false, transit: false, dpad4: false, low_batt: 0, player: 1, palette: 0, gbc: false },
  wifi: { phase: "online", ssid: "home", networks: [{ ssid: "home", rssi: -50, locked: true }, { ssid: "cafe", rssi: -70, locked: false }] },
  device: { name: "Chromatic 1a2b", mcu: "v0.13.4-korta7", fpga: "18.8", battery: 64, charging: false, ota: true },
  games: [{ slot: 0, name: "Snake", size: 2048 }],
};
const calls = [];
let paired = false;

const browser = await puppeteer.launch({ executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", headless: "new" });
const page = await browser.newPage();
await page.emulate({ viewport: { width: 390, height: 844, deviceScaleFactor: 2, isMobile: true, hasTouch: true },
  userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148" });
const errors = [];
page.on("pageerror", e => errors.push(e.message));
page.on("console", m => { if (m.type() === "error" && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
page.on("dialog", d => d.accept());
await page.setRequestInterception(true);
page.on("request", req => {
  const u = new URL(req.url());
  if (u.origin !== base || !u.pathname.startsWith("/api/")) return req.continue();
  const body = req.postData() ? JSON.parse(req.postData()) : null;
  calls.push({ method: req.method(), path: u.pathname, body });
  const reply = (status, data) => req.respond({ status, contentType: "application/json", body: JSON.stringify(data) });
  if (u.pathname === "/api/pair") {
    if (body.code !== "123456") return reply(403, { error: "Wrong or expired code. Check the Phone screen on your Chromatic." });
    paired = true;
    return reply(200, { token: "a".repeat(64) });
  }
  if (u.pathname === "/api/state") return reply(paired ? 200 : 401, { online: true, awake: true, seen: Date.now(), now: Date.now(), state });
  if (u.pathname === "/api/notes") { state.notes = { ...state.notes, rev: state.notes.rev + 1, items: body.items, pinned: body.pinned }; return reply(200, { ok: true }); }
  if (u.pathname === "/api/alarm") { state.alarms[body.index] = { on: body.on, hour: body.hour, minute: body.minute, days: body.days, label: body.label }; return reply(200, { ok: true }); }
  if (u.pathname === "/api/settings") { state.settings[body.key] = body.value; return reply(200, { ok: true }); }
  if (u.pathname === "/api/firmware") return reply(200, { version: "0.13.4-korta8", newer: true });
  if (u.pathname === "/api/update") { state.update = { phase: "download", percent: 40 }; return reply(200, { ok: true }); }
  if (u.pathname === "/api/unpair") { paired = false; return reply(200, { ok: true }); }
  if (u.pathname === "/api/openai") return reply(200, { signed_in: false });
  return reply(200, { ok: true });
});
const last = path => [...calls].reverse().find(c => c.path === path);
const visible = sel => page.$eval(sel, e => !e.hidden && !!e.offsetParent).catch(() => false);
const text = sel => page.$eval(sel, e => e.textContent).catch(() => "");

await page.goto(base, { waitUntil: "networkidle0" });
check(await visible("#pair"), "an unpaired browser sees the pairing screen");
await page.type("#code", "000000");
await settle(600);
check(/Wrong/.test(await text("#status")), "a wrong code says what refused");
await page.$eval("#code", e => (e.value = ""));
await page.type("#code", "123 456");
await settle(900);
check(await visible("#app") && last("/api/pair").body.code === "123456", "six digits (with a space) pair on their own");
check(await page.$eval("#who", e => e.dataset.state === "online" && /Connected/.test(e.textContent) && /64%/.test(e.textContent)), "the status shows the Chromatic connected, with its battery");
check(await page.$$eval(".dot", d => d.length) === 0, "no status dot");
check(await page.$$eval("#list .note", n => n.length) === 2 && /Pinned/.test(await text("#list")), "notes list, with a Pinned group");
check((await text("#noteCount")) === "2 of 16", "the header counts notes");

await page.click("#newNote");
await settle(400);
check(await page.$eval("#sheet", e => e.classList.contains("open")), "New note opens the editor");
await page.type("#text", "Café “list”");
check((await text("#count")) === "11 / 200", "the editor counts cleaned characters");
await page.click("#save");
await settle(600);
let n = last("/api/notes").body;
check(JSON.stringify(n.items) === JSON.stringify(["Pinned one", 'Cafe "list"', "Second"]) && n.pinned === 1, "a new note goes below the pinned one, cleaned");
check(/Note saved/.test(await text("#status")), "saving says so");
await page.click("#list .note:nth-of-type(2)");
await settle(400);
await page.click("#delete");
await settle(600);
check(JSON.stringify(last("/api/notes").body.items) === JSON.stringify(["Pinned one", "Second"]), "delete is immediate");
await page.click("#status .act");
await settle(800);
check(JSON.stringify(last("/api/notes").body.items) === JSON.stringify(["Pinned one", 'Cafe "list"', "Second"]), "Undo puts the note back in place");

await page.click('.seg .tab[data-tab="time"]');
await settle(400);
check(await page.$$eval("#alarms .alarm", r => r.length) === 4, "four alarms");
await page.click("#alarms .alarm:nth-child(2) > button");
await settle(300);
await settle(500);
check(await visible("#alarmEdit") && (await text("#alarmEditTitle")) === "Alarm 2", "tapping an alarm opens its editor");
check(await page.evaluate(() => document.getElementById("alarmSave").getBoundingClientRect().bottom <= document.querySelector(".seg").getBoundingClientRect().top),
  "the editor scrolls clear of the tab bar");
await page.$eval("#alarmAt", e => (e.value = "05:45"));
await page.type("#alarmLabel", "Run");
for (const c of await page.$$("#alarmDays .day")) if (await c.evaluate(e => e.getAttribute("aria-pressed") === "true")) await c.click();
await page.click("#alarmDays .day:nth-child(1)");
await page.click("#alarmDays .day:nth-child(3)");
await page.click("#alarmSave");
await settle(700);
const a = (last("/api/alarm") || { body: {} }).body;
check(a.index === 1 && a.hour === 5 && a.minute === 45 && a.days === 5 && a.label === "Run" && a.on, "the alarm is saved with time, label and days");
check(/Run/.test(await text("#alarms .alarm:nth-child(2)")) && !(await visible("#alarmEdit")), "the list shows it and the editor closes");
await page.click("#alarms .alarm:nth-child(3) .switch input");
await settle(500);
check(last("/api/alarm").body.index === 2 && last("/api/alarm").body.on === true, "an alarm's switch turns it on");
await page.click("#timerChips .chip:nth-child(1)");
await settle(400);
check(last("/api/timer") && last("/api/timer").body.seconds === 60, "a timer chip sets the length");

await page.click('.seg .tab[data-tab="settings"]');
await settle(400);
await page.click('[data-set="night"]');
await settle(400);
check(last("/api/settings").body.key === "night" && last("/api/settings").body.value === true, "a switch changes the setting");
await page.select("#setPalette", "4");
await settle(400);
check(last("/api/settings").body.key === "palette" && last("/api/settings").body.value === 4, "a select changes the setting");
await page.evaluate(() => say(""));  /* toasts from earlier steps float over the form */
await page.click("#wifiList .net:nth-of-type(2)");
await page.click("#wifiJoin");
await settle(400);
check(last("/api/wifi") && last("/api/wifi").body.ssid === "cafe", "tapping a network fills the form, Join sends it");

await page.click('.seg .tab[data-tab="device"]');
await settle(600);
check(/Chromatic 1a2b/.test(await text("#info")) && /korta7/.test(await text("#info")), "device facts");
check(await visible("#updateCard") && /Update available · korta8/.test(await text("#updateTitle")), "a newer firmware is offered");
if (shots) await (await page.$("#updateCard")).screenshot({ path: shots + "/mock-update-available.png" });
await page.click("#updateBtn");
await settle(4600);
check(last("/api/update") && /Updating 40%/.test(await text("#updateTitle")) && await visible("#updateMeter"), "Update starts and shows progress from the state");
if (shots) await (await page.$("#updateCard")).screenshot({ path: shots + "/mock-update-progress.png" });
await page.click("#findMe");
await settle(300);
check(last("/api/message") && last("/api/message").body.find === true, "Find my Chromatic");
check((await text("#notifyHint")).length > 0, "notifications explain themselves");

await page.click('.seg .tab[data-tab="games"]');
await settle(1200);
check(await page.$$eval("#gameList .row", r => r.length) === 1, "installed games are listed");
await page.click('#genMode [data-mode="key"]');
await settle(300);
check(await visible("#gameKey") && !(await visible('[data-gen="copy"]')), "the generator mode switches panels");
check(await page.$eval("#genMode .fill", e => e.style.transform.includes("translateX")), "one fill slides to the chosen mode");
if (shots) await page.screenshot({ path: shots + "/mock-games.png", fullPage: true });

check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), "nothing scrolls sideways");
await page.click('.seg .tab[data-tab="device"]');
await settle(300);
await page.click("#unpair");
await settle(800);
check(await visible("#pair"), "unpairing returns to the pairing screen");
check(errors.length === 0, "no script or CSP errors" + (errors.length ? ": " + errors.join("; ") : ""));
await browser.close();
server.close();
console.log(`\n${failures} failure(s)`);
process.exit(failures ? 1 : 0);
