// Service worker mínimo: permite instalar como app e abrir a "casca" do painel sem rede.
// Os dados sempre vêm ao vivo do servidor MQTT (não são guardados aqui).
const CACHE = "biodigestor-v1";
const SHELL = ["./", "./index.html", "./manifest.webmanifest", "./icon-192.png", "./icon-512.png",
  "https://cdn.jsdelivr.net/npm/mqtt@5.10.1/dist/mqtt.min.js",
  "https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.js",
  "https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"];
self.addEventListener("install", e => e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener("activate", e => e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim())));
// Rede primeiro (painel sempre atualizado); cache só se estiver offline
self.addEventListener("fetch", e => {
  if (e.request.method !== "GET") return;
  e.respondWith(fetch(e.request).then(r => { const cp = r.clone(); caches.open(CACHE).then(c => c.put(e.request, cp)); return r; })
    .catch(() => caches.match(e.request)));
});
