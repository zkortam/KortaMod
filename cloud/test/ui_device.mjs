// Drives the deployed site, paired with a real Chromatic, in a phone-sized browser.
//   node ui_device.mjs https://kortamod.app 123456 [screenshot-dir]
// Needs puppeteer-core and Google Chrome. Restores the alarm it changes and
// deletes the note it creates.
import puppeteer from "puppeteer-core";

const [base, code, shots] = process.argv.slice(2);
const CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
let failures = 0;
const check = (ok, what) => { console.log((ok ? "  ok   " : "  FAIL ") + what); if (!ok) failures++; };

const browser = await puppeteer.launch({ executablePath: CHROME, headless: true });
const page = await browser.newPage();
await page.emulate({
  viewport: { width: 375, height: 740, deviceScaleFactor: 2, isMobile: true, hasTouch: true },
  userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148",
});
page.on("dialog", d => d.accept());
const errors = [];
page.on("pageerror", e => errors.push(e.message));
const shot = async name => { if (shots) await page.screenshot({ path: `${shots}/${name}.png` }); };
const fits = async () => page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth);
const visible = sel => page.$eval(sel, el => !el.hidden && el.offsetParent !== null).catch(() => false);
const status = () => page.$eval("#status", el => el.textContent);
const api = (method, path, body) => page.evaluate(async (method, path, body) => {
  const r = await fetch(path, { method, headers: { Authorization: "Bearer " + localStorage.getItem("kortamod.token"),
    "Content-Type": "application/json" }, body: body ? JSON.stringify(body) : undefined });
  return { status: r.status, body: await r.json().catch(() => ({})) };
}, method, path, body);
const settle = (ms = 700) => new Promise(r => setTimeout(r, ms));

await page.goto(base, { waitUntil: "networkidle0" });
check(await visible("#pair"), "a new browser is asked to pair");
check(await fits(), "pair screen fits a phone width");
await shot("1-pair");
await page.type("#code", "000 000" === code ? "111 111" : "000 000");
await page.click("#pairBtn");
await settle();
check(/Wrong/.test(await status()), "a wrong code says so");
await page.$eval("#code", el => (el.value = ""));
await settle(400);
await page.type("#code", code.slice(0, 3) + " " + code.slice(3));  /* six digits submit on their own */
await settle(2500);
check(await visible("#app"), "the right code (typed with a space) pairs");
check(await page.$eval("#who", el => el.dataset.state === "online" || el.dataset.state === "waking"), "the page shows the Chromatic online");
const before = await api("GET", "/api/state");
const count = await page.$$eval("#list .note", n => n.length);
check(count === before.body.state.notes.items.length, `notes list shows the device's ${count} note(s)`);
check(await fits(), "notes screen fits a phone width");
check(await page.$$eval(".seg button", bs => bs.every(b => b.scrollWidth <= b.clientWidth)), "no tab label is cut off");
await shot("2-notes");

const savedAlarms = before.body.state.alarms;
try {
  await page.click("#newNote");
  await settle(500);
  await page.type("#text", "UI test “note” — café");
  check(/\d+ \/ 200/.test(await page.$eval("#count", el => el.textContent)), "the editor counts characters");
  check(await fits(), "editor fits a phone width");
  await shot("3-edit");
  await page.click("#save");
  await settle(4000);
  const after = await api("GET", "/api/state");
  check(after.body.state.notes.items[0] === 'UI test "note" - cafe', "a new note reaches the Chromatic, cleaned: " + after.body.state.notes.items[0]);
  check(/saved/i.test(await status()), "saving says so");
  const firstCard = await page.$eval("#list .note .t", el => el.textContent);
  check(firstCard === 'UI test "note" - cafe', "the list shows it at the top");
  await page.click("#list .note");
  await settle(500);
  await page.click("#delete");
  await settle(4000);
  const gone = await api("GET", "/api/state");
  check(JSON.stringify(gone.body.state.notes.items) === JSON.stringify(before.body.state.notes.items), "deleting it restores the original list");

  await page.click('.seg button[data-tab="time"]');
  await settle(300);
  check(await fits(), "alarm & clock screen fits a phone width");
  check(await page.$$eval("#alarms .alarm", r => r.length) === 4, "four alarms are listed");
  await page.click("#alarms .alarm:nth-child(2) > button");
  await settle(300);
  check(await visible("#alarmEdit"), "tapping an alarm opens its editor");
  await page.$eval("#alarmAt", el => (el.value = "05:45"));
  await page.$eval("#alarmLabel", el => (el.value = ""));
  await page.type("#alarmLabel", "Run");
  for (const c of await page.$$("#alarmDays .chip")) if (await c.evaluate(el => el.getAttribute("aria-pressed") === "true")) await c.click();
  await page.click("#alarmDays .chip:nth-child(1)");
  await page.click("#alarmDays .chip:nth-child(3)");
  await page.click("#alarmSave");
  /* A Chromatic that isn't checking in fast yet can take up to ~20 s. */
  let said = "";
  for (let t = 0; t < 60 && !/Alarm saved|\./.test(said); t++) { await settle(500); said = await status(); }
  const alarm = (await api("GET", "/api/state")).body.state.alarms[1];
  check(alarm.on === true && alarm.hour === 5 && alarm.minute === 45 && alarm.days === 5 && alarm.label === "Run",
    "alarm 2 reaches the Chromatic with label and days: " + JSON.stringify(alarm) + " (" + said + ")");
  check(/Run/.test(await page.$eval("#alarms .alarm:nth-child(2)", el => el.textContent)), "the list shows its label");
  check(await fits(), "alarms fit a phone width");
  await shot("4-time");
  await page.click('.seg button[data-tab="device"]');
  await settle(300);
  const info = await page.$eval("#info", el => el.textContent);
  check(/Chromatic/.test(info) && /korta/.test(info), "device screen shows name and firmware");
  check(await page.$eval("#notifyHint", el => el.textContent.length > 0), "notifications explain themselves");
  check(await fits(), "device screen fits a phone width");
  await shot("5-device");
} finally {
  for (const [index, a] of savedAlarms.entries()) await api("PUT", "/api/alarm", { ...a, index });
  const restored = (await api("GET", "/api/state")).body.state.alarms;
  check(JSON.stringify(restored) === JSON.stringify(savedAlarms), "your alarms are restored");
}
await page.click("#unpair");
await settle(800);
check(await visible("#pair"), "unpairing returns to the pair screen");
check(errors.length === 0, "no script errors" + (errors.length ? ": " + errors.join("; ") : ""));
await browser.close();
console.log(`\n${failures} failure(s)`);
process.exit(failures ? 1 : 0);
