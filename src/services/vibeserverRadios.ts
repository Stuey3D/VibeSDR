/**
 * A multi-radio VibeServer (V3), from the app's side.
 *
 * ★★★ A V3 SERVER IS A FRONT DOOR THAT OWNS NO RADIO. Every radio lives behind `/r/<id>/…`, so a
 *     client that asks the door for `/ws/user-spectrum` is asking for a radio the door does not
 *     have. It answers 503 — which, as a WebSocket handshake, reaches the app as a bare 1006 with
 *     nothing in it, indistinguishable from "server down" (measured on the demo, 2026-08-10).
 *
 * ★★★ BUT CONNECTING IS NOT THE FEATURE. Pointing the base URL at `/r/<id>` makes it work and is
 *     a useful diagnostic; the actual gaps are that the app cannot CHOOSE a radio and has no way
 *     into the admin or setup pages at all. This module supplies the first half: what is behind
 *     the door, described well enough to choose from WITHOUT opening anything.
 *
 * ★★ `id`, never `serial`. Serials are deliberately kept out of URLs — a serial identifies the
 *    hardware and turns up in logs, referrers and shared links. The door still accepts a serial so
 *    old links keep working, which is exactly why the app must not start minting new ones.
 */

/** One radio behind the door. Field names are the server's own — see GET /vibeserver/radios. */
export interface VibeRadio {
  id: string;
  label: string;
  driver: string;
  /** Configured listener cap: 1 is a single-user radio, more is a shared one. */
  users: number;
  /** A locked-range profile — listeners tune inside a captured window but cannot move the radio. */
  locked: boolean;
  /** The owner has restricted which frequencies may be tuned. */
  restricted: boolean;
  /** Where it is pointed right now, so the picker can say what a radio is FOR. */
  centreHz?: number;
  spanHz?: number;
  mode?: string;
  /** Tuning ranges the owner permits, [loHz, hiHz] pairs. */
  coverage?: [number, number][];
  allowed?: [number, number][];
  /** ★ Those same permitted ranges NAMED, where they match the server's band plan — "FM
   *  broadcast", "AM (medium wave) broadcast". The server names them because the plan is region
   *  aware; a client that recited the hardware's reach instead described a receiver nobody can
   *  use as advertised. Absent when nothing matched, or on an older server. */
  allowedNames?: string[];
  /** The radio this machine nominates for the spectrogram and band conditions. */
  primary?: boolean;
  /** ★★ WHAT IS ACTUALLY CONNECTED TO IT — "YouLoop 10 kHz – 300 MHz". A receiver publishes what
   *  the TUNER can reach and never what the AERIAL can, and the gap between those two is the
   *  difference between a promising card and a disappointing listen. The browser has shown this
   *  since 2026-08-19; the app never asked for it. */
  antenna?: string;
  /** Which line drawing goes beside it — a key, so an app that does not know this one draws its
   *  default rather than nothing. */
  antennaIcon?: string;
}

export interface VibeFrontDoor {
  /** The machine's name, shown above the list. */
  name: string;
  radios: VibeRadio[];
  /** ★★★ THE OWNER'S STANDING MESSAGE — donation link, house rules, "5 fps idle is normal". NOT
   *  the transient notice: this one does not expire and is not dismissed, because it is what the
   *  operator wants every visitor to read. It has been on the browser's landing page since
   *  2026-08-19 and the app showed nothing at all. */
  landingMessage?: string;
  landingLinkUrl?: string;
  landingLinkLabel?: string;
}

const num = (v: any, d = 0) => (typeof v === 'number' && isFinite(v) ? v : d);

/**
 * Ask a server what is behind it.
 *
 * Returns null when this is NOT a front door — an ordinary single-radio VibeServer, a Kiwi, an
 * OpenWebRX, anything. ★★ Callers must treat null as "connect as you always have": a V2 server, a
 * Simple-mode Mac or phone, and every third-party backend all answer that way, and they are the
 * overwhelming majority.
 */
export async function fetchFrontDoor(
  baseUrl: string, timeoutMs = 4000,
): Promise<VibeFrontDoor | null> {
  const base = baseUrl.replace(/\/+$/, '');
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const r = await fetch(`${base}/vibeserver/radios`, { signal: ctrl.signal, cache: 'no-store' });
    if (!r.ok) return null;
    const j: any = await r.json();
    // ★★ `frontDoor` is ABSENT on a radio and on every older server, so a missing key must read as
    //    "ordinary receiver" — which `!== true` gives for free. The endpoint itself is not enough
    //    to tell them apart: a single-radio V3 answers it too, describing only itself.
    if (j?.frontDoor !== true || !Array.isArray(j?.radios)) return null;
    const radios: VibeRadio[] = j.radios
      .filter((x: any) => x && typeof x.id === 'string' && x.id)
      .map((x: any) => ({
        id: String(x.id),
        label: String(x.label || x.driver || 'Radio'),
        driver: String(x.driver || ''),
        users: num(x.users, 1),
        locked: x.locked === true,
        restricted: x.restricted === true,
        centreHz: typeof x.centreHz === 'number' ? x.centreHz : undefined,
        spanHz: typeof x.spanHz === 'number' ? x.spanHz : undefined,
        mode: typeof x.mode === 'string' ? x.mode : undefined,
        coverage: Array.isArray(x.coverage) ? x.coverage : undefined,
        allowed: Array.isArray(x.allowed) ? x.allowed : undefined,
        allowedNames: Array.isArray(x.allowedNames)
          ? x.allowedNames.filter((n: any) => typeof n === 'string') : undefined,
        primary: x.primary === true,
        antenna: typeof x.antenna === 'string' && x.antenna ? x.antenna : undefined,
        antennaIcon: typeof x.antennaIcon === 'string' ? x.antennaIcon : undefined,
      }));
    if (!radios.length) return null;         // a door with nothing behind it is not a choice
    return {
      name: String(j.name || 'VibeServer'),
      radios,
      landingMessage: typeof j.landingMessage === 'string' && j.landingMessage
        ? j.landingMessage : undefined,
      landingLinkUrl: typeof j.landingLinkUrl === 'string' && j.landingLinkUrl
        ? j.landingLinkUrl : undefined,
      landingLinkLabel: typeof j.landingLinkLabel === 'string' && j.landingLinkLabel
        ? j.landingLinkLabel : undefined,
    };
  } catch {
    return null;                             // offline, or not a VibeServer — same answer either way
  } finally {
    clearTimeout(t);
  }
}

/**
 * The base URL for one radio behind a door.
 *
 * ★★ Everything the client builds — websockets, /connection, /vibeserver/hardware, bookmarks —
 *    hangs off the base URL, so putting the prefix HERE is what makes one change reach all of
 *    them. The server strips it once on arrival, deliberately, so that every route below keeps
 *    matching bare paths (see the note in local_sdr_shim.cpp: stripping at each of the dozens of
 *    routes would be a list to keep in step for ever, and the first one forgotten is a 404 that
 *    appears only on multi-radio machines).
 */
export function radioBaseUrl(baseUrl: string, id: string): string {
  const base = baseUrl.replace(/\/+$/, '');
  return id ? `${base}/r/${encodeURIComponent(id)}` : base;
}

/** ★★★ ONE DIAL EVERYBODY HEARS — an UNLOCKED radio with room for more than one listener.
 *
 *  The distinction the app has to make before it connects, because it decides whether arriving is
 *  allowed to move the radio: on a shared dial, restoring the frequency you were last on would
 *  drag everybody else's listening to your remembered station the moment you appeared, and
 *  nobody would have asked for it (Stuart, 2026-08-20).
 *  ★ Derived, not a new wire field: `locked` and `users` already say it, and a third way to
 *    describe one state is a thing to keep in step for ever.
 */
export function isSharedDial(r: VibeRadio | null | undefined): boolean {
  return !!r && !r.locked && (r.users ?? 1) > 1;
}

/** A short human description of what a radio is for — driver, where it is pointed, how it shares. */
export function describeRadio(r: VibeRadio): string {
  const bits: string[] = [];
  if (r.centreHz && r.centreHz > 0) {
    const mhz = r.centreHz / 1e6;
    bits.push(`${mhz >= 100 ? mhz.toFixed(1) : mhz.toFixed(3)} MHz${r.mode ? ' ' + r.mode.toUpperCase() : ''}`);
  }
  // ★★ SAY WHICH KIND OF SHARING. "Shared" was true of two completely different receivers — one
  //    dial everybody hears, or a fixed window in which everybody tunes independently — and those
  //    are the two things a listener most wants to know apart before choosing.
  if (isSharedDial(r))     bits.push(`shared VFO · up to ${r.users}`);
  else if (r.users > 1)    bits.push(`individual VFOs · up to ${r.users}`);
  else                     bits.push('one listener at a time');
  if (r.locked) bits.push('fixed window');
  // ★ Name the bands where the server named them; "limited range" says only that a wall exists.
  if (r.allowedNames && r.allowedNames.length) bits.push(r.allowedNames.join(', '));
  else if (r.restricted) bits.push('limited range');
  return bits.join(' · ');
}
