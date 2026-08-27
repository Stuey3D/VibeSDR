/**
 * DeepLinkHandler — parse / validate / resolve `vibesdr://` deep links.
 *
 * URL grammar (contract agreed with UberSDR host-side, see
 * briefs/BRIEF-deep-linking-uri-scheme.md §2):
 *
 *   vibesdr://connect?uuid=<collector id>[&freq=&mode=&zoom=]
 *   vibesdr://connect?url=<pct-encoded https/wss>&backend=<id>[&freq=&mode=&zoom=]
 *
 * This module is pure logic: it turns an untrusted URL string into a validated
 * ResolvedTarget (or null). All navigation / UI lives in useDeepLinks.
 */

import { fetchInstances } from '../services/instancesApi';
import type { SDRMode } from '../services/UberSDRClient';

export type LinkBackend = 'ubersdr' | 'kiwi' | 'web888' | 'owrx' | 'rtltcp' | 'vibeserver';
// Route serverType is a subset — rtltcp isn't a plain URL backend (needs the
// on-device shim + host:port), so it's rejected in Phase 1.
export type RouteServerType = 'ubersdr' | 'kiwi' | 'web888' | 'owrx' | 'vibeserver';

export interface DeepLinkRequest {
  uuid?:    string;
  url?:     string;
  backend?: LinkBackend;
  freq?:    number;
  mode?:    SDRMode;
  zoom?:    number;
}

export interface ResolvedTarget {
  baseUrl:      string;
  instanceName: string;
  serverType:   RouteServerType;
  freq?:        number;
  mode?:        SDRMode;
  zoom?:        number;
}

export type ResolveResult =
  | { ok: true;  target: ResolvedTarget }
  | { ok: false; reason: string };

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const MAX_URL_LEN = 2048;

/**
 * Is this URL's host on the network the device is already on?
 *
 * ★★ Deliberately CONSERVATIVE — an unparseable host, a name we do not recognise, or anything
 *    routable answers false, so the caller falls back to demanding https. A wrong "yes" here would
 *    let a link from anywhere push the app into plaintext against a public address.
 * ★ 172.16-31 is the awkward one: 172.15 and 172.32 are PUBLIC, so the second octet is range
 *   checked rather than matched with a prefix — the mistake that turns a private-only rule into
 *   an almost-private one.
 */
function isPrivateHost(u: string): boolean {
  const m = /^[a-z]+:\/\/([^/?#]+)/i.exec(u);
  if (!m) return false;
  let host = m[1];
  const at = host.lastIndexOf('@');            // strip any userinfo before parsing the host
  if (at >= 0) host = host.slice(at + 1);
  host = host.replace(/:\d+$/, '').replace(/^\[|\]$/g, '').toLowerCase();
  if (host === 'localhost' || host.endsWith('.local') || host.endsWith('.localhost')) return true;
  if (host === '::1') return true;
  if (/^f[cd][0-9a-f]{2}:/.test(host)) return true;                  // IPv6 unique-local
  if (/^fe80:/.test(host)) return true;                              // IPv6 link-local
  const v4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  if (!v4) return false;
  const [a, b] = [Number(v4[1]), Number(v4[2])];
  if (v4.slice(1).some(x => Number(x) > 255)) return false;
  return a === 127 || a === 10 || (a === 172 && b >= 16 && b <= 31)
      || (a === 192 && b === 168) || (a === 169 && b === 254);
}

// Brief's mode vocabulary → the app's SDRMode. `cw`/`cwr` differ, `iq` is
// unsupported (dropped — connect at the default mode instead).
const MODE_MAP: Record<string, SDRMode> = {
  usb: 'usb', lsb: 'lsb', am: 'am', sam: 'sam',
  fm: 'fm', nfm: 'nfm', wfm: 'wfm',
  cw: 'cwu', cwr: 'cwl',
};

/** Minimal, dependency-free query parser for `scheme://action?a=b&c=d`. */
function splitUrl(raw: string): { action: string; params: Record<string, string> } | null {
  // vibesdr://connect?uuid=...   → strip scheme, then action[?query]
  const m = /^vibesdr:\/\/([^?#]*)(?:\?([^#]*))?/i.exec(raw);
  if (!m) return null;
  const action = (m[1] || '').replace(/\/+$/, '').toLowerCase();
  const params: Record<string, string> = {};
  if (m[2]) {
    for (const pair of m[2].split('&')) {
      if (!pair) continue;
      const eq = pair.indexOf('=');
      const k = eq === -1 ? pair : pair.slice(0, eq);
      const v = eq === -1 ? '' : pair.slice(eq + 1);
      try {
        params[decodeURIComponent(k).toLowerCase()] = decodeURIComponent(v.replace(/\+/g, ' '));
      } catch { /* skip malformed pair */ }
    }
  }
  return { action, params };
}

/**
 * Parse + validate a raw URL into a DeepLinkRequest. Returns null on any
 * validation failure (caller shows a generic toast — never reflects raw input).
 */
export function parseVibeSdrUrl(raw: string): DeepLinkRequest | null {
  if (typeof raw !== 'string' || raw.length > MAX_URL_LEN) return null;
  const parsed = splitUrl(raw);
  if (!parsed || parsed.action !== 'connect') return null;
  const p = parsed.params;

  const req: DeepLinkRequest = {};

  // uuid wins over url when both are present (brief §2.4).
  if (p.uuid && UUID_RE.test(p.uuid)) {
    req.uuid = p.uuid;
  } else if (p.url) {
    let u: string;
    try { u = p.url; } catch { return null; }
    const backend = (p.backend || '').toLowerCase();
    if (backend !== 'ubersdr' && backend !== 'kiwi' && backend !== 'web888'
        && backend !== 'owrx' && backend !== 'rtltcp' && backend !== 'vibeserver') return null;
    // ★★★ https/wss ONLY, EXCEPT ON YOUR OWN NETWORK. The rule exists because a link can arrive
    //     from anywhere — a QR code on a poster, a forum post — and sending credentials in clear
    //     to a stranger's address is not a risk worth a convenience. But it also rejected the one
    //     link we generate OURSELVES: VibeServer on a Mac or a Pi serves plain http on a LAN
    //     address and cannot hold a certificate for 192.168.x.x, so clicking the VibeServer
    //     menu-bar icon opened the app and then refused its own link (Stuart, 2026-08-12).
    // ★★ RELAXED ONLY WHERE PLAINTEXT IS ALREADY UNAVOIDABLE AND ALREADY LOCAL: loopback, RFC1918
    //    and link-local addresses, and .local names. Nothing routable is admitted, so a link from
    //    the internet still cannot talk the app into plaintext — it can only ever name a machine
    //    on the network the phone is already sitting on.
    if (!/^(https|wss):\/\//i.test(u) && !(/^(http|ws):\/\//i.test(u) && isPrivateHost(u)))
      return null;
    req.url = u.replace(/\/+$/, '');
    req.backend = backend as LinkBackend;
  } else {
    return null; // neither a valid uuid nor a valid url
  }

  // Optional tuning — clamped / whitelisted; invalid values are dropped, not fatal.
  if (p.freq != null && p.freq !== '') {
    const f = Number(p.freq);
    if (Number.isFinite(f) && f >= 0 && f <= 2_000_000_000) req.freq = Math.round(f);
  }
  if (p.mode) {
    const m = MODE_MAP[p.mode.toLowerCase()];
    if (m) req.mode = m;
  }
  if (p.zoom != null && p.zoom !== '') {
    const z = Number(p.zoom);
    if (Number.isFinite(z) && z >= 0 && z <= 14) req.zoom = Math.round(z);
  }

  return req;
}

/**
 * Resolve a request to a concrete connection target. For the uuid form this
 * looks the instance up in the collector (fetching fresh if needed). Returns a
 * generic reason string on failure (safe to toast).
 */
export async function resolveRequest(req: DeepLinkRequest): Promise<ResolveResult> {
  const tune = { freq: req.freq, mode: req.mode, zoom: req.zoom };

  if (req.uuid) {
    let list;
    try { list = await fetchInstances(); }
    catch { return { ok: false, reason: 'Instance list unreachable' }; }
    const hit = list.find((i) => i.uuid === req.uuid);
    if (!hit || !hit.url) return { ok: false, reason: 'Instance not found in directory' };
    return {
      ok: true,
      target: {
        baseUrl: hit.url,
        instanceName: hit.name,
        serverType: (hit.serverType ?? 'ubersdr') as RouteServerType,
        ...tune,
      },
    };
  }

  if (req.url && req.backend) {
    if (req.backend === 'rtltcp') {
      return { ok: false, reason: 'RTL-TCP links are not supported yet' };
    }
    return {
      ok: true,
      target: {
        baseUrl: req.url,
        instanceName: req.url.replace(/^wss?:\/\/|^https?:\/\//i, ''),
        serverType: req.backend,
        ...tune,
      },
    };
  }

  return { ok: false, reason: 'Invalid VibeSDR link' };
}

/** Build an outbound url-form link from the current session (share button). */
export function buildShareLink(opts: {
  baseUrl: string;
  serverType?: RouteServerType;
  freq?: number;
  mode?: SDRMode;
}): string {
  const backend = opts.serverType ?? 'ubersdr';
  const parts = [`url=${encodeURIComponent(opts.baseUrl)}`, `backend=${backend}`];
  if (opts.freq != null) parts.push(`freq=${Math.round(opts.freq)}`);
  if (opts.mode) {
    // Emit the brief's vocabulary (cwu/cwl → cw/cwr) so links round-trip cleanly.
    const out = opts.mode === 'cwu' ? 'cw' : opts.mode === 'cwl' ? 'cwr' : opts.mode;
    parts.push(`mode=${out}`);
  }
  return `vibesdr://connect?${parts.join('&')}`;
}
