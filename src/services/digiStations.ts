// digiStations.ts — V12 beta90 THE DIGI MODES MAP's stored stations in the app (docs/v12/DIGI-MODES-MAP.md).
//
// ★★★ Stuart (2026-11-21): background decoding and every listener's live spots on ONE map. The server keeps every station
//     its decoders heard (vibe_station_store.h) and answers GET /vibeserver/stations.json?window=1h|24h|7d|30d|all
//     [&since=<UTC s>] — one entry per call. The app polls it while its spots map is open (every 15 s, only what changed
//     since the last answer) and hands the stations to the map as spot rows beside its own live ones: the page draws each
//     call once (its key is call|band|mode, the newest report wins).
// ★ A station heard without a locator (CW, PSK, RTTY) has no place on the map and is not drawn — never placed at a guess.
// ★ Every field came off the air: cut to allow-lists here, as the web's map window does (main.ts cleanStations).
import type { SpotRow } from './DecoderClient';
import { entryRadios, logRadioChoices, type LogRadio } from '../utils/logRadio';

export interface StoredStation {
  call: string; grid: string; mode: string; band: string; freq: number; count: number; snr: number; lastSnr: number;
  first: number; last: number; src: number; modes: string[]; bands: string[]; country: string; lat?: number; lon?: number; km?: number;
  /** ★ beta92 which radio(s) heard it (ids; [] = unknown — kept before beta92) */
  radios: string[];
}
/** `radios` (beta92): the radio filter's choices — null unless the window holds stations from 2+ radios (logRadio.ts). */
export interface StoredStations { now: number; total: number; truncated: boolean; off: boolean; stations: StoredStation[]; radios: LogRadio[] | null }

/** The windows the map offers, in the page's age select (the server's own words). */
export const DIGI_STORE_WINDOWS: Array<{ id: string; label: string }> = [
  { id: '1h', label: '1 hour (all listeners)' }, { id: '24h', label: '24 hours' }, { id: '7d', label: '7 days' },
  { id: '30d', label: '30 days' }, { id: 'all', label: 'All kept' },
];
export function digiWindowSec(w: string): number {
  return w === '1h' ? 3600 : w === '24h' ? 86400 : w === '7d' ? 8 * 86400 : w === '30d' ? 31 * 86400 : 0;
}

const okStr = (v: unknown, re: RegExp, max: number) => String(v ?? '').replace(re, '').slice(0, max);
const okNum = (v: unknown): number => { const n = Number(v); return v == null || v === '' || !Number.isFinite(n) ? 0 : n; };

export function digiStationsSanitise(j: any): StoredStations | null {
  if (!j || typeof j !== 'object') return null;
  if (j.off) return { now: okNum(j.now), total: 0, truncated: false, off: true, stations: [], radios: null };
  if (!Array.isArray(j.stations)) return null;
  const stations: StoredStation[] = [];
  for (const s of j.stations.slice(0, 6000)) {
    if (!s || typeof s !== 'object') continue;
    const call = okStr(s.call, /[^A-Za-z0-9/-]/g, 16);
    if (!call) continue;
    const placed = Number.isFinite(Number(s.lat)) && Number.isFinite(Number(s.lon)) && s.lat != null && s.lon != null;
    stations.push({
      call, grid: okStr(s.grid, /[^A-Za-z0-9]/g, 8), mode: okStr(s.mode, /[^A-Za-z0-9+-]/g, 12), band: okStr(s.band, /[^A-Za-z0-9]/g, 8),
      freq: okNum(s.freq), count: Math.max(0, Math.round(okNum(s.count))), snr: okNum(s.snr), lastSnr: okNum(s.lastSnr),
      first: okNum(s.first), last: okNum(s.last), src: okNum(s.src),
      modes: Array.isArray(s.modes) ? s.modes.slice(0, 8).map((m: unknown) => okStr(m, /[^A-Za-z0-9+-]/g, 12)) : [],
      bands: Array.isArray(s.bands) ? s.bands.slice(0, 8).map((m: unknown) => okStr(m, /[^A-Za-z0-9]/g, 8)) : [],
      country: okStr(s.country, /[<>&"'`\\]/g, 40),
      ...(placed ? { lat: Math.max(-90, Math.min(90, Number(s.lat))), lon: Math.max(-180, Math.min(180, Number(s.lon))) } : {}),
      ...(s.km != null && Number.isFinite(Number(s.km)) ? { km: Math.round(Number(s.km)) } : {}),
      radios: entryRadios(s.radios).slice(0, 8),
    });
  }
  return { now: okNum(j.now), total: Math.max(0, Math.round(okNum(j.total))), truncated: j.truncated === 1, off: false, stations,
           radios: logRadioChoices(j.radios) };
}

/** ★ A live poll (since=) merged into what is held: changed calls replace theirs; a sliding window drops the aged ones. */
export function digiStationsMerge(held: Map<string, StoredStation>, got: StoredStations, full: boolean, window: string): Map<string, StoredStation> {
  const m = full ? new Map<string, StoredStation>() : new Map(held);
  for (const s of got.stations) m.set(s.call, s);
  const w = digiWindowSec(window);
  if (w && got.now) for (const [k, s] of m) if (s.last < got.now - w) m.delete(k);
  return m;
}

/** The stored stations as the map's spot rows — only those with a locator (the map places nothing it cannot). */
export function digiStationRows(held: Map<string, StoredStation>): SpotRow[] {
  const out: SpotRow[] = [];
  for (const s of held.values()) {
    if (!s.grid) continue;
    out.push({ kind: 'digi', time: s.last * 1000, mode: s.mode, band: s.band, call: s.call, snr: s.snr, freqHz: s.freq,
               grid: s.grid, country: s.country, ...(s.km != null ? { distKm: s.km } : {}), ...(s.radios.length ? { radios: s.radios } : {}) });
  }
  return out;
}
