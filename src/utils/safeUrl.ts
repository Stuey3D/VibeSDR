/**
 * ★★★ A URL FROM ANYWHERE ELSE IS UNTRUSTED (security pass, 2026-10-03). Server and directory URLs arrive from
 *     a link anyone can make, from directories anyone can list on, from a server's own config. They end up in
 *     fetch() calls, WebSocket addresses, a WebView page's <script>, and Linking.openURL — so a quote, a
 *     backslash or a `javascript:` scheme is not a cosmetic problem.
 *
 * ★★ WHY NOT `new URL()`: React Native's URL (Libraries/Blob/URL.js) is a set of regexes, not a parser. It
 *    accepts anything, never throws on a bad URL, and its host/hostname only work for http(s). It cannot be a
 *    validator. This is a deliberately STRICT parser for the one shape we accept: scheme://host[:port][/rest].
 *
 *  - Not a string, empty, over 2048 characters → null.
 *  - Any whitespace, control character, quote, backslash, backtick or angle bracket anywhere → null. None of
 *    them belong in a server address, and each is how a URL breaks out of the string it is written into.
 *  - Userinfo ('@' in the authority) → null: `https://trusted.example@evil.example` shows one host and opens
 *    another.
 *  - The scheme must be in the caller's list; the host must be a DNS name, IPv4 or a bracketed IPv6; a port,
 *    when present, 1–65535.
 */
export interface ParsedUrl {
  /** Lower-case, without the colon: 'https'. */
  scheme: string;
  /** Lower-case host as written, brackets kept for IPv6: 'example.org', '[fe80::1]'. */
  host: string;
  /** '' when absent. */
  port: string;
  /** scheme://host[:port] */
  origin: string;
}

const BAD_CHARS = /[\s\u0000-\u001f\u007f-\u009f"'`\\<>{}|^]/;
const SHAPE = /^([a-z][a-z0-9+.-]*):\/\/([^/?#]*)([/?#].*)?$/i;
const HOST_DNS = /^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9_-]*[a-z0-9])?)*\.?$/i;
const HOST_V6 = /^\[[0-9a-f:.]+(%[0-9a-z]+)?\]$/i;

export function parseUrlStrict(raw: unknown, schemes: readonly string[]): ParsedUrl | null {
  if (typeof raw !== 'string' || raw.length === 0 || raw.length > 2048) return null;
  if (BAD_CHARS.test(raw)) return null;
  const m = SHAPE.exec(raw);
  if (!m) return null;
  const scheme = m[1].toLowerCase();
  if (!schemes.includes(scheme)) return null;
  const authority = m[2];
  if (!authority || authority.includes('@')) return null;
  let host = authority;
  let port = '';
  const pm = /^(.*):(\d{1,5})$/.exec(authority);
  if (pm && !(authority.startsWith('[') && !pm[1].endsWith(']'))) {
    host = pm[1];
    port = pm[2];
    const n = Number(port);
    if (!(n >= 1 && n <= 65535)) return null;
  }
  if (!host || !(HOST_DNS.test(host) || HOST_V6.test(host))) return null;
  host = host.toLowerCase();
  return { scheme, host, port, origin: `${scheme}://${host}${port ? ':' + port : ''}` };
}

/** The URL, trailing slashes removed, if it passes parseUrlStrict for these schemes; otherwise ''. */
export function safeUrl(raw: unknown, schemes: readonly string[]): string {
  if (!parseUrlStrict(raw, schemes)) return '';
  return (raw as string).replace(/\/+$/, '');
}

export const HTTP_SCHEMES = ['http', 'https'] as const;
export const SERVER_SCHEMES = ['http', 'https', 'ws', 'wss'] as const;

/** The ws(s) origin of a WebSocket base URL ('' when it does not parse), e.g. for a page's CSP connect-src.
 *  ★ Not `new URL()`: React Native's URL only finds a host in http(s) URLs, so a ws:// base came back with
 *    an empty host — and a CSP naming a bare `ws://` would have blocked the very socket it was for. */
export function wsOriginFor(wsBase: unknown): string {
  return parseUrlStrict(wsBase, ['ws', 'wss'])?.origin ?? '';
}
