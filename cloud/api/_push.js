// Web Push: browsers that asked for notifications get one when their Chromatic's
// alarm or timer rings or its battery runs low. Events are found by comparing the
// state a device just reported with the one it reported before.
import webpush from "web-push";
import { keys as k, store } from "./_lib.js";

const LOW_BATTERY = 15;
let send = (sub, body) => webpush.sendNotification(sub, body, { TTL: 600, urgency: "high" });
let custom = false;
/* Tests replace the sender. */
export function setSender(fn) { send = fn; custom = true; }

let configured = false;
function configure() {
  if (configured) return true;
  const pub = process.env.VAPID_PUBLIC_KEY, priv = process.env.VAPID_PRIVATE_KEY;
  if (!pub || !priv) return false;
  webpush.setVapidDetails("https://kortamod.app", pub, priv);
  configured = true;
  return true;
}

const pad = n => String(n).padStart(2, "0");

/* What changed between two reported states that is worth a notification. */
export function events(before, after) {
  const out = [];
  if (!after) return out;
  const a0 = before && before.alert, a1 = after.alert;
  if (a1 && a1.kind && (!a0 || a0.kind !== a1.kind)) {
    if (a1.kind === "alarm") {
      const at = after.clock ? new Date((after.clock.epoch + after.clock.zone_minutes * 60) * 1000) : null;
      const time = at ? pad(at.getUTCHours()) + ":" + pad(at.getUTCMinutes()) : "";
      out.push({ title: a1.label || "Alarm", body: time ? "Ringing on your Chromatic · " + time : "Ringing on your Chromatic", tag: "alert" });
    } else if (a1.kind === "timer") out.push({ title: "Timer done", body: "On your Chromatic", tag: "alert" });
  }
  const d0 = before && before.device, d1 = after.device;
  if (d1 && Number.isFinite(d1.battery) && d1.battery <= LOW_BATTERY && !d1.charging
      && (!d0 || !Number.isFinite(d0.battery) || d0.battery > LOW_BATTERY || d0.charging)) {
    out.push({ title: "Battery low", body: d1.battery + "% left on your Chromatic", tag: "battery" });
  }
  return out;
}

/* Sends to every subscribed browser of a device; drops subscriptions that are gone. */
export async function notify(id, list) {
  if (!list.length || (!custom && !configure())) return 0;
  const db = store();
  const subs = (await db.hgetall(k.push(id))) || {};
  let sent = 0;
  await Promise.all(Object.entries(subs).flatMap(([hash, raw]) => list.map(async ev => {
    try { await send(JSON.parse(raw), JSON.stringify(ev)); sent++; }
    catch (e) { if (e && (e.statusCode === 404 || e.statusCode === 410)) await db.hdel(k.push(id), hash); }
  })));
  return sent;
}
