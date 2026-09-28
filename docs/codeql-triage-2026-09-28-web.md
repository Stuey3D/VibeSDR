# CodeQL triage — JavaScript / TypeScript / HTML, 2026-09-28

Scope: `directory/`, `web/client/src/`, `website/`, `src/` (app TS). Source of truth: the open alerts on
analysis 1854037380 (javascript-typescript, 50 results), with the SARIF code flows read for every alert.
Nothing here has been dismissed on GitHub — the lead applies these.

## A. Fixed in code (expect these to close on the next scan)

| # | Rule | Where | Fix |
|---|------|-------|-----|
| 49 | js/xss | directory/public/index.html:2954 | EiBi frequency `row[0]` now goes through `esc()` like every other field in the row. |
| 50 | js/xss | directory/public/index.html:3285 | `stationPick.khz` (the same EiBi field) now `esc()`-ed in the ranking header. |
| 31 | js/xss | website/index.html:1105 | Release `html_url` from api.github.com is used only if it starts `https://github.com/Stuey3D/VibeSDR/`; otherwise the markup's releases-page href stays. |
| 25 | js/client-side-unvalidated-url-redirection | website/index.html:1105 | Same fix as #31. |
| 89 | js/tainted-format-string | directory/public/store.html:142 | `console.error('portable store: %s failed', String(d.op), err)` — the message-supplied op is an argument, not part of the format string. Message protocol and cookie format untouched. |
| 4 | js/bad-code-sanitization | src/components/BrowserOverlay.tsx:147 | After `JSON.stringify`, `< > /` and U+2028/2029 are escaped as `\uXXXX` so the literal cannot close a script or break a line (round-trip tested: `eval(css) === injectCSS`). |
| 32, 26 | js/xss, redirect | web/client/src/main.ts:3702 | New `webImageUrl()` (http/https via `new URL`, else '') applied where the logo enters: the server's `/vibeserver/stationlogo` answer and the name-search result. |
| 41, 29 | js/xss, redirect | web/client/src/main.ts:9591 | Same source guard as #32 (these sinks read the same `rdsLogoUrl`). |
| 40, 27 | js/xss, redirect | web/client/src/main.ts:8562 | The localStorage frequency-logo cache is passed through `webImageUrl()` on read. |
| 28 | redirect | web/client/src/main.ts:8580 | Bookmark-row logo passed through `webImageUrl()` before `img.src`. |

If a rescan still reports any of the main.ts logo alerts (CodeQL does not model a `new URL().protocol`
check as a sanitiser), dismiss as **false positive**: every sink is an `<img>.src`, and every value now
passes `webImageUrl()`, which admits only http:/https:.

## B. Hardened, then to dismiss

| # | Rule | Where | Reason | Justification |
|---|------|-------|--------|---------------|
| 3 | js/insecure-download | src/services/directories.ts:78 | won't fix | rx.linkfanel.net has no HTTPS at all (port 443 refuses, measured 2026-09-28); the list is parsed as JSON data, never executed, names lose `<>`, and rows whose url is not plain `http(s)://host` are now dropped (855/855 live rows pass). |
| 22 | js/request-forgery | src/services/radiodns.ts:53 | false positive | The SPI host is the broadcaster's own SRV target — that is the RadioDNS protocol — and it is now required to be a plain DNS host name with a valid port before it becomes a URL authority; the path is fixed and no credentials are sent. |

## C. Dismiss — directory page (host is always a directory-issued `<slug>.vibeserver.vibesdr.net`)

The worker builds `address` as `${slugify(name)}.vibeserver.vibesdr.net` (directory/src/index.js:612,
slugify = a-z0-9 and dashes) and the page now also nulls any `address` not matching that pattern
(`ownAddresses()` in `load()`), so none of these can be pointed outside our own zone.

| # | Rule | Where | Reason | Justification |
|---|------|-------|--------|---------------|
| 53 | js/request-forgery | directory/public/index.html:1155 | false positive | The PIN challenge `GET https://<address>/vibeserver/auth` goes to the server the visitor typed a PIN for, whose address is a directory-issued slug on our own zone. |
| 61 | js/request-forgery | directory/public/index.html:1160 | false positive | `/vibeserver/unlock` carries only an HMAC of the PIN over that server's single-use nonce, sent back to the same directory-issued host that issued the nonce. |
| 62 | js/request-forgery | directory/public/index.html:1179 | false positive | `/vibeserver/auth/verify` is the same nonce-bound proof to the same slug host, only on a server that published `pin: true`. |
| 55 | js/request-forgery | directory/public/index.html:1205 | false positive | Fresh nonce for re-reading an unlocked server's radios, from the directory-issued host the PIN was entered for. |
| 56 | js/request-forgery | directory/public/index.html:1208 | false positive | `/vibeserver/radios` read from that same slug host with a nonce-bound proof; the reply only updates rows whose ids the PIN opened. |
| 57 | js/request-forgery | directory/public/index.html:1229 | false positive | Per-radio nonce from `https://<slug host>/r/<encodeURIComponent(id)>`, the radio card the visitor clicked; the id cannot escape the path. |
| 66 | js/request-forgery | directory/public/index.html:3053 | false positive | Admin sign-in challenge to the server whose card holds the admin box; host matched from `ALL` by address, never from the form. |
| 67 | js/request-forgery | directory/public/index.html:3056 | false positive | `/vibeserver/admin-ticket` sends an HMAC of the password over the nonce to the same directory-issued host; the password itself never leaves the page. |
| 63 | js/request-forgery | directory/public/index.html:3125 | false positive | Whole-server PIN nonce, from the slug host of the card the visitor clicked (matched from `ALL`). |
| 20 | js/request-forgery | directory/public/index.html:3368 | false positive | Live listener count `GET https://<slug host>/vibeserver.json`, no credentials, 4 s timeout, reply read only for numbers. |
| 21 | js/request-forgery | directory/public/index.html:3400 | false positive | Per-radio `/r/<encoded id>/vibeserver.json` on the same slug host, bounded to 6 radios, reply read only for numbers/booleans. |
| 58 | js/client-side-unvalidated-url-redirection | directory/public/index.html:1226 | false positive | Opens `radioUrl()` = `https://<slug host>/r/<encoded id>/?join=1`, or the slug host root — the receiver the visitor clicked. |
| 88 | js/client-side-unvalidated-url-redirection | directory/public/index.html:1239 | false positive | Same `radioUrl()` target with the fresh proof in the fragment and the ticket as an encoded query value — the host part is fixed. |
| 60 | js/client-side-unvalidated-url-redirection | directory/public/index.html:1243 | false positive | Fallback to the same `radioUrl()` target without the proof when the nonce fetch fails. |
| 64 | js/client-side-unvalidated-url-redirection | directory/public/index.html:3127 | false positive | Opens `https://<slug host>/` with an encoded nonce/proof query — the whole-server card the visitor clicked. |
| 65 | js/client-side-unvalidated-url-redirection | directory/public/index.html:3130 | false positive | Fallback to `https://<slug host>/` when the nonce fetch fails. |

## D. Dismiss — web client (host is the page's own origin)

Every flow starts from `search.ts:82` only because CodeQL taints the whole localStorage prefs blob once
a bookmark's `mode` is written into it; the value that actually reaches these URLs is `host`, which
`initSplash()` sets to `location.host + BASE_PATH` (the page's own server). Only the Mac dev server on
port 8080 shows a typed host field, and there the host is the developer's own entry. WebSocket URLs may
use `__VIBE_DIRECT_HOST__`, which only our directory Worker injects (the verified tunnel of that server).

| # | Rule | Where | Reason | Justification |
|---|------|-------|--------|---------------|
| 82 | js/request-forgery | web/client/src/main.ts:338 | false positive | Admin-connect `/vibeserver.json` probe to `httpBase(host)`, where host is the page's own origin (or the dev field). |
| 83 | js/request-forgery | web/client/src/main.ts:427 | false positive | `/vibeserver.json` capability read from the page's own server via `httpBase(host)`. |
| 84 | js/request-forgery | web/client/src/main.ts:432 | false positive | `/vibeserver/auth` PIN challenge to the page's own server. |
| 85 | js/request-forgery | web/client/src/main.ts:622 | false positive | `loadAudioPolicy()` reads `/vibeserver.json` from the `httpBase(host)` computed at connect for the page's own server. |
| 86 | js/request-forgery | web/client/src/main.ts:978 | false positive | The spectrum probe WebSocket goes to `wsBase(host)` — the page's own server, or the tunnel our Worker injected for it. |
| 87 | js/request-forgery | web/client/src/main.ts:8842 | false positive | `/location` read from the page's own server to place the receiver on the map. |
| 81 | js/request-forgery | web/client/src/spectrum.ts:623 | false positive | `this.url` is the spectrum socket URL built in `connect()` from `wsBase(host)` — the page's own receiver. |
| 70 | js/request-forgery | web/client/src/decoders.ts:122 | false positive | Decoder socket built by `new DecoderClient(host, …)` from the same connect-time host. |
| 69 | js/request-forgery | web/client/src/auth.ts:39 | false positive | `fetchAuthChallenge(base)` is only called with the page's own `httpBase(host)`. |
| 71 | js/request-forgery | web/client/src/admin.ts:71 | false positive | Admin GET to `base()` = `httpBase(host)`, host being `openAdmin(currentHost)` — the connected server — with a fresh nonce-bound admin proof. |
| 72 | js/request-forgery | web/client/src/admin.ts:76 | false positive | Admin POST to the same connected-server `base()`; `path` is a code constant. |
| 73 | js/request-forgery | web/client/src/admin.ts:756 | false positive | `/vibeserver/radios` on `machineBase()`, which is the same host with its `/r/<id>` suffix stripped. |
| 74 | js/request-forgery | web/client/src/admin.ts:773 | false positive | Per-radio admin read on `machineBase()` with the serial `encodeURIComponent`-ed from that server's own radio list. |
| 75 | js/request-forgery | web/client/src/admin.ts:795 | false positive | Admin-ticket mint on the connected server's `base()`. |
| 76 | js/request-forgery | web/client/src/admin.ts:1279 | false positive | Admin-ticket renewal on the same `base()`, at most once per 4 minutes. |
| 77 | js/request-forgery | web/client/src/search.ts:82 | false positive | `/bookmarks` read from `httpBase(host)` of the connected server — the same server whose reply CodeQL then treats as the "source". |
| 78 | js/request-forgery | web/client/src/search.ts:137 | false positive | Bookmark POST to `bmHost`, set only by `loadServerBookmarks(host)` for the connected server; name/mode are `encodeURIComponent`-ed and the frequency is `Math.round`-ed. |
| 79 | js/request-forgery | web/client/src/search.ts:156 | false positive | Bookmark DELETE to the same `bmHost` with a rounded frequency and a numeric sid. |
| 80 | js/request-forgery | web/client/src/search.ts:173 | false positive | `/stations` read from the connected server's `httpBase(host)`. |
