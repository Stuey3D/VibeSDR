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

export type DirectoryId = 'ubersdr' | 'receiverbook' | 'kiwisdr' | 'fmdx' | 'spyserver';

export interface DirectoryMeta {
  id:    DirectoryId;
  name:  string;
  desc:  string;
  /** which backends this directory yields — drives the footer logo/labels. */
  kinds: ('ubersdr' | 'owrx' | 'kiwi' | 'fmdx' | 'spyserver')[];
}

export const DIRECTORIES: DirectoryMeta[] = [
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
  const res = await fetch(RECEIVERBOOK_URL);
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
  const res = await fetch(KIWI_LIST_URL);
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
        serverType: 'kiwi',
      });
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
  const res = await fetch(SPYSERVER_DIR_URL);
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
  if (id === 'ubersdr')      list = await fetchInstances(lat, lon);
  else if (id === 'receiverbook') list = await fetchReceiverbook(lat, lon);
  else if (id === 'fmdx')    list = await fetchFmdx(lat, lon);
  else if (id === 'spyserver') list = await fetchSpyServers(lat, lon);
  else                       list = await fetchKiwiList(lat, lon);

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
