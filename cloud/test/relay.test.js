// Relay behaviour against an in-memory store, with a simulated Chromatic.
import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";
import { useMemoryStore, keys } from "../api/_lib.js";
import { POST as sync } from "../api/device/sync.js";
import { POST as pair } from "../api/pair.js";
import { POST as unpair } from "../api/unpair.js";
import { GET as getState } from "../api/state.js";
import {
  notes as putNotes, alarm as putAlarm, clock as putClock, settings as putSetting,
  message as postMessage, timer as postTimer, wifi as postWifi,
} from "../api/_commands.js";
import { PUT as routePut, POST as routePost } from "../api/command.js";
import { GET as getPush, POST as postPush, DELETE as deletePush } from "../api/push.js";
import { setSender, events } from "../api/_push.js";

const ID = "0123456789abcdef";
const SECRET = "a".repeat(64);
let db;
beforeEach(() => { db = useMemoryStore(); });

const req = (method, body, headers = {}) => new Request("http://x/api", {
  method, headers: { "content-type": "application/json", ...headers },
  body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
});
const device = (body, id = ID, secret = SECRET) => sync(req("POST", body, { "x-device-id": id, authorization: "Bearer " + secret }));
const browser = (handler, method, body, token, ip = "1.2.3.4") =>
  handler(req(method, body, { ...(token ? { authorization: "Bearer " + token } : {}), "x-forwarded-for": ip }));

/* A Chromatic: syncs, applies commands like the firmware, reports results. */
function chromatic() {
  const dev = { notes: { rev: 1, items: ["THIS IS COOL"], editing: false }, alarm: { on: false, hour: 7, minute: 0 }, code: "482913", results: [] };
  dev.sync = async (extra = {}) => {
    const r = await device({ state: { notes: dev.notes, alarm: dev.alarm }, code: dev.code, results: dev.results, ...extra });
    dev.results = [];
    const body = await r.json();
    if (r.status !== 200) return { status: r.status, body };
    for (const c of body.commands) {
      let result = 0;
      if (body.now > c.exp) result = 4;
      else if (c.kind === "notes") {
        if (dev.notes.editing) result = 2;
        else if (c.rev !== dev.notes.rev) result = 1;
        else dev.notes = { rev: dev.notes.rev + 1, items: c.items, pinned: c.pinned, editing: false };
      } else if (c.kind === "alarm" && c.index === 0) dev.alarm = { on: c.on, hour: c.hour, minute: c.minute };
      else if (c.kind === "message" && dev.ringing) result = 2;
      else dev.last = c;
      dev.results.push({ id: c.id, result });
    }
    if (body.rotate_code) dev.code = String(Number(dev.code) + 1).padStart(6, "0");
    return { status: 200, body };
  };
  return dev;
}
/* Runs the device loop while a browser request is waiting on it. */
async function withDevice(dev, promise) {
  let done = false;
  const loop = (async () => { while (!done) { await dev.sync(); await new Promise(r => setTimeout(r, 50)); } })();
  const result = await promise;
  done = true;
  await loop;
  return result;
}
async function pairBrowser(dev) {
  await dev.sync();
  const r = await browser(pair, "POST", { code: dev.code });
  assert.equal(r.status, 200);
  return (await r.json()).token;
}

test("a device registers on first sync and must keep its secret", async () => {
  assert.equal((await device({})).status, 200);
  assert.equal((await device({}, ID, "b".repeat(64))).status, 401);
  assert.equal((await device({}, "nothex", SECRET)).status, 401);
  assert.equal((await device("not json")).status, 400);
});

test("pairing: right code once, then the device shows a new code", async () => {
  const dev = chromatic();
  await dev.sync();
  assert.equal((await browser(pair, "POST", { code: "000000" })).status, 403);
  const r = await browser(pair, "POST", { code: "482 913" });
  assert.equal(r.status, 200);
  assert.match((await r.json()).token, /^[0-9a-f]{64}$/);
  assert.equal((await browser(pair, "POST", { code: "482913" })).status, 403, "a code works once");
  const s = await dev.sync();
  assert.equal(s.body.rotate_code, true);
  assert.equal(s.body.poll_ms, 2000, "pairing switches the device to fast sync");
  assert.equal(s.body.paired, 1);
  await dev.sync();
  assert.equal((await browser(pair, "POST", { code: dev.code })).status, 200, "the new code works");
});

test("pairing is rate limited per address", async () => {
  const dev = chromatic();
  await dev.sync();
  for (let i = 0; i < 20; i++) await browser(pair, "POST", { code: "000000" }, null, "9.9.9.9");
  assert.equal((await browser(pair, "POST", { code: dev.code }, null, "9.9.9.9")).status, 429);
  assert.equal((await browser(pair, "POST", { code: dev.code }, null, "8.8.8.8")).status, 200);
});

test("a code held by another device makes the newcomer rotate", async () => {
  await device({ code: "111111" }, "1111111111111111", "c".repeat(64));
  const r = await (await device({ code: "111111" })).json();
  assert.equal(r.rotate_code, true);
});

test("state needs a token and reports online and the device's data", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  assert.equal((await browser(getState, "GET", undefined, "f".repeat(64))).status, 401);
  const r = await browser(getState, "GET", undefined, token);
  const s = await r.json();
  assert.equal(r.status, 200);
  assert.equal(s.online, true);
  assert.deepEqual(s.state.notes.items, ["THIS IS COOL"]);
  assert.equal((await dev.sync()).body.poll_ms, 2000, "a watching browser speeds the device up");
});

test("notes: applied at the current revision, refused when stale or being edited", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  let r = await withDevice(dev, browser(putNotes, "PUT", { rev: 1, items: ["A", "B"] }, token));
  assert.equal(r.status, 200);
  assert.deepEqual(dev.notes.items, ["A", "B"]);
  r = await withDevice(dev, browser(putNotes, "PUT", { rev: 1, items: ["C"] }, token));
  assert.equal(r.status, 409);
  dev.notes.editing = true;
  r = await withDevice(dev, browser(putNotes, "PUT", { rev: dev.notes.rev, items: ["C"] }, token));
  assert.equal(r.status, 423);
  assert.equal((await browser(putNotes, "PUT", { rev: 1, items: Array(17).fill("x") }, token)).status, 400);
  assert.equal((await browser(putNotes, "PUT", { rev: 1, items: ["a"], pinned: 2 }, token)).status, 400, "more pinned than notes");
  assert.equal((await browser(putNotes, "PUT", { rev: 1, items: [1] }, token)).status, 400);
  assert.equal((await browser(putNotes, "PUT", "{bad", token)).status, 400);
});

test("alarm and clock reach the device; bad values are refused", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  assert.equal((await browser(putAlarm, "PUT", { on: true, hour: 24, minute: 0 }, token)).status, 400);
  const r = await withDevice(dev, browser(putAlarm, "PUT", { on: true, hour: 6, minute: 30 }, token));
  assert.equal(r.status, 200);
  assert.deepEqual(dev.alarm, { on: true, hour: 6, minute: 30 });
  assert.equal((await browser(putAlarm, "PUT", { index: 4, on: true, hour: 6, minute: 0 }, token)).status, 400);
  assert.equal((await browser(putAlarm, "PUT", { index: 1, on: true, hour: 6, minute: 0, days: 128 }, token)).status, 400);
  assert.equal((await withDevice(dev, browser(putAlarm, "PUT", { index: 2, on: true, hour: 9, minute: 5, days: 31, label: "Gym" }, token))).status, 200);
  assert.deepEqual([dev.last.index, dev.last.days, dev.last.label], [2, 31, "Gym"]);
  assert.equal((await browser(putClock, "PUT", { zone_minutes: 9999 }, token)).status, 400);
  assert.equal((await withDevice(dev, browser(putClock, "PUT", { zone_minutes: -420 }, token))).status, 200);
});

test("an offline device is reported, not waited on", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  await db.hset(keys.dev(ID), { seen: Date.now() - 60000 });
  assert.equal((await (await browser(getState, "GET", undefined, token)).json()).online, false);
  assert.equal((await browser(putAlarm, "PUT", { on: true, hour: 6, minute: 0 }, token)).status, 503);
});

test("a command the device never answers times out with a clear error", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  const { command, replyFor } = await import("../api/_lib.js");
  const r = replyFor(await command(ID, {}, "alarm", { on: true, hour: 1, minute: 1 }, 400));
  assert.equal(r.status, 504);
});

test("forget on the device and unpair in a browser revoke tokens", async () => {
  const dev = chromatic();
  const a = await pairBrowser(dev);
  await dev.sync();
  const b = await pairBrowser(dev);
  assert.equal((await browser(unpair, "POST", undefined, a)).status, 200);
  assert.equal((await browser(getState, "GET", undefined, a)).status, 401);
  assert.equal((await browser(getState, "GET", undefined, b)).status, 200);
  await dev.sync({ forget: true });
  assert.equal((await browser(getState, "GET", undefined, b)).status, 401);
  assert.equal((await dev.sync()).body.paired, 0);
});

test("using a browser renews its pairing", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  const { sha256 } = await import("../api/_lib.js");
  await db.expire(keys.token(sha256(token)), 5);
  await browser(getState, "GET", undefined, token);
  assert.ok(db.m.get(keys.token(sha256(token))).exp - Date.now() < 10000, "polling alone doesn't write");
  await getState(new Request("http://x/api/state?load=1", { headers: { authorization: "Bearer " + token } }));
  const entry = db.m.get(keys.token(sha256(token)));
  assert.ok(entry.exp - Date.now() > 100 * 24 * 3600 * 1000, "renewed to the full term");
});

test("at most 8 browsers per device", async () => {
  const dev = chromatic();
  for (let i = 0; i < 8; i++) { await dev.sync(); await pairBrowser(dev); }
  await dev.sync(); await dev.sync();
  assert.equal((await browser(pair, "POST", { code: dev.code }, null, "5.5.5.5")).status, 409);
});

test("remote control: settings, messages, timer and Wi-Fi reach the device as validated commands", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  const run = (h, m, b) => withDevice(dev, browser(h, m, b, token));
  assert.equal((await run(putSetting, "PUT", { key: "brightness", value: 12 })).status, 200);
  assert.deepEqual([dev.last.kind, dev.last.key, dev.last.value], ["setting", "brightness", 12]);
  assert.equal((await browser(putSetting, "PUT", { key: "brightness", value: 17 }, token)).status, 400);
  assert.equal((await browser(putSetting, "PUT", { key: "night", value: 1 }, token)).status, 400, "booleans must be booleans");
  assert.equal((await browser(putSetting, "PUT", { key: "rm -rf", value: true }, token)).status, 400);
  assert.equal((await run(putSetting, "PUT", { key: "night", value: true })).status, 200);
  assert.equal((await run(postMessage, "POST", { text: "Dinner at 7" })).status, 200);
  assert.deepEqual([dev.last.kind, dev.last.text, dev.last.find], ["message", "Dinner at 7", false]);
  assert.equal((await run(postMessage, "POST", { text: "", find: true })).status, 200, "find needs no text");
  assert.equal((await browser(postMessage, "POST", { text: "   " }, token)).status, 400);
  dev.ringing = true;
  assert.equal((await run(postMessage, "POST", { text: "hi" })).status, 423);
  dev.ringing = false;
  assert.equal((await run(postTimer, "POST", { action: "set", countdown: true, seconds: 300 })).status, 200);
  assert.equal(dev.last.seconds, 300);
  assert.equal((await browser(postTimer, "POST", { action: "set", countdown: true, seconds: 3 }, token)).status, 400);
  assert.equal((await browser(postTimer, "POST", { action: "explode" }, token)).status, 400);
  assert.equal((await run(postTimer, "POST", { action: "start" })).status, 200);
  assert.equal((await run(postWifi, "POST", { ssid: "Home", password: "correct horse" })).status, 200);
  assert.deepEqual([dev.last.ssid, dev.last.password], ["Home", "correct horse"]);
  assert.equal((await browser(postWifi, "POST", { ssid: "Home", password: "short" }, token)).status, 400);
  assert.equal((await browser(postWifi, "POST", { ssid: "x".repeat(33) }, token)).status, 400);
});

test("the device is told how often to check in", async () => {
  const dev = chromatic();
  assert.equal((await dev.sync()).body.poll_ms, 240000, "nobody paired: rarely");
  const token = await pairBrowser(dev);
  assert.equal((await dev.sync()).body.poll_ms, 2000, "just paired: fast");
  await db.hset(keys.dev(ID), { active_until: Date.now() - 1 });
  assert.equal((await dev.sync()).body.poll_ms, 20000, "paired, nobody watching");
  await browser(getState, "GET", undefined, token);
  assert.equal((await dev.sync()).body.poll_ms, 2000, "a browser opened the page");
});

test("a command reaches a device that is checking in slowly", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  await dev.sync();
  await db.hset(keys.dev(ID), { active_until: Date.now() - 1, fast: 0 });
  const s = await (await browser(getState, "GET", undefined, token)).json();
  assert.equal(s.awake, false);
  /* The device checks in only after 1.5 s, past the fast-path wait. */
  const pending = browser(putAlarm, "PUT", { on: true, hour: 5, minute: 5 }, token);
  await new Promise(r => setTimeout(r, 1500));
  const r = await withDevice(dev, pending);
  assert.equal(r.status, 200);
  assert.deepEqual(dev.alarm, { on: true, hour: 5, minute: 5 });
});

test("a command past its deadline is refused by the device, not applied late", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  await db.rpush(keys.queue(ID), JSON.stringify({ id: "00000000000000aa", kind: "alarm", t: Date.now() - 40000, exp: Date.now() - 1, on: true, hour: 3, minute: 3 }));
  await db.hset(keys.dev(ID), { pending: 1 });
  await dev.sync();
  assert.deepEqual(dev.alarm, { on: false, hour: 7, minute: 0 });
  assert.equal(await db.get(keys.result("00000000000000aa")), null, "result reported on the next sync");
  await dev.sync();
  assert.equal(await db.get(keys.result("00000000000000aa")), "4");
  assert.ok(token);
});

test("database cost: an idle check-in is 2 commands, a watched page 3 at most", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  await dev.sync(); await dev.sync();
  let before = db.calls;
  await dev.sync();
  assert.equal(db.calls - before, 2, "routine check-in: read and write the device record");
  await browser(getState, "GET", undefined, token);
  before = db.calls;
  await browser(getState, "GET", undefined, token);
  assert.equal(db.calls - before, 2, "page refresh: token and device record (activity already fresh)");
  before = db.calls;
  await browser(getState, "GET", undefined, token, "1.2.3.4");
  assert.ok(db.calls - before <= 3);
});

test("the paired count recovers from tokens that lapsed", async () => {
  const dev = chromatic();
  const tokens = [];
  for (let i = 0; i < 8; i++) { await dev.sync(); tokens.push(await pairBrowser(dev)); }
  const { sha256 } = await import("../api/_lib.js");
  for (const t of tokens.slice(0, 3)) await db.del(keys.token(sha256(t)));
  await dev.sync(); await dev.sync();
  assert.equal((await browser(pair, "POST", { code: dev.code }, null, "6.6.6.6")).status, 200);
  assert.equal((await dev.sync()).body.paired, 6);
});

test("forget clears every pairing and the device goes back to rare check-ins", async () => {
  const dev = chromatic();
  await pairBrowser(dev);
  await db.hset(keys.dev(ID), { active_until: Date.now() - 1 });
  await dev.sync({ forget: true });
  assert.equal((await dev.sync()).body.poll_ms, 240000);
});

test("a device registered under the old layout keeps its secret and browsers", async () => {
  const { sha256 } = await import("../api/_lib.js");
  await db.set(`km:dev:${ID}:secret`, sha256(SECRET));
  await db.sadd(`km:dev:${ID}:tokens`, sha256("t".repeat(64)), sha256("u".repeat(64)));
  await db.set(keys.token(sha256("t".repeat(64))), ID);
  assert.equal((await device({}, ID, "b".repeat(64))).status, 401, "an impostor can't take over the id");
  const r = await (await device({})).json();
  assert.equal(r.paired, 1, "only the live browser is carried over");
  assert.deepEqual(await db.smembers(keys.tokens(ID)), [sha256("t".repeat(64))]);
  assert.equal((await device({})).status, 200);
});

test("hash replies from the database client are read as records", async () => {
  const { hashReply } = await import("../api/_lib.js");
  assert.deepEqual(hashReply(["seen", "5", "code", "123456"]), { seen: "5", code: "123456" });
  assert.equal(hashReply([]), null);
  assert.equal(hashReply(null), null);
  assert.deepEqual(hashReply({ seen: "5" }), { seen: "5" });
});

test("notifications: alarm, timer and low battery reach subscribed browsers once", async () => {
  const sent = [];
  setSender(async (sub, body) => {
    if (sub.endpoint.endsWith("/gone")) throw Object.assign(new Error("gone"), { statusCode: 410 });
    sent.push([sub.endpoint, JSON.parse(body)]);
  });
  const dev = chromatic();
  const a = await pairBrowser(dev);
  await dev.sync();
  const b = await pairBrowser(dev);
  const sub = endpoint => ({ subscription: { endpoint, keys: { p256dh: "p".repeat(87), auth: "a".repeat(22) } } });
  assert.equal((await browser(postPush, "POST", { subscription: { endpoint: "http://insecure" } }, a)).status, 400);
  assert.equal((await browser(postPush, "POST", sub("https://push.example/a"), a)).status, 200);
  assert.equal((await browser(postPush, "POST", sub("https://push.example/gone"), b)).status, 200);
  const report = extra => device({ code: dev.code, state: { notes: dev.notes, ...extra } });
  await report({ device: { battery: 50, charging: false } });
  assert.equal(sent.length, 0);
  await report({ device: { battery: 50, charging: false }, alert: { kind: "alarm", label: "Wake up" },
    clock: { epoch: 1792300000, zone_minutes: -420 } });
  await report({ device: { battery: 50, charging: false }, alert: { kind: "alarm", label: "Wake up" } });
  assert.equal(sent.length, 1, "one notification per ring, to the live subscription only");
  assert.equal(sent[0][1].title, "Wake up");
  assert.equal(await db.hgetall(keys.push(ID)).then(h => Object.keys(h).length), 1, "the dead subscription was dropped");
  await report({ device: { battery: 12, charging: false } });
  await report({ device: { battery: 11, charging: false } });
  assert.deepEqual(sent.slice(1).map(s => s[1].title), ["Battery low"]);
  await report({ device: { battery: 11, charging: false }, alert: { kind: "timer" } });
  assert.equal(sent.at(-1)[1].title, "Timer done");
  assert.equal((await browser(deletePush, "DELETE", undefined, a)).status, 200);
  await report({ device: { battery: 11, charging: false } });
  await report({ device: { battery: 11, charging: false }, alert: { kind: "timer" } });
  assert.equal(sent.length, 3, "unsubscribed");
  assert.equal((await getPush(new Request("http://x/api/push"))).status, 503, "no key configured in tests");
});

test("notification events ignore what didn't change", () => {
  assert.deepEqual(events(null, null), []);
  assert.deepEqual(events({ device: { battery: 10, charging: true } }, { device: { battery: 10, charging: false } }).map(e => e.tag), ["battery"]);
  assert.deepEqual(events({ device: { battery: 10 } }, { device: { battery: 9 } }), []);
});

test("command URLs route to their handlers and check the method", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  const call = (h, method, path, body) => h(new Request("http://x" + path, {
    method, headers: { "content-type": "application/json", authorization: "Bearer " + token }, body: JSON.stringify(body) }));
  assert.equal((await call(routePut, "PUT", "/api/alarm", { index: 0, on: true, hour: 25, minute: 0 })).status, 400, "reached the alarm validator");
  assert.equal((await call(routePost, "POST", "/api/alarm", { index: 0 })).status, 405);
  assert.equal((await call(routePost, "POST", "/api/nope", {})).status, 404);
  assert.equal((await call(routePost, "POST", "/api/command?kind=timer", { action: "explode" })).status, 400, "rewritten form");
});
