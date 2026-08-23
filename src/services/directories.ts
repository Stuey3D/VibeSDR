// SDR directory providers — separate receiver lists the picker presents as
// distinct "directories" (like different websites). Each is fetched on demand
// and normalised to SDRInstance[]; duplicates ACROSS directories are fine (they
// are independent lists). Unsupported server types (WebSDR/Nova/Phantom) are
// filtered out — we only handle UberSDR / OpenWebRX / KiwiSDR.

import { SDRInstance, fetchInstances } from './instancesApi';
import { fetchFmdxServers } from './fmdxDirectory';
import { countryForCoord } from './countryLookup';   // Kiwi/Receiverbook carry no country code
import { countryFromText } from './countryFromText'; // last resort: parse the name/location text
import { isoForCallsign } from './callsignIso';       // final resort: map the callsign prefix

export type DirectoryId = 'vibeserver' | 'ubersdr' | 'receiverbook' | 'kiwisdr' | 'fmdx' | 'spyserver';

export interface DirectoryMeta {
  id:    DirectoryId;
  name:  string;
  desc:  string;
  /** which backends this directory yields — drives the footer logo/labels. */
  kinds: ('vibeserver' | 'ubersdr' | 'owrx' | 'kiwi' | 'web888' | 'fmdx' | 'spyserver')[];
}

export const DIRECTORIES: DirectoryMeta[] = [
  // ★★ FIRST AMONG THE DIRECTORIES — which is where it belongs, and where Stuart put it back
  //    after seeing it given a section of its own at the top of the whole screen (2026-08-22:
  //    "it can be with the rest of the directories at the bottom, just at the top of the list
  //    above UberSDR"). It is a directory; it reads as one alongside the others, and being first
  //    is enough to say it is ours without inventing a second place for it to live.
  { id: 'vibeserver',  name: 'VibeServer',  desc: 'Public VibeServers — list yours from the app',
    kinds: ['vibeserver'] },
  { id: 'ubersdr',     name: 'UberSDR',     desc: 'Official UberSDR instances',                 kinds: ['ubersdr'] },
  { id: 'receiverbook', name: 'Receiverbook', desc: 'OpenWebRX + KiwiSDR (receiverbook.de)',     kinds: ['owrx', 'kiwi'] },
  { id: 'kiwisdr',     name: 'KiwiSDR',     desc: 'Public KiwiSDR network (kiwisdr.com)',        kinds: ['kiwi'] },
  { id: 'fmdx',        name: 'FM-DX',       desc: 'FM-DX Webserver network (servers.fmdx.org)',  kinds: ['fmdx'] },
  // SpyServer directory listing DELIBERATELY NOT shown (2026-07-09). The public
  // directory is 219 random hobbyist servers, most full/unreachable/session-
  // limited, so browsing it is a wall of try-and-fail. fetchSpyServers + the
  // 'spyserver' dispatch below stay (dead but trivially revived). Connecting to a
  // SPECIFIC known SpyServer is still available via the manual add-server modal.
];

/** ★★★ EVERY DIRECTORY FETCH IS TIMED OUT — a TestFlight report from Shanghai (2026-07-31) sat on
 *  "Loading…" for ever on the Apple Watch, because the WATCH asks the PHONE to browse and the phone
 *  never replied: its fetch had NO timeout, so a blocked or crawling host hung indefinitely and the
 *  watch's `nil` ("still waiting") never became `[]` ("couldn't load"). The retry UI existed and was
 *  unreachable.
 *  ★★ Every one of these hosts is foreign to a large part of the world and at least one is commonly
 *  unreachable from mainland China. A directory that fails FAST and says so is strictly better than
 *  one that hangs: the user can retry, pick another, or type a custom address.
 *  ★ 12 s is deliberately generous — receiverbook is a ~400 KB page we parse — but finite. */
const DIR_TIMEOUT_MS = 12_000;
const dirFetch = (url: string, init?: RequestInit) =>
  fetch(url, { ...init, signal: AbortSignal.timeout(DIR_TIMEOUT_MS) });

const VIBESERVER_DIR_URL = 'https://vibeserver.vibesdr.net/api/directory';

/**
 * Public VibeServers, from our own directory.
 *
 * ★★★ CONNECT TO THE TUNNEL DIRECTLY. THE STABLE ADDRESS CANNOT CARRY A SOCKET.
 *     `address` — the slug on our own zone — is served by our WORKER, which proxies the landing
 *     page and injects the direct host so the BROWSER can then open its sockets straight to the
 *     receiver. A Worker does not proxy WebSockets: pointed at the slug, every upgrade came back
 *     "426 Upgrade Required" from Cloudflare and the app simply would not connect (measured
 *     2026-08-23 — the slug 426, the tunnel 101, same server, same second).
 *  ★★ AND IT WOULD HAVE BEEN WRONG EVEN IF IT WORKED. Audio and waterfall through our Worker is
 *     precisely what the whole design exists to avoid: "the tunnel only being for discovery and
 *     the data being handled directly so that we dont hammer our cloudflare free limit" (Stuart,
 *     at the very start of this). The stable address is for FINDING and SHARING; the data goes
 *     direct.
 *  ★ The cost is that a favourite saved from a listing goes stale when the tunnel rotates — the
 *    directory is the way back, and that is a far smaller price than not connecting at all.
 * ★★ The directory already answers with what a listener needs BEFORE connecting — occupancy, the
 *    session limit, whether a PIN is required — so none of it has to be probed here. That is the
 *    whole reason the listing carries a status blob.
 * ★ Anything the server did not say stays absent rather than being guessed: an unlisted radio
 *   model shows nothing, not "unknown".
 */
async function fetchVibeServers(lat?: number, lon?: number): Promise<SDRInstance[]> {
  const res = await dirFetch(VIBESERVER_DIR_URL);
  if (!res.ok) throw new Error(`VibeServer directory: HTTP ${res.status}`);
  const body: any = await res.json();
  const rows: any[] = Array.isArray(body?.servers) ? body.servers : [];

  return rows.map((s: any): SDRInstance => {
    const max = Number(s.maxListeners) || 0;
    const users = Number(s.listeners) || 0;
    const radios: any[] = Array.isArray(s.radios) ? s.radios : [];
    // ★ One radio: name it. Several: say how many rather than picking one to be the face of the
    //   machine — a front door holding an HF+ and a dongle is not "an HF+".
    const device = radios.length === 1
      ? String(radios[0]?.name || radios[0]?.driver || '').trim() || undefined
      : radios.length > 1 ? `${radios.length} radios` : undefined;
    // ★ The tunnel first; `address` only where a listing has no direct URL at all.
    const url = String(s.url || (s.address ? `https://${s.address}` : '')).replace(/\/+$/, '');
    return {
      uuid: typeof s.id === 'string' ? s.id : null,
      name: String(s.name || 'VibeServer'),
      url,
      location: String(s.grid || ''),
      callsign: '',
      users,
      maxUsers: max || 1,
      online: !s.unreachable,
      version: null,
      latitude: typeof s.lat === 'number' ? s.lat : null,
      longitude: typeof s.lon === 'number' ? s.lon : null,
      countryCode: typeof s.country === 'string' && s.country.length === 2 ? s.country : null,
      distance: null,
      bestSnr: null,
      serverType: 'vibeserver',
      deviceType: device,
      full: max > 0 && users >= max,
      sessionLimitMins: Number(s.limitMin) > 0 ? Number(s.limitMin) : undefined,
      needsPin: !!s.pin,
    };
  }).filter((i: SDRInstance) => !!i.url);
}

const SPYSERVER_DIR_URL = 'https://airspy.com/directory/status.json';

const RECEIVERBOOK_URL = 'https://www.receiverbook.de/map';
const KIWI_LIST_URL    = 'http://rx.linkfanel.net/kiwisdr_com.js';

/** Pull a `var <name> = [ … ];` array out of a JS/HTML blob by walking balanced
 *  brackets (string-aware) — the arrays are large and nested, so a regex can't
 *  reliably find the closing bracket. */
function extractJsArray(text: string, varName: string): any[] | null {
  const start = text.indexOf(varName);
  if (start < 0) return null;
  const open = text.indexOf('[', start);
  if (open < 0) return null;
  let depth = 0, inStr = false, quote = '';
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (inStr) {
      if (c === '\\') { i++; continue; }
      if (c === quote) inStr = false;
      continue;
    }
    if (c === '"' || c === "'") { inStr = true; quote = c; continue; }
    if (c === '[') depth++;
    else if (c === ']') { depth--; if (depth === 0) {
      // These arrays are generated JS, not strict JSON — the KiwiSDR list ends
      // every object/array with a trailing comma (`},\n]`), which JSON.parse
      // rejects. Strip trailing commas before parsing.
      const slice = text.slice(open, i + 1).replace(/,(\s*[\]}])/g, '$1');
      try { return JSON.parse(slice); } catch { return null; }
    } }
  }
  return null;
}

function haversineKm(aLat: number, aLon: number, bLat: number, bLon: number): number {
  const R = 6371, toRad = (d: number) => (d * Math.PI) / 180;
  const dLat = toRad(bLat - aLat), dLon = toRad(bLon - aLon);
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(aLat)) * Math.cos(toRad(bLat)) * Math.sin(dLon / 2) ** 2;
  return Math.round(R * 2 * Math.atan2(Math.sqrt(s), Math.sqrt(1 - s)));
}

const blank = (over: Partial<SDRInstance>): SDRInstance => ({
  uuid: null,
  name: '', url: '', location: '', callsign: '', users: 0, maxUsers: 0,
  online: true, version: null, latitude: null, longitude: null, countryCode: null,
  distance: null, bestSnr: null, ...over,
});

/** receiverbook.de — its /map page embeds `var receivers = [ {label, url,
 *  location:{coordinates:[lng,lat]}, receivers:[{type,version,url,label}]} ]`.
 *  Flatten to individual receivers, keep only OWRX/Kiwi (drop WebSDR/unknown). */
async function fetchReceiverbook(lat?: number, lon?: number): Promise<SDRInstance[]> {
  const res = await dirFetch(RECEIVERBOOK_URL);
  const html = await res.text();
  const sites = extractJsArray(html, 'var receivers');
  if (!sites) return [];
  const out: SDRInstance[] = [];
  for (const site of sites) {
    const coords = site?.location?.coordinates;
    const slat = Array.isArray(coords) ? Number(coords[1]) : null;
    const slon = Array.isArray(coords) ? Number(coords[0]) : null;
    for (const ro of site?.receivers ?? []) {
      const t = String(ro?.type ?? '').toLowerCase();
      const kind: SDRInstance['serverType'] | null =
        t === 'openwebrx' ? 'owrx' : t === 'kiwisdr' ? 'kiwi' : null;
      if (!kind) continue;                                   // drop WebSDR etc.
      const url = ro?.url ?? site?.url;
      if (!url) continue;
      out.push(blank({
        name: String(ro?.label ?? site?.label ?? 'Unknown').replace(/<[^>]*>/g, '').slice(0, 120),
        url: String(url).replace(/\/+$/, ''),
        latitude: Number.isFinite(slat) ? slat : null,
        longitude: Number.isFinite(slon) ? slon : null,
        countryCode: countryForCoord(slat, slon) || null,   // derived offline from coordinates
        distance: (lat != null && lon != null && Number.isFinite(slat) && Number.isFinite(slon))
          ? haversineKm(lat, lon, slat as number, slon as number) : null,
        version: ro?.version ?? null,
        serverType: kind,
      }));
    }
  }
  return out;
}

/** kiwisdr.com public list (via linkfanel's snapshot) — `var kiwisdr_com = [ … ]`
 *  with name/url/loc/gps/users/users_max/snr per receiver. */
async function fetchKiwiList(lat?: number, lon?: number): Promise<SDRInstance[]> {
  const res = await dirFetch(KIWI_LIST_URL);
  const js = await res.text();
  const arr = extractJsArray(js, 'var kiwisdr_com');
  if (!arr) return [];
  return arr
    .filter((r) => r?.url && String(r?.offline ?? '').toLowerCase() !== 'yes')
    .map((r) => {
      const gps = /\(([-\d.]+),\s*([-\d.]+)\)/.exec(String(r.gps ?? ''));
      const glat = gps ? Number(gps[1]) : null;
      const glon = gps ? Number(gps[2]) : null;
      // "snr":"46,47" → best of the pair
      const snr = String(r.snr ?? '').split(',').map(Number).filter((n) => Number.isFinite(n));
      return blank({
        name: String(r.name ?? 'KiwiSDR').replace(/<[^>]*>/g, '').slice(0, 120),
        url: String(r.url).replace(/\/+$/, ''),
        location: String(r.loc ?? ''),
        users: Number(r.users) || 0,
        maxUsers: Number(r.users_max) || 0,
        latitude: glat, longitude: glon,
        countryCode: countryForCoord(glat, glon) || null,   // derived offline from GPS
        distance: (lat != null && lon != null && glat != null && glon != null)
          ? haversineKm(lat, lon, glat, glon) : null,
        bestSnr: snr.length ? Math.max(...snr) : null,
        // ★ The row NAMES the firmware, so use it rather than assuming the parent product: a
        //   Web-888 reports `sw_version: "Web888_v2026.609"` where a Kiwi reports
        //   `"KiwiSDR_v1.902"`. The two want different WebSocket URLs (see isKiwiProtocol), and
        //   getting it right here saves the adapter a failed socket and a retry round-trip.
        //   ★★ This directory has no `type` field to tell them apart — sw_version is the only
        //      signal in the feed, and Receiverbook does not publish even that, which is why
        //      KiwiAdapter still needs its own fallback probe.
        serverType: /web[_-]?888/i.test(String(r.sw_version ?? '')) ? 'web888' : 'kiwi',
        // ★★★ THE OWNER'S THIRD-PARTY ALLOWANCE — see SDRInstance.extApi. 0 = apps not permitted,
        //     which is the receiver that admits you and drops you at ~10 s saying nothing.
        //     Reading it here means we can WARN BEFORE CONNECTING rather than burn an attempt on
        //     someone's radio to discover a policy they already published.
        extApi: Number.isFinite(Number(r.ext_api)) ? Number(r.ext_api) : undefined,
        // ★ `sdr_hw` is "KiwiSDR 2 v1.902 ⁣ 📡 GPS ⁣ ⏳ Limits ⁣ 📻 DRM ⁣⁣ 🌀D11.9" — the product and
        //   firmware, then feature badges separated by an INVISIBLE SEPARATOR (U+2063). Keep the
        //   leading product + version and drop the badges; it names the hardware where sw_version
        //   only gives a number.
        hardware: (String(r.sdr_hw ?? '').split('\u2063')[0] || '').trim().slice(0, 40) || undefined,
      });
    });
}

// ── Receiverbook ✕ KiwiSDR cross-reference ───────────────────────────────────
/** ★★★ RECEIVERBOOK DOES NOT PUBLISH `ext_api`, BUT THE KIWI DIRECTORY DOES — SO JOIN THEM.
 *
 *  A Kiwi whose owner set `ext_api=0` admits an app, streams for ~10 s and closes saying nothing
 *  ([[kiwi_ext_api_10s_kick]]). On the Kiwi directory we can warn before connecting; on
 *  Receiverbook the same receiver looked perfectly fine, so the user burned an attempt on it.
 *  ★ Stuart, 2026-08-04, from his own early testing: "I'd rather catch and block most of them than
 *    have users try a few Kiwi's and find they dont connect — an issue I had before I realised it
 *    wasnt our app to blame, it was the Kiwi itself."
 *  ★★ IT MATTERS MOST ON THE WATCH, which has NO COMPATIBILITY MODE to fall back to: an unflagged
 *     blocked receiver there is a dead end with no explanation available.
 *
 *  MEASURED against both live directories (2026-08-04, 795 Receiverbook kiwis / 849 Kiwi rows):
 *      by address (host+port, then unique host) ... 556
 *      by name AND within 25 km ................... 112
 *      name matched but geographically far ........   0   <- the guard cost nothing here
 *      unmatched (Receiverbook-only receivers) .... 127
 *      => 668 of 795 joined (84%), identifying 96 of the 126 blocked receivers.
 *  ★ The 25 km guard is kept even though nothing failed it: a future name collision would
 *    otherwise HIDE A WORKING RECEIVER, which is a worse failure than missing a blocked one.
 */
const hostOf = (u: string): string => {
  try {
    const raw = String(u || '');
    const x = new URL(/^https?:/i.test(raw) ? raw : `http://${raw}`);
    return x.hostname.toLowerCase().replace(/^www\./, '');
  } catch { return ''; }
};
const hostPortOf = (u: string): string => {
  try {
    const raw = String(u || '');
    const x = new URL(/^https?:/i.test(raw) ? raw : `http://${raw}`);
    const port = x.port || (x.protocol === 'https:' ? '443' : '80');
    return `${x.hostname.toLowerCase().replace(/^www\./, '')}:${port}`;
  } catch { return ''; }
};
const joinName = (s: string): string =>
  String(s || '').toLowerCase().replace(/<[^>]*>/g, '').replace(/[^a-z0-9]/g, '').slice(0, 40);

/** Session cache — the join needs the Kiwi list even when the user opened Receiverbook, and they
 *  may well open both. One fetch per session, not per screen. */
let kiwiJoinCache: { at: number; rows: SDRInstance[] } | null = null;
const KIWI_JOIN_TTL_MS = 10 * 60 * 1000;

async function kiwiRowsForJoin(): Promise<SDRInstance[]> {
  if (kiwiJoinCache && Date.now() - kiwiJoinCache.at < KIWI_JOIN_TTL_MS) return kiwiJoinCache.rows;
  const rows = await fetchKiwiList();
  kiwiJoinCache = { at: Date.now(), rows };
  return rows;
}

/** Tag Receiverbook kiwis with the `extApi` the Kiwi directory publishes for the same receiver.
 *  Never throws: a directory that fails to load must not take the screen down with it — the
 *  result is simply today's behaviour, unflagged. */
async function annotateExtApiFromKiwiDirectory(list: SDRInstance[]): Promise<SDRInstance[]> {
  const needs = list.some(i => (i.serverType === 'kiwi' || i.serverType === 'web888') && i.extApi === undefined);
  if (!needs) return list;
  let kiwis: SDRInstance[];
  try { kiwis = await kiwiRowsForJoin(); } catch { return list; }
  if (!kiwis.length) return list;

  const byHostPort = new Map<string, SDRInstance>();
  const byHost = new Map<string, SDRInstance[]>();
  const byName = new Map<string, SDRInstance[]>();
  for (const k of kiwis) {
    const hp = hostPortOf(k.url); if (hp) byHostPort.set(hp, k);
    const h = hostOf(k.url);
    if (h) { const a = byHost.get(h); if (a) a.push(k); else byHost.set(h, [k]); }
    const n = joinName(k.name);
    if (n) { const a = byName.get(n); if (a) a.push(k); else byName.set(n, [k]); }
  }

  return list.map((i) => {
    if ((i.serverType !== 'kiwi' && i.serverType !== 'web888') || i.extApi !== undefined) return i;
    // 1. ADDRESS — the strong key. Exact host+port, else a host that hosts exactly one receiver.
    let m = byHostPort.get(hostPortOf(i.url));
    if (!m) {
      const onHost = byHost.get(hostOf(i.url));
      if (onHost && onHost.length === 1) m = onHost[0];
    }
    // 2. NAME, but only when the two agree on WHERE they are — see the 25 km note above.
    if (!m) {
      const cands = byName.get(joinName(i.name));
      if (cands && cands.length === 1 && i.latitude != null && i.longitude != null) {
        const c = cands[0];
        if (c.latitude != null && c.longitude != null &&
            haversineKm(i.latitude, i.longitude, c.latitude, c.longitude) <= 25) m = c;
      }
    }
    if (!m || m.extApi === undefined) return i;
    return { ...i, extApi: m.extApi };
  });
}

/** FM-DX Webserver network (servers.fmdx.org) → SDRInstance rows tagged 'fmdx'.
 *  location carries "city · TUNER" so the row shows the tuner type at a glance. */
async function fetchFmdx(lat?: number, lon?: number): Promise<SDRInstance[]> {
  const servers = await fetchFmdxServers(lat, lon);
  return servers.map((s) => blank({
    name: s.name,
    url: s.url,
    location: [s.city, s.tuner ? s.tuner.toUpperCase() : ''].filter(Boolean).join(' · '),
    latitude: s.lat, longitude: s.lon, distance: s.distance,
    countryCode: s.iso ? s.iso.toUpperCase() : null,
    serverType: 'fmdx',
  }));
}

/** Fetch a directory's instances, normalised + distance-sorted when located. */
/**
 * Public SpyServer-compatible receivers, from Airspy's own directory.
 *
 * status.json carries the full capability set for every server, so we never have
 * to connect to one to describe it. Three device families appear (RTL-SDR 8-bit,
 * AirspyHF+ 16-bit, AirspyOne 12-bit) and a server that is full closes the socket
 * right after the hello — indistinguishable from one with no radio — so surfacing
 * `full` here saves the user a confusing failed connect.
 */
async function fetchSpyServers(lat?: number, lon?: number): Promise<SDRInstance[]> {
  const res = await dirFetch(SPYSERVER_DIR_URL);
  if (!res.ok) throw new Error(`SpyServer directory: HTTP ${res.status}`);
  const json = await res.json();
  const rows: any[] = Array.isArray(json?.servers) ? json.servers : [];
  return rows
    .filter(r => r?.online && r?.streamingHost && r?.streamingPort)
    .map((r): SDRInstance => {
      const la = typeof r?.antennaLocation?.lat === 'number' ? r.antennaLocation.lat : null;
      const lo = typeof r?.antennaLocation?.long === 'number' ? r.antennaLocation.long : null;
      // 0,0 is the "operator never set a location" default, not the Gulf of Guinea.
      const hasLoc = la != null && lo != null && !(la === 0 && lo === 0);
      const users = r.currentClientCount ?? 0;
      const maxUsers = r.maxClients ?? 1;
      return {
        uuid: null,
        name: r.ownerName || r.generalDescription || `${r.streamingHost}:${r.streamingPort}`,
        url: `spyserver://${r.streamingHost}:${r.streamingPort}`,
        location: r.antennaType || r.generalDescription || '',
        callsign: '',
        users,
        maxUsers,
        online: true,
        version: r.serverVersion ?? null,
        latitude:  hasLoc ? la : null,
        longitude: hasLoc ? lo : null,
        countryCode: null,
        distance: hasLoc && lat != null && lon != null ? haversineKm(lat, lon, la!, lo!) : null,
        bestSnr: null,
        serverType: 'spyserver',
        deviceType: r.deviceType ?? undefined,
        full: users >= maxUsers,
        sessionLimitMins: typeof r.maxSessionDuration === 'number' && r.maxSessionDuration > 0
          ? r.maxSessionDuration : undefined,
      };
    });
}

export async function fetchDirectory(id: DirectoryId, lat?: number, lon?: number): Promise<SDRInstance[]> {
  let list: SDRInstance[];
  if (id === 'vibeserver')   list = await fetchVibeServers(lat, lon);
  else if (id === 'ubersdr') list = await fetchInstances(lat, lon);
  else if (id === 'receiverbook') list = await fetchReceiverbook(lat, lon);
  else if (id === 'fmdx')    list = await fetchFmdx(lat, lon);
  else if (id === 'spyserver') list = await fetchSpyServers(lat, lon);
  else                       list = await fetchKiwiList(lat, lon);

  // ★ Receiverbook publishes no `ext_api`, so borrow it from the Kiwi directory — see
  //   annotateExtApiFromKiwiDirectory. Costs one extra (cached) fetch on that screen, which is
  //   cheap beside a user concluding OUR app is broken when the receiver refused them.
  if (id === 'receiverbook') list = await annotateExtApiFromKiwiDirectory(list);

  // CENTRAL country enrichment — applies to EVERY directory (UberSDR incl., e.g. the popular
  // Canaries server). Fill any missing countryCode from coordinates first, then from the
  // name/location text (a tiny island the world map omits, or a server with wrong GPS).
  list = list.map(i => {
    if (i.countryCode) return i;
    const text = `${i.name} ${i.location ?? ''}`;
    let cc = countryForCoord(i.latitude, i.longitude);
    if (!cc) {
      // Coordinates embedded in the name (Kiwi/Receiverbook style "…/@-37.70,176.16") — some
      // servers publish a wrong GPS field but a correct @lat,lon in their title.
      const m = text.match(/@\s*(-?\d{1,2}(?:\.\d+)?)\s*,\s*(-?\d{1,3}(?:\.\d+)?)/);
      if (m) cc = countryForCoord(parseFloat(m[1]), parseFloat(m[2]));
    }
    if (!cc) cc = countryFromText(text);   // an explicit country/territory word wins over…
    if (!cc) cc = isoForCallsign(text);     // …the callsign prefix (CS8ACT→PT, EA8DJF→ES)
    return cc ? { ...i, countryCode: cc } : i;
  });

  // distance ascending when we have it, else leave source order
  if (lat != null && lon != null) {
    list = [...list].sort((a, b) => (a.distance ?? 1e9) - (b.distance ?? 1e9));
  }
  return list;
}
