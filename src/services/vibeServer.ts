import { NativeModules, Platform } from 'react-native';
import { loadActiveEibi } from './eibi';
import { getUserLocation } from './instancesApi';
import { getServerName, PUBLIC_NAME_KEY } from './rtlTcpServer';
import { latLonToGrid, gridToLatLon } from './grid';
import type { ServerBookmark } from './stations';
import { parseBookmarksAny } from './userBookmarks';
import AsyncStorage from '@react-native-async-storage/async-storage';

// VibeServer: share this device's USB dongle with server-side DSP (compressed
// audio + waterfall over a WebSocket, ~25x lighter than raw RTL-TCP IQ). The
// heavy lifting is the SAME shim used for local listening — here it's LAN-bound
// and silent on the serving phone, so the single client slot goes to the one
// remote VibeSDR. Android-only (only Android owns the local USB dongle).

const Local: any = (NativeModules as any).VibeLocalSDR;

export const vibeServerSupported =
  Platform.OS === 'android' && !!Local?.startVibeServer;

// Waterfall frame-rate tiers. Our Local Hardware default is 20 fps; Half (10) is
// exactly UberSDR's shipping default, Quarter (5) sits just under OWRX's 9 — the
// client interpolates the waterfall so a throttled rate still scrolls smoothly.
export type FpsTier = 'full' | 'half' | 'quarter';
export const FPS_TIERS: { key: FpsTier; label: string; fps: number }[] = [
  { key: 'full',    label: 'Full · 20 fps',    fps: 20 },
  { key: 'half',    label: 'Half · 10 fps',    fps: 10 },
  { key: 'quarter', label: 'Quarter · 5 fps',  fps: 5  },
];
export const fpsForTier = (t: FpsTier) => FPS_TIERS.find(x => x.key === t)?.fps ?? 20;

// Optional demod-bandwidth cap (server-side), for low-end hosts / slow networks.
// 0 = no cap (client gets the full control set).
export type VibeServerConfig = {
  name: string;
  centerFreq?: number;
  sampleRate?: number;
  mode?: string;
  pin: string;              // '' = open access (no PIN)
  maxBandwidthHz?: number;  // 0 = no cap
  maxFftRate?: number;      // 0 = server default (20 fps)
  compressAudio?: boolean;
  /** ★ Admin password — gates CONTROL (bias-T, direct sampling, calibration), NOT access.
   *  Separate from the listening PIN: a public receiver can welcome every listener and still
   *  refuse a visitor putting DC on the feedline. Empty = nothing protected. */
  adminPassword?: string;
  /** 0 = off, 1 = listener's choice, 2 = compatibility fallback only.
   *  ★ Loopback is OUTSIDE this setting entirely — it rations the owner's uplink. */
  uncompressedAudio?: 0 | 1 | 2;
  /** ★ Per-listener time limit, MINUTES. 0 = unlimited (default, and right for a private
   *  receiver). Loopback and admin sessions are exempt; an expired listener is held on a short
   *  cooldown, without which their client would simply reconnect and carry on. */
  sessionLimitMin?: number;  // default true
  /** Serve the browser client at GET /. Off = only the VibeSDR app can connect,
   *  so a stranger can't stumble in from a URL. Default true. */
  webServer?: boolean;
  /** Pin the capture rate: clients cannot change it, and their picker is hidden.
   *  0 (the default) = client-controlled, as on the RTL-TCP server. */
  lockedRate?: number;
  /** mDNS advertise. Passed to native only so a crash-restored server re-advertises. */
  advertise?: boolean;
  /** Rebuild the server if the app process dies under it. Default true. */
  autoRestore?: boolean;
  /** ★★★ ADVANCED MODE — the management surface. NOT "Full": the Mac and the Pi serve several
   *  radios behind a front door, and a phone serves ONE (it cannot power three over OTG), so
   *  calling it Full would promise a parity it cannot deliver (Stuart, 2026-08-12).
   *  ★ It adds no process and no port. Everything below is applied to the radio already running. */
  advanced?: boolean;
  /** Listeners sharing one radio. 1 = single occupant, the Simple-mode behaviour. */
  maxUsers?: number;

  // ── The rest of the server's settings ──────────────────────────────────────────────────────
  // ★★★ THE PHONE RUNS THE SAME SERVER, ONE RADIO AT A TIME (Stuart, 2026-08-19: "functionally
  //     identical to the main server app just with only one radio at a time and the setup done on
  //     the phone rather than a web gui/tui"). Everything below already worked in the engine and
  //     was reachable only from the desktop binary — the phone simply had no way to ask.

  /** The time limit is a GUARANTEE rather than a deadline: kept past its time until someone waits.
   *  ★ Absent/false = hard, which is what a limit has always meant here. */
  sessionLimitSoft?: boolean;
  /** Minutes with NO interaction before a listener is asked "still listening?", then released.
   *  0 = off (the default). The server clamps to a 15-minute floor, and a listener watching a
   *  decoder is never interrupted. Shared radios only. */
  idleKickMin?: number;
  /** ★★★ THE CAPTURED WINDOW, in Hz — what makes shared listening possible: everyone gets a slice
   *  of ONE window, so the centre must not move. 0 = follows the listener, which is the only
   *  behaviour the phone has ever had and is right for a single occupant who retunes the radio
   *  itself. Meaningless the moment several people share it. */
  lockedCentre?: number;
  /** Real bins at deep zoom instead of interpolation. Without it a shared, locked receiver goes
   *  blocky the moment anybody zooms, which is the whole point of the mode. */
  zoomSpectrum?: boolean;
  /** Draw the landing page's 24-hour spectrogram from this radio. Not available when the radio
   *  powers down while idle — it cannot picture a band it is not listening to. */
  spectrogram?: boolean;
  /** The spectrum slowdown when nobody is looking. CPU and uplink, not the radio. */
  forceIdleSaver?: boolean;
  /** Seconds after the last listener before the capture parks to save power. The device stays
   *  CLAIMED so it restarts instantly.
   *  ★ NOT the Linux "release to another program": Android's permission model means nothing else
   *    can pick the dongle up anyway, so releasing would cost the restart and buy nothing. */
  idleGraceSec?: number;
  /** What is bolted to this radio, and which of the eleven icons to draw beside it. */
  antenna?: string;
  antennaIcon?: string;
  /** The owner's standing message on the landing screen, and an optional link.
   *  ★ NOT the transient maintenance notice: this one stays up. http/https only — the server
   *    drops anything else. */
  landingMessage?: string;
  landingLinkUrl?: string;
  landingLinkLabel?: string;
  /** Where listeners may tune. Block always wins over allow. */
  allowRanges?: string;
  blockRanges?: string;
  /** Per-band gain ceilings ("all:250,fm:150"), the gain to return to when everyone leaves
   *  (-1 = leave alone), and whether the AGC is locked on. */
  gainLimits?: string;
  restGain?: number;
  agcLock?: boolean;
  /** ★ RTL gain automation. Protection defaults ON (it can only prevent clipping); the AGC defaults
   *  OFF, because it may raise the gain above the owner's figure. See VibeServerBoot. */
  rtlAgc?: boolean;
  /** ★ RTL only: the tuner IF filter narrows as a listener zooms in. */
  tunerBwAuto?: boolean;
  /** Reverse proxies whose X-Forwarded-For we believe — required behind a tunnel, or every
   *  visitor arrives as 127.0.0.1 and the limits and ban list cannot tell anyone apart. */
  trustedProxies?: string;
  /** ★ False lets one address hold several radios — see the server screen. Default true. */
  oneRadioPerIp?: boolean;
};

export type VibeServerInfo = { ip: string; port: number; name: string };

export type VibeServerStatus = {
  running: boolean;
  client: boolean;
  clientAddr: string;
  specBytesPerSec: number;
  audioBytesPerSec: number;
  compressed: boolean;
  pinEnabled: boolean;
  fftRate: number;
  bandwidthHz: number;
  /** Capture sample rate the CLIENT currently has the server running at. Shown on
   *  the sharing screen so the host can SEE the server answering the client. */
  sampleRate: number;
  port: number;
  ip: string;
  /** Percent of ONE core used by the whole app (so >100 is possible and meaningful on a
   *  multi-core phone) — the same convention the DSP benchmarks use, so a reading here is
   *  directly comparable with the Pi figures. 0 = not measured. */
  cpu: number;
  cores: number;
  /** ★★ HOW MANY ARE LISTENING, from the shim's one authoritative counter — the same number the
   *  admin page and every picker use. `client` is derived from it, so the host's screen and the
   *  server can no longer hold different opinions about whether anybody is on. */
  listeners: number;
  maxUsers: number;
};

export async function startVibeServer(cfg: VibeServerConfig): Promise<VibeServerInfo> {
  const info = await Local.startVibeServer({
    name: cfg.name,
    centerFreq: cfg.centerFreq,
    sampleRate: cfg.sampleRate,
    mode: cfg.mode,
    pin: cfg.pin,
    maxBandwidthHz: cfg.maxBandwidthHz ?? 0,
    maxFftRate: cfg.maxFftRate ?? 0,
    compressAudio: cfg.compressAudio ?? true,
    adminPassword: cfg.adminPassword ?? '',
    uncompressedAudio: cfg.uncompressedAudio ?? 0,
    sessionLimitMin: cfg.sessionLimitMin ?? 0,
    webServer: cfg.webServer ?? true,
    lockedRate: cfg.lockedRate ?? 0,
    advertise: cfg.advertise ?? true,
    autoRestore: cfg.autoRestore ?? true,
    advanced: cfg.advanced ?? false,
    maxUsers: cfg.maxUsers ?? 1,
    // ★ Every one of these is applied on EVERY start, including the crash-restore path, so a
    //   rebuilt server is the same server rather than one that quietly lost half its settings.
    sessionLimitSoft: cfg.sessionLimitSoft ?? false,
    idleKickMin: cfg.idleKickMin ?? 0,
    lockedCentre: cfg.lockedCentre ?? 0,
    zoomSpectrum: cfg.zoomSpectrum ?? false,
    spectrogram: cfg.spectrogram ?? false,
    forceIdleSaver: cfg.forceIdleSaver ?? false,
    // ★ 300 s matches the desktop default. The radio parks; it is never handed away.
    idleGraceSec: cfg.idleGraceSec ?? 300,
    antenna: cfg.antenna ?? '',
    antennaIcon: cfg.antennaIcon ?? '',
    landingMessage: cfg.landingMessage ?? '',
    landingLinkUrl: cfg.landingLinkUrl ?? '',
    landingLinkLabel: cfg.landingLinkLabel ?? '',
    allowRanges: cfg.allowRanges ?? '',
    blockRanges: cfg.blockRanges ?? '',
    gainLimits: cfg.gainLimits ?? '',
    restGain: cfg.restGain ?? -1,
    agcLock: cfg.agcLock ?? false,
    rtlAgc: cfg.rtlAgc ?? false,
    tunerBwAuto: cfg.tunerBwAuto ?? false,
    trustedProxies: cfg.trustedProxies ?? '',
    // ★ Absent = true, matching the server's own default: refuse a second radio to one address.
    oneRadioPerIp: cfg.oneRadioPerIp ?? true,
  });
  // Hand the web client's search its station list. Fire-and-forget: the server is
  // already up and useful without it, and this can involve a network fetch.
  void publishStations();
  void publishLocation();
  startBookmarkAutosave();
  return info;
}

/**
 * Publish the station list the web client searches (GET /stations on the shim).
 *
 * The APP does this, not the shim, for two reasons:
 *   1. A browser CANNOT fetch eibispace.de — it sends no Access-Control-Allow-Origin,
 *      and unlike React Native a browser enforces CORS. Served from the shim it's
 *      same-origin, so the problem disappears.
 *   2. The app already owns the EiBi download + seasonal cache, so the search keeps
 *      working with no internet at query time — the allotment case.
 *
 * Same model as UberSDR: the server presents the stations, the client renders them.
 */
export async function publishStations(): Promise<void> {
  if (!Local?.setStationsJson) return;
  try {
    const eibi = await loadActiveEibi();
    if (!eibi.length) return;
    Local.setStationsJson(JSON.stringify(eibi));
  } catch {
    // Offline or EiBi unreachable — the web client degrades to bookmarks + band
    // plan, which are both local. Not worth surfacing.
  }
}

// ── Learned station bookmarks (RDS) ─────────────────────────────────────────
//
// The SHIM learns these — it is the only place that sees both the tuned frequency
// and the decoded RDS name. It has no storage of its own, so the app persists them,
// exactly as it does the station list.

const BM_KEY = 'vs_learned_bookmarks';

/**
 * Bookmark persistence now lives in the SHIM (setBookmarksPath), not here.
 *
 * The app used to pull the list out on a 60s JS timer and write it to AsyncStorage. It
 * did not work: while the server is serving, the app is BACKGROUNDED, and JS timers are
 * throttled or suspended there — so the save often never ran, and an import of 145
 * bookmarks appeared in the list, lived in memory, and vanished at the next restart.
 * Worse, on start-up the JS pushed its stale copy back INTO the shim, which would now
 * clobber whatever the shim had correctly loaded from its own file.
 *
 * The shim owns the bookmarks, so the shim saves them — on every change, atomically,
 * with no JS runtime in the path. These no-ops remain so callers need not change.
 */
export function startBookmarkAutosave(): void {}
export function stopBookmarkAutosave(): void {}

/** Empty the server's bookmark list — learned AND saved. An auto-learning list needs a
 *  way to be wiped: a wrong station would otherwise sit there for its 30-day expiry, and
 *  a manually saved one never expires at all. */
export async function clearServerBookmarks(): Promise<void> {
  try { await Local?.clearBookmarks?.(); } catch {}
}

/**
 * Import a bookmark file straight into the SERVER, from the phone.
 *
 * Goes through parseBookmarksAny, so it takes an UberSDR YAML export as happily as JSON —
 * that is the file people actually have.
 *
 * Written as ONE setBookmarksJson call rather than a POST per row: the web client has to
 * write them one at a time over HTTP, but here we are in-process and can hand the shim
 * the whole list at once. Returns how many landed.
 */
export async function importServerBookmarks(text: string): Promise<number> {
  if (!Local?.setBookmarksJson || !Local?.getBookmarksJson) return 0;
  const rows = parseBookmarksAny(text, '');
  if (!rows.length) throw new Error('No bookmarks found in that file');

  // Merge with what's already there, keyed by frequency, so an import ADDS rather than
  // replaces — and so re-importing the same file can't pile up duplicates.
  const existing = JSON.parse((await Local.getBookmarksJson()) || '[]') as any[];
  const byFreq = new Map<number, any>();
  for (const b of existing) byFreq.set(Math.round(b.frequency / 1000), b);
  const now = Math.floor(Date.now() / 1000);
  for (const b of rows) {
    if (!b?.name || !b?.frequency) continue;
    byFreq.set(Math.round(b.frequency / 1000), {
      frequency: Math.round(b.frequency),
      name: String(b.name),
      pi: -1,
      lastHeard: now,
      manual: true,                 // imported: never expires
      mode: b.mode || 'am',
      source: 'server',
    });
  }
  Local.setBookmarksJson(JSON.stringify([...byFreq.values()]));
  return rows.length;
}

/** The shim's learned list, right now — for the app's own search + VTS. */
export async function getLearnedBookmarksNow(): Promise<ServerBookmark[]> {
  if (!Local?.getBookmarksJson) return [];
  try {
    const json = await Local.getBookmarksJson();
    const arr = JSON.parse(json || '[]');
    if (!Array.isArray(arr)) return [];
    return arr
      .filter((b: any) => b?.name && b?.frequency)
      .map((b: any) => ({
        name: String(b.name),
        frequency: Number(b.frequency),
        mode: b.mode ?? 'wfm',
        source: 'server' as const,
      })) as ServerBookmark[];
  } catch { return []; }
}


/** Where the host has manually said the receiver is (city picker fallback). */
const LOC_KEY = 'lsv_server_location';

export type ServerLocation = { lat: number; lon: number; label?: string };

export async function setManualServerLocation(loc: ServerLocation | null): Promise<void> {
  if (loc) await AsyncStorage.setItem(LOC_KEY, JSON.stringify(loc));
  else await AsyncStorage.removeItem(LOC_KEY);
  await publishLocation();
}

/**
 * Resolve whatever the host typed into a position — a place name OR a Maidenhead
 * locator ("Northampton" or "IO92nh").
 *
 * A LOCATOR is tried first and never touches the network: it decodes arithmetically.
 * That matters because a VibeServer is exactly the thing likely to be sitting in a
 * shed on a solar panel with no internet — a radio amateur knows their grid square,
 * and making them depend on a geocoding API to state it would be daft.
 *
 * A place name falls back to Nominatim. Called ONCE, when the host saves — never per
 * client — and the result is stored, so the server keeps serving its location offline
 * forever after.
 */
export async function resolveLocation(input: string): Promise<ServerLocation | null> {
  const q = input.trim();
  if (!q) return null;

  // Maidenhead: 2 letters, 2 digits, optionally 2 more letters. Decoded locally.
  if (/^[A-R]{2}[0-9]{2}([A-X]{2})?$/i.test(q)) {
    const ll = gridToLatLon(q);
    if (ll) {
      // Canonical casing: fields/squares upper, subsquare lower (IO92nh).
      const g = q.length >= 6
        ? q.slice(0, 4).toUpperCase() + q.slice(4, 6).toLowerCase()
        : q.toUpperCase();
      return { lat: ll.lat, lon: ll.lon, label: g };
    }
  }
  return geocodeCity(q);
}

/**
 * Turn a typed place name into a position ("Northampton" → 52.24, -0.90).
 *
 * Needs the network. Prefer resolveLocation(), which handles a grid square offline.
 * Nominatim asks for an identifying User-Agent and rate-limits; both fine for a
 * one-shot.
 */
export async function geocodeCity(name: string): Promise<ServerLocation | null> {
  const q = name.trim();
  if (!q) return null;
  try {
    const r = await fetch(
      `https://nominatim.openstreetmap.org/search?format=json&limit=1&q=${encodeURIComponent(q)}`,
      { headers: { 'User-Agent': 'VibeSDR/8 (https://github.com/stuey3d/VibeSDR)' } },
    );
    const j = await r.json() as Array<{ lat: string; lon: string; display_name?: string }>;
    if (!j?.length) return null;
    const lat = parseFloat(j[0].lat), lon = parseFloat(j[0].lon);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
    // Keep the name the HOST typed as the label, not Nominatim's verbose
    // "Northampton, West Northamptonshire, England, United Kingdom".
    return { lat, lon, label: q };
  } catch {
    return null;
  }
}

/** Cache of coarse lat/lon → place, so the reverse lookup happens once ever.
 *
 *  _v2: the value used to be a bare NAME STRING and is now { name, country, iso }.
 *  Reading the old shape back gave `undefined` for every field — the receiver silently
 *  lost its town, its country AND the ISO that validates a station's PI country nibble.
 *  Versioning the key retires the old entries instead of misreading them. */
const RGEO_KEY = 'vs_rgeo_v2';

/**
 * Name the place we're at ("Moulton"), from a coarse position.
 *
 * Done on the SERVER, once — not in each client. Clients would otherwise each hit a
 * geocoder for the same answer, and a name is a property of the receiver just as its
 * position is. Cached against the ROUNDED coordinates, so it survives a restart and
 * never repeats. With no internet we keep the bare coordinates: ugly but honest, and
 * the grid square is there regardless.
 */
export async function reverseGeocode(
  lat: number, lon: number,
): Promise<{ name: string; country?: string; iso?: string } | null> {
  const key = `${lat.toFixed(2)},${lon.toFixed(2)}`;
  try {
    const raw = await AsyncStorage.getItem(RGEO_KEY);
    const cache: Record<string, { name: string; country?: string; iso?: string }> = raw ? JSON.parse(raw) : {};
    if (cache[key]) return cache[key];

    const r = await fetch(
      `https://nominatim.openstreetmap.org/reverse?format=json&zoom=13&lat=${lat}&lon=${lon}`,
      { headers: { 'User-Agent': 'VibeSDR/8 (https://github.com/stuey3d/VibeSDR)' } },
    );
    const j = await r.json() as { address?: Record<string, string> };
    const a = j?.address ?? {};
    // Most specific useful name first. zoom=13 keeps this to a town, never a street:
    // the position is deliberately coarsened to ~1 km and the label must not imply
    // more precision than that.
    const name = a.town || a.village || a.city || a.suburb || a.municipality
              || a.county || a.state || null;
    if (!name) return null;

    // The COUNTRY is the valuable half. Station-logo lookup needs it to anchor a name
    // match (without one it demands a near-exact name and so almost always fails), and
    // the RDS country code that would otherwise supply it rides in group 1A, which many
    // stations never transmit. FM is line-of-sight, so a station this receiver can hear
    // is essentially always in the receiver's own country — a very good default.
    const out = {
      name,
      country: a.country || undefined,
      iso: (a.country_code || '').toUpperCase() || undefined,
    };
    cache[key] = out;
    await AsyncStorage.setItem(RGEO_KEY, JSON.stringify(cache));
    return out;
  } catch {
    return null;   // offline — coordinates + grid still work
  }
}

export async function getManualServerLocation(): Promise<ServerLocation | null> {
  try {
    const raw = await AsyncStorage.getItem(LOC_KEY);
    return raw ? JSON.parse(raw) : null;
  } catch { return null; }
}

/**
 * How the server decides what position to publish. Defaults to 'off'.
 *
 * This is a SEPARATE consent from the app's own location permission, on purpose.
 * Granting location so the instance list can be sorted by distance is NOT consent
 * to BROADCAST that position to every client that connects — and once VibeServer
 * can be public, "every client" could mean anyone. So publishing is opt-in, and
 * the default is to publish nothing.
 */
export type LocationMode = 'off' | 'device' | 'manual';
const LOCMODE_KEY = 'vs_locmode';

export async function getServerLocationMode(): Promise<LocationMode> {
  try {
    const v = await AsyncStorage.getItem(LOCMODE_KEY);
    return v === 'device' || v === 'manual' ? v : 'off';
  } catch { return 'off'; }
}

export async function setServerLocationMode(m: LocationMode): Promise<void> {
  await AsyncStorage.setItem(LOCMODE_KEY, m);
  await publishLocation();
}

/**
 * Publish the RECEIVER's position (GET /location on the shim).
 *
 * This is the SERVER's location, deliberately — NOT the client's. A VibeServer
 * might be left at a relative's house in another town, and once it can be public
 * it could be listened to from anywhere. Spot distances, map centring and the ITU
 * REGION are all properties of the ANTENNA: computing them from the listener's
 * position gives nonsense distances and, worse, the wrong region's band edges
 * (80m is 3.5–3.8 MHz in R1 but 3.5–4.0 in R2).
 *
 * Publishes ONLY what the host explicitly opted into — see LocationMode. When the
 * mode is 'off' (the default) we publish nothing at all, and the client shows a
 * "receiver location not set" warning rather than silently pretending to know.
 */
/**
 * The receiver's RESOLVED position — whichever way the owner set it.
 *
 * ★★★ THE GRID IS DERIVED, NEVER TYPED. publishLocation() has always done this: a city is
 *     geocoded, the device fix is coarsened, and latLonToGrid() turns either into a locator. The
 *     public-listing switch first read the raw location BOX instead, which is empty on the device
 *     path and holds a town name on the manual one — so listing refused with "a valid Maidenhead
 *     locator is required" from a server that knew perfectly well where it was (Stuart,
 *     2026-08-22).
 * ★★ Which is also the answer to "we should allow city too": we already do. The city becomes
 *    coordinates HERE, on the server side, so the directory never needs to geocode anything.
 *
 * Returns null when the owner has opted out ('off') or the fix is unavailable — the caller must
 * treat that as "no location", never as a reason to invent one.
 */
export async function getResolvedServerLocation():
    Promise<{ lat: number; lon: number; grid: string; label?: string; country?: string } | null> {
  try {
    const mode = await getServerLocationMode();
    if (mode === 'off') return null;
    const manual = await getManualServerLocation();
    const loc = mode === 'manual' ? manual : await getUserLocation();
    if (!loc) return null;
    // ★ Coarsened to ~1 km, exactly as publishLocation does — a grid square is a square, not a
    //   house, and the two must not disagree about where this receiver is.
    const lat = Math.round(loc.lat * 100) / 100;
    const lon = Math.round(loc.lon * 100) / 100;
    const rev = await reverseGeocode(lat, lon);
    return {
      lat, lon,
      grid: latLonToGrid(lat, lon),
      label: (mode === 'manual' ? manual?.label : undefined) ?? rev?.name ?? undefined,
      country: rev?.iso ?? undefined,
    };
  } catch { return null; }
}

export async function publishLocation(): Promise<void> {
  if (!Local?.setLocationJson) return;
  // The NAME is always published — it identifies the receiver and is not sensitive
  // (the host typed it). The POSITION is published only when opted into. Clients
  // show "Moto G35 / Northampton IO92nh" when both are known, and just the name
  // with a "location not set" note when only the name is.
  // ★★★ THE PUBLIC NAME WINS WHERE ONE IS SET. Somebody arriving from the directory was promised
  //     "Stuey3D XCover4S" and then met a page calling itself "VibeSDR" — the local mDNS name,
  //     which the owner had never had a reason to change (Stuart, 2026-08-23). The public name IS
  //     the receiver's public identity: it is what strangers were told, what the shareable address
  //     is derived from, and the only name most visitors will ever have seen.
  //  ★★ It does not touch mDNS. The `.local` label still comes from the LOCAL name, because that
  //     is the one the owner's own network knows it by — two audiences, two names, and only the
  //     public one belongs on a page reached from the directory.
  //  ★ Falls back exactly as before when no public name is set, so a private server is unchanged.
  let name = await getServerName('VibeSDR');
  try {
    const pub = (await AsyncStorage.getItem(PUBLIC_NAME_KEY))?.trim();
    if (pub) name = pub;
  } catch {}
  const emit = (extra: object = {}) => {
    try { Local.setLocationJson(JSON.stringify({ name, ...extra })); } catch {}
  };
  try {
    const mode = await getServerLocationMode();
    if (mode === 'off') { emit(); return; }

    const manual = await getManualServerLocation();
    const loc = mode === 'manual' ? manual : await getUserLocation();
    if (!loc) { emit(); return; }

    // Coarsened to ~1 km — enough for distances, rings and the ITU region, and
    // nowhere near enough to point at a house. It is served to every client.
    const lat = Math.round(loc.lat * 100) / 100;
    const lon = Math.round(loc.lon * 100) / 100;

    // A bare "52.29, -0.85" means nothing to a human. On the DEVICE path there's no
    // label to show, so name the place — once, here, and cached — rather than make
    // every client reverse-geocode the same point for itself.
    const rev = await reverseGeocode(lat, lon);
    let label = mode === 'manual' ? manual?.label ?? undefined : undefined;
    if (!label) label = rev?.name ?? undefined;

    emit({
      lat, lon, label,
      // Receiver country — clients use it to anchor station-logo lookups and to
      // VALIDATE a station's PI country nibble, and they show it beside the town.
      country: rev?.country,
      iso: rev?.iso,
      // The grid is DERIVED here, so no client ever has to ask a human for it —
      // a locator is a property of the antenna, not something the listener knows.
      grid: latLonToGrid(lat, lon),
    });
  } catch {
    // Permission revoked, or the picked city went missing — publish the name only,
    // never a position the host never agreed to share.
    emit();
  }
}

export async function stopVibeServer(): Promise<void> {
  stopBookmarkAutosave();
  try { await Local?.stopVibeServer?.(); } catch {}
}

export async function getVibeServerStatus(): Promise<VibeServerStatus | null> {
  // ★★★ AN OPTIONAL CALL HIDES A MISSING BRIDGE METHOD. `Local?.x?.()` returns undefined when the
  //     native side never exported `x` — no throw, no log, indistinguishable from a server with
  //     nothing to report. That is exactly how a missing @ReactMethod cost two days: the status
  //     screen showed "Waiting for a client…" for ever while the server had a listener
  //     (2026-08-19/20). A method we depend on is either there or it is a BUG, so say so once.
  //  ★ Once, not per poll: this is called every 1.5 s and a repeated warning would be noise that
  //    teaches everyone to ignore the channel.
  if (typeof (Local as any)?.getVibeServerStatus !== 'function') {
    if (!warnedNoStatusBridge) {
      warnedNoStatusBridge = true;
      console.error('[vibeserver] getVibeServerStatus is not exported by the native module — '
                  + 'the status screen cannot work. Check @ReactMethod in VibeLocalSdrModule.kt.');
    }
    return null;
  }
  try { return await Local.getVibeServerStatus(); } catch { return null; }
}
let warnedNoStatusBridge = false;

// Live toggle — flip compressed audio without restarting the server (a fallback
// if a client hits a decode issue).
export function setVibeServerCompressAudio(on: boolean): void {
  try { Local?.setVibeServerCompressAudio?.(on); } catch {}
}

// Live, no restart — the same two levers the Mac exposes while serving.
export function setVibeServerAdminSecret(secret: string): void {
  try { Local?.setVibeServerAdminSecret?.(secret); } catch {}
}
/** ★★★ BIAS-T, APPLIED AFTER THE RADIO IS OPEN. It is NOT a start option: `startVibeServer` on the
 *  Kotlin side reads no such key, so passing one there would have been a switch that saved, redrew
 *  itself green, and did nothing to the feedline — which is worse than not offering it. The native
 *  setter has existed all along (VibeLocalSdrModule.setBiasTee); it just needs calling once the
 *  radio is up.
 *  ★ Only for a radio that HAS one — the Airspy HF+ does not, and the caller checks. */
export function setVibeServerBiasT(on: boolean): void {
  try { (Local as any)?.setBiasTee?.(on); } catch { /* older build, or no bias-T on this radio */ }
}

export function setVibeServerUncompressedAudio(mode: 0 | 1 | 2): void {
  try { Local?.setVibeServerUncompressedAudio?.(mode); } catch {}
}
/** ★ The radio currently plugged in, from its USB descriptor — no device open, no permission
 *  prompt. Null when nothing supported is attached. Used to draw the right menus BEFORE the
 *  server exists; the radio's definitive capabilities still come from hwinfo once running. */
export async function getConnectedRadio(): Promise<{ driver: string; model: string } | null> {
  try { return (await Local?.getConnectedRadio?.()) ?? null; } catch { return null; }
}

export function setVibeServerSessionLimit(minutes: number): void {
  try { Local?.setVibeServerSessionLimit?.(minutes); } catch {}
}

// A fresh random 6-digit default PIN. The user can keep it, set their own, or
// disable auth entirely on the sharing screen.
export function randomPin(seed: number): string {
  // Caller passes a seed (e.g. Date.now()) so this stays pure/testable.
  let x = (seed ^ 0x9e3779b9) >>> 0;
  x ^= x << 13; x >>>= 0; x ^= x >> 17; x ^= x << 5; x >>>= 0;
  return String(100000 + (x % 900000));
}

// Format a byte/sec rate for the live telemetry readout.
export function fmtRate(bytesPerSec: number): string {
  if (!bytesPerSec || bytesPerSec <= 0) return '0 KB/s';
  const kb = bytesPerSec / 1024;
  return kb >= 1000 ? `${(kb / 1024).toFixed(1)} MB/s` : `${kb.toFixed(0)} KB/s`;
}


// ── Finding a VibeServer on a typed host ─────────────────────────────────────

/** The range the server itself picks from: `--port` defaults to "the first free
 *  port in 48000-48049", and the multi-radio design puts the hub on 48000 with
 *  one radio per port above it. So the port a user needs is frequently NOT the
 *  48000 that DEFAULT_PORT fills in for a bare hostname. */
const VS_PORT_LO = 48000;
const VS_PORT_HI = 48100;

/**
 * Probe a SINGLE, USER-SUPPLIED host for a VibeServer, so typing just the IP is
 * enough.
 *
 * ★ This is NOT a subnet scan, and the distinction is deliberate: discovery
 * remains advertise-only (`_vibesdr._tcp`, see services/mdns.ts — "the
 * App-Store-clean path... no subnet scanning"). This only ever touches the one
 * address the user typed in, which they could equally have typed a port onto.
 *
 * ★ Identifies via `/vibeserver.json`, which answers definitively and is served
 * even when the host has turned the web client off. A VibeServer older than
 * that endpoint will not be found — it must be reached by typing its port.
 *
 * Returns the LOWEST matching port (the hub, in a multi-radio setup), or null.
 */
export async function findVibeServerPort(host: string): Promise<number | null> {
  const scheme = 'http://';
  const probe = async (port: number): Promise<number | null> => {
    const ctrl = new AbortController();
    // Short: these are LAN hosts, and 101 of them. A port with nothing on it
    // refuses immediately; only a firewalled one burns the whole timeout.
    const timer = setTimeout(() => ctrl.abort(), 1200);
    try {
      const r = await fetch(`${scheme}${host}:${port}/vibeserver.json`, { signal: ctrl.signal });
      if (!r.ok) return null;
      const d = await r.json();
      return d && d.server === 'vibeserver' ? port : null;
    } catch {
      return null;
    } finally {
      clearTimeout(timer);
    }
  };

  // Batched rather than all-at-once: 101 simultaneous sockets is enough to upset
  // a phone's networking stack and some home routers. Ordered batches also mean
  // we can stop at the first hit instead of always paying for the full range.
  const BATCH = 20;
  for (let lo = VS_PORT_LO; lo <= VS_PORT_HI; lo += BATCH) {
    const ports: number[] = [];
    for (let p = lo; p < Math.min(lo + BATCH, VS_PORT_HI + 1); p++) ports.push(p);
    const hits = (await Promise.all(ports.map(probe))).filter((p): p is number => p != null);
    if (hits.length) return Math.min(...hits);
  }
  return null;
}

// ── The multi-radio front door ────────────────────────────────────────────────
//
// ★★★ A V3 SERVER IS A FRONT DOOR THAT OWNS NO RADIO. Every radio lives behind `/r/<id>/…`, so a
//     client that connects to the bare host asks for a radio the door does not have and is
//     refused — as a WebSocket handshake that surfaces as a bare 1006 with no error text, which
//     reads as "the server is down" rather than "choose a radio first". That is exactly how
//     VibeSDR 10.0 fails against the public demo.
//
// ★★ DETECTED FROM `/vibeserver.json`, WHICH WE ALREADY FETCH. The door reports `frontDoor:true`
//    there, so knowing costs no extra round trip; the directory below is fetched only when there
//    is actually something to choose.
//    ★ A MISSING key means "ordinary receiver" — every radio, and every server older than this,
//      omits it. So test `=== true`, never `!== false`.

/** One radio behind a front door, as the directory describes it. */
export type VibeRadio = {
  /** ★ The opaque id, and the ONLY thing that belongs in a URL — serials are deliberately kept
   *  out of them. The door still accepts a serial for old links, but we must not mint new ones. */
  id: string;
  label: string;
  driver: string;
  /** What the hardware can reach, and what the OWNER permits — [[loHz, hiHz], …]. */
  coverage: [number, number][];
  allowed: [number, number][];
  /** True when this radio is behind the admin password (a locked, shared receiver). */
  restricted: boolean;
  /** Owner's cap for this radio. */
  maxUsers: number;
  locked: boolean;
  centreHz: number;
  spanHz: number;
  mode: string;
  // ── the LIVE half, filled in per radio; undefined when that radio did not answer ──
  listeners?: number;
  busy?: boolean;
  waiting?: number;
  /** Seconds until the current occupant's limit expires; -1 = no limit. */
  freeInSec?: number;
  /** ★ False when the radio did not answer at all. A radio that is DOWN must be shown as down,
   *  not quietly rendered as free — "unknown" and "available" are very different promises. */
  reachable: boolean;
};

export type VibeDirectory = { name: string; radios: VibeRadio[] };

const asRanges = (v: any): [number, number][] =>
  Array.isArray(v) ? v.filter(r => Array.isArray(r) && r.length >= 2)
                      .map(r => [Number(r[0]), Number(r[1])] as [number, number])
                   : [];

/**
 * Is this a multi-radio front door, and if so what is behind it?
 *
 * Returns null for an ordinary receiver — the caller then behaves exactly as it always has, which
 * is what keeps single-radio servers unchanged.
 *
 * ★★ THE LIVE HALF COMES FROM EACH RADIO, NOT FROM THE DOOR. The directory is built from the
 *    config file and is honest about being a directory: it knows what exists, not who is
 *    listening. Each radio is its own PROCESS, so only that process knows whether it is busy —
 *    which is also why a radio that is down simply does not answer, and can be SHOWN as down
 *    instead of the directory claiming it is fine.
 * ★ Asked in parallel and independently: one slow or dead radio must not hold up the others.
 */
export async function fetchVibeDirectory(
  baseUrl: string, timeoutMs = 4000,
): Promise<VibeDirectory | null> {
  const base = baseUrl.replace(/\/+$/, '');
  const get = async (path: string, ms: number) => {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), ms);
    try {
      const r = await fetch(base + path, { signal: ctrl.signal, cache: 'no-store' });
      return r.ok ? await r.json() : null;
    } catch { return null; } finally { clearTimeout(t); }
  };

  const id = await get('/vibeserver.json', timeoutMs);
  if (!id || id.server !== 'vibeserver') return null;
  // ★ Not a door: an ordinary receiver, and the caller must treat it exactly as before.
  if (id.frontDoor !== true) return null;

  const dir = await get('/vibeserver/radios', timeoutMs);
  const list: any[] = Array.isArray(dir?.radios) ? dir.radios : [];
  if (!list.length) return { name: String(dir?.name || ''), radios: [] };

  const radios = await Promise.all(list.map(async (r: any): Promise<VibeRadio> => {
    const live = await get(`/r/${r.id}/vibeserver.json`, 2500);
    return {
      id: String(r.id ?? ''),
      label: String(r.label || r.driver || 'Radio'),
      driver: String(r.driver || ''),
      coverage: asRanges(r.coverage),
      allowed: asRanges(r.allowed),
      restricted: r.restricted === true,
      maxUsers: Number(r.users) || 1,
      locked: r.locked === true,
      centreHz: Number(r.centreHz) || 0,
      spanHz: Number(r.spanHz) || 0,
      mode: String(r.mode || ''),
      listeners: live ? Number(live.listeners) || 0 : undefined,
      busy: live ? live.busy === true : undefined,
      waiting: live ? Number(live.waiting) || 0 : undefined,
      freeInSec: live && typeof live.freeInSec === 'number' ? live.freeInSec : -1,
      reachable: !!live,
    };
  }));
  return { name: String(dir?.name || ''), radios };
}

/** The base URL for one radio behind a front door.
 *  ★ Everything in the client appends to its base URL, so addressing a radio is entirely a matter
 *    of handing it the right one — there is no per-request plumbing to thread through. */
export const radioBaseUrl = (baseUrl: string, id: string) =>
  `${baseUrl.replace(/\/+$/, '')}/r/${id}`;
