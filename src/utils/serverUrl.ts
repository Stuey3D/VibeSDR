/**
 * ★★★ ONE RULE FOR EVERY SERVER ADDRESS: THE SCHEME AND THE PORT ARE A PAIR, AND WE KEEP BOTH.
 *
 * GitHub #52 (casm, 2026-10-15): an OpenWebRX+ 1.2.126 with its NATIVE TLS on its default port —
 * `https://host:8073` — would not connect. "OpenWebRX server stopped responding", every time.
 * OWRX+ wraps its listening socket in TLS when /etc/openwebrx/cert.pem exists, so that port speaks
 * ONLY TLS. Reproduced on the Lenovo with the same 1.2.126 image and a self-signed cert:
 *     wss://host:8073/ws/  → CLIENT DE SERVER v1.2.126, config, profiles … (works)
 *     ws://host:8073/ws/   → close 1006 in 50 ms, nothing sent       (the card casm saw)
 *     http://host:8073/    → connection reset (curl rc 56)
 * Several picker paths still reduced an address to `{host, port}` and later rebuilt it as
 * `http://host:port` — the explicit-type "OpenWebRX" choice in the + sheet (no probe, so 8073
 * was GUESSED to be plain http), and every saved custom server (TcpFav had nowhere to keep a
 * scheme, so Reconnect rebuilt the same wrong address). Same family as the Kiwi proxy port
 * (utils/kiwiProxy.ts): a port we were not expecting.
 *
 * The rule, enforced here and nowhere else:
 *  - What the user (or a directory) gives is kept AS GIVEN: an explicit scheme is never changed,
 *    an explicit port is never changed.
 *  - The port is never inferred from the scheme when one was given, and the SCHEME IS NEVER
 *    INFERRED FROM THE PORT. 8073 can be TLS; 8443 can be plain.
 *  - Defaults fill in ONLY a missing part: a portless *.proxy.kiwisdr.com → 8073; a caller's
 *    per-type default when NO scheme was typed either; otherwise https → 443, http → 80.
 *  - A missing scheme becomes http here, flagged `explicitScheme: false` — callers that can
 *    probe (sdrTypes.resolveScheme) must try https first rather than trust that guess.
 *  - ws/wss follows the scheme, never the port.
 *
 * Pure: no React Native imports, so Node can run scripts/test_server_url.ts against it directly.
 */
import { withKiwiProxyPort } from './kiwiProxy.ts';

export type HttpScheme = 'http' | 'https';

export interface ServerUrl {
  scheme: HttpScheme;
  /** Did the address name a scheme (http/https/ws/wss)? If not, `scheme` is only a guess. */
  explicitScheme: boolean;
  /** Bare host — an IPv6 literal WITHOUT its brackets. */
  host: string;
  port: number;
  /** Did the address (or the caller's port field) name the port? */
  explicitPort: boolean;
  /** '' or '/sub/folder' — no trailing slash, no query or fragment. */
  path: string;
  /** Normalised http(s) base: scheme://host[:port]path — the port omitted only when it is the
   *  scheme's own default. No trailing slash. */
  url: string;
}

export interface ParseOpts {
  /** A port the user typed in a SEPARATE field. Wins over a missing port in the text; a port
   *  written into the address itself still wins over this. */
  port?: number;
  /** The server type's usual port, used ONLY when the address named neither a scheme nor a port
   *  (a bare `host`). An explicit `https://host` means 443, not this. */
  defaultPort?: number;
}

const SCHEME_RE = /^([a-z][a-z0-9+.-]*):\/\//i;

export function defaultPortFor(scheme: HttpScheme): number {
  return scheme === 'https' ? 443 : 80;
}

/** Host for a URL authority: IPv6 literals go back in brackets. */
export function authorityHost(host: string): string {
  return host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
}

export function buildServerUrl(scheme: HttpScheme, host: string, port: number, path = ''): string {
  const p = port === defaultPortFor(scheme) ? '' : `:${port}`;
  return `${scheme}://${authorityHost(host)}${p}${path}`;
}

function validPort(n: number): boolean {
  return Number.isInteger(n) && n > 0 && n < 65536;
}

/** Parse anything a person or a directory might hand us. null = not an address. */
export function parseServerUrl(raw: string, opts: ParseOpts = {}): ServerUrl | null {
  let s = (raw || '').trim();
  if (!s) return null;
  // ★ The Kiwi proxy rule runs FIRST, on the text as given — it only ever touches a portless
  //   *.proxy.kiwisdr.com host, so it cannot alter anyone else's address.
  s = withKiwiProxyPort(s);

  let scheme: HttpScheme = 'http';
  let explicitScheme = false;
  const sm = SCHEME_RE.exec(s);
  if (sm) {
    const name = sm[1].toLowerCase();
    if (name === 'https' || name === 'wss') scheme = 'https';
    else if (name === 'http' || name === 'ws') scheme = 'http';
    else return null;                     // sdr://, spyserver:// … are not http receivers
    explicitScheme = true;
    s = s.slice(sm[0].length);
  }
  if (!s) return null;

  const slash = s.search(/[/?#]/);
  const authority = slash >= 0 ? s.slice(0, slash) : s;
  let path = slash >= 0 ? s.slice(slash) : '';
  path = path.replace(/[?#].*$/, '').replace(/\/+$/, '');
  if (path && !path.startsWith('/')) path = '';
  if (!authority) return null;

  let host: string;
  let portText: string | undefined;
  if (authority.startsWith('[')) {
    const m = /^\[([^\]]+)\](?::(\d+))?$/.exec(authority);
    if (!m) return null;
    host = m[1]; portText = m[2];
  } else if ((authority.match(/:/g) || []).length > 1) {
    host = authority;                      // bare IPv6 with no brackets: cannot carry a port
  } else {
    const m = /^([^:]+)(?::(\d*))?$/.exec(authority);
    if (!m) return null;
    host = m[1]; portText = m[2] || undefined;
  }
  if (!host) return null;

  let port: number;
  let explicitPort: boolean;
  if (portText) {
    port = parseInt(portText, 10); explicitPort = true;
  } else if (opts.port != null) {
    port = opts.port; explicitPort = true;
  } else if (!explicitScheme && opts.defaultPort != null) {
    port = opts.defaultPort; explicitPort = false;
  } else {
    port = defaultPortFor(scheme); explicitPort = false;
  }
  if (!validPort(port)) return null;

  return { scheme, explicitScheme, host, port, explicitPort, path,
           url: buildServerUrl(scheme, host, port, path) };
}

/** The same server with the other scheme, everything else kept — for a scheme PROBE only. */
export function withScheme(u: ServerUrl, scheme: HttpScheme): ServerUrl {
  return { ...u, scheme, explicitScheme: true, url: buildServerUrl(scheme, u.host, u.port, u.path) };
}

/** Any address → its http(s) form, keeping host, port and path exactly. ws→http, wss→https; a
 *  missing scheme becomes http (callers that can probe should not rely on that — see above). */
export function toHttpUrl(raw: string): string {
  const s = (raw || '').trim().replace(/\/+$/, '');
  if (/^wss:\/\//i.test(s)) return 'https://' + s.slice(6);
  if (/^ws:\/\//i.test(s))  return 'http://'  + s.slice(5);
  if (/^https?:\/\//i.test(s)) return s;
  return 'http://' + s;
}

/** Any address → its ws(s) form, keeping host, port and path exactly. https→wss, http→ws.
 *  ★ The socket's TLS follows the SCHEME, never the port. */
export function toWsUrl(raw: string): string {
  const s = (raw || '').trim().replace(/\/+$/, '');
  if (/^https:\/\//i.test(s)) return 'wss://' + s.slice(8);
  if (/^http:\/\//i.test(s))  return 'ws://'  + s.slice(7);
  if (/^wss?:\/\//i.test(s))  return s;
  return 'ws://' + s;
}

/** OpenWebRX: http(s)://host:port[/path] → ws(s)://host:port[/path]/ws/ (trailing slash required;
 *  the StaticRoute is exactly "/ws/", a bare /ws 404s). */
export function owrxWsUrl(baseUrl: string): string {
  return toWsUrl(baseUrl) + '/ws/';
}

/** OpenWebRX: ws(s)/http(s)://host:port[/path][/ws/] → http(s)://host:port[/path] (for /status.json). */
export function owrxHttpBase(baseUrl: string): string {
  return toHttpUrl(baseUrl).replace(/\/ws\/?$/, '');
}

/** ★★ The words for a socket that closed, for the card AND the diagnostics log (GitHub #52).
 *  `beforeHandshake` = the server never answered our hello, so it never "stopped responding" — we
 *  never reached it, and saying otherwise sent casm looking at a healthy server. A plain ws:// that
 *  dies instantly is very often a TLS-only port (OWRX+ with its own certificate), so say so. */
export function owrxCloseReason(wsUrl: string, beforeHandshake: boolean, code: number, err: string): string {
  const what = err ? `${err} (code ${code})` : `code ${code}`;
  if (!beforeHandshake) return `connection to ${wsUrl} dropped: ${what}`;
  const tlsHint = /^ws:\/\//i.test(wsUrl)
    ? ' — if this receiver uses https, add it with https:// in front of the address'
    : '';
  return `could not open ${wsUrl}: ${what}${tlsHint}`;
}
