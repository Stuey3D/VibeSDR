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
export async function probeServer(
  host: string, port: number, hint?: BackendType | null,
): Promise<BackendType | null> {
  if (hint) return hint;

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
