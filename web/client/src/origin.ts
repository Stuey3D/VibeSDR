/**
 * origin.ts — build a URL for the server using the scheme the PAGE was served over.
 *
 * ★★★ WHY THIS IS A SHARED HELPER AND NOT A LOCAL FIX. Every one of these URLs used to be written
 *     as `http://${host}` or `ws://${host}` inline. Behind an HTTPS reverse proxy the page loads
 *     over https and then every one of those is BLOCKED as mixed content, so the receiver fails to
 *     connect with nothing wrong at the proxy — it reads as a broken server (Saber, 2026-08-08,
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
 * where the receiver actually is. HTTP stays on the stable origin — same-origin, so no CORS and no
 * new surface — while the WEBSOCKET, which carries the audio and the spectrum, goes DIRECT to the
 * tunnel. The bytes never cross our Worker, which is the whole reason the address is a stable name
 * rather than a proxy for everything.
 *
 * ★ Absent on a LAN server, a port-forwarded one, or anything reached directly: then this returns
 *   the host it was given and nothing changes.
 */
function socketHost(host: string): string {
  const direct = (typeof window !== 'undefined' && (window as any).__VIBE_DIRECT_HOST__) || '';
  if (!direct) return host;
  // ★★ `host` MAY CARRY A /r/<id> PREFIX — see BASE_PATH in main.ts, where the front door routes
  //    several radios by path. Only the authority is swapped; the prefix has to survive or a
  //    multi-radio server would send every socket to the wrong radio.
  const slash = host.indexOf('/');
  return slash === -1 ? direct : direct + host.slice(slash);
}

/** True when this page was served over https (so everything it loads must be too). */
export function pageIsSecure(): boolean {
  return typeof location !== 'undefined' && location.protocol === 'https:';
}

/** `https://host` or `http://host`, matching the page. */
export function httpBase(host: string): string {
  return `${pageIsSecure() ? 'https' : 'http'}://${host}`;
}

/** `wss://host` or `ws://host`, matching the page — but pointed at the RECEIVER, not the page's
 *  own origin, when the two differ. See socketHost(). */
export function wsBase(host: string): string {
  return `${pageIsSecure() ? 'wss' : 'ws'}://${socketHost(host)}`;
}
