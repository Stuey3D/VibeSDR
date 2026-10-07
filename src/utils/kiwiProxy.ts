/**
 * ★★★ A KIWI BEHIND proxy.kiwisdr.com ANSWERS ON 8073 ONLY — AND THE DIRECTORY LISTS IT WITH NO PORT.
 *
 * MEASURED 2026-10-07 (Stuart: "some work, others don't — it's been an issue every time we tried to
 * get Kiwi working"). Of 866 public Kiwis in the linkfanel snapshot, **404 (47 %) are listed as
 * `http://<name>.proxy.kiwisdr.com` with no port.** A portless http URL means port 80, and the proxy
 * REFUSES port 80 (and 443) at the TCP level; it only listens on 8073. Twelve random online Kiwis
 * from the Mac: every proxied one REFUSED on :80 and OPENED on :8073 (`/status` → 200); every directly
 * hosted one opened. The app's dialog then read "This KiwiSDR closed the connection: Connection
 * refused" — the receiver never even saw us; the address was wrong.
 *
 * So: a proxy.kiwisdr.com host with no explicit port gets :8073. An explicit port is always kept.
 * ONE RULE, SEVERAL READERS — the connect (KiwiAdapter.toWsBase), both directory parsers (Kiwi list,
 * Receiverbook) and the watch's KiwiClient.swift / SDRDirectory.swift all apply it.
 */
const KIWI_PROXY_HOST = /^([a-z][a-z0-9+.-]*:\/\/)?([^/:?#\s]+\.proxy\.kiwisdr\.com)(?=[/?#]|$)/i;

export function withKiwiProxyPort(url: string): string {
  if (!url) return url;
  return url.replace(KIWI_PROXY_HOST, (_m, scheme: string | undefined, host: string) => `${scheme ?? ''}${host}:8073`);
}
