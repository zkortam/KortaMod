// Games tab: describe a game, have ChatGPT write it, play it here, install it.
// Runs after the page's main script and uses its api(), say(), state, online, tab
// and token. The preview is the Chromatic's own engine compiled to WebAssembly.
"use strict";
(() => {
const $ = id => document.getElementById(id);
const store = {
  get: k => { try { return localStorage.getItem("kortamod." + k) || ""; } catch (e) { return ""; } },
  set: (k, v) => { try { v ? localStorage.setItem("kortamod." + k, v) : localStorage.removeItem("kortamod." + k); } catch (e) {} },
};

/* ---------- the model's instructions (from docs/games.md) ---------- */

const SPEC = `You write games for KortaMod, a tiny game engine on the ModRetro Chromatic handheld.
Write ONE complete, self-contained program in KortaScript, a strict subset of JavaScript.
Reply with exactly one \`\`\`js code block and nothing else.

SCREEN AND LOOP
- 160 x 144 pixels, origin top-left. 16 colours by index (PICO-8 palette):
  0 black, 1 dark blue, 2 dark purple, 3 dark green, 4 brown, 5 dark grey, 6 light grey, 7 white,
  8 red, 9 orange, 10 yellow, 11 green, 12 blue, 13 lavender, 14 pink, 15 peach.
- Define any of: init() (once at start), update() (30 times a second: logic), draw() (30 times a second after update: drawing).
- Top-level code runs once before init(). Keep game state in top-level variables.

API (all coordinates are floored)
- cls(c = 0) clear the screen
- pset(x, y, c); pget(x, y) -> colour
- rect(x, y, w, h, c) filled rectangle; rectb(x, y, w, h, c) outline
- circ(x, y, r, c) filled circle; circb(x, y, r, c) outline
- line(x0, y0, x1, y1, c)
- text(s, x, y, c, scale = 1) 5x7 font, 6 px per character at scale 1 (26 characters fit across); returns the width in pixels
- btn(name) true while held; btnp(name) true only on the frame it was pressed. Names: "left" "right" "up" "down" "a" "b"
- rnd(n) random number in [0, n); time() seconds since start
- save(n) keep one number across plays (e.g. a high score); load() returns it, 0 the first time
- WIDTH = 160, HEIGHT = 144
- Start pauses the game and is never seen by it. There is no sound.

LANGUAGE (only these features exist)
- let, const, var; numbers (32-bit float), strings, booleans, null, undefined, arrays, objects {x: 1, y} (shorthand ok), template literals \`Score \${n}\`
- function declarations and expressions, arrow functions, closures
- if/else, for (;;), for (const x of array), while, do/while, switch, break, continue, return, ?:
- operators: + - * / % **, ++ --, = += -= *= /= %=, == != === !== (no type coercion except null == undefined), < > <= >=, && || !, & | ^ ~ << >> >>>, typeof
- obj.prop, obj["key"], arr[i], arr.length, str.length
- array methods: push pop shift unshift splice slice indexOf includes join reverse fill forEach map filter find findIndex some every reduce sort
- string methods: charAt charCodeAt indexOf includes slice substring toUpperCase toLowerCase padStart padEnd repeat split trim; numbers: toFixed toString
- Math.floor ceil round trunc abs sign min max sqrt pow sin cos tan atan atan2 hypot random PI; String(x) Number(x) parseInt(s) isNaN(x); Array(n) / new Array(n); console.log
NOT available (never use): classes, this, new (except new Array), spread/rest (...), destructuring, getters/setters,
async/await, Promises, try/catch, regular expressions, Date, Map, Set, JSON, Object.keys, labels, delete, in, optional chaining (?.), ??, setTimeout, DOM, canvas, images, sound.

LIMITS
- At most 12,000 characters of code. About 20 KB of memory: avoid creating thousands of objects; reuse arrays.
- Each frame has a fixed instruction budget: keep per-frame loops small (e.g. no pixel-by-pixel cls in loops; use rect).
- At most 64 nested calls (no deep recursion).

DESIGN
- Make it fun and complete: a title screen ("PRESS A"), the game, a game-over screen with score and best (via save/load), and A to restart.
- Use the whole 160x144 screen; keep text short and readable; strong colour contrast.
- Movement in pixels per frame (30 fps). Use btnp for menus, jumping and firing; btn for movement.
- Check every variable is declared and every function you call exists in the API above.

EXAMPLE
\`\`\`js
let x = 80, y = 72, score = 0, best = load(), coin = { x: 40, y: 40 };
function update() {
  if (btn("left")) x -= 2;
  if (btn("right")) x += 2;
  if (btn("up")) y -= 2;
  if (btn("down")) y += 2;
  x = Math.max(4, Math.min(WIDTH - 4, x));
  y = Math.max(12, Math.min(HEIGHT - 4, y));
  if (Math.abs(x - coin.x) < 6 && Math.abs(y - coin.y) < 6) {
    score++;
    if (score > best) { best = score; save(best); }
    coin = { x: 8 + Math.floor(rnd(WIDTH - 16)), y: 16 + Math.floor(rnd(HEIGHT - 24)) };
  }
}
function draw() {
  cls(1);
  circ(coin.x, coin.y, 3, 10);
  rect(x - 3, y - 3, 7, 7, 11);
  text(\`SCORE \${score}  BEST \${best}\`, 2, 2, 7);
}
\`\`\``;

const EXAMPLES = {
  Snake: "Snake: steer with the D-pad, eat apples to grow, speed up slowly, game over when hitting a wall or yourself.",
  Breakout: "Breakout: move the paddle with left/right, bounce the ball to break rows of colourful bricks, 3 lives, levels get faster.",
  Flappy: "Flappy bird: press A to flap, fly through gaps in scrolling pipes, score one point per pipe.",
  "Space shooter": "Space shooter: ship at the bottom moves left/right, A fires, waves of enemies descend and shoot back, power-ups.",
};

const ideaPrompt = idea => `Make this game for the Chromatic:\n\n${idea}`;
const fixPrompt = error => `The game failed in the KortaMod engine:\n\n${error}\n\nFix it, using only the features listed in the instructions. Reply with the complete corrected program in one \`\`\`js block.`;
const fullPrompt = idea => `${SPEC}\n\n---\n\n${ideaPrompt(idea)}`;

/* The program inside a reply: the longest fenced block, else the whole text if it looks like code. */
function extractCode(text) {
  const blocks = [...String(text || "").matchAll(/```[a-zA-Z]*\s*\n([\s\S]*?)```/g)].map(m => m[1]);
  if (blocks.length) return blocks.sort((a, b) => b.length - a.length)[0].trim() + "\n";
  const t = String(text || "").trim();
  return /function\s|=>|\bdraw\b/.test(t) ? t + "\n" : "";
}

/* ---------- the engine (WebAssembly) ---------- */

const W = 160, H = 144, STEP = 1000 / 30;
const Engine = {
  x: null, mem: null, failed: "", pal: null,
  async ready() {
    if (this.x) return true;
    if (this.failed) return false;
    try {
      const res = await fetch("/kgame.wasm", { cache: "no-cache" });
      if (!res.ok) throw new Error("missing");
      const module = await WebAssembly.compile(await res.arrayBuffer());
      const imports = {};
      for (const imp of WebAssembly.Module.imports(module)) {
        imports[imp.module] = imports[imp.module] || {};
        if (imp.kind === "function") imports[imp.module][imp.name] = () => 0;
        else if (imp.kind === "memory") imports[imp.module][imp.name] = new WebAssembly.Memory({ initial: 32 });
      }
      const inst = await WebAssembly.instantiate(module, imports);
      this.x = inst.exports;
      this.mem = this.x.memory || Object.values(imports).map(m => Object.values(m).find(v => v instanceof WebAssembly.Memory)).find(Boolean);
      const p = new Uint32Array(this.mem.buffer, this.x.kg_palette(), 16);
      /* ImageData is RGBA bytes: little-endian 0xAABBGGRR words. */
      this.pal = Uint32Array.from(p, c => 0xff000000 | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff));
      return true;
    } catch (e) {
      this.failed = "The preview engine isn't available yet.";
      return false;
    }
  },
  cstr(ptr) {
    if (!ptr) return "";
    const b = new Uint8Array(this.mem.buffer);
    let end = ptr;
    while (end < b.length && b[end]) end++;
    return new TextDecoder().decode(b.subarray(ptr, end));
  },
  /* Compiles and starts a program; "" on success, else the error. */
  load(source, seed) {
    const bytes = new TextEncoder().encode(source);
    if (bytes.length > 16000) return "The game is too long (16,000 bytes at most).";
    this.x.kg_init(seed >>> 0);
    new Uint8Array(this.mem.buffer, this.x.kg_source(), bytes.length).set(bytes);
    return this.x.kg_load(bytes.length) ? (this.cstr(this.x.kg_error()) || "The game failed to start.") : "";
  },
  frame(buttons) { return this.x.kg_frame(buttons) ? (this.cstr(this.x.kg_error()) || "The game stopped.") : ""; },
  log() { return this.x.kg_log ? this.cstr(this.x.kg_log()) : ""; },
  paint(image) {
    const px = new Uint8Array(this.mem.buffer, this.x.kg_pixels(), W * H / 2);
    const out = new Uint32Array(image.data.buffer);
    for (let i = 0, j = 0; i < px.length; i++) {
      out[j++] = this.pal[px[i] >> 4];
      out[j++] = this.pal[px[i] & 15];
    }
  },
};

/* Runs a program without a screen for ~6 s of game time, idle then with pseudo-random
 * input, to catch errors before anyone plays it. "" if it held up. */
function smokeTest(source) {
  const err = Engine.load(source, 12345);
  if (err) return err;
  let r = 7;
  for (let f = 0; f < 180; f++) {
    if (f >= 90 && f % 6 === 0) { r = (r * 1103515245 + 12345) >>> 0; }
    const e = Engine.frame(f < 90 ? 0 : (r >>> 8) & 63);
    if (e) return e;
  }
  return "";
}

/* ---------- preview ---------- */

const canvas = $("gameScreen"), ctx = canvas.getContext("2d");
const image = ctx.createImageData(W, H);
let code = store.get("game.draft"), running = false, crashed = "", acc = 0, last = 0, held = 0, saved = {};
const KEYS = { ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8, z: 16, Z: 16, j: 16, " ": 16, x: 32, X: 32, k: 32 };
const touch = new Map();
const visible = () => typeof tab !== "undefined" && tab === "games" && !document.hidden;

function logLine(text) {
  if (!text) return;
  const box = $("gameLog");
  box.hidden = false;
  box.textContent = (box.textContent + text).split("\n").slice(-12).join("\n");
}
/* fixable: the game's own error, which a model can fix (not a missing engine). */
function showError(text, fixable = true) {
  crashed = text;
  $("gameError").textContent = text;
  $("gameError").hidden = !text;
  $("gameFix").hidden = !text || !fixable;
  $("gameFix").textContent = mode === "copy" ? "Copy fix request" : "Fix with ChatGPT";
  paintInstall();
}
async function start(source) {
  code = source;
  store.set("game.draft", code);
  $("gameCode").value = code;
  $("gamePlay").hidden = false;
  $("gameLog").textContent = "";
  $("gameLog").hidden = true;
  if (!(await Engine.ready())) { showError(Engine.failed, false); return; }
  const err = Engine.load(code, (Math.random() * 4294967296) >>> 0);
  logLine(Engine.log());
  showError(err);
  running = !err;
  acc = 0;
  last = performance.now();
  if (running) { Engine.paint(image); ctx.putImageData(image, 0, 0); }
  paintInstall();
}
function loop(now) {
  requestAnimationFrame(loop);
  if (!running || !visible()) { last = now; return; }
  acc = Math.min(acc + (now - last), STEP * 4);
  last = now;
  let ran = false;
  while (acc >= STEP && running) {
    acc -= STEP;
    let buttons = held;
    for (const b of touch.values()) buttons |= b;
    const err = Engine.frame(buttons);
    ran = true;
    if (err) { running = false; showError(err); }
  }
  if (ran) {
    Engine.paint(image);
    ctx.putImageData(image, 0, 0);
    logLine(Engine.log());
  }
}
requestAnimationFrame(loop);

addEventListener("keydown", e => {
  if (!visible() || !running || /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement && document.activeElement.tagName)) return;
  const bit = KEYS[e.key];
  if (bit) { held |= bit; e.preventDefault(); }
});
addEventListener("keyup", e => { const bit = KEYS[e.key]; if (bit) held &= ~bit; });
addEventListener("blur", () => { held = 0; touch.clear(); });
for (const b of document.querySelectorAll("#gamePad [data-b]")) {
  const bit = Number(b.dataset.b);
  const down = e => { e.preventDefault(); b.setPointerCapture && b.setPointerCapture(e.pointerId); touch.set(e.pointerId, bit); b.classList.add("on"); };
  const up = e => { touch.delete(e.pointerId); b.classList.remove("on"); };
  b.addEventListener("pointerdown", down);
  b.addEventListener("pointerup", up);
  b.addEventListener("pointercancel", up);
  b.addEventListener("lostpointercapture", up);
  b.addEventListener("contextmenu", e => e.preventDefault());
}
$("gameRestart").onclick = () => code && start(code);
$("gameRun").onclick = () => { const c = $("gameCode").value; if (c.trim()) start(c); };

/* ---------- generation ---------- */

let mode = store.get("game.mode") || "copy", busy = false;
const MODELS = ["gpt-6.1-sol", "gpt-6-astra", "gpt-5.6-sol", "gpt-5.3-codex"];
function setMode(m) {
  mode = m;
  store.set("game.mode", m);
  for (const c of document.querySelectorAll("#genMode .seg-item")) c.setAttribute("aria-pressed", String(c.dataset.mode === m));
  moveFill();
  for (const p of document.querySelectorAll("[data-gen]")) p.hidden = p.dataset.gen !== m;
  $("gameCreate").textContent = m === "copy" ? "Copy prompt and open ChatGPT" : "Create game";
  showError(crashed, !Engine.failed);
  if (m === "plan") planStatus();
  paintCreate();
}
function paintCreate() {
  const idea = $("gameIdea").value.trim();
  let ok = !!idea && !busy;
  if (mode === "key") ok = ok && $("gameKey").value.trim().length > 20;
  if (mode === "plan") ok = ok && plan.signedIn;
  $("gameCreate").disabled = !ok;
}
function progress(text) {
  $("gameBusy").textContent = text;
  $("gameBusy").hidden = !text;
}

async function viaKey(history) {
  const key = $("gameKey").value.trim();
  const model = $("gameModelKey").value.trim() || MODELS[0];
  let res;
  try {
    res = await fetch("https://api.openai.com/v1/responses", {
      method: "POST",
      headers: { Authorization: "Bearer " + key, "Content-Type": "application/json" },
      body: JSON.stringify({ model, instructions: SPEC, input: history }),
    });
  } catch (e) { throw new Error("Couldn't reach OpenAI."); }
  let data = {};
  try { data = await res.json(); } catch (e) {}
  if (!res.ok) throw new Error((data.error && data.error.message) || "OpenAI refused the request.");
  if (typeof data.output_text === "string") return data.output_text;
  let text = "";
  for (const item of data.output || []) for (const c of item.content || []) if (c.type === "output_text") text += c.text;
  return text;
}
async function viaPlan(history) {
  const model = $("gameModelPlan").value.trim() || MODELS[0];
  const data = await planCall("generate", "POST", { model, instructions: SPEC, input: history });
  return data.text || "";
}

/* Asks the model, tests the result in the engine, and sends errors back up to twice. */
async function generate(history) {
  if (!(await Engine.ready())) throw new Error(Engine.failed);
  const ask = mode === "key" ? viaKey : viaPlan;
  let best = "";
  for (let attempt = 0; attempt < 3; attempt++) {
    progress(attempt ? `Fixing (${attempt} of 2)…` : "Writing your game…");
    const reply = await ask(history);
    const program = extractCode(reply);
    if (!program) throw new Error("The reply had no code. Try again.");
    best = program;
    progress("Testing…");
    const err = smokeTest(program);
    if (!err) return { program, history: [...history, { role: "assistant", content: "```js\n" + program + "```" }] };
    history = [...history, { role: "assistant", content: "```js\n" + program + "```" }, { role: "user", content: fixPrompt(err) }];
  }
  return { program: best, history };
}
let conversation = [];
$("gameCreate").onclick = async () => {
  const idea = $("gameIdea").value.trim();
  if (!idea) return;
  if (!$("gameName").value.trim()) $("gameName").value = nameFrom(idea);
  if (mode === "copy") {
    const text = fullPrompt(idea);
    window.open("https://chatgpt.com/", "_blank", "noopener");
    try { await navigator.clipboard.writeText(text); say("Prompt copied; paste it into ChatGPT", "info"); }
    catch (e) { $("gamePrompt").value = text; $("gamePrompt").hidden = false; say("Couldn't copy: use the prompt below", "err"); }
    $("gameReplyBox").hidden = false;
    $("gameReply").focus();
    return;
  }
  busy = true; paintCreate();
  try {
    const out = await generate([{ role: "user", content: ideaPrompt(idea) }]);
    conversation = out.history;
    await start(out.program);
    say(crashed ? "Game made, with an error" : "Game ready");
  } catch (e) { say(e.message, "err"); }
  busy = false; progress(""); paintCreate();
};
$("gameReplyRun").onclick = () => {
  const program = extractCode($("gameReply").value);
  if (!program) { say("No code in that reply: paste all of it", "err"); return; }
  $("gameReply").value = "";
  start(program);
};
$("gameFix").onclick = async () => {
  if (!crashed) return;
  if (mode === "copy") {
    try { await navigator.clipboard.writeText(fixPrompt(crashed)); say("Fix request copied; send it in the same chat", "info"); }
    catch (e) { say("Couldn't copy", "err"); }
    $("gameReplyBox").hidden = false;
    return;
  }
  if (busy) return;
  busy = true; paintCreate();
  const history = conversation.length
    ? [...conversation, { role: "user", content: fixPrompt(crashed) }]
    : [{ role: "user", content: ideaPrompt($("gameIdea").value.trim() || "the game below") },
       { role: "assistant", content: "```js\n" + code + "```" }, { role: "user", content: fixPrompt(crashed) }];
  try {
    const out = await generate(history);
    conversation = out.history;
    await start(out.program);
    say(crashed ? "Still failing" : "Fixed");
  } catch (e) { say(e.message, "err"); }
  busy = false; progress(""); paintCreate();
};
for (const c of document.querySelectorAll("#genMode .seg-item")) c.onclick = () => setMode(c.dataset.mode);
/* One fill slides between the segments. */
function moveFill() {
  const on = document.querySelector('#genMode .seg-item[aria-pressed="true"]'), fill = document.querySelector("#genMode .fill");
  if (!on || !fill || !on.offsetWidth) return;
  fill.style.width = on.offsetWidth + "px";
  fill.style.transform = `translateX(${on.offsetLeft - 4}px)`;
}
addEventListener("resize", moveFill);
for (const [label, idea] of Object.entries(EXAMPLES)) {
  const c = document.createElement("button");
  c.className = "chip";
  c.textContent = label;
  c.onclick = () => { $("gameIdea").value = idea; $("gameName").value = label.slice(0, 12); paintCreate(); $("gameIdea").focus(); };
  $("gameExamples").appendChild(c);
}
$("gameIdea").addEventListener("input", paintCreate);
$("gameKey").value = store.get("openai.key");
$("gameKey").addEventListener("input", () => { store.set("openai.key", $("gameKey").value.trim()); paintCreate(); });
$("gameModelKey").value = store.get("openai.model") || MODELS[0];
$("gameModelKey").addEventListener("change", () => store.set("openai.model", $("gameModelKey").value.trim()));
for (const m of MODELS) $("gameModels").appendChild(new Option(m, m));

function nameFrom(idea) {
  const head = idea.split(/[:.,\n]/)[0].trim();
  const words = head.replace(/[^A-Za-z0-9 ]/g, "").split(/\s+/).filter(Boolean);
  let name = "";
  for (const w of words) { const next = name ? name + " " + w : w; if (next.length > 12) break; name = next; }
  return (name || "Game").slice(0, 12);
}

/* ---------- Sign in with ChatGPT (tokens stay in an HttpOnly cookie) ---------- */

const plan = { signedIn: false, email: "", models: false };
async function planCall(op, method, body) {
  let res;
  try {
    res = await fetch("/api/openai?op=" + op, {
      method, credentials: "same-origin", cache: "no-store",
      headers: body ? { "Content-Type": "application/json" } : {},
      body: body ? JSON.stringify(body) : undefined,
    });
  } catch (e) { throw new Error("No connection."); }
  let data = {};
  try { data = await res.json(); } catch (e) {}
  if (res.status === 401 && op !== "session") { plan.signedIn = false; paintPlan(); }
  if (!res.ok) throw new Error(data.error || "Something went wrong.");
  return data;
}
/* A stable id for this browser, in the URN UUID form OpenAI accepts. */
function hostId() {
  let h = store.get("openai.host");
  if (!/^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(h || "")) {
    const b = crypto.getRandomValues(new Uint8Array(16));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const x = [...b].map(v => v.toString(16).padStart(2, "0")).join("");
    h = `urn:uuid:${x.slice(0, 8)}-${x.slice(8, 12)}-${x.slice(12, 16)}-${x.slice(16, 20)}-${x.slice(20)}`;
    store.set("openai.host", h);
  }
  return h;
}
function paintPlan() {
  $("planOut").hidden = plan.signedIn;
  $("planIn").hidden = !plan.signedIn;
  $("planWho").textContent = plan.email || "ChatGPT";
  paintCreate();
}
async function planStatus() {
  try {
    const s = await planCall("session", "GET");
    plan.signedIn = !!s.signed_in;
    plan.email = s.email || "";
    if (plan.signedIn && !plan.models) {
      plan.models = true;
      planCall("models", "GET").then(d => {
        const list = $("gamePlanModels");
        list.textContent = "";
        for (const m of d.models || []) list.appendChild(new Option(m.name, m.slug));
        if (d.models && d.models.length && !$("gameModelPlan").value) $("gameModelPlan").value = d.models[0].slug;
      }).catch(() => { plan.models = false; });
    }
  } catch (e) { plan.signedIn = false; }
  paintPlan();
}
$("planStart").onclick = async () => {
  /* Open the tab inside the click so it isn't blocked, then point it at OpenAI. */
  const win = window.open("about:blank", "_blank");
  try {
    const { url } = await planCall("start", "POST", { host: hostId(), client_id: store.get("openai.client"), login_hint: store.get("openai.email") });
    if (win) { win.opener = null; win.location = url; } else location.assign(url);
    $("planPaste").hidden = false;
    $("planUrl").focus();
  } catch (e) { if (win) win.close(); say(e.message, "err"); }
};
$("planConnect").onclick = async () => {
  const url = $("planUrl").value.trim();
  if (!url) return;
  try {
    const r = await planCall("finish", "POST", { url });
    store.set("openai.client", r.client_id);
    store.set("openai.email", r.email);
    $("planUrl").value = "";
    $("planPaste").hidden = true;
    plan.models = false;
    await planStatus();
    say("Signed in");
  } catch (e) { say(e.message, "err"); }
};
$("planSignOut").onclick = async () => {
  try {
    const r = await planCall("signout", "POST", {});
    say(r.revoked ? "Signed out" : "Signed out here. Disconnect KortaMod in ChatGPT settings to finish.");
  } catch (e) { say(e.message, "err"); }
  plan.signedIn = false; plan.email = ""; plan.models = false;
  paintPlan();
};
$("gameModelPlan").value = store.get("openai.planModel");
$("gameModelPlan").addEventListener("change", () => store.set("openai.planModel", $("gameModelPlan").value.trim()));

/* ---------- install and the Chromatic's games ---------- */

function paintInstall() {
  const ready = !!code && !crashed && typeof token !== "undefined" && !!token && typeof online !== "undefined" && online;
  $("gameInstall").disabled = !ready || !$("gameName").value.trim();
  $("gameInstallBox").hidden = !code;
}
$("gameName").addEventListener("input", paintInstall);
$("gameInstall").onclick = async () => {
  const name = $("gameName").value.trim().slice(0, 12);
  $("gameInstall").disabled = true;
  try {
    await api("POST", "/api/games", { name, source: code });
    say("Game installed");
    if (typeof refresh === "function") refresh();
  } catch (e) { say(e.message, "err"); }
  paintInstall();
};
/* Each installed game gets a tile colour from its name, like its Home tile. */
const TINTS = ["#2563eb", "#e6782a", "#16a34a", "#9333ea", "#0891b2", "#dc2626", "#ca8a04", "#db2777"];
const tint = name => TINTS[[...name].reduce((h, c) => (h * 31 + c.charCodeAt(0)) >>> 0, 7) % TINTS.length];
function render() {
  paintInstall();
  moveFill();
  const box = $("gameList");
  const games = (typeof state !== "undefined" && state && state.games) || [];
  $("gameCount").textContent = games.length ? games.length + (games.length === 1 ? " on your Chromatic" : " on your Chromatic") : "";
  const shape = JSON.stringify([games, typeof online !== "undefined" && online]);
  if (box.dataset.shape === shape) return;
  box.dataset.shape = shape;
  box.textContent = "";
  if (!games.length) {
    const e = document.createElement("div");
    e.className = "empty small";
    const b = document.createElement("b");
    b.textContent = "No games yet";
    const t = document.createElement("span");
    t.textContent = "Games you install appear on its Home screen.";
    e.append(b, t);
    box.appendChild(e);
    return;
  }
  for (const g of games) {
    const r = document.createElement("div");
    r.className = "row";
    const art = document.createElement("span");
    art.className = "game-art";
    art.style.background = tint(g.name);
    art.innerHTML = '<svg class="icon"><use href="#i-game-controller-fill"/></svg>';
    const main = document.createElement("span");
    main.className = "main";
    const title = document.createElement("span");
    title.className = "title";
    title.textContent = g.name;
    const size = document.createElement("span");
    size.className = "desc num";
    size.textContent = (g.size / 1024).toFixed(1) + " KB";
    main.append(title, size);
    const del = document.createElement("button");
    del.className = "icon-btn danger";
    del.setAttribute("aria-label", "Delete " + g.name);
    del.innerHTML = '<svg class="icon"><use href="#i-trash"/></svg>';
    del.disabled = !online;
    del.onclick = async () => {
      if (!confirm(`Delete ${g.name} from your Chromatic?`)) return;
      try { await api("DELETE", "/api/games", { slot: g.slot }); say("Game deleted"); if (typeof refresh === "function") refresh(); }
      catch (e) { say(e.message, "err"); }
    };
    r.append(art, main, del);
    box.appendChild(r);
  }
}

window.KMGames = { render, extractCode, smokeTest, Engine, SPEC };
setMode(mode);
if (code) { $("gameCode").value = code; start(code); }
render();
})();
