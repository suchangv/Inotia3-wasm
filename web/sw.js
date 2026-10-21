// Offline-capable app shell for the Inotia 3 wasm port.
//
// The page needs HTTPS (or localhost) for this to register at all, which GitHub
// Pages provides. What it buys: the 660 KB module and the shell are served from
// the device after the first visit, so a flaky connection does not cost a
// re-download, and the installed PWA opens without a network round trip.
//
// Deliberately *not* cached: the user's APK and saves. Those live in IndexedDB
// (APK, and /data through IDBFS), are large, and must never be duplicated into
// the Cache API where a version bump would silently drop them.

const VERSION = 'inotia3-wasm-v8';
const SHELL = [
  './',
  './index.html',
  './inotia3.js',
  './inotia3.wasm',
  './vorbis.js',
  './vorbis.wasm',
  './manifest.webmanifest',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/icon-512-maskable.png',
  './icons/apple-touch-icon.png'
];

self.addEventListener('install', (e) => {
  e.waitUntil((async () => {
    const cache = await caches.open(VERSION);
    // addAll is all-or-nothing; a single 404 would leave the shell empty, so add
    // one by one and let a missing icon be a missing icon.
    await Promise.all(SHELL.map(async (url) => {
      try { await cache.add(new Request(url, { cache: 'reload' })); }
      catch (err) { console.warn('[sw] could not cache ' + url, err); }
    }));
    self.skipWaiting();
  })());
});

self.addEventListener('activate', (e) => {
  e.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;      // Bing, GitHub links: leave alone

  // Navigations go to the network first: a stale page paired with a new module
  // is the one mismatch that produces confusing errors, and the shell is tiny.
  if (req.mode === 'navigate') {
    e.respondWith((async () => {
      try {
        const fresh = await fetch(req);
        const cache = await caches.open(VERSION);
        cache.put('./index.html', fresh.clone());
        return fresh;
      } catch (err) {
        return (await caches.match('./index.html')) || (await caches.match('./')) ||
               Response.error();
      }
    })());
    return;
  }

  // The module itself is network-first. It is the one asset whose stale copy
  // produces a page that half works -- the HTML is updated by the navigation
  // path above, so a cached module leaves the two out of step and the symptom is
  // a contradiction in the UI rather than an error. The browser's own HTTP cache
  // absorbs the cost of asking.
  const isModule = url.pathname.endsWith('/inotia3.js') ||
                   url.pathname.endsWith('/inotia3.wasm');
  if (isModule) {
    e.respondWith((async () => {
      try {
        const fresh = await fetch(req);
        if (fresh.ok) {
          const cache = await caches.open(VERSION);
          cache.put(req, fresh.clone());
        }
        return fresh;
      } catch (err) {
        return (await caches.match(req)) || Response.error();
      }
    })());
    return;
  }

  // Everything else is versioned by the build that shipped it: cache first, then
  // network (and cache what comes back).
  e.respondWith((async () => {
    const hit = await caches.match(req);
    if (hit) return hit;
    try {
      const res = await fetch(req);
      if (res.ok && res.type === 'basic') {
        const cache = await caches.open(VERSION);
        cache.put(req, res.clone());
      }
      return res;
    } catch (err) {
      return Response.error();
    }
  })());
});
