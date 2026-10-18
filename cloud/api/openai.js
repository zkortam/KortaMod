// "Sign in with ChatGPT" for making games: the person's own ChatGPT plan pays for
// generation (OpenAI's open-source dynamic-client flow, a preview).
//
// The flow's redirect must be http://127.0.0.1:<port>/auth/callback, which a
// website can't receive, so the person pastes that address back after approving.
// OpenAI asks that tokens stay out of browser storage, so they live only in an
// encrypted, HttpOnly cookie that this function decrypts per request. Nothing is
// stored on the server.
//
//   POST ?op=start    {host, client_id?, login_hint?} -> {url}
//   POST ?op=finish   {url}                            -> {email, client_id}
//   GET  ?op=session                                   -> {signed_in, email, client_id}
//   GET  ?op=models                                    -> {models: [{slug, name}]}
//   POST ?op=generate {model, instructions, input}     -> {text}
//   POST ?op=signout                                   -> {ok, revoked}
import { createCipheriv, createDecipheriv, createHash, createPublicKey, randomBytes, verify } from "node:crypto";
import { readJson, json, fail, guarded } from "./_lib.js";

export const AUTHORIZE = "https://auth.openai.com/api/accounts/authorize";
export const TOKEN = "https://auth.openai.com/api/accounts/oauth/token";
export const ISSUER = "https://auth.openai.com";
export const OPENID = "https://auth.openai.com/.well-known/openid-configuration";
export const API = "https://api.openai.com/v1";
export const REDIRECT = "http://127.0.0.1:1455/auth/callback";
export const SCOPE = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
const PLAN_SCOPE = "chatgpt.tokens.use.direct";
const SESSION = "km_oa";          // split across km_oa0.. if long
const PENDING = "km_oap";
const PATH = "/api/openai";
const CHUNK = 3500;
const PENDING_S = 900;
const SESSION_S = 30 * 24 * 3600;  // refresh tokens last 30 days
/* OpenAI accepts the host id as a URN UUID (what its docs show); a bare hex
 * string is rejected with invalid_authorize_request on ext_agent_host_id. */
const HOST_RE = /^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const CLIENT_RE = /^[A-Za-z0-9_.-]{4,128}$/;

/* ---- encrypted cookies ---- */

function cookieKey() {
  const secret = process.env.SIWC_COOKIE_KEY || process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
  if (!secret) throw new Error("No cookie key configured");
  return createHash("sha256").update("kortamod-siwc-cookie|" + secret).digest();
}
export function seal(value) {
  const iv = randomBytes(12);
  const c = createCipheriv("aes-256-gcm", cookieKey(), iv);
  const body = Buffer.concat([c.update(JSON.stringify(value), "utf8"), c.final()]);
  return Buffer.concat([iv, c.getAuthTag(), body]).toString("base64url");
}
export function open(text) {
  try {
    const raw = Buffer.from(text, "base64url");
    if (raw.length < 29) return null;
    const d = createDecipheriv("aes-256-gcm", cookieKey(), raw.subarray(0, 12));
    d.setAuthTag(raw.subarray(12, 28));
    return JSON.parse(Buffer.concat([d.update(raw.subarray(28)), d.final()]).toString("utf8"));
  } catch { return null; }
}
function cookies(request) {
  const out = {};
  for (const part of (request.headers.get("cookie") || "").split(";")) {
    const i = part.indexOf("=");
    if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
  }
  return out;
}
const cookie = (name, value, maxAge) =>
  `${name}=${value}; Path=${PATH}; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Strict`;

function readSession(request) {
  const jar = cookies(request);
  let text = "";
  for (let i = 0; i < 4 && jar[SESSION + i]; i++) text += jar[SESSION + i];
  return text ? open(text) : null;
}
/* Set-Cookie headers for a session (or its removal), cleared chunks included. */
function sessionCookies(session) {
  const text = session ? seal(session) : "";
  const out = [];
  for (let i = 0; i < 4; i++) {
    const part = text.slice(i * CHUNK, (i + 1) * CHUNK);
    out.push(part ? cookie(SESSION + i, part, SESSION_S) : cookie(SESSION + i, "", 0));
  }
  if (text.length > 4 * CHUNK) throw new Error("Session too large for cookies");
  return out;
}
function reply(status, body, setCookies = []) {
  const res = json(status, body);
  for (const c of setCookies) res.headers.append("Set-Cookie", c);
  return res;
}

/* ---- OAuth helpers ---- */

const b64url = buf => Buffer.from(buf).toString("base64url");
const random = () => b64url(randomBytes(32));

async function form(url, fields) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded", Accept: "application/json" },
    body: new URLSearchParams(fields).toString(),
  });
  let data = {};
  try { data = await res.json(); } catch { data = {}; }
  return { status: res.status, data };
}

let openid = null;
async function discovery() {
  if (openid) return openid;
  const res = await fetch(OPENID);
  if (!res.ok) throw new Error("OpenID configuration unavailable");
  openid = await res.json();
  return openid;
}
let jwks = null, jwksAt = 0;
async function signingKey(kid, retry = true) {
  if (!jwks || Date.now() - jwksAt > 3600e3) {
    const res = await fetch((await discovery()).jwks_uri);
    if (!res.ok) throw new Error("OpenAI signing keys unavailable");
    jwks = (await res.json()).keys || [];
    jwksAt = Date.now();
  }
  const jwk = jwks.find(k => k.kid === kid);
  if (!jwk && retry) { jwks = null; return signingKey(kid, false); }
  return jwk || null;
}
/* Test hook: reset caches. */
export function resetCaches() { openid = null; jwks = null; jwksAt = 0; }

/* Verifies an OpenID ID token's signature and claims; returns its claims or throws. */
export async function verifyIdToken(token, clientId, nonce) {
  const parts = String(token || "").split(".");
  if (parts.length !== 3) throw new Error("Malformed ID token");
  const header = JSON.parse(Buffer.from(parts[0], "base64url").toString("utf8"));
  const claims = JSON.parse(Buffer.from(parts[1], "base64url").toString("utf8"));
  const jwk = await signingKey(header.kid);
  if (!jwk) throw new Error("Unknown ID token key");
  const key = createPublicKey({ key: jwk, format: "jwk" });
  const data = Buffer.from(parts[0] + "." + parts[1]);
  const sig = Buffer.from(parts[2], "base64url");
  const alg = header.alg;
  let ok = false;
  if (alg === "RS256") ok = verify("sha256", data, key, sig);
  else if (alg === "ES256") ok = verify("sha256", data, { key, dsaEncoding: "ieee-p1363" }, sig);
  else throw new Error("Unsupported ID token algorithm");
  if (!ok) throw new Error("ID token signature is invalid");
  const now = Math.floor(Date.now() / 1000);
  const aud = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
  if (claims.iss !== ISSUER) throw new Error("ID token issuer mismatch");
  if (!aud.includes(clientId)) throw new Error("ID token audience mismatch");
  if (!(claims.exp > now - 60)) throw new Error("ID token expired");
  if (claims.nonce !== nonce) throw new Error("ID token nonce mismatch");
  return claims;
}

const REFRESH_DEAD = ["invalid_grant", "invalid_refresh_token", "token_expired", "refresh_token_expired",
  "refresh_token_invalidated", "refresh_token_reused"];

/* A session with a usable access token: refreshed if it expires within 2 minutes.
 * Returns {session, changed} or {dead: true} when the person must sign in again. */
async function fresh(session) {
  if (session.expires_at - Date.now() > 120e3) return { session, changed: false };
  if (!session.refresh_token) return { dead: true };
  const { status, data } = await form(TOKEN, {
    grant_type: "refresh_token", client_id: session.client_id, refresh_token: session.refresh_token,
    resource: API,
  });
  if (status === 200 && data.access_token) {
    return {
      changed: true,
      session: {
        ...session,
        access_token: data.access_token,
        refresh_token: data.refresh_token || session.refresh_token,   /* rotates */
        expires_at: Date.now() + (Number(data.expires_in) || 3600) * 1000,
        scope: data.scope || session.scope,
      },
    };
  }
  if (REFRESH_DEAD.includes(data.error)) return { dead: true };
  throw Object.assign(new Error("Couldn't refresh the ChatGPT session. Try again."), { status: 503 });
}

/* ---- inference ---- */

const ERRORS = {
  subscription_sharing_user_not_eligible: [403, "Your ChatGPT plan can't be used here. Try the API key option."],
  subscription_sharing_usage_limit_exceeded: [429, "You've reached your ChatGPT usage limit. Check usage in ChatGPT settings."],
  subscription_sharing_usage_unavailable: [503, "ChatGPT usage couldn't be checked. Try again in a moment."],
  subscription_sharing_unsupported_capability: [400, "That model or option isn't available with your ChatGPT plan."],
  subscription_sharing_invalid_user: [401, "ChatGPT didn't accept this sign-in. Sign in again."],
  subscription_sharing_user_unavailable: [503, "ChatGPT is temporarily unavailable. Try again shortly."],
};
function apiError(code, message, status) {
  const known = ERRORS[code];
  return Object.assign(new Error(known ? known[1] : message || "ChatGPT couldn't make the game."), { status: known ? known[0] : status || 502, code });
}

/* Reads a Responses API event stream to the end; returns the output text. */
export async function readStream(body) {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  let buffer = "", text = "", done = false;
  const handle = block => {
    const data = block.split("\n").filter(l => l.startsWith("data:")).map(l => l.slice(5).trim()).join("\n");
    if (!data || data === "[DONE]") return;
    let ev;
    try { ev = JSON.parse(data); } catch { return; }
    if (ev.type === "response.output_text.delta" && typeof ev.delta === "string") text += ev.delta;
    else if (ev.type === "response.completed") {
      done = true;
      if (!text && ev.response && Array.isArray(ev.response.output)) text = outputText(ev.response);
    } else if (ev.type === "response.failed") {
      const e = (ev.response && ev.response.error) || {};
      throw apiError(e.code, e.message);
    } else if (ev.type === "response.incomplete") {
      throw apiError("", "ChatGPT stopped before finishing. Try a simpler idea.");
    } else if (ev.type === "error") {
      throw apiError(ev.code || (ev.error && ev.error.code), ev.message || (ev.error && ev.error.message));
    }
  };
  for (;;) {
    const { value, done: end } = await reader.read();
    if (value) buffer += decoder.decode(value, { stream: true });
    let cut;
    while ((cut = buffer.indexOf("\n\n")) >= 0) { handle(buffer.slice(0, cut)); buffer = buffer.slice(cut + 2); }
    if (end) break;
  }
  if (buffer.trim()) handle(buffer);
  if (!done) throw apiError("", "The connection to ChatGPT ended early. Try again.");
  return text;
}
function outputText(response) {
  let text = "";
  for (const item of response.output || []) {
    for (const c of item.content || []) if (c.type === "output_text" && typeof c.text === "string") text += c.text;
  }
  return text;
}

async function generate(session, body) {
  const model = typeof body.model === "string" && /^[A-Za-z0-9._:-]{1,64}$/.test(body.model) ? body.model : "";
  const instructions = typeof body.instructions === "string" ? body.instructions.slice(0, 40000) : "";
  const input = Array.isArray(body.input) ? body.input.slice(0, 12) : [];
  if (!model || !input.length || !input.every(m => m && (m.role === "user" || m.role === "assistant")
      && typeof m.content === "string" && m.content.length <= 40000)) {
    throw Object.assign(new Error("Expected {model, instructions, input}."), { status: 400 });
  }
  /* The plan route requires store:false and stream:true and rejects sampling options. */
  const res = await fetch(API + "/responses", {
    method: "POST",
    headers: { Authorization: "Bearer " + session.access_token, "Content-Type": "application/json", Accept: "text/event-stream" },
    body: JSON.stringify({ model, instructions, input: input.map(m => ({ role: m.role, content: m.content })), store: false, stream: true }),
  });
  if (!res.ok) {
    let e = {};
    try { e = (await res.json()).error || {}; } catch { e = {}; }
    if (res.status === 401 && !e.code) throw apiError("subscription_sharing_invalid_user");
    throw apiError(e.code, e.message, res.status === 401 || res.status === 403 || res.status === 429 ? res.status : 502);
  }
  return readStream(res.body);
}

/* ---- handlers ---- */

async function start(request) {
  const body = (await readJson(request, 1024)) || {};
  if (!HOST_RE.test(body.host || "")) return fail(400, "Expected {host}.");
  const returning = typeof body.client_id === "string" && CLIENT_RE.test(body.client_id) ? body.client_id : "";
  const state = random(), nonce = random(), verifier = random();
  const challenge = b64url(createHash("sha256").update(verifier).digest());
  const params = new URLSearchParams({
    client_id: returning || "dynamic_agent_client",
    ...(returning ? {} : { agent_name_hint: "KortaMod" }),
    ext_agent_host_id: body.host,
    response_type: "code",
    redirect_uri: REDIRECT,
    scope: SCOPE,
    resource: API,
    state, nonce,
    code_challenge_method: "S256",
    code_challenge: challenge,
  });
  if (returning && typeof body.login_hint === "string" && body.login_hint.length < 200) params.set("login_hint", body.login_hint);
  const pending = seal({ state, nonce, verifier, client_id: returning, host: body.host, at: Date.now() });
  return reply(200, { url: AUTHORIZE + "?" + params.toString() }, [cookie(PENDING, pending, PENDING_S)]);
}

/* The address of the page the browser couldn't open after approval. */
export function parseCallback(text) {
  const s = String(text || "").trim();
  let url;
  try { url = new URL(s); } catch {
    try { url = new URL("http://127.0.0.1/auth/callback" + (s.startsWith("?") ? s : "?" + s)); } catch { return null; }
  }
  const q = url.searchParams;
  return { code: q.get("code") || "", state: q.get("state") || "", client_id: q.get("client_id") || "",
    scope: q.get("scope") || "", error: q.get("error") || "" };
}

async function finish(request) {
  const pending = open(cookies(request)[PENDING] || "");
  if (!pending || Date.now() - pending.at > PENDING_S * 1000) return fail(400, "Sign-in expired. Start again.");
  const body = (await readJson(request, 8192)) || {};
  const cb = parseCallback(body.url);
  const clearPending = cookie(PENDING, "", 0);
  if (!cb || !cb.state) return reply(400, { error: "That address isn't the one from the sign-in page." }, []);
  if (cb.state !== pending.state) return reply(400, { error: "That address is from another sign-in. Start again." }, [clearPending]);
  if (cb.error === "access_denied") return reply(403, { error: "Sign-in was cancelled." }, [clearPending]);
  if (cb.error || !cb.code) return reply(400, { error: "Sign-in didn't finish. Start again." }, [clearPending]);
  let clientId = pending.client_id;
  if (clientId) {
    if (cb.client_id && cb.client_id !== clientId) return reply(400, { error: "ChatGPT returned a different app registration. Start again." }, [clearPending]);
  } else {
    clientId = cb.client_id;
    if (!CLIENT_RE.test(clientId || "")) return reply(400, { error: "Registration didn't complete. Start again." }, [clearPending]);
  }
  const { status, data } = await form(TOKEN, {
    grant_type: "authorization_code", client_id: clientId, code: cb.code, code_verifier: pending.verifier,
    redirect_uri: REDIRECT, resource: API,
  });
  if (status !== 200 || !data.access_token) {
    return reply(400, { error: data.error === "invalid_grant" ? "That sign-in code was already used or expired. Start again." : "ChatGPT didn't accept the sign-in. Start again." }, [clearPending]);
  }
  const claims = await verifyIdToken(data.id_token, clientId, pending.nonce).catch(e => ({ error: e.message }));
  if (claims.error) return reply(400, { error: "Sign-in couldn't be verified. Start again." }, [clearPending]);
  const scope = data.scope || cb.scope.replace(/\+/g, " ");
  if (!scope.split(/\s+/).includes(PLAN_SCOPE)) {
    return reply(403, { error: "Using your ChatGPT plan wasn't allowed. Sign in again and allow it, or use an API key." }, [clearPending]);
  }
  const session = {
    v: 1, client_id: clientId, sub: claims.sub, email: claims.email || "", host: pending.host,
    access_token: data.access_token, refresh_token: data.refresh_token || "",
    expires_at: Date.now() + (Number(data.expires_in) || 3600) * 1000, scope,
  };
  return reply(200, { email: session.email, client_id: clientId }, [clearPending, ...sessionCookies(session)]);
}

async function withSession(request, work) {
  const session = readSession(request);
  if (!session) return fail(401, "Sign in with ChatGPT first.");
  let state;
  try { state = await fresh(session); } catch (e) { return fail(e.status || 503, e.message); }
  if (state.dead) return reply(401, { error: "Your ChatGPT sign-in ended. Sign in again." }, sessionCookies(null));
  try {
    const out = await work(state.session);
    return reply(200, out, state.changed ? sessionCookies(state.session) : []);
  } catch (e) {
    const cookiesOut = e.code === "subscription_sharing_invalid_user" ? sessionCookies(null) : state.changed ? sessionCookies(state.session) : [];
    return reply(e.status || 502, { error: e.message, code: e.code || undefined }, cookiesOut);
  }
}

async function models(request) {
  return withSession(request, async session => {
    const res = await fetch(API + "/models", { headers: { Authorization: "Bearer " + session.access_token } });
    if (!res.ok) throw apiError("", "Couldn't list your ChatGPT models.", res.status === 401 ? 401 : 502);
    const data = await res.json();
    const list = Array.isArray(data.models) ? data.models : Array.isArray(data.data) ? data.data : [];
    return {
      models: list.filter(m => !m.visibility || m.visibility === "list")
        .map(m => ({ slug: m.slug || m.id, name: m.display_name || m.slug || m.id }))
        .filter(m => typeof m.slug === "string"),
    };
  });
}

async function signout(request) {
  const session = readSession(request);
  let revoked = false;
  if (session && session.refresh_token) {
    try {
      const endpoint = (await discovery()).revocation_endpoint;
      if (endpoint) {
        const { status } = await form(endpoint, { token: session.refresh_token, token_type_hint: "refresh_token", client_id: session.client_id });
        revoked = status === 200;
      }
    } catch { revoked = false; }
  }
  return reply(200, { ok: true, revoked }, [...sessionCookies(null), cookie(PENDING, "", 0)]);
}

async function handle(request) {
  const op = new URL(request.url).searchParams.get("op") || "";
  const method = request.method;
  if (method === "GET" && op === "session") {
    const s = readSession(request);
    return json(200, s ? { signed_in: true, email: s.email, client_id: s.client_id } : { signed_in: false });
  }
  if (method === "GET" && op === "models") return models(request);
  if (method !== "POST") return fail(405, "Method not allowed.");
  /* Same-origin only: the cookie is SameSite=Strict, and this rejects other sites outright. */
  const origin = request.headers.get("origin");
  if (origin && origin !== new URL(request.url).origin) return fail(403, "Forbidden.");
  if (op === "start") return start(request);
  if (op === "finish") return finish(request);
  if (op === "signout") return signout(request);
  if (op === "generate") {
    const body = (await readJson(request, 200000)) || {};
    return withSession(request, async session => ({ text: await generate(session, body) }));
  }
  return fail(404, "Unknown operation.");
}

export const GET = guarded(handle);
export const POST = guarded(handle);
