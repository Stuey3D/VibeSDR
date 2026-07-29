// FM-DX Webserver public directory (servers.fmdx.org). Backs the v7 FM-DX
// backend's server browser. Kept standalone (not folded into SDRInstance yet)
// so the Phase 0 spike can list + connect without touching the picker/connect
// flow. Schema verified 2026-07-08: GET returns { dataset: [ … ] }.
//
// NOTE: the endpoint is plain HTTP (https redirects to http). The app already
// permits cleartext for Kiwi/OWRX, so this fetch is fine on both platforms.

import { USER_AGENT } from '../constants/version';

const FMDX_API = 'http://servers.fmdx.org/api/';

export interface FmdxServer {
  name:     string;
  url:      string;          // server root, e.g. http://host:port
  city:     string;
  country:  string;          // full name where available
  iso:      string;          // ISO country code (lowercase)
  tuner:    string;          // 'tef' | 'sdr' | 'xdr'
  bwLimit:  string;          // e.g. "65 - 108 MHz"
  audio:    string;          // e.g. "128k"
  lat:      number | null;
  lon:      number | null;
  distance: number | null;   // km from user, when located
}

/**
 * ★★★ THE TUNING RANGE, FROM THE ONE PLACE IT IS PUBLISHED.
 *
 * An FM-DX Webserver exposes its tuning limits nowhere a client can read them —
 * not /static_data, not /api, not the /text socket (see constants/fmBand.ts).
 * But it DOES announce them outbound: `server/fmdx_list.js` posts a `bwLimit`
 * string to the fm-dx.org list, which is the directory we already fetch. So the
 * answer was in a field we have been parsing and discarding all along.
 * (Confirmed by NoobishSVK, the webserver's author, 2026-07-28: "they do
 * announce it, but only on maps, i might have to add something in the API tho".)
 *
 * ★ An EMPTY string means no limit is configured — the common case, since
 * `tuningLimit` defaults false — and the receiver's full sweep is available.
 * A non-empty one is the exact range, and it is the case we could not otherwise
 * discover at all: a limited server drops out-of-range tunes in silence, so
 * probing it looks identical to a dead connection.
 *
 * Format is human text ("65 - 108 MHz"), so parse defensively and return null
 * on anything unexpected rather than inventing a band.
 */
export function parseBwLimit(s: string): { lo: number; hi: number } | null {
  const m = /(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)/.exec(s ?? '');
  if (!m) return null;
  const lo = parseFloat(m[1]) * 1e6, hi = parseFloat(m[2]) * 1e6;
  if (!Number.isFinite(lo) || !Number.isFinite(hi) || hi <= lo) return null;
  return { lo, hi };
}

/** Host+port, protocol- and trailing-slash-insensitive, for matching a connected
 *  server against a directory entry (we may hold either spelling). */
function hostKey(url: string): string {
  return url.trim().toLowerCase()
    .replace(/^wss?:\/\//, '').replace(/^https?:\/\//, '')
    .replace(/\/+$/, '');
}

// One directory fetch serves every FM-DX connect for a while. The list is the
// same for everyone and changes slowly; re-fetching it per connect would put an
// HTTP round trip in front of the tuner for nothing.
let limitCache: { at: number; byHost: Map<string, string> } | null = null;
const LIMIT_TTL_MS = 10 * 60_000;

/**
 * The declared tuning range for a server, or null if it declares none (or is not
 * listed). See parseBwLimit — this is the only place the limit is published.
 *
 * ★ Never throws and never blocks the connect: the directory is plain HTTP and
 * may be unreachable, in which case we simply learn the range the slow way, by
 * watching where the radio actually goes.
 */
export async function fmdxDeclaredRange(baseUrl: string): Promise<{ lo: number; hi: number } | null> {
  try {
    if (!limitCache || Date.now() - limitCache.at > LIMIT_TTL_MS) {
      const list = await fetchFmdxServers();
      limitCache = { at: Date.now(), byHost: new Map(list.map(s => [hostKey(s.url), s.bwLimit])) };
    }
    const raw = limitCache.byHost.get(hostKey(baseUrl));
    return raw ? parseBwLimit(raw) : null;
  } catch {
    return null;
  }
}

function haversineKm(aLat: number, aLon: number, bLat: number, bLon: number): number {
  const R = 6371, toRad = (d: number) => (d * Math.PI) / 180;
  const dLat = toRad(bLat - aLat), dLon = toRad(bLon - aLon);
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(aLat)) * Math.cos(toRad(bLat)) * Math.sin(dLon / 2) ** 2;
  return Math.round(R * 2 * Math.atan2(Math.sqrt(s), Math.sqrt(1 - s)));
}

/** Fetch the public FM-DX server list, active servers only, distance-sorted
 *  when a location is supplied (else alphabetical). */
export async function fetchFmdxServers(lat?: number, lon?: number): Promise<FmdxServer[]> {
  const res = await fetch(FMDX_API, { headers: { 'User-Agent': USER_AGENT } });
  const json = await res.json();
  const rows: any[] = Array.isArray(json?.dataset) ? json.dataset : [];
  const out: FmdxServer[] = [];
  for (const r of rows) {
    if (Number(r?.status) !== 1) continue;            // 1 = active, 2 = offline
    const url = String(r?.url ?? '').replace(/\/+$/, '');
    if (!url) continue;
    const coords = Array.isArray(r?.coords) ? r.coords : [];
    const slat = coords.length >= 2 ? Number(coords[0]) : NaN;
    const slon = coords.length >= 2 ? Number(coords[1]) : NaN;
    out.push({
      name:    String(r?.name ?? 'FM-DX').slice(0, 120),
      url,
      city:    String(r?.city ?? r?.countryName ?? ''),
      country: String(r?.countryName ?? ''),
      iso:     String(r?.country ?? '').toLowerCase(),
      tuner:   String(r?.tuner ?? '').toLowerCase(),
      bwLimit: String(r?.bwLimit ?? ''),
      audio:   String(r?.audioQuality ?? ''),
      lat: Number.isFinite(slat) ? slat : null,
      lon: Number.isFinite(slon) ? slon : null,
      distance: (lat != null && lon != null && Number.isFinite(slat) && Number.isFinite(slon))
        ? haversineKm(lat, lon, slat, slon) : null,
    });
  }
  if (lat != null && lon != null) {
    out.sort((a, b) => (a.distance ?? 1e9) - (b.distance ?? 1e9));
  } else {
    out.sort((a, b) => a.name.localeCompare(b.name));
  }
  return out;
}
