// Shows KortaMod notifications and opens the site when one is tapped. Pages are
// always fetched from the network: the site is useless offline anyway.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));

self.addEventListener("push", e => {
  let n = {};
  try { n = e.data ? e.data.json() : {}; } catch { n = { title: "KortaMod", body: e.data && e.data.text() }; }
  e.waitUntil(self.registration.showNotification(n.title || "KortaMod", {
    body: n.body || "", tag: n.tag || "kortamod", renotify: true,
    icon: "/icons/icon-192.png", badge: "/icons/icon-192.png",
  }));
});

self.addEventListener("notificationclick", e => {
  e.notification.close();
  e.waitUntil((async () => {
    const open = await self.clients.matchAll({ type: "window", includeUncontrolled: true });
    if (open.length) return open[0].focus();
    return self.clients.openWindow("/");
  })());
});
