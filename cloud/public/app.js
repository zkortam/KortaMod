// KortaMod companion: pairing, the five sections, the note editor and toasts.
// games.js runs after this file and shares api(), say(), refresh(), state, online,
// tab and token (classic scripts share their top-level bindings).
"use strict";
const $ = id => document.getElementById(id);
const TOKEN = "kortamod.token";
/* Arriving from the old address with its pairing in the fragment: keep it, then
 * drop it from the address bar and history. */
if (location.hash.startsWith("#kt=")) {
  const moved = location.hash.slice(4);
  if (/^[0-9a-f]{64}$/.test(moved)) { try { localStorage.setItem(TOKEN, moved); } catch (e) {} }
  history.replaceState(null, "", location.pathname + location.search);
}
let token = localStorage.getItem(TOKEN) || "";
let state = null, online = false, awake = false, loaded = false;
let editing = -1, tab = "notes";
const icon = (name, cls = "icon") => `<svg class="${cls}"><use href="#i-${name}"/></svg>`;
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };

/* Mirrors korta_clean_text on the device, so the counter matches what is kept. */
const LATIN = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
function clean(s) {
  let out = "";
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c < 128) out += (c === 10 || c === 13 || c === 9) ? " " : (c < 32 || c === 127) ? "?" : ch;
    else if (c >= 0xC0 && c <= 0xFF) out += LATIN[c - 0xC0];
    else if (c === 0xA0) out += " ";
    else if (c === 0x2018 || c === 0x2019) out += "'";
    else if (c === 0x201C || c === 0x201D) out += '"';
    else if (c === 0x2013 || c === 0x2014) out += "-";
    else if (c === 0x2022) out += "*";
    else if (c === 0x2026) out += ".";
    else out += "?";
  }
  return out;
}

/* ---------- toasts: object plus past participle; errors say what refused ---------- */
function say(text, kind, action) {
  const dock = $("status");
  if (!text) { dock.textContent = ""; return; }
  const t = el("div", "toast" + (kind === "err" ? " err" : kind === "info" ? " info" : ""));
  t.setAttribute("role", kind === "err" ? "alert" : "status");
  t.innerHTML = icon(kind === "err" ? "x-circle-fill" : kind === "info" ? "info-fill" : "check-circle-fill");
  t.append(el("span", "msg", text));
  if (action) {
    const b = el("button", "act", action.label);
    b.onclick = () => { dismiss(t); action.run(); };
    t.append(b);
  }
  /* A tap dismisses it, so it never sits on a control for long. */
  t.addEventListener("click", e => { if (!e.target.closest(".act")) dismiss(t); });
  dock.prepend(t);
  while (dock.children.length > 3) dock.lastElementChild.remove();
  setTimeout(() => dismiss(t), action ? 5000 : kind === "err" ? 5000 : 4000);
}
function dismiss(t) {
  if (!t.isConnected || t.classList.contains("out")) return;
  t.classList.add("out");
  setTimeout(() => t.remove(), 180);
}

async function api(method, path, body) {
  const headers = {};
  if (token) headers.Authorization = "Bearer " + token;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  /* A Chromatic that isn't checking in fast yet takes up to ~20 s to answer. */
  if (method !== "GET" && online && !awake) say("Waking your Chromatic…", "info");
  let res;
  try { res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body), cache: "no-store" }); }
  catch (e) { throw new Error("No connection"); }
  let data = {};
  try { data = await res.json(); } catch (e) {}
  if (res.status === 401 && token) { forget(); throw new Error("This browser was unpaired"); }
  if (!res.ok) { const err = new Error(data.error || "The site refused the request"); err.status = res.status; throw err; }
  return data;
}

function forget() { token = ""; localStorage.removeItem(TOKEN); state = null; loaded = false; show(); }

function show() {
  $("pair").hidden = !!token;
  if (!token) retroGround();
  $("app").hidden = !token;
  if (token) refresh(); else setTimeout(() => $("code").focus(), 50);
}

/* ---------- navigation: one travelling indicator ---------- */
const TITLES = { notes: "Notes", time: "Time", settings: "Settings", games: "Games", device: "Device" };
function setTab(name) {
  tab = name;
  [...document.querySelectorAll(".seg .tab")].forEach((b, i) => {
    const on = b.dataset.tab === name;
    b.setAttribute("aria-selected", on);
    if (on) $("thumb").style.setProperty("--i", i);
  });
  for (const p of document.querySelectorAll("#app [data-panel]")) p.hidden = p.dataset.panel !== name;
  document.title = TITLES[name] + " – KortaMod";
  scrollTo({ top: 0 });
  render();
}

async function refresh() {
  if (!token || editing >= 0) return;
  try {
    const r = await api("GET", loaded ? "/api/state" : "/api/state?load=1");
    loaded = true;
    online = r.online;
    awake = !!r.awake;
    if (r.state) state = r.state;
    render();
  } catch (e) { if (token) say(e.message, "err"); }
}

const pad = n => String(n).padStart(2, "0");
const PALETTES = ["Default", "Brown", "Blue", "Pastel", "Green", "Red", "Dark blue", "Orange", "Dark green", "Dark brown", "Gray", "Yellow", "Negative", "DMG 1", "DMG 2"];
PALETTES.forEach((n, i) => $("setPalette").add(new Option(n, i)));
for (let i = 1; i <= 8; i++) $("setPlayer").add(new Option(String(i), i));
let timerAt = 0;  /* when the last timer reading arrived */
function fmt(ms) {
  const t = Math.max(0, Math.floor(ms / 1000)), h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60, sec = t % 60;
  return (h ? h + ":" + pad(m) : m) + ":" + pad(sec);
}
function tickTimer() {
  if (!state || !state.timer) return;
  const t = state.timer, gone = t.running ? Date.now() - timerAt : 0;
  $("timerShown").textContent = t.done ? "Done" : fmt(t.countdown ? t.ms - gone : t.ms + gone);
}
setInterval(tickTimer, 250);

function row(k, v) {
  const r = el("div", "row");
  r.append(el("span", "k", k), el("span", "v", v));
  return r;
}
function zoneText(z) { return "UTC" + (z < 0 ? "−" : "+") + Math.floor(Math.abs(z) / 60) + (Math.abs(z) % 60 ? ":" + pad(Math.abs(z) % 60) : ""); }
function rangeFill(r) { const min = +r.min || 0, max = +r.max || 100; r.style.setProperty("--p", ((r.value - min) / (max - min) * 100) + "%"); }

/* ---------- pairing ground ----------
 * Pixel-art sprites over the LCD: each is a tiny bitmap drawn as crisp SVG
 * squares in the LCD's purples (# darkest, . middle, + light), scattered on a
 * seeded grid (the same every load) and kept clear of the card's centre. */
const SPRITES = {
  handheld: [" ######## ", "#........#", "#.######.#", "#.#++++#.#", "#.#++++#.#", "#.#++++#.#", "#.######.#", "#........#",
             "#.#....#.#", "###..#.#.#", "#.#....#.#", "#........#", "#...##...#", " ######## "],
  cartridge: ["#######  ", "#.....## ", "#.+++..##", "#.+++...#", "#.+++...#", "#.......#", "#.......#", "#.#.#.#.#", "#########"],
  dpad: ["  ###  ", "  #.#  ", "###.###", "#.....#", "###.###", "  #.#  ", "  ###  "],
  buttons: ["     ## ", "    #..#", " ## #..#", "#..# ## ", "#..#    ", " ##     "],
  heart: [" ## ## ", "#++#..#", "#+....#", " #...# ", "  #.#  ", "   #   "],
  coin: [" #### ", "#.++.#", "#.+..#", "#.+..#", "#.+..#", "#.+..#", "#....#", " #### "],
  star: ["   #   ", "   #   ", "  #.#  ", "##...##", "  #.#  ", "   #   ", "   #   "],
  ghost: ["  ####  ", " #....# ", "#.++.++#", "#.#..#.#", "#......#", "#......#", "#.##.##.", "##  ##  "],
};
function sprite(rows, px) {
  const w = Math.max(...rows.map(r => r.length)), h = rows.length;
  const fill = { "#": "var(--px0)", ".": "var(--px1)", "+": "var(--px2)" };
  let rects = "";
  rows.forEach((r, y) => [...r].forEach((c, x) => { if (fill[c]) rects += `<rect x="${x}" y="${y}" width="1" height="1" fill="${fill[c]}"/>`; }));
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", `0 0 ${w} ${h}`);
  svg.setAttribute("width", w * px);
  svg.setAttribute("height", h * px);
  svg.setAttribute("shape-rendering", "crispEdges");
  svg.innerHTML = rects;
  return svg;
}
function retroGround() {
  const field = $("retroSprites");
  if (!field || field.childElementCount) return;
  let seed = 1987;
  const random = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  const kinds = Object.keys(SPRITES);
  /* Keep sprites off the card, pills and footer (with a margin), measured from the layout. */
  const area = field.getBoundingClientRect(), wrap = document.querySelector(".pair-wrap");
  const box = wrap && wrap.offsetWidth ? wrap.getBoundingClientRect() : null;
  const clear = (x, y, w, h) => {
    if (!box || !area.width) return Math.abs(x - 50) > 26 || Math.abs(y - 50) > 32;
    const left = area.left + x / 100 * area.width, top = area.top + y / 100 * area.height, m = 20;
    return left + w < box.left - m || left > box.right + m || top + h < box.top - m || top > box.bottom + m;
  };
  /* A jittered 6 x 5 grid: even spacing without looking tiled. */
  const COLS = 6, ROWS = 5;
  for (let gy = 0; gy < ROWS; gy++) for (let gx = 0; gx < COLS; gx++) {
    const x = (gx + .2 + random() * .6) / COLS * 100, y = (gy + .2 + random() * .6) / ROWS * 100;
    if (random() < .18) continue;
    const kind = kinds[Math.floor(random() * kinds.length)];
    const px = 3 + Math.floor(random() * 3);
    const rows = SPRITES[kind];
    if (!clear(x, y, rows[0].length * px + 6 * px, rows.length * px + 4 * px)) continue;
    const el = sprite(SPRITES[kind], px);
    el.classList.add("retro-sprite");
    if (random() < .35) el.classList.add("drift");
    el.style.cssText = `left:${x.toFixed(1)}%;top:${y.toFixed(1)}%;--px:${px}px;--dur:${(1.4 + random() * 1.6).toFixed(2)}s;--delay:${(-random() * 3).toFixed(2)}s`;
    if (el.classList.contains("drift")) el.style.setProperty("--dur", `${(14 + random() * 10).toFixed(1)}s`);
    field.appendChild(el);
  }
}

/* ---------- connection status (top bar on phones, sidebar on desktop) ---------- */
function paintStatus() {
  const st = online ? (awake ? "online" : "waking") : "offline";
  const word = { online: "Connected", waking: "Waking", offline: "Offline" }[st];
  const icon = { online: "#i-wifi-high", waking: "#i-circle-notch", offline: "#i-wifi-slash" }[st];
  const d = (state && state.device) || {};
  const batt = online && d.battery !== undefined;
  const battIcon = d.charging ? "#i-battery-charging" : "#i-battery-high";
  for (const [box, text, ic, bt, bi, pct] of [["who", "whoText", "whoIcon", "whoBatt", "whoBattIcon", "whoBattPct"],
                                              ["sideDevice", "sideState", "sideIcon", "sideBatt", "sideBattIcon", "sideBattPct"]]) {
    $(box).dataset.state = st;
    $(text).textContent = word;
    $(ic).setAttribute("href", icon);
    $(bt).hidden = !batt;
    $(bi).setAttribute("href", battIcon);
    $(pct).textContent = batt ? d.battery + "%" : "";
  }
  $("sideName").textContent = d.name || "Chromatic";
  $("who").setAttribute("aria-label", word + (batt ? ", battery " + d.battery + "%" : ""));
}

/* ---------- render ---------- */
function render() {
  if (window.KMGames) window.KMGames.render();
  $("offline").hidden = online || !state;
  paintStatus();
  for (const b of ["save", "delete", "alarmSave", "clockSet"]) $(b).disabled = !online;
  if (!state) { $("newNote").disabled = true; return; }
  const d = state.device;
  renderNotes();
  renderAlarms();
  renderClock();
  renderSettings();
  renderTimer();
  renderWifi();
  renderDevice(d);
  for (const b of ["wifiJoin", "findMe", "msgSend"]) $(b).disabled = !online;
}

function renderNotes() {
  const items = state.notes.items, pinned = state.notes.pinned || 0, list = $("list");
  $("noteCount").textContent = items.length + " of " + state.notes.slots;
  /* Rebuild only when the notes change, so a tap never lands on a replaced card. */
  const shape = JSON.stringify([items, pinned]);
  if (list.dataset.shape !== shape) {
    list.dataset.shape = shape;
    list.textContent = "";
    if (!items.length) {
      const e = el("div", "empty");
      e.innerHTML = `<div class="tile">${icon("note")}</div>`;
      e.append(el("b", "", "No notes"), el("span", "", "Notes you write here show up on your Chromatic."));
      list.append(e);
    }
    items.forEach((text, i) => {
      if (pinned && i === 0) list.append(el("p", "group-label", "Pinned"));
      if (pinned && i === pinned) list.append(el("p", "group-label", "Notes"));
      const b = el("button", "note");
      b.append(el("div", "t", text));
      b.onclick = () => openEditor(i);
      list.append(b);
    });
  }
  $("newNote").disabled = !online || items.length >= state.notes.slots;
  $("export").hidden = !items.length;
}

function renderClock() {
  const zone = state.clock.zone_minutes;
  const dev = new Date((state.clock.epoch + zone * 60) * 1000);
  $("devTime").textContent = state.clock.known ? hm(dev.getUTCHours(), dev.getUTCMinutes()) + "  " + zoneText(zone) : "Not set";
  const me = new Date();
  $("myTime").textContent = hm(me.getHours(), me.getMinutes()) + "  " + zoneText(-me.getTimezoneOffset());
  const on = (state.alarms || []).filter(a => a.on).length;
  $("timeSub").textContent = on ? on + (on === 1 ? " alarm on" : " alarms on") : "No alarms on";
}

function renderSettings() {
  if (!state.settings) return;
  for (const c of document.querySelectorAll("[data-set]")) {
    if (c === document.activeElement || c.dataset.busy) continue;
    const v = state.settings[c.dataset.set];
    if (c.type === "checkbox") c.checked = !!v; else c.value = String(v);
    c.disabled = !online || (c.dataset.set === "palette" && state.settings.gbc);
    if (c.type === "range") rangeFill(c);
  }
}

function renderTimer() {
  if (!state.timer) return;
  timerAt = Date.now();
  const t = state.timer;
  $("timerGo").textContent = t.running ? "Stop" : "Start";
  $("timerState").textContent = t.done ? "Finished" : t.running ? (t.countdown ? "Counting down" : "Stopwatch running") : (t.countdown ? "Countdown" : "Stopwatch");
  for (const c of document.querySelectorAll("#timerChips .chip")) {
    const s = Number(c.dataset.s);
    c.setAttribute("aria-pressed", s ? (t.countdown && t.set_s === s) : !t.countdown);
    c.disabled = !online || t.running;
  }
  $("timerGo").disabled = $("timerReset").disabled = !online;
  tickTimer();
}

function renderWifi() {
  if (!state.wifi) return;
  const w = state.wifi, list = $("wifiList"), shape = JSON.stringify(w);
  if (list.dataset.shape === shape) return;
  list.dataset.shape = shape;
  list.textContent = "";
  list.append(el("div", "card-label", "Networks"));
  const nets = w.networks.length ? w.networks : (w.ssid ? [{ ssid: w.ssid, locked: true }] : []);
  if (!nets.length) {
    const e = el("div", "empty small");
    e.append(el("b", "", "No networks seen"));
    list.append(e);
  }
  for (const n of nets) {
    const here = n.ssid === w.ssid && w.phase === "online";
    const b = el("button", "row tap net");
    b.innerHTML = `<span class="well">${icon("wifi-high")}</span>`;
    b.append(el("span", "main title", n.ssid));
    if (here) b.append(el("span", "tag ok", "Connected"));
    else if (n.locked) { const l = el("span", "k"); l.style.flex = "none"; l.innerHTML = icon("lock"); l.setAttribute("aria-label", "Locked"); b.append(l); }
    else b.append(el("span", "tag", "Open"));
    b.onclick = () => { $("wifiSsid").value = n.ssid; $("wifiPass").value = ""; (n.locked ? $("wifiPass") : $("wifiJoin")).focus(); };
    list.append(b);
  }
}

function renderDevice(d) {
  $("heroName").textContent = d.name || "Chromatic";
  const st = $("heroState");
  st.textContent = online ? (awake ? "Online" : "Waking") : "Offline";
  st.className = "tag" + (online ? " ok" : "");
  $("heroFw").textContent = d.mcu || "";
  const hasBatt = d.battery !== undefined;
  $("heroBatt").hidden = !hasBatt;
  if (hasBatt) {
    $("heroBattText").textContent = d.charging ? "Battery, charging" : "Battery";
    $("heroBattPct").textContent = d.battery + "%";
    $("heroMeter").style.setProperty("--v", Math.max(0, Math.min(100, d.battery)) / 100);
    $("heroMeter").classList.toggle("low", d.battery <= 20 && !d.charging);
  }
  const info = $("info"); info.textContent = "";
  info.append(row("Name", d.name || "—"), row("Battery", hasBatt ? d.battery + "%" + (d.charging ? ", charging" : "") : "—"),
    row("Firmware", d.mcu || "—"), row("FPGA", d.fpga || "—"));
  paintUpdate(d);
}

/* ---------- firmware updates over the air ---------- */
const fw = { for: "", info: null };
const UPDATE_PHASE = { download: "Updating", verify: "Checking", restart: "Restarting", finish: "Finishing" };
const shortVersion = v => (v || "").replace(/^v?0\.13\.4-/, "") || v;
function paintUpdate(d) {
  const current = (d.mcu || "").replace(/^v/, "");
  if (current && fw.for !== current) {
    fw.for = current;
    fw.info = null;
    fetch("/api/firmware?current=" + encodeURIComponent(current), { cache: "no-store" })
      .then(r => r.ok ? r.json() : null).then(m => { fw.info = m; if (state) paintUpdate(state.device); }).catch(() => {});
  }
  const u = state.update && state.update.phase && state.update.phase !== "idle" ? state.update : null;
  const newer = !!(fw.info && fw.info.newer);
  $("updateCard").hidden = !newer && !u;
  if ($("updateCard").hidden) return;
  const btn = $("updateBtn"), meter = $("updateMeter"), desc = $("updateDesc");
  const busy = u && u.phase !== "failed";
  meter.hidden = !busy;
  btn.hidden = busy;
  desc.hidden = true;
  if (busy) {
    const pct = Number.isFinite(u.percent) ? u.percent : 0;
    const counted = u.phase === "download" || u.phase === "finish";
    $("updateTitle").textContent = (UPDATE_PHASE[u.phase] || "Updating") + (counted ? " " + pct + "%" : "");
    meter.style.setProperty("--v", counted ? pct / 100 : 1);
    return;
  }
  if (u) {
    $("updateTitle").textContent = "Update failed";
    desc.textContent = u.error || "Try again.";
    desc.hidden = false;
    btn.textContent = "Retry";
  } else {
    const t = $("updateTitle");
    t.textContent = "Update available";
    const v = document.createElement("span");
    v.className = "ver";
    v.textContent = " · " + shortVersion(fw.info ? fw.info.version : "");
    t.append(v);
    /* Before the USB installer's layout, updates go through /install instead. */
    btn.textContent = d.ota === false ? "Install" : "Update";
  }
  btn.disabled = d.ota !== false && (!online || !newer);
}
$("updateBtn").onclick = async () => {
  if (state && state.device && state.device.ota === false) { location.href = "/install?update"; return; }
  const b = $("updateBtn");
  b.classList.add("busy"); b.disabled = true;
  try { await api("POST", "/api/update"); say("Update started"); refresh(); }
  catch (e) { say(e.message, "err"); }
  b.classList.remove("busy");
  if (state) paintUpdate(state.device);
};

/* ---------- alarms: four slots; tapping a row edits it below the list ---------- */
const DAY = ["M", "T", "W", "T", "F", "S", "S"], DAY_NAME = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
let alarmSel = -1;
DAY.forEach((d, i) => {
  const c = el("button", "day", d);
  c.setAttribute("aria-label", DAY_NAME[i]); c.setAttribute("aria-pressed", "false");
  c.onclick = () => c.setAttribute("aria-pressed", c.getAttribute("aria-pressed") === "true" ? "false" : "true");
  $("alarmDays").append(c);
});
function hm(h, m) {
  if (!state || !state.clock || !state.clock.hour12) return pad(h) + ":" + pad(m);
  return ((h % 12) || 12) + ":" + pad(m) + (h < 12 ? " AM" : " PM");
}
function repeatText(d) {
  if (!d) return "Once";
  if (d === 127) return "Every day";
  if (d === 31) return "Weekdays";
  if (d === 96) return "Weekends";
  return DAY_NAME.filter((_, i) => d >> i & 1).join(" ");
}
function renderAlarms() {
  const list = state.alarms || [], box = $("alarms");
  const shape = JSON.stringify([list, alarmSel, online, state.clock && state.clock.hour12]);
  if (box.dataset.shape === shape) return;
  box.dataset.shape = shape;
  box.textContent = "";
  list.forEach((a, i) => {
    const r = el("div", "alarm" + (a.on ? "" : " off"));
    r.setAttribute("aria-current", i === alarmSel);
    const b = el("button");
    const sub = el("span", "sub");
    if (a.label) sub.append(el("b", "", a.label));
    sub.append(el("span", "", repeatText(a.days)));
    b.append(el("span", "at", hm(a.hour, a.minute)), sub);
    b.onclick = () => {
      alarmSel = alarmSel === i ? -1 : i;
      fillAlarm();
      renderAlarms();
      if (alarmSel >= 0) $("alarmEdit").scrollIntoView({ block: "nearest", behavior: matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth" });
    };
    const sw = el("label", "switch");
    const inp = el("input"); inp.type = "checkbox"; inp.checked = a.on; inp.disabled = !online;
    inp.setAttribute("aria-label", "Alarm " + (i + 1) + " on");
    inp.onchange = async () => { if (!(await setAlarm(i, { ...a, on: inp.checked }))) inp.checked = a.on; };
    sw.append(inp, el("span"));
    r.append(b, sw);
    box.append(r);
  });
  $("alarmEdit").hidden = alarmSel < 0;
}
function fillAlarm() {
  const a = (state.alarms || [])[alarmSel];
  if (!a) return;
  $("alarmEditTitle").textContent = "Alarm " + (alarmSel + 1);
  $("alarmAt").value = pad(a.hour) + ":" + pad(a.minute);
  $("alarmLabel").value = a.label || "";
  [...$("alarmDays").children].forEach((c, i) => c.setAttribute("aria-pressed", String(!!(a.days >> i & 1))));
}
async function setAlarm(index, a) {
  try {
    await api("PUT", "/api/alarm", { index, on: a.on, hour: a.hour, minute: a.minute, days: a.days, label: a.label || "" });
    await refresh();
    return true;
  } catch (e) { say(e.message, "err"); return false; }
}
$("alarmCancel").onclick = () => { alarmSel = -1; renderAlarms(); };

/* ---------- note editor ---------- */
function openEditor(i) {
  editing = i;
  const isNew = i >= state.notes.items.length;
  $("text").value = isNew ? "" : state.notes.items[i];
  $("sheetTitle").textContent = isNew ? "New note" : "Note";
  $("delete").hidden = $("pin").hidden = isNew;
  $("pinText").textContent = i < (state.notes.pinned || 0) ? "Unpin" : "Pin";
  $("scrim").classList.add("open");
  $("sheet").classList.add("open");
  count();
  setTimeout(() => $("text").focus(), 240);
}
function closeEditor() {
  editing = -1;
  $("scrim").classList.remove("open");
  $("sheet").classList.remove("open");
  $("text").blur();
  refresh();
}
function count() {
  const kept = clean($("text").value).length, cap = state ? state.notes.capacity : 200;
  $("count").textContent = Math.min(kept, cap) + " / " + cap;
}
async function saveNotes(items, pinned, done, action) {
  try {
    await api("PUT", "/api/notes", { rev: state.notes.rev, items, pinned });
    say(done, undefined, action);
    closeEditor();
    return true;
  } catch (e) {
    if (e.status === 409) {
      const keep = editing; editing = -1; await refresh(); editing = keep;
      say("Changed on the Chromatic. Your text is kept; save again", "err");
    } else say(e.message, "err");
    return false;
  }
}

/* ---------- pairing: cells mirror one real input, so typing, paste and autofill work ---------- */
function paintCode() {
  const digits = $("code").value.replace(/\D/g, "").slice(0, 6);
  const cells = document.querySelectorAll("#cells .cell");
  cells.forEach((c, i) => { c.textContent = digits[i] || ""; c.classList.toggle("cur", document.activeElement === $("code") && i === Math.min(digits.length, 5)); });
  $("pairBtn").disabled = digits.length !== 6;
  return digits;
}
$("code").addEventListener("input", () => { if (paintCode().length === 6) $("pairBtn").click(); });
$("code").addEventListener("focus", paintCode);
$("code").addEventListener("blur", paintCode);
$("code").addEventListener("keydown", e => { if (e.key === "Enter") $("pairBtn").click(); });
$("pairBtn").onclick = async () => {
  const code = paintCode(), b = $("pairBtn");
  if (code.length !== 6 || b.dataset.busy) return;
  b.dataset.busy = "1";
  b.classList.add("busy");
  try {
    const r = await api("POST", "/api/pair", { code });
    token = r.token; localStorage.setItem(TOKEN, token);
    $("code").value = ""; paintCode(); say("Chromatic paired"); show();
  } catch (e) { say(e.message, "err"); $("code").select(); }
  delete b.dataset.busy;
  b.classList.remove("busy");
};

/* ---------- actions ---------- */
for (const b of document.querySelectorAll(".seg .tab")) b.onclick = () => setTab(b.dataset.tab);
$("newNote").onclick = () => openEditor(state.notes.items.length);
$("cancel").onclick = closeEditor;
$("scrim").onclick = closeEditor;
/* Keyboard: Esc closes the editor, Cmd/Ctrl+Enter saves, N starts a note. */
document.addEventListener("keydown", e => {
  if (editing >= 0) {
    if (e.key === "Escape") closeEditor();
    else if (e.key === "Enter" && (e.metaKey || e.ctrlKey) && !$("save").disabled) { e.preventDefault(); $("save").click(); }
    return;
  }
  const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement && document.activeElement.tagName);
  if (!typing && token && tab === "notes" && (e.key === "n" || e.key === "N") && !e.metaKey && !e.ctrlKey && !$("newNote").disabled) {
    e.preventDefault(); $("newNote").click();
  }
});
$("text").oninput = count;
$("save").onclick = () => {
  const text = clean($("text").value).trim().slice(0, state.notes.capacity);
  if (!text) { say("Write something first", "err"); return; }
  const items = state.notes.items.slice(), pinned = state.notes.pinned || 0;
  /* New notes go first, below the pinned ones, as on the Chromatic. */
  if (editing >= items.length) items.splice(pinned, 0, text); else items[editing] = text;
  saveNotes(items, pinned, "Note saved");
};
/* Deleting is immediate and offers Undo, which puts the note back where it was. */
$("delete").onclick = () => {
  const at = editing, items = state.notes.items.slice(), pinned = state.notes.pinned || 0;
  const [text] = items.splice(at, 1);
  saveNotes(items, at < pinned ? pinned - 1 : pinned, "Note deleted", {
    label: "Undo",
    run: async () => {
      await refresh();
      const back = state.notes.items.slice(), p = state.notes.pinned || 0, wasPinned = at < pinned;
      back.splice(Math.min(at, back.length), 0, text);
      try { await api("PUT", "/api/notes", { rev: state.notes.rev, items: back, pinned: wasPinned ? p + 1 : p }); say("Note restored"); refresh(); }
      catch (e) { say(e.message, "err"); }
    },
  });
};
/* Pinning moves a note to the end of the pinned block; unpinning to the top of the rest. */
$("pin").onclick = () => {
  const items = state.notes.items.slice();
  let pinned = state.notes.pinned || 0;
  const typed = clean($("text").value).trim().slice(0, state.notes.capacity);
  const [note] = items.splice(editing, 1);
  const text = typed || note;
  if (editing < pinned) { pinned--; items.splice(pinned, 0, text); saveNotes(items, pinned, "Note unpinned"); }
  else { items.splice(pinned, 0, text); saveNotes(items, pinned + 1, "Note pinned"); }
};
$("export").onclick = () => {
  const items = state.notes.items;
  const body = items.map((t, i) => (i < (state.notes.pinned || 0) ? "[Pinned] " : "") + t).join("\n\n") + "\n";
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([body], { type: "text/plain" }));
  a.download = (state.device.name || "Chromatic").replace(/\s+/g, "-") + "-notes.txt";
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  say("Notes exported");
};
$("alarmSave").onclick = async () => {
  const [h, m] = ($("alarmAt").value || "").split(":").map(Number);
  if (!(h >= 0 && h <= 23 && m >= 0 && m <= 59)) { say("Pick a time", "err"); return; }
  const days = [...$("alarmDays").children].reduce((d, c, i) => d | (c.getAttribute("aria-pressed") === "true" ? 1 << i : 0), 0);
  const label = clean($("alarmLabel").value).trim().slice(0, 12);
  /* Saving an alarm turns it on: that is almost always why it was edited. */
  if (await setAlarm(alarmSel, { on: true, hour: h, minute: m, days, label })) { say("Alarm saved"); alarmSel = -1; renderAlarms(); }
};
$("clockSet").onclick = async () => {
  try { await api("PUT", "/api/clock", { zone_minutes: -new Date().getTimezoneOffset() }); say("Clock synced"); setTimeout(refresh, 400); }
  catch (e) { say(e.message, "err"); }
};
/* Settings apply on change; a refusal puts the control back. */
for (const c of document.querySelectorAll("[data-set]")) {
  if (c.type === "range") c.addEventListener("input", () => rangeFill(c));
  c.addEventListener("change", async () => {
    const key = c.dataset.set;
    const value = c.type === "checkbox" ? c.checked : Number(c.value);
    c.dataset.busy = "1";
    try { await api("PUT", "/api/settings", { key, value }); state.settings[key] = value; }
    catch (e) { say(e.message, "err"); if (c.type === "checkbox") c.checked = !value; else c.value = String(state.settings[key]); if (c.type === "range") rangeFill(c); }
    delete c.dataset.busy;
  });
}
for (const c of document.querySelectorAll("#timerChips .chip")) {
  c.onclick = async () => {
    const s = Number(c.dataset.s);
    try { await api("POST", "/api/timer", { action: "set", countdown: s > 0, seconds: s || 0 }); refresh(); }
    catch (e) { say(e.message, "err"); }
  };
}
$("timerGo").onclick = async () => {
  try { await api("POST", "/api/timer", { action: state.timer.running ? "stop" : "start" }); refresh(); }
  catch (e) { say(e.message, "err"); }
};
$("timerReset").onclick = async () => {
  try { await api("POST", "/api/timer", { action: "reset" }); refresh(); }
  catch (e) { say(e.message, "err"); }
};
$("wifiJoin").onclick = async () => {
  const ssid = $("wifiSsid").value.trim(), password = $("wifiPass").value;
  if (!ssid) { say("Enter a network name", "err"); return; }
  if (password && password.length < 8) { say("Wi-Fi passwords have at least 8 characters", "err"); return; }
  try { await api("POST", "/api/wifi", { ssid, password }); $("wifiPass").value = ""; say("Joining " + ssid, "info"); }
  catch (e) { say(e.message, "err"); }
};
$("findMe").onclick = async () => {
  try { await api("POST", "/api/message", { text: "", find: true }); say("Flashing on your Chromatic", "info"); }
  catch (e) { say(e.message, "err"); }
};
$("msgSend").onclick = async () => {
  const text = $("msgText").value.trim();
  if (!text) { say("Write a message first", "err"); return; }
  try { await api("POST", "/api/message", { text }); $("msgText").value = ""; say("Message shown"); }
  catch (e) { say(e.message, "err"); }
};
$("unpair").onclick = async () => {
  if (!confirm("Unpair this browser?")) return;
  try { await api("POST", "/api/unpair"); } catch (e) {}
  swReady.then(r => r && r.pushManager.getSubscription()).then(sub => sub && sub.unsubscribe()).catch(() => {});
  forget(); say("Browser unpaired");
};

/* ---------- installing and notifications (iPhone allows push only in the installed app) ---------- */
const pushable = "serviceWorker" in navigator && "PushManager" in window && "Notification" in window;
const standalone = matchMedia("(display-mode: standalone)").matches || navigator.standalone === true;
const ios = /iPhone|iPad|iPod/.test(navigator.userAgent) || (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
let installEvent = null;
const swReady = "serviceWorker" in navigator ? navigator.serviceWorker.register("/sw.js").then(() => navigator.serviceWorker.ready).catch(() => null) : Promise.resolve(null);
addEventListener("beforeinstallprompt", e => { e.preventDefault(); installEvent = e; $("installRow").hidden = false; });
addEventListener("appinstalled", () => { $("installRow").hidden = true; say("App installed"); });
$("install").onclick = async () => { if (!installEvent) return; installEvent.prompt(); await installEvent.userChoice; installEvent = null; $("installRow").hidden = true; };
function b64(key) {
  const s = atob(key.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - key.length % 4) % 4));
  return Uint8Array.from(s, c => c.charCodeAt(0));
}
async function paintNotify() {
  const reg = await swReady;
  const sub = reg && pushable ? await reg.pushManager.getSubscription() : null;
  $("notify").checked = !!sub && Notification.permission === "granted";
  $("notify").disabled = !pushable;
  $("notifyHint").textContent = !pushable ? (ios && !standalone ? "Add to Home Screen first, then open it from there" : "Not supported in this browser")
    : Notification.permission === "denied" ? "Blocked in browser settings" : "Alarms, timers and low battery";
}
$("notify").onchange = async () => {
  const on = $("notify").checked;
  try {
    const reg = await swReady;
    if (!reg) throw new Error("Not supported in this browser");
    let sub = await reg.pushManager.getSubscription();
    if (on) {
      if (await Notification.requestPermission() !== "granted") throw new Error("Notifications are blocked");
      const { key } = await api("GET", "/api/push");
      sub = sub || await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: b64(key) });
      await api("POST", "/api/push", { subscription: sub.toJSON() });
      say("Notifications on");
    } else {
      await api("DELETE", "/api/push");
      if (sub) await sub.unsubscribe();
      say("Notifications off");
    }
  } catch (e) { say(e.message, "err"); }
  paintNotify();
};
paintNotify();
document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });
setInterval(() => { if (!document.hidden) refresh(); }, 4000);
paintCode();
show();
