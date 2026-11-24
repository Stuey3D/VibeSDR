# The page's HTTP goes direct (beta93, 2026-11-24)

## Why
Every `<slug>.vibeserver.vibesdr.net` address is served by the directory's Cloudflare Worker
(`directory/src/index.js` `serveBySlug`). It proxies the page so the page sits on a stable origin: a Quick
Tunnel's hostname changes on every restart, and a browser keys localStorage by origin. The WebSocket has
always gone straight to the tunnel (`__VIBE_DIRECT_HOST__` / `<meta name="vibe-direct-host">`).

Until beta93, every HTTP request the page made after loading also went to its own origin, and so through
the Worker. Each one is a billed Worker request. On 24 Nov a client bug (fixed in e375ccafc) made one page
fetch `/vibeserver/dxlog.json` about 6,500 times a minute. That used up the account's Workers allowance
and took every tunnel down.

Stuart: *"the gallery etc should all be handled via the free tunnel not the main workers — they should
only establish the connection."*

## What goes where now
| Request | Before | beta93 server | beta93 server + new directory |
|---|---|---|---|
| The HTML document | Worker | Worker | Worker (it has to be: localStorage) |
| `/vs/` scripts, Leaflet, MapLibre scripts and CSS | Worker | Worker | **direct** |
| vibeserver.json, /r/<id>/vibeserver.json, radios, auth, unlock | Worker | **direct** | **direct** |
| /vibeserver/* (gallery lists and files, telemetry, skyplot, DX Log + alerts, conditions, spectrogram, logos, DAB slides, aircraft, icom power, admin, admin-ticket) | Worker | **direct** | **direct** |
| /bookmarks, /stations, /location, /mapdata, /mapgl (style, fonts, PMTiles ranges), /adsb images, /icon-512.png | Worker | **direct** | **direct** |
| WebSockets | direct | direct | direct |
| /sw.js, /manifest.webmanifest, /favicon.png | Worker | Worker | Worker |
| A re-learn after the tunnel rotated | — | one vibeserver.json, rate-limited | same |

The last three rows stay on the Worker on purpose. A service worker must be same-origin. A manifest's
`start_url` must be same-origin with the document. The favicon is fetched by the browser itself, once per
visit, and cached for an hour.

## How
- **Client, one helper:** `web/client/src/origin.ts`.
  - `dataUrl(path)` sends a path (or a URL on the page's own origin) to `https://<direct host>` when the
    page was proxied. Otherwise it returns the path unchanged.
  - `httpBase(host)` swaps the authority when `host` is the page's own host.
  - main.ts `P()` and `galUrl()` go through `dataUrl()`. So do the map, ADS-B, popup-map and pocket bases.
  - `scripts/test-web-direct-data.mjs` counts what reaches a stand-in Worker. A new feature that builds a
    relative URL fails it.
- **Scripts:** the build's head loader (`scripts/build-web.mjs`) loads `/vs/` from the direct host only
  when `<meta name="vibe-direct-assets">` is present. The new directory writes that meta only when the
  receiver's own CSP names the host in `script-src`. A beta93+ server adds the host when the request came
  through the directory (`vibe_cors.h` `cspDirect`, using `x-vibesdr-direct-host` or the tunnel's Host).
  The upshot is that a newer directory never breaks an older server's page. A module that fails to load
  from the direct host falls back to the Worker.
- **Server CORS:** `android/app/src/main/cpp/vibe_cors.h`, decided once per request in `handleConnection`.
  - The socket rewrites the head of the answer (`net_shim` `setCorsHead`), so no route decides for itself.
  - A slug origin (`https://<label>.vibeserver.vibesdr.net` or the directory) gets its own origin echoed,
    plus `Vary: Origin`. It never gets `*`.
  - A foreign origin on a request that carries a credential (`vs_nonce`/`vs_auth`, `vs_admin_*`, `tok`) or
    reaches admin/setup/config gets **no** `Access-Control-Allow-Origin`. Admin answers used to say `*`.
  - Everything else is unchanged. The public `*` on vibeserver.json stays.
  - `OPTIONS` from a slug origin gets a 204 preflight (Range, Content-Type, If-None-Match).
  - Nothing relies on cookies: VibeServer has none, and the Worker strips them both ways.
- **The tunnel rotates:** a direct fetch that fails at the network layer (watched once, in origin.ts), or a
  socket lost with 1006, asks the Worker for `/vibeserver.json` **once**. Its `directHost` is the new host.
  - If the host moved, the page reloads onto it.
  - If it did not move, the next try waits 1, 2, 4… minutes (at most 30).
  - It never falls back to proxying.
- **Worker:** proxies everything as before. Nothing is refused, because older servers' pages still read
  through it. Every slug request's log line now carries `slug: doc | status | furniture | legacy`.
  `legacy` is an older server's page. When it reaches zero, the Worker can stop proxying anything but the
  document.

## What it does not cover
- **The apps:** `src/services/sdrTypes.ts` `vibeDirectUrl`, and the watch's directory code.
  - A listing connects straight to the tunnel URL.
  - A typed slug costs one probe and one vibeserver.json through the Worker. After that, everything is
    direct.
  - No change was needed.
- **An owner who fronts VibeServer with their own Cloudflare and a rule that refuses foreign Origins:** the
  Worker strips Origin on status reads for exactly that case. The page's direct reads carry the slug
  Origin, as its WebSocket always has. Such a rule would refuse them. Not seen yet.
- **The R36S handheld page:** its own `/handheld/*` reads are localhost and never proxied.
