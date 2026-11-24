/**
 * origin.ts — build a URL for the server using the scheme the PAGE was served over.
 *
 * ★★★ WHY THIS IS A SHARED HELPER AND NOT A LOCAL FIX. Every one of these URLs used to be written
 *     as `http://${host}` or `ws://${host}` inline. Behind an HTTPS reverse proxy the page loads
 *     over https and then every one of those is BLOCKED as mixed content, so the receiver fails to
 *     connect with nothing wrong at the proxy — it reads as a broken server (TGCFabian, 2026-08-08,
 *     running nginx: "cant use my vibeserver publicly ... doesnt follow the http scheme from the
 *     URL already set").
 *
 * ★★ IT WAS FIXED IN TWO PLACES FIRST, AND THAT WAS THE BUG. There were fifteen: the auth
 *    challenge, the admin override, the bookmarks and stations lookups, the admin page, the
 *    location fetch, vibeserver.json. Fixing the two that were easy to find left the rest to fail
 *    exactly as before, and a page that half-works behind a proxy is harder to diagnose than one
 *    that does not work at all. One helper, so there is nowhere left to miss.
 *
 * ★ The test is the PAGE's protocol, not the host's: an https page may not load ANY http
 *   subresource, wherever it points. On a plain http page these return exactly what the old
 *   inline strings did, so nothing changes for a LAN server.
 */

/**
 * ★★★ WHERE THE SOCKET REALLY GOES, when the page did not come from the receiver.
 *
 * A phone behind CGNAT is published through a Cloudflare Quick Tunnel, whose hostname CHANGES ON
 * EVERY RESTART. An origin that rotates takes localStorage with it, so a browser listener lost
 * every view setting each time the server came back (Stuart, 2026-08-22: "saving the view settings
 * wont be remembered"). ★★ And the obvious cure — a shared vibesdr.net iframe all servers can read
 * — does NOT work: `trycloudflare.com` is on the PUBLIC SUFFIX LIST, so each tunnel hostname is
 * its own site and browsers partition third-party storage per site. Every tunnel gets a different
 * bucket.
 *
 * So the page is served from the server's STABLE address (…vibeserver.vibesdr.net) and told, here,
 * where the receiver actually is. The WEBSOCKET, which carries the audio and the spectrum, goes
 * DIRECT to the tunnel. ★★ Until beta93 HTTP stayed on the stable origin (same-origin, no CORS) — and
 * so crossed the directory's Worker one billed request at a time; since beta93 it goes direct as well
 * (dataUrl, below). Only the document is the Worker's.
 *
 * ★ Absent on a LAN server, a port-forwarded one, or anything reached directly: then this returns
 *   the host it was given and nothing changes.
 */
/* ★★ ALSO READ FROM A <meta name="vibe-direct-host"> (security audit, 2026-10-03). The directory used
 *    to say this only with an injected inline <script>, which forces the receiver's CSP to allow
 *    'unsafe-inline' script. It now injects the meta as well; a page served under a CSP without
 *    'unsafe-inline' still learns the host from the meta. Only a bare host[:port] is believed. */
function directHost(): string {
  if (typeof window === 'undefined') return '';
  if (learntHost) return learntHost;          // ★ re-learnt after the tunnel rotated — see relearnDirectHost()
  const w = (window as any).__VIBE_DIRECT_HOST__;
  const m = typeof document !== 'undefined'
    ? document.querySelector('meta[name="vibe-direct-host"]')?.getAttribute('content') : '';
  const v = String(w || m || '');
  return HOST_SHAPE.test(v) ? v : '';
}
const HOST_SHAPE = /^(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(:\d{1,5})?$/;
let learntHost = '';
function socketHost(host: string): string {
  const direct = directHost();
  if (!direct) return host;
  // ★★ `host` MAY CARRY A /r/<id> PREFIX — see BASE_PATH in main.ts, where the front door routes
  //    several radios by path. Only the authority is swapped; the prefix has to survive or a
  //    multi-radio server would send every socket to the wrong radio.
  const slash = host.indexOf('/');
  return slash === -1 ? direct : direct + host.slice(slash);
}

/* ★★ WHAT MAY STAND WHERE `host` STANDS (CodeQL js/request-forgery #97-#99, 2026-10-05). Every URL in
 *    this file is `scheme://${host}…`, so `host` is an AUTHORITY plus at most the front door's
 *    /r/<id> prefix (BASE_PATH in main.ts) — never a scheme, a userinfo '@' before the host, a query,
 *    a fragment or a backslash. On a receiver the value is location.host + BASE_PATH and always
 *    passes; only the dev page (port 8080) lets a person type one, and its remembered `lastHost`
 *    comes back out of the same prefs blob that server-supplied fields are written into. That
 *    blob is why CodeQL drew a path from a server bookmark to fetch(): not reachable in practice,
 *    but a shape check costs nothing and makes the rule explicit.
 *  ★ The /r/ segment is a URL path segment as location.pathname gives it (already %-encoded), so
 *    anything but '/', '?', '#', '\' and whitespace may appear in it. */
const SERVER_HOST_RE = /^(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9._-]+)(:\d{1,5})?(\/r\/[^/?#\\\s]+)?$/;
export function isValidServerHost(h: unknown): h is string {
  return typeof h === 'string' && h.length <= 300 && SERVER_HOST_RE.test(h);
}

/** True when this page was served over https (so everything it loads must be too). */
export function pageIsSecure(): boolean {
  return typeof location !== 'undefined' && location.protocol === 'https:';
}

/** `https://host` or `http://host`, matching the page.
 *  ★★ EVERY URL TO A RECEIVER IS BUILT HERE, SO THE SHAPE CHECK LIVES HERE (CodeQL js/request-forgery #98, #99,
 *     #101, 2026-10-11). The check above existed but the fetches did not pass through it: bookmarks, stations and
 *     /location each pasted `host` straight into a URL — with the PIN or admin proof appended. A host that is not
 *     a bare authority (plus the front door's /r/<id>) is refused outright, so no proof can be aimed elsewhere. */
export function httpBase(host: string): string {
  if (!isValidServerHost(host)) throw new Error('not a server address');
  return `${pageIsSecure() ? 'https' : 'http'}://${pageDataHost(host)}`;
}
/** ★★ beta93: THIS page's own host (plus /r/<id>) goes to the direct host when the page was proxied — see dataUrl().
 *  Any other host (the dev page aimed at another receiver) is left as given. */
function pageDataHost(host: string): string {
  if (!proxied()) return host;
  const slash = host.indexOf('/');
  const auth = slash === -1 ? host : host.slice(0, slash);
  if (auth.toLowerCase() !== location.host.toLowerCase()) return host;
  return directHost() + (slash === -1 ? '' : host.slice(slash));
}

/** `wss://host` or `ws://host`, matching the page — but pointed at the RECEIVER, not the page's
 *  own origin, when the two differ. See socketHost(). Same shape check as httpBase. */
export function wsBase(host: string): string {
  if (!isValidServerHost(host)) throw new Error('not a server address');
  return `${pageIsSecure() ? 'wss' : 'ws'}://${socketHost(host)}`;
}

/* ══ PAGE HTTP GOES DIRECT TOO (beta93, 2026-11-24) ══════════════════════════════════════════════════
 * ★★★ STUART: "the gallery etc should all be handled via the free tunnel not the main workers — they should only
 *     establish the connection." Until beta93 only the WEBSOCKET went to the direct host; every HTTP read the page
 *     made (vibeserver.json, the gallery, the DX Log, telemetry charts, logos, map data, recordings…) went to the
 *     page's own origin — the *.vibeserver.vibesdr.net address — and so through the directory's Cloudflare Worker,
 *     one billed Worker request each. On 2026-11-24 one page's runaway DX Log poll (~6,500 a minute, fixed in
 *     e375ccafc) spent the whole account's Workers allowance in minutes and took EVERY listed tunnel down with it.
 * ★★ Now: when the page came through the slug proxy (it was told a direct host that is not its own), every HTTP
 *    request after load goes to https://<direct host> + the radio's /r/<id> prefix, exactly like the socket. The
 *    receiver answers CORS for *.vibeserver.vibesdr.net origins (local_sdr_shim.cpp vsCorsFor). Only the HTML
 *    document — the one thing that needs the stable origin, for localStorage — and the service worker (which must be
 *    same-origin) stay on the Worker; the scripts follow when the receiver's policy allows it (assetOrigin()).
 * ★★ ONE HELPER, so nowhere is left to miss: dataUrl() for a path, httpBase() for `host` (it swaps the authority when
 *    `host` is this page's own), dataOrigin() for code that builds its own base. scripts/test-web-direct-data.mjs
 *    counts what reaches a stand-in Worker after load — anything new that forgets this fails it. */

/** True when the page came through the directory's slug proxy and the receiver is somewhere else. */
function proxied(): boolean {
  if (typeof location === 'undefined' || !/^https?:$/.test(location.protocol)) return false;
  const d = directHost();
  return !!d && d.toLowerCase() !== location.host.toLowerCase();
}

/** `https://<direct host>` when the page was proxied, else '' (same origin, exactly as before). */
export function dataOrigin(): string {
  return proxied() ? `${location.protocol}//${directHost()}` : '';
}

/** The URL for an HTTP request to THIS receiver. `path` is an absolute path ('/vibeserver/…', BASE_PATH included)
 *  or a URL on this page's own origin; either is re-pointed at the direct host when the page was proxied. Anything
 *  else (a third-party URL, blob:, data:, a relative path) is returned untouched. */
export function dataUrl(path: string): string {
  const o = dataOrigin();
  if (!o) return path;
  if (path.startsWith('/') && !path.startsWith('//')) return o + path;
  const own = location.origin;
  if (path === own || path.startsWith(own + '/') || path.startsWith(own + '?')) return o + path.slice(own.length);
  return path;
}

/** ★ Where the page's SCRIPTS and stylesheets come from: the direct host only when the page's head loader chose it
 *  (window.__vsJsOrigin, set by the loader build-web.mjs writes — and only when the directory said the receiver's
 *  Content-Security-Policy allows it), else '' (this page's own origin, as before). */
export function assetOrigin(): string {
  if (typeof window === 'undefined') return '';
  const o = String((window as any).__vsJsOrigin || '');
  return /^https:\/\/(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(:\d{1,5})?$/.test(o) ? o : '';
}

/** True when `url` is one dataUrl() pointed at the direct host. */
export function isDirectUrl(url: string): boolean {
  const o = dataOrigin();
  return !!o && (url === o || url.startsWith(o + '/'));
}

/* ★★★ THE TUNNEL HOST ROTATES. A Quick Tunnel gets a new hostname whenever the server restarts, so a page left open
 *     can be holding a host that no longer exists. Then: ask the Worker ONCE (one /vibeserver.json through the slug,
 *     whose answer carries `directHost`), and carry on direct at the new host — never fall back to proxying
 *     everything, which is the bill this whole change exists to stop.
 *  ★★ RATE-LIMITED: one re-learn in flight, the first at once, then 1, 2, 4… minutes apart (at most 30) while the
 *     answer stays "same host" — the server is simply down, and that costs a handful of Worker requests an hour, not
 *     one per failed read. A real move resets the ladder. */
export const RELEARN_MIN_MS = 60_000;
const RELEARN_MAX_MS = 30 * 60_000;
let relearnAt = 0;
let relearnGap = RELEARN_MIN_MS;      // ★ doubles while the answer keeps being "same host" (the server is just down)
let relearning: Promise<boolean> | null = null;
let relearnCount = 0;
let origFetch: typeof fetch | null = null;
export function relearnDirectHost(): Promise<boolean> {
  if (!proxied() || typeof fetch === 'undefined') return Promise.resolve(false);
  if (relearning) return relearning;
  const now = Date.now();
  if (relearnAt && now - relearnAt < relearnGap) return Promise.resolve(false);
  if (relearnAt) relearnGap = Math.min(RELEARN_MAX_MS, relearnGap * 2);
  relearnAt = now;
  relearnCount++;
  const base = (location.pathname.match(/^\/r\/[^/]+/) || [''])[0];
  const doFetch = origFetch || fetch;
  relearning = (async () => {
    try {
      // ★ The page's OWN origin on purpose: this is the one read that must go through the Worker.
      const r = await doFetch(`${location.origin}${base}/vibeserver.json`, { cache: 'no-store' });
      if (!r.ok) return false;
      const j = await r.json();
      const h = String(j?.directHost || '');
      if (!HOST_SHAPE.test(h) || h.toLowerCase() === location.host.toLowerCase()) return false;
      const changed = h !== directHost();
      learntHost = h;
      if (changed) {
        relearnGap = RELEARN_MIN_MS;
        console.info(`[direct] the receiver moved to ${h} — HTTP and sockets follow it`);
        // ★ main.ts reloads on this: every socket was dialling the old host, and a fresh page is the clean way over.
        try { window.dispatchEvent(new CustomEvent('vibe-direct-moved', { detail: h })); } catch { /* no window */ }
      }
      return changed;
    } catch {
      return false;
    } finally {
      relearning = null;
    }
  })();
  return relearning;
}
/** For the tests: how many re-learns this page has made, and the host it now uses. */
export function relearnStats(): { count: number; host: string } { return { count: relearnCount, host: directHost() }; }

/* ★★ EVERY DIRECT FETCH THAT FAILS AT THE NETWORK LAYER asks for a re-learn — watched here, once, rather than at each
 *    of the ~80 call sites (a new feature cannot forget it). An HTTP error status is an ANSWER from the receiver and
 *    proves the host is right, so only a rejected fetch (DNS gone, connection refused, CORS) counts; an abort is the
 *    page's own doing and counts for nothing. Installed only on a proxied page — everywhere else fetch is untouched. */
export function watchDirectFetches(): void {
  if (origFetch || !proxied() || typeof window === 'undefined' || typeof window.fetch !== 'function') return;
  const orig = window.fetch.bind(window);
  origFetch = orig;
  window.fetch = ((input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : (input as Request).url;
    return orig(input, init).catch((e: unknown) => {
      if (isDirectUrl(url) && !(e instanceof DOMException && e.name === 'AbortError')) void relearnDirectHost();
      throw e;
    });
  }) as typeof fetch;
  (window as any).__vibeDirect = relearnStats;
}
