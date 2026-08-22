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
// ★ 6.25 kHz is the narrowband digital-voice raster — dPMR and NXDN both channel
//   on it, and so does the 12.5 kHz offset grid. Without it those channels can
//   only be reached by typing the frequency, which is what a user reported on
//   GitHub. It sits between 5k and 12.5k so the ladder stays ascending.
// ★★ 100 Hz and 500 Hz added 2026-07-30. 1 kHz was the floor above 30 MHz, which made some
//    airband channels UNREACHABLE by the controls at all — an airband VOLMET at 128.5928 MHz was
//    the case that surfaced it: the grid snaps to whole kHz and steps straight over it. Typeable on
//    the phone, but on the WATCH there is no keypad on that path, so the channel simply did not
//    exist. Exactly the argument that added 6.25 kHz above, one order of magnitude finer.
//    ★ It is only an option — nobody has to cross VHF in 100 Hz steps, they just gain the ability
//      to land on a channel that is not on a kHz boundary.
export const STEPS_VHF = [100, 500, 1000, 5000, 6250, 12500, 25000, 50000, 100000];
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

export type ServerType = 'ubersdr' | 'kiwi' | 'web888' | 'owrx';

/** ★★★ WEB-888 SPEAKS KIWI, BUT NOT AT THE SAME URL — and that one difference is the whole
 *  reason a Web-888 could not be connected to at all, on either the KiwiSDR or the OpenWebRX
 *  setting, until 2026-08-03.
 *
 *  A Web-888 (and anything else built on RaspSDR/server, e.g. the RX-888 boxes) runs a FORK of
 *  the KiwiSDR server taken before jks-prv moved to the mongoose 7 API. Upstream Kiwi now needs
 *  a `ws/` marker on the URL so its web server can tell a WebSocket upgrade from a plain GET —
 *  its own source says so (Beagle_SDR_GPS/rx/rx_server.cpp):
 *
 *      // The new mongoose API requires something in the URL to distinguish web socket
 *      // connections from normal HTTP transfers. … We prefix web socket URLs with "ws/".
 *      if ((n=sscanf(uri_ts, "ws/%8m[^/]/%lld/%256m[^\?]", …)) == 3) isWebSocket = true;
 *
 *  The fork never gained that branch. RaspSDR/server/rx/rx_server.cpp accepts ONLY:
 *
 *      kiwi/<ts>/<stream>      no_wf/<ts>/<stream>      <ts>/<stream>   (kiwirecorder)
 *      else printf("bad URI_TS format\n"); return NULL;      // ← line 280, and our fate
 *
 *  So `/ws/kiwi/<ts>/SND` hit that else, the server returned NULL, and the socket was closed
 *  with code 1006 and ZERO bytes sent. VERIFIED against a live Web-888 (sw_version
 *  Web888_v2026.609) on 2026-08-03: `/ws/kiwi/…` closes instantly; `/kiwi/…` completes the
 *  handshake and streams both audio and waterfall.
 *
 *  ★★ AND THE ERROR MESSAGE BLAMED THE OWNER. A reasonless close is what a Kiwi that only
 *  allows its own web page looks like, so onSocketDrop told the user this receiver "blocks apps
 *  like VibeSDR" — about their OWN radio, sitting on their own desk. A wrong diagnosis is worse
 *  than none: it sends someone to argue with a restriction that does not exist.
 *
 *  ★ 'openwebrx' IS NOT THE ANSWER EITHER, however much the lineage suggests it. Kiwi's *web UI*
 *  was forked from OpenWebRX years ago — that is why our own client string is
 *  `SERVER DE CLIENT openwebrx.js` and why the landing page still says "openwebrx". The modern
 *  OpenWebRX+ WIRE protocol shares nothing with it: one socket at `/ws/`, JSON control plane.
 *  OwrxAdapter cannot talk to a Web-888 and never could.
 */
export function isKiwiProtocol(t?: string | null): boolean {
  return t === 'kiwi' || t === 'web888';
}

/** How each Kiwi-protocol dialect names itself to the user. Keep these in step with
 *  PROTO_LABEL in InstancePickerScreen — a Web-888 owner told "KiwiSDR closed the
 *  connection" has to guess that the app means their radio. */
export function kiwiFamilyLabel(t?: string | null): string {
  return t === 'web888' ? 'Web-888' : 'KiwiSDR';
}

/** Everything the "Custom server" box can reach. The HTTP kinds are what
 *  detectServerType() can sniff; the raw-TCP kinds (rtl_tcp, SpyServer) speak no
 *  HTTP at all, so they can only be reached by an explicit choice or a port
 *  convention — see probeServer(). */
export type BackendType = ServerType | 'fmdx' | 'vibeserver' | 'rtltcp' | 'spyserver';

/** Default port per backend, used to guess a bare host and to prefill the form. */
export const DEFAULT_PORT: Record<BackendType, number> = {
  ubersdr: 8073, kiwi: 8073, web888: 8073, owrx: 8073, fmdx: 8080,
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
  //
  // ★★★ ONE BUDGET PER REQUEST, NOT ONE FOR THE WHOLE FUNCTION. A single shared controller meant
  // the VibeServer identity probe could spend the ENTIRE 5 s and then abort the landing-page fetch
  // — the one that actually identifies the server — before it had sent a byte. Detection then
  // returned null: "nothing there", for a receiver answering in half a second.
  //
  // ★★ And it is not hypothetical. Measured 2026-08-03: a KiwiSDR does not 404 an unknown path,
  // it simply NEVER ANSWERS. `GET /vibeserver.json` against kiwisdr.areg.org.au:8073 and
  // sdr.ironstonerange.com:8073 hangs open indefinitely, while `GET /` on both returns in ~0.6 s.
  // So the shared timer made the sniff unreachable on the whole KiwiSDR family — including
  // Web-888, whose auto-detection this fix is what makes reliable.
  const withTimeout = async (u: string): Promise<Response> => {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 5000);
    try { return await fetch(u, { signal: ctrl.signal }); }
    finally { clearTimeout(timer); }
  };
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
      const idr = await withTimeout(base + '/vibeserver.json');
      if (idr.ok) {
        const id = await idr.json();
        if (id && id.server === 'vibeserver') return 'vibeserver';
      }
    } catch { /* not a VibeServer, or older than this endpoint — fall through */ }

    const r = await withTimeout(base + '/');
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
    // Web-888 / RaspSDR BEFORE Kiwi, for the same reason UberSDR goes before Kiwi above: it IS a
    // Kiwi fork, so its page carries every Kiwi marker (`kiwi_util`, `data-type=kiwi`, and
    // "openwebrx" too) and matched as plain 'kiwi' — which then used the upstream `ws/` URL and
    // could not connect at all. See isKiwiProtocol() for why the URL differs.
    // ★ Match the BRANDING, which is the only thing that separates the fork from its parent: the
    //   firmware serves its own logo (`gfx/web-888.51x60.png`) and links rx-888.com. Verified on a
    //   live Web-888, 2026-08-03. `raspsdr` covers the upstream project's own builds.
    // ★★ These are hyphen-optional on purpose — the product is written "Web-888" and "web888"
    //    (its own sw_version is `Web888_v2026.609`) about equally often.
    if (/web-?888|rx-?888|raspsdr/.test(body)) return 'web888';
    if (/kiwisdr|kiwi sdr|\/kiwi\/|kiwi_util|owrx_ws_open/.test(body)) return 'kiwi';
    if (body.includes('openwebrx')) return 'owrx';
    if (/fm-dx|fmdx/.test(body)) return 'fmdx';
    return 'ubersdr';            // reachable but unidentifiable → assume ubersdr
  } catch {
    return null;                // couldn't reach — caller keeps any known type
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
  /** ★★★ HOW THAT LIMIT BEHAVES. 'soft' means it is a GUARANTEE, not a deadline: when it runs out
   *  you keep the radio until somebody else actually wants it, and only then does the server give
   *  60 seconds' notice. ★ ABSENT MEANS HARD — every older server implies that, and a client which
   *  cannot tell them apart has to assume the worst and says so, wrongly, for the rest of a session
   *  that is not ending. (Jr did exactly that: it declared the session over at zero while the audio
   *  was still playing — GitHub #21 / Stuart, 2026-08-21.) */
  limitSoft: boolean;
  /** Does this server have an admin password? Only then is an override box worth offering. */
  admin: boolean;
  /** ★★★ THE SERVER'S OWN IDENTITY, stable across every route into it. A VibeServer reached as
   *  `demo.vibesdr.net` and as `192.168.86.88:48000` is ONE receiver, and anything keyed on the URL
   *  believes it is two — which is exactly what made bookmarks appear to vanish (Stuart, explaining
   *  it on GitHub #21: "to VibeSDR these are 2 completely different servers"). The shim mints this
   *  per radio and it does not change with the address you arrived by.
   *  ★ undefined on every other backend and on older VibeServers, so callers must keep a URL
   *    fallback rather than assuming it. */
  instance?: string;
  /** ★★ THE OWNER'S UNCOMPRESSED-AUDIO POLICY, and it is THREE-way for a reason — see
   *  the note in VibeServer: 'off' never offers raw PCM to a networked listener, 'compat'
   *  falls back automatically with no control shown, and only 'choice' means the listener
   *  may pick. A bool could not express it, because compatibility and quality want opposite
   *  defaults. ★ Show the switch ONLY for 'choice': raw PCM is ~187 KB/s off the owner's
   *  uplink, and offering it where they said no is spending someone else's bandwidth.
   *  ★ Undefined = an older server that predates the field: treat as 'off'. */
  uncompressed?: 'off' | 'choice' | 'compat';
  /** ★ The server's own version ("3.0.0"). Absent from builds that predate the field — treat a
   *  missing version as UNKNOWN and say nothing, never as "old": a wrong version on screen is
   *  worse than none, because it is the thing people quote in a bug report. */
  version?: string;
  /** ★ The owner's notice to listeners ("antenna maintenance in progress"), or absent. Read on
   *  connect so someone arriving AFTER it was posted still sees it — the live push only reaches
   *  sockets that were already open. */
  notice?: string;
  /** ★★ WHAT IS PLUGGED INTO IT, and the owner's standing message. Carried here as well as in the
   *  door's radio list because a SINGLE-radio server has no door — and those two fields are
   *  exactly what decides whether it deserves a landing screen at all (Stuart, 2026-08-20: "if no
   *  antenna details or landing screen text then it connects straight through"). */
  antenna?: string;
  antennaIcon?: string;
  landingMessage?: string;
  landingLinkUrl?: string;
  landingLinkLabel?: string;
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
      limitSoft: j.limitMode === 'soft',
      admin:     j.admin === true,
      uncompressed: j.uncompressed === 'choice' || j.uncompressed === 'compat'
                    || j.uncompressed === 'off' ? j.uncompressed : undefined,
      version:   typeof j.version === 'string' && j.version ? j.version : undefined,
      instance:  typeof j.instance === 'string' && j.instance ? j.instance : undefined,
      notice:    typeof j.notice === 'string' && j.notice ? j.notice : undefined,
      antenna:   typeof j.antenna === 'string' && j.antenna ? j.antenna : undefined,
      antennaIcon: typeof j.antennaIcon === 'string' ? j.antennaIcon : undefined,
      landingMessage: typeof j.landingMessage === 'string' && j.landingMessage
                      ? j.landingMessage : undefined,
      landingLinkUrl: typeof j.landingLinkUrl === 'string' && j.landingLinkUrl
                      ? j.landingLinkUrl : undefined,
      landingLinkLabel: typeof j.landingLinkLabel === 'string' && j.landingLinkLabel
                        ? j.landingLinkLabel : undefined,
    };
  } catch { return null; }
  finally { clearTimeout(t); }
}

/** What answered, and WHERE. */
export interface ProbeResult {
  type: BackendType;
  /** ★★★ THE ADDRESS THAT ACTUALLY ANSWERED — connect to THIS, not to what the user typed.
   *  probeServer tries several candidate URLs and used to throw away which one worked, returning
   *  only the type. Every caller then connected to its own guess instead, so detection succeeding
   *  on one address and the connection being made to another was a silent, routine outcome:
   *
   *    - typed `web-888.local` + `8073` in the separate Port box → detection found it at
   *      `http://web-888.local:8073`, the connect went to `http://web-888.local` (port 80) and
   *      died with "connection refused". Reported 2026-08-03 against a Web-888; the favourite it
   *      saved was typed CORRECTLY, which is what made it look like a Web-888 bug rather than an
   *      address bug — tapping the saved row worked, because that path rebuilds the URL.
   *    - an https-only receiver on a custom port: detected over https, connected over http.
   *
   *  Returning the winning URL makes "what we probed" and "what we connect to" the same string
   *  by construction, which is the only way this class of bug stays fixed. */
  url: string;
}

/**
 * ★★★ A VIBESERVER MAY ANSWER AT AN ADDRESS THAT IS NOT WHERE IT LIVES.
 *
 * A server behind a Cloudflare Quick Tunnel is published at a STABLE name —
 * <slug>.vibeserver.vibesdr.net — because the tunnel's own hostname rotates on every restart and a
 * BROWSER keys localStorage by origin. That stable address serves the web page and proxies HTTP,
 * but it deliberately does NOT carry the WebSocket: the audio and the spectrum go straight to the
 * tunnel so they never cross our Worker.
 *
 * ★★ The web client is told where to point via an injected global. AN APP NEVER LOADS THAT PAGE,
 *    so it would proxy its HTTP happily and then open a socket against the Worker, which refuses
 *    upgrades — failing at the last step with everything before it working (Stuart, 2026-08-22,
 *    having typed the friendly address into the app).
 *
 * So the directory tells any client the real address in the identity response every client already
 * fetches. Absent on a LAN server, a port-forwarded one, or anything reached directly — then this
 * returns null and the caller keeps the address it had.
 */
async function vibeDirectUrl(base: string): Promise<string | null> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 5000);
  try {
    const r = await fetch(base + '/vibeserver.json', { signal: ctrl.signal, cache: 'no-store' });
    if (!r.ok) return null;
    const j = await r.json();
    const direct = typeof j?.directUrl === 'string' ? j.directUrl.trim() : '';
    // ★ Only ever an http(s) URL, and only when it actually differs — a server that answers for
    //   itself must not be sent somewhere else on the strength of a stray field.
    if (!/^https?:\/\//i.test(direct)) return null;
    return direct.replace(/\/+$/, '') === base.replace(/\/+$/, '') ? null : direct.replace(/\/+$/, '');
  } catch { return null; } finally { clearTimeout(timer); }
}

export async function probeServer(
  host: string, port: number, hint?: BackendType | null, baseUrl?: string,
): Promise<ProbeResult | null> {
  const fallbackUrl = baseUrl || `http://${host}:${port}`;
  // An explicit choice is not probed at all, so the caller's address is all we know.
  if (hint) return { type: hint, url: fallbackUrl };

  // ★ A URL WINS WHEN WE HAVE ONE. A receiver in a subfolder does not answer at the root, so
  // probing `host:port` alone reports "nothing there" for a server that is plainly running.
  if (baseUrl) {
    const t = await detectServerType(baseUrl);
    // ★★ FOLLOW THE SERVER'S OWN ANSWER ABOUT WHERE IT LIVES — see vibeDirectUrl(). Without this
    //    a tunnelled receiver connects for everything except the socket.
    if (t === 'vibeserver') return { type: t, url: (await vibeDirectUrl(baseUrl)) || baseUrl };
    if (t) return { type: t, url: baseUrl };
  }

  const authority = `${host}:${port}`;
  for (const scheme of ['https', 'http'] as const) {
    const url = `${scheme}://${authority}`;
    const t = await detectServerType(url);
    if (t === 'vibeserver') return { type: t, url: (await vibeDirectUrl(url)) || url };
    if (t) return { type: t, url };
  }

  // No HTTP answered. Raw-TCP backends can only be inferred from the port.
  if (port === DEFAULT_PORT.rtltcp) return { type: 'rtltcp', url: fallbackUrl };
  if (port === DEFAULT_PORT.spyserver) return { type: 'spyserver', url: fallbackUrl };
  return null;                  // unreachable, or raw TCP on a port we can't name
}
