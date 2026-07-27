// Shared SDR types used across clients and UI
// 'wfm' = broadcast FM (V4 local hardware only); not in MODES (HF default list).
export type SDRMode = 'usb' | 'lsb' | 'am' | 'sam' | 'fm' | 'nfm' | 'cwu' | 'cwl' | 'wfm';
export type Mode = SDRMode; // alias

export const MODES: SDRMode[] = ['usb', 'lsb', 'am', 'sam', 'fm', 'nfm', 'cwu', 'cwl'];
export const MODE_LABELS: Record<SDRMode, string> = {
  usb: 'USB', lsb: 'LSB', am: 'AM', sam: 'SAM',
  fm: 'FM', nfm: 'NFM', cwu: 'CWU', cwl: 'CWL', wfm: 'WFM',
};

export const STEPS = [10, 100, 500, 1000, 9000, 10000];
// VHF/UHF tuning steps — 10 kHz is uselessly small for broadcast FM (100 kHz),
// NFM repeaters (12.5/25 kHz) and air/marine. Used above 30 MHz (e.g. OWRX VHF
// profiles). 12.5k/25k shown as "12.5k"/"25k" by formatStep.
export const STEPS_VHF = [1000, 5000, 12500, 25000, 50000, 100000];
export function stepsForFreq(hz: number): number[] {
  return hz >= 30_000_000 ? STEPS_VHF : STEPS;
}
export const STEP_LABELS: Record<number, string> = {
  10: '10Hz', 100: '100Hz', 500: '500Hz',
  1000: '1kHz', 9000: '9kHz', 10000: '10kHz',
};

export const MIN_HZ = 10_000;
export const MAX_HZ = 30_000_000;

export const MIN_FREQ_HZ = MIN_HZ;
export const MAX_FREQ_HZ = MAX_HZ;
export const STEPS_HZ    = STEPS;
export interface ConnectionResult {
  allowed: boolean;
  passwordRequired: boolean;
  reason?: string;
}
export async function checkConnection(_url: string, _password?: string): Promise<ConnectionResult> {
  // UberSDRClient handles the real /connection POST with a proper UUID session ID.
  // A pre-flight probe here causes 400s (server rejects non-UUID session IDs).
  // Optimistically allow — UberSDRClient will surface auth errors on connect.
  return { allowed: true, passwordRequired: false };
}

export type ServerType = 'ubersdr' | 'kiwi' | 'owrx';

/** Everything the "Custom server" box can reach. The HTTP kinds are what
 *  detectServerType() can sniff; the raw-TCP kinds (rtl_tcp, SpyServer) speak no
 *  HTTP at all, so they can only be reached by an explicit choice or a port
 *  convention — see probeServer(). */
export type BackendType = ServerType | 'fmdx' | 'vibeserver' | 'rtltcp' | 'spyserver';

/** Default port per backend, used to guess a bare host and to prefill the form. */
export const DEFAULT_PORT: Record<BackendType, number> = {
  ubersdr: 8073, kiwi: 8073, owrx: 8073, fmdx: 8080,
  vibeserver: 48000, rtltcp: 1234, spyserver: 5555,
};

/** Probe a manually-entered host to pick the backend (v3). Fetches the landing
 *  page and sniffs markers. Returns null when the host can't be reached (the
 *  caller keeps any previously-known type rather than guessing). */
export async function detectServerType(url: string): Promise<BackendType | null> {
  const base = url.trim().replace(/\/+$/, '')
    .replace(/^ws:\/\//i, 'http://').replace(/^wss:\/\//i, 'https://');
  // Manual AbortController + setTimeout — AbortSignal.timeout() isn't reliably
  // available in Android's Hermes runtime and throws before the fetch even runs,
  // which used to make detection fail → default to ubersdr → 404 on OWRX servers.
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 5000);
  try {
    // ★★ ASK, DON'T SNIFF — VibeServer is the one protocol we own both ends of,
    // so it identifies itself instead of being guessed at from page prose.
    //
    // The sniff below cannot see a VibeServer whose host turned the web client
    // off (--no-web / webServer:false): `GET /` then returns a page saying only
    // "VibeSDR", the CLIENT's name, and matching "vibesdr" is exactly what
    // mis-typed genuine UberSDR servers in v8.0.0 — so it fell through to the
    // ubersdr default every single time. /vibeserver.json is served regardless
    // of that toggle.
    try {
      const idr = await fetch(base + '/vibeserver.json', { signal: ctrl.signal });
      if (idr.ok) {
        const id = await idr.json();
        if (id && id.server === 'vibeserver') return 'vibeserver';
      }
    } catch { /* not a VibeServer, or older than this endpoint — fall through */ }

    const r = await fetch(base + '/', { signal: ctrl.signal });
    const body = (await r.text()).toLowerCase();
    // ORDER MATTERS, and every rule here exists because a later backend's page
    // contains an earlier one's marker:
    //
    // VibeServer FIRST. It serves our own web client, whose bundle mentions
    // "ubersdr" (the shim speaks the UberSDR protocol, so the client code names
    // it) — checked in the old order, a VibeServer detected as plain UberSDR.
    //
    // Then UberSDR, positively: it can enable a KiwiSDR-emulation feature, so its
    // page carries Kiwi markers and would otherwise mis-detect as Kiwi.
    //
    // Then Kiwi, whose web UI is built ON OpenWebRX and so also contains
    // "openwebrx" — it must beat OWRX.
    //
    // MATCH "vibeserver" ONLY — NEVER "vibesdr" (v8.0.0 regression, fixed 8.0.1).
    // "vibesdr" is the CLIENT's name, not the server's: UberSDR instances carry
    // vibesdr:// deep-link banners, so that alternative matched genuine UberSDR
    // pages, and because this rule runs first they were typed as VibeServer — and
    // the picker then WROTE that back over the saved favourite. "vibeserver" is
    // safe: it appears in our served page (VIBESERVER, /vibeserver/auth) and has
    // no reason to appear on anyone else's.
    if (body.includes('vibeserver')) return 'vibeserver';
    if (body.includes('ubersdr')) return 'ubersdr';
    if (/kiwisdr|kiwi sdr|\/kiwi\/|kiwi_util|owrx_ws_open/.test(body)) return 'kiwi';
    if (body.includes('openwebrx')) return 'owrx';
    if (/fm-dx|fmdx/.test(body)) return 'fmdx';
    return 'ubersdr';            // reachable but unidentifiable → assume ubersdr
  } catch {
    return null;                // couldn't reach — caller keeps any known type
  } finally {
    clearTimeout(timer);
  }
}

/**
 * Work out what's listening at host:port, for the Custom-server box.
 *
 * Two families, and they need different treatment:
 *
 *  - HTTP backends (UberSDR, Kiwi, OWRX, FM-DX, VibeServer) serve a landing page,
 *    so detectServerType() can sniff them. That's the reliable path and it's tried
 *    first, over https then http.
 *  - rtl_tcp and SpyServer are RAW TCP. They serve no HTTP, so a fetch just fails
 *    and there is nothing to sniff. We cannot identify them by probing from JS —
 *    fetch() gives us no socket. So they fall back to their well-known PORTS,
 *    which is exactly the convention the old two-pill RTL-TCP/SpyServer toggle
 *    encoded by hand.
 *
 * `hint` is the user's explicit choice, if they made one — it always wins, so a
 * non-standard port is never unreachable.
 */
/** ★★★ A SERVER ADDRESS IS A URL, NOT A HOST AND A PORT.
 *
 *  The picker used to reduce whatever was typed to `{host, port}`, keeping the scheme only
 *  long enough to choose a default port and discarding the path outright. That shape cannot
 *  express either half of a perfectly ordinary receiver URL:
 *
 *      https://kiwisdr.tgcfabian.nl/OpenWebRX/      <- TLS *and* a subfolder
 *      https://teftuner.tgcfabian.nl/main/
 *
 *  Both were reported by Fabian (NL13999) on 2026-07-27: the OWRX one would not connect at
 *  all, and the FM-DX one was correctly DETECTED and then failed on the WebSocket — because
 *  the socket was rebuilt from host+port as `ws://host:port`, losing both the `wss` and the
 *  `/main/`. The adapters were innocent; they handle scheme and path correctly. Nothing ever
 *  handed them a URL.
 *
 *  ★ So parse once, keep everything, and pass the URL down. `host`/`port` remain for display,
 *  for the raw-TCP backends that genuinely have no URL, and for the VibeServer port sweep.
 */
export interface ServerAddress {
  host: string;
  port: number;
  /** Full normalised base, e.g. `https://host:8443/OpenWebRX` — no trailing slash. */
  url: string;
  /** Did the user actually name a scheme? If not we may still probe both. */
  explicitScheme: boolean;
  /** Did they name a path? A subfolder must be probed where it lives, not at the root. */
  hasPath: boolean;
}

export function parseServerAddress(
  raw: string, defaultPort?: number,
): ServerAddress | null {
  let s = raw.trim().replace(/^ws:\/\//i, 'http://').replace(/^wss:\/\//i, 'https://');
  const schemeM = /^(https?):\/\//i.exec(s);
  const explicitScheme = !!schemeM;
  const https = schemeM ? schemeM[1].toLowerCase() === 'https' : false;
  if (schemeM) s = s.slice(schemeM[0].length);
  if (!s) return null;

  // Split authority from path BEFORE touching either — the old code deleted the path here.
  const slash = s.indexOf('/');
  const authority = slash >= 0 ? s.slice(0, slash) : s;
  let path = slash >= 0 ? s.slice(slash) : '';
  path = path.replace(/[?#].*$/, '').replace(/\/+$/, '');   // drop query/fragment + trailing /

  const m = /^(.+?)(?::(\d+))?$/.exec(authority);
  if (!m || !m[1]) return null;
  const host = m[1];
  const port = m[2] ? parseInt(m[2], 10)
    : defaultPort != null ? defaultPort
    : https ? 443 : 80;
  if (!Number.isFinite(port) || port <= 0 || port >= 65536) return null;

  // ★ Omit the port when it is the scheme's default: a URL that says `:443` on https works,
  // but it is not what anyone pasted and it makes every log and label harder to read.
  const scheme = https ? 'https' : 'http';
  const defaultForScheme = https ? 443 : 80;
  const authorityOut = port === defaultForScheme ? host : `${host}:${port}`;
  return { host, port, url: `${scheme}://${authorityOut}${path}`,
           explicitScheme, hasPath: path.length > 0 };
}

/** ★★ Occupancy of a VibeServer, from its identity endpoint.
 *  A VibeServer serves ONE listener at a time, so a public one that is busy has to say so
 *  BEFORE someone taps it — otherwise a busy server is indistinguishable from a broken one,
 *  and the user's conclusion is that our software does not work (Stuart, 2026-07-27). */
export interface ServerOccupancy {
  busy: boolean;
  /** Seconds until the current listener's time limit expires. -1 = no limit set, so the
   *  honest answer to "how long?" is "no idea", NOT "any moment now". */
  freeInSec: number;
  /** The owner's per-listener limit in minutes, 0 = unlimited. */
  limitMin: number;
  /** Does this server have an admin password? Only then is an override box worth offering. */
  admin: boolean;
}

/** Ask a VibeServer whether it is free. Returns null for anything that is not a VibeServer or
 *  does not answer — callers must treat that as "unknown", never as "free". */
export async function fetchOccupancy(baseUrl: string, timeoutMs = 2500):
    Promise<ServerOccupancy | null> {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const r = await fetch(baseUrl.replace(/\/$/, '') + '/vibeserver.json',
                          { signal: ctrl.signal, cache: 'no-store' });
    if (!r.ok) return null;
    const j = await r.json();
    if (!j || j.server !== 'vibeserver') return null;
    return {
      busy:      j.busy === true,
      // ★ Older servers predate these fields. Defaulting freeInSec to -1 keeps "unknown"
      // distinct from "free now", which are very different things to show someone.
      freeInSec: typeof j.freeInSec === 'number' ? j.freeInSec : -1,
      limitMin:  typeof j.limitMin === 'number' ? j.limitMin : 0,
      admin:     j.admin === true,
    };
  } catch { return null; }
  finally { clearTimeout(t); }
}

export async function probeServer(
  host: string, port: number, hint?: BackendType | null, baseUrl?: string,
): Promise<BackendType | null> {
  if (hint) return hint;

  // ★ A URL WINS WHEN WE HAVE ONE. A receiver in a subfolder does not answer at the root, so
  // probing `host:port` alone reports "nothing there" for a server that is plainly running.
  if (baseUrl) {
    const t = await detectServerType(baseUrl);
    if (t) return t;
  }

  const authority = `${host}:${port}`;
  for (const scheme of ['https', 'http'] as const) {
    const t = await detectServerType(`${scheme}://${authority}`);
    if (t) return t;
  }

  // No HTTP answered. Raw-TCP backends can only be inferred from the port.
  if (port === DEFAULT_PORT.rtltcp) return 'rtltcp';
  if (port === DEFAULT_PORT.spyserver) return 'spyserver';
  return null;                  // unreachable, or raw TCP on a port we can't name
}
