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

/** True when this page was served over https (so everything it loads must be too). */
export function pageIsSecure(): boolean {
  return typeof location !== 'undefined' && location.protocol === 'https:';
}

/** `https://host` or `http://host`, matching the page. */
export function httpBase(host: string): string {
  return `${pageIsSecure() ? 'https' : 'http'}://${host}`;
}

/** `wss://host` or `ws://host`, matching the page. */
export function wsBase(host: string): string {
  return `${pageIsSecure() ? 'wss' : 'ws'}://${host}`;
}
