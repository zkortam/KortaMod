// Games: install/delete through the relay, the device's download, and the
// "Sign in with ChatGPT" relay with OpenAI's endpoints simulated.
import { test, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import { createHash, generateKeyPairSync, sign } from "node:crypto";
import { useMemoryStore, keys } from "../api/_lib.js";
import { POST as sync } from "../api/device/sync.js";
import { POST as pair } from "../api/pair.js";
import { POST as postGame, DELETE as deleteGame, gameKey } from "../api/games.js";
import { GET as download } from "../api/device/game.js";
import * as oa from "../api/openai.js";

const ID = "0123456789abcdef", OTHER = "fedcba9876543210";
const SECRET = "a".repeat(64), OTHER_SECRET = "b".repeat(64);
let db;
beforeEach(() => { db = useMemoryStore(); process.env.SIWC_COOKIE_KEY = "test-key"; });

const req = (url, method, body, headers = {}) => new Request(url, {
  method, headers: { "content-type": "application/json", ...headers },
  body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
});
const sha = s => createHash("sha256").update(s).digest("hex");
const syncAs = (id, secret, body) => sync(req("http://x/api/device/sync", "POST", body, { "x-device-id": id, authorization: "Bearer " + secret }));
const fetchGame = (gid, id = ID, secret = SECRET) =>
  download(req("http://x/api/device/game?gid=" + gid, "GET", undefined, { "x-device-id": id, authorization: "Bearer " + secret }));

/* A Chromatic with a games partition: installs by downloading, like the firmware. */
function chromatic({ storage = true } = {}) {
  const dev = { code: "482913", games: [], results: [], running: false };
  dev.sync = async () => {
    const r = await syncAs(ID, SECRET, { code: dev.code, results: dev.results, state: { games: dev.games } });
    dev.results = [];
    const body = await r.json();
    for (const c of body.commands || []) {
      let result = 0;
      if (c.kind !== "game") result = 3;
      else if (!storage) result = 5;
      else if (dev.running) result = 2;
      else if (c.op === "install") {
        const res = await fetchGame(c.gid);
        const text = res.status === 200 ? await res.text() : null;
        const free = [...Array(32).keys()].find(s => !dev.games.some(g => g.slot === s));
        if (text === null || new TextEncoder().encode(text).length !== c.size || sha(text) !== c.sha256 || free === undefined) result = 3;
        else dev.games.push({ slot: free, name: c.name, size: c.size, source: text });
      } else if (c.op === "delete") {
        const before = dev.games.length;
        dev.games = dev.games.filter(g => g.slot !== c.slot);
        if (dev.games.length === before) result = 3;
      } else result = 3;
      dev.results.push({ id: c.id, result });
    }
    if (body.rotate_code) dev.code = String(Number(dev.code) + 1).padStart(6, "0");
    return body;
  };
  return dev;
}
async function withDevice(dev, promise) {
  let done = false;
  const loop = (async () => { while (!done) { await dev.sync(); await new Promise(r => setTimeout(r, 40)); } })();
  const result = await promise;
  done = true;
  await loop;
  return result;
}
async function pairBrowser(dev) {
  await dev.sync();
  const r = await pair(req("http://x/api/pair", "POST", { code: dev.code }, { "x-forwarded-for": "1.2.3.4" }));
  assert.equal(r.status, 200);
  const token = (await r.json()).token;
  await dev.sync();
  return token;
}
const asBrowser = (handler, method, body, token) => handler(req("http://x/api/games", method, body, token ? { authorization: "Bearer " + token } : {}));

const GAME = "let x = 0;\nfunction update() { x++; }\nfunction draw() { cls(1); text(`Héllo ${x}`, 4, 4, 7); }\n";

test("a game is stored briefly, downloaded by its device and installed", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  const r = await withDevice(dev, asBrowser(postGame, "POST", { name: " Snake ", source: GAME }, token));
  assert.equal(r.status, 200);
  assert.equal(dev.games.length, 1);
  assert.deepEqual([dev.games[0].name, dev.games[0].source], ["Snake", GAME]);
  assert.equal(dev.games[0].size, new TextEncoder().encode(GAME).length, "size is in UTF-8 bytes");
  const stored = [...db.m.keys()].find(k => k.startsWith("km:game:"));
  assert.ok(db.m.get(stored).exp - Date.now() <= 3600e3, "kept for an hour at most");
});

test("only the device a game was sent to can download it", async () => {
  const dev = chromatic();
  await pairBrowser(dev);
  await syncAs(OTHER, OTHER_SECRET, {});
  const gid = "00112233445566ff";
  await db.set(gameKey(gid), JSON.stringify({ id: ID, source: GAME }), { ex: 60 });
  assert.equal((await fetchGame(gid)).status, 200);
  assert.equal(await (await fetchGame(gid)).text(), GAME);
  assert.equal((await fetchGame(gid, ID, OTHER_SECRET)).status, 401, "wrong secret");
  assert.equal((await fetchGame(gid, OTHER, OTHER_SECRET)).status, 404, "another device");
  assert.equal((await fetchGame(gid, "nothex", SECRET)).status, 401);
  assert.equal((await fetchGame("123")).status, 400);
  assert.equal((await fetchGame("0011223344556600")).status, 404);
  assert.equal((await fetchGame(gid, "1111111111111111", SECRET)).status, 401, "unregistered device");
});

test("bad games are refused before reaching the device", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  assert.equal((await asBrowser(postGame, "POST", { name: "", source: GAME }, token)).status, 400);
  assert.equal((await asBrowser(postGame, "POST", { name: "x".repeat(13), source: GAME }, token)).status, 400);
  assert.equal((await asBrowser(postGame, "POST", { name: "Big", source: "é".repeat(8001) }, token)).status, 400, "over 16,000 bytes");
  assert.equal((await asBrowser(postGame, "POST", { name: "Empty", source: "  \n" }, token)).status, 400);
  assert.equal((await asBrowser(postGame, "POST", { name: "Snake", source: GAME })).status, 401);
  assert.equal((await asBrowser(deleteGame, "DELETE", { slot: 32 }, token)).status, 400);
  assert.equal((await asBrowser(deleteGame, "DELETE", { slot: "1" }, token)).status, 400);
});

test("device answers become clear errors; delete removes a game", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  assert.equal((await withDevice(dev, asBrowser(postGame, "POST", { name: "One", source: GAME }, token))).status, 200);
  dev.running = true;
  const busy = await withDevice(dev, asBrowser(postGame, "POST", { name: "Two", source: GAME }, token));
  assert.equal(busy.status, 423);
  assert.match((await busy.json()).error, /game is running/);
  dev.running = false;
  assert.equal((await withDevice(dev, asBrowser(deleteGame, "DELETE", { slot: 0 }, token))).status, 200);
  assert.equal(dev.games.length, 0);
  assert.equal((await withDevice(dev, asBrowser(deleteGame, "DELETE", { slot: 0 }, token))).status, 400, "nothing in that slot");
  for (let i = 0; i < 32; i++) assert.equal((await withDevice(dev, asBrowser(postGame, "POST", { name: "G" + i, source: GAME }, token))).status, 200);
  assert.equal((await withDevice(dev, asBrowser(postGame, "POST", { name: "Full", source: GAME }, token))).status, 400, "no free slot");
});

test("a Chromatic without the games partition says to run the installer", async () => {
  const dev = chromatic({ storage: false });
  const token = await pairBrowser(dev);
  const r = await withDevice(dev, asBrowser(postGame, "POST", { name: "Snake", source: GAME }, token));
  assert.equal(r.status, 409);
  assert.match((await r.json()).error, /installer/);
});

test("an offline device is reported, not waited on", async () => {
  const dev = chromatic();
  const token = await pairBrowser(dev);
  await db.hset(keys.dev(ID), { seen: Date.now() - 600000 });
  assert.equal((await asBrowser(postGame, "POST", { name: "Snake", source: GAME }, token)).status, 503);
});

/* ---- Sign in with ChatGPT relay ---- */

const realFetch = globalThis.fetch;
afterEach(() => { globalThis.fetch = realFetch; oa.resetCaches(); });

const { publicKey, privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
const JWK = { ...publicKey.export({ format: "jwk" }), kid: "k1", alg: "RS256", use: "sig" };
function idToken(claims) {
  const head = Buffer.from(JSON.stringify({ alg: "RS256", kid: "k1", typ: "JWT" })).toString("base64url");
  const body = Buffer.from(JSON.stringify(claims)).toString("base64url");
  return head + "." + body + "." + sign("sha256", Buffer.from(head + "." + body), privateKey).toString("base64url");
}
const SITE = "https://kortamod.app/api/openai";
const call = (op, method, body, cookie = "") => (method === "GET" ? oa.GET : oa.POST)(
  req(SITE + "?op=" + op, method, body, { ...(cookie ? { cookie } : {}), origin: "https://kortamod.app" }));
const setCookies = res => res.headers.getSetCookie();
const jar = res => setCookies(res).map(c => c.split(";")[0]).filter(c => !c.endsWith("=")).join("; ");

/* Simulated OpenAI: discovery, JWKS, token endpoint, models, streaming responses. */
function openai(opts = {}) {
  const calls = [];
  globalThis.fetch = async (url, init = {}) => {
    calls.push({ url: String(url), init });
    const u = String(url);
    const body = typeof init.body === "string" ? init.body : "";
    if (u === oa.OPENID) return Response.json({ jwks_uri: "https://auth.openai.com/jwks", revocation_endpoint: "https://auth.openai.com/revoke" });
    if (u === "https://auth.openai.com/jwks") return Response.json({ keys: [JWK] });
    if (u === "https://auth.openai.com/revoke") return new Response("", { status: 200 });
    if (u === oa.TOKEN) {
      const f = new URLSearchParams(body);
      if (f.get("grant_type") === "refresh_token") {
        if (opts.refreshDead) return Response.json({ error: "refresh_token_expired" }, { status: 400 });
        return Response.json({ access_token: "access-2", refresh_token: "refresh-2", expires_in: 3600, scope: oa.SCOPE });
      }
      assert.equal(f.get("redirect_uri"), oa.REDIRECT);
      assert.equal(f.get("resource"), "https://api.openai.com/v1");
      assert.ok(f.get("code_verifier"));
      if (f.get("code") !== "good-code") return Response.json({ error: "invalid_grant" }, { status: 400 });
      return Response.json({
        access_token: "access-1", refresh_token: "refresh-1", expires_in: opts.expiresIn ?? 3600,
        scope: opts.scope ?? oa.SCOPE, token_type: "Bearer",
        id_token: idToken({ iss: "https://auth.openai.com", aud: f.get("client_id"), sub: "user-1", email: "p@example.com",
          exp: Math.floor(Date.now() / 1000) + 3600, nonce: opts.nonce ?? opts.wantNonce }),
      });
    }
    if (u === "https://api.openai.com/v1/models") return Response.json({ models: [{ slug: "gpt-6.1-sol", display_name: "GPT-6.1 Sol", visibility: "list" }, { slug: "hidden", visibility: "hide" }] });
    if (u === "https://api.openai.com/v1/responses") {
      const sent = JSON.parse(body);
      assert.equal(sent.store, false);
      assert.equal(sent.stream, true);
      assert.equal(sent.temperature, undefined);
      if (opts.limit) {
        return new Response('data: {"type":"response.failed","response":{"error":{"code":"subscription_sharing_usage_limit_exceeded"}}}\n\n',
          { headers: { "content-type": "text/event-stream" } });
      }
      const events = ["```js\nfunction draw(){", "cls(1);}\n```"].map(d => `data: ${JSON.stringify({ type: "response.output_text.delta", delta: d })}\n\n`);
      events.push(`data: ${JSON.stringify({ type: "response.completed", response: {} })}\n\n`);
      const enc = new TextEncoder();
      return new Response(new ReadableStream({ start(c) { for (const e of events) c.enqueue(enc.encode(e)); c.close(); } }),
        { headers: { "content-type": "text/event-stream" } });
    }
    throw new Error("unexpected fetch " + u);
  };
  return calls;
}
async function signIn(opts = {}) {
  const start = await call("start", "POST", { host: "urn:uuid:12345678-1234-4234-8234-123456789abc" });
  assert.equal(start.status, 200);
  const url = new URL((await start.json()).url);
  assert.equal(url.origin + url.pathname, oa.AUTHORIZE);
  assert.equal(url.searchParams.get("client_id"), "dynamic_agent_client");
  assert.equal(url.searchParams.get("agent_name_hint"), "KortaMod");
  assert.equal(url.searchParams.get("redirect_uri"), "http://127.0.0.1:1455/auth/callback");
  assert.equal(url.searchParams.get("code_challenge_method"), "S256");
  assert.equal(url.searchParams.get("scope"), oa.SCOPE);
  const pending = jar(start);
  openai({ ...opts, wantNonce: url.searchParams.get("nonce") });
  const back = `http://127.0.0.1:1455/auth/callback?code=${opts.code || "good-code"}&scope=chatgpt.tokens.use.direct+email&state=${opts.state || url.searchParams.get("state")}&client_id=oaiapp_test123`;
  const fin = await call("finish", "POST", { url: back }, pending);
  return { fin, pending };
}

test("sign in with ChatGPT: pasted callback, verified ID token, encrypted HttpOnly cookie", async () => {
  const { fin } = await signIn();
  assert.equal(fin.status, 200);
  assert.deepEqual(await fin.json(), { email: "p@example.com", client_id: "oaiapp_test123" });
  const cookies = setCookies(fin);
  assert.ok(cookies.some(c => c.startsWith("km_oa0=") && /HttpOnly/.test(c) && /Secure/.test(c) && /SameSite=Strict/.test(c)));
  assert.ok(!cookies.join("").includes("access-1"), "tokens never appear in clear");
  const session = await call("session", "GET", undefined, jar(fin));
  assert.deepEqual(await session.json(), { signed_in: true, email: "p@example.com", client_id: "oaiapp_test123" });
  const models = await call("models", "GET", undefined, jar(fin));
  assert.deepEqual((await models.json()).models, [{ slug: "gpt-6.1-sol", name: "GPT-6.1 Sol" }]);
  const gen = await call("generate", "POST", { model: "gpt-6.1-sol", instructions: "spec", input: [{ role: "user", content: "snake" }] }, jar(fin));
  assert.equal(gen.status, 200);
  assert.equal((await gen.json()).text, "```js\nfunction draw(){cls(1);}\n```");
});

test("sign-in rejects a wrong state, a reused code, a wrong nonce and a missing plan scope", async () => {
  assert.equal((await signIn({ state: "forged" })).fin.status, 400);
  assert.equal((await signIn({ code: "used" })).fin.status, 400);
  assert.equal((await signIn({ nonce: "other" })).fin.status, 400);
  const { fin } = await signIn({ scope: "openid email profile offline_access" });
  assert.equal(fin.status, 403);
  assert.equal((await call("finish", "POST", { url: "http://127.0.0.1:1455/auth/callback?code=x&state=y" })).status, 400, "no pending sign-in");
});

test("expired access tokens refresh (rotating the cookie); a dead refresh token signs out", async () => {
  const { fin } = await signIn({ expiresIn: 30 });
  const cookie = jar(fin);
  openai();
  const gen = await call("generate", "POST", { model: "m", instructions: "", input: [{ role: "user", content: "x" }] }, cookie);
  assert.equal(gen.status, 200);
  assert.ok(setCookies(gen).some(c => c.startsWith("km_oa0=") && !c.startsWith("km_oa0=;")), "new session cookie");
  openai({ refreshDead: true });
  const dead = await call("generate", "POST", { model: "m", instructions: "", input: [{ role: "user", content: "x" }] }, cookie);
  assert.equal(dead.status, 401);
  assert.ok(setCookies(dead).some(c => c.startsWith("km_oa0=;") && /Max-Age=0/.test(c)));
});

test("plan errors map to plain messages; bad requests and other origins are refused", async () => {
  const { fin } = await signIn();
  const cookie = jar(fin);
  openai({ limit: true });
  const limited = await call("generate", "POST", { model: "m", instructions: "", input: [{ role: "user", content: "x" }] }, cookie);
  assert.equal(limited.status, 429);
  assert.match((await limited.json()).error, /usage limit/);
  assert.equal((await call("generate", "POST", { model: "m", input: [] }, cookie)).status, 400);
  assert.equal((await call("generate", "POST", { model: "m", input: [{ role: "system", content: "x" }] }, cookie)).status, 400);
  assert.equal((await call("generate", "POST", { model: "m", input: [{ role: "user", content: "x" }] })).status, 401, "no session");
  const cross = await oa.POST(req(SITE + "?op=start", "POST", { host: "urn:uuid:12345678-1234-4234-8234-123456789abc" }, { origin: "https://evil.example" }));
  assert.equal(cross.status, 403);
  assert.equal((await call("start", "POST", { host: "short" })).status, 400);
  assert.equal((await call("start", "POST", { host: "a".repeat(32) })).status, 400, "bare hex is what OpenAI rejects");
  openai();
  const out = await call("signout", "POST", {}, cookie);
  assert.deepEqual(await out.json(), { ok: true, revoked: true });
});

test("the pasted address can be the full URL or just its query", () => {
  assert.deepEqual(oa.parseCallback("http://127.0.0.1:1455/auth/callback?code=c&state=s&client_id=oaiapp_1&scope=a+b"),
    { code: "c", state: "s", client_id: "oaiapp_1", scope: "a b", error: "" });
  assert.equal(oa.parseCallback("?code=c&state=s").code, "c");
  assert.equal(oa.parseCallback("error=access_denied&state=s").error, "access_denied");
  assert.equal(oa.open(oa.seal({ a: 1 }) + "x"), null, "tampered cookie rejected");
});
