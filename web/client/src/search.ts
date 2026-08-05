/**
 * search.ts — station/bookmark/band search for the web client.
 *
 * Three sources, ordered the way the app orders them (server first, EiBi as
 * fallback, band plan ALWAYS included — that ordering is what fixed Kiwi search):
 *
 *   1. USER bookmarks — local to this browser, exportable. The app's own module
 *      does the work (userBookmarks.ts, aliased onto localStorage at bundle time)
 *      so the export is byte-compatible with the phone AND with desktop UberSDR.
 *   2. SERVER stations — GET /stations on the shim. That's the EiBi schedule the
 *      APP downloaded and cached, handed over when Server mode started. The
 *      browser can't fetch eibispace.de itself: it sends no CORS headers, and
 *      unlike React Native a browser enforces them.
 *   3. BAND PLAN — always searched, bundled, offline.
 *
 * A future "save to server" (shared list, gated behind the admin password once
 * public use lands) drops in as a fourth source — the shape here allows for it.
 */

import {
  loadUserBookmarks, saveUserBookmarks, exportBookmarksJSON,
  parseBookmarksAny, mergeBookmarks, type UserBookmark,
} from '../../../src/services/userBookmarks';
import { BAND_PLAN } from '../../../src/constants/bandPlan';
import type { SDRMode } from './spectrum';

export type ResultSource = 'user' | 'server' | 'eibi' | 'band';

export interface SearchResult {
  name: string;
  frequency: number;         // Hz
  mode?: string;
  source: ResultSource;
  detail?: string;           // group / comment / band type
  flag?: string;             // EiBi transmitter country
  itu?: string;              // EiBi transmitter-country code — AUTHORITATIVE, no guessing
  bandwidthLow?: number | null;
  bandwidthHigh?: number | null;
}

/** A station as the shim serves it (src/services/stations.ts ServerBookmark). */
interface ServerStation {
  name: string;
  frequency: number;
  mode?: string;
  group?: string;
  comment?: string;
  flag?: string;
  itu?: string;
  source?: 'eibi' | 'server' | 'user';
  bandwidth_low?: number;
  bandwidth_high?: number;
}

let stations: ServerStation[] = [];
let bookmarks: UserBookmark[] = [];

/**
 * SERVER bookmarks — the stations this receiver has actually HEARD.
 *
 * Learned by the shim from RDS and served at GET /bookmarks. Distinct from the EiBi
 * schedule (which lists everything that exists, whether or not you can receive it)
 * and from the browser's own bookmarks (which live only on this computer). These are
 * shared with every client, and they expire if the receiver stops hearing them.
 */
let serverBookmarks: ServerStation[] = [];
let bmHost = '';
/** The auth suffix, so writes carry the PIN. Empty on an open server. */
let bmAuth = '';

export async function loadServerBookmarks(host: string, authSuffix = ''): Promise<number> {
  bmHost = host;
  bmAuth = authSuffix;
  try {
    const r = await fetch(`http://${host}/bookmarks`, { cache: 'no-store' });
    if (!r.ok) return 0;
    const arr = await r.json();
    serverBookmarks = Array.isArray(arr)
      ? arr.filter((s: any) => s && s.name && s.frequency)
           .map((s: any) => ({ ...s, source: 'server' as const }))
      : [];
  } catch {
    serverBookmarks = [];      // older shim, or not reachable
  }
  return serverBookmarks.length;
}

export function getServerBookmarks(): ServerStation[] { return serverBookmarks; }

/** Query string carrying the PIN, so the shim's write gate accepts us. */
function authQs(): string {
  // bmAuth is "&vs_nonce=...&vs_auth=..." — as the first param it needs a '?'.
  return bmAuth ? '?' + bmAuth.replace(/^&/, '') : '';
}

/** Save to the RECEIVER, shared with every client. Returns the new list.
 *
 *  The MODE has to go with it. Without one the shim can only assume, and an imported
 *  list is mostly AM, USB, CW and fax — every one of them arrived as WFM and was
 *  unlistenable. */
export async function saveToServer(
  frequency: number, name: string, mode?: string,
): Promise<boolean> {
  if (!bmHost) return false;
  try {
    const qs = authQs();
    const sep = qs ? '&' : '?';
    const r = await fetch(
      `http://${bmHost}/bookmarks${qs}${sep}frequency=${Math.round(frequency)}` +
      `&name=${encodeURIComponent(name)}` +
      (mode ? `&mode=${encodeURIComponent(mode)}` : ''), { method: 'POST' });
    if (!r.ok) return false;
    const arr = await r.json();
    if (Array.isArray(arr)) {
      serverBookmarks = arr.filter((s: any) => s && s.name && s.frequency)
                           .map((s: any) => ({ ...s, source: 'server' as const }));
    }
    return true;
  } catch { return false; }
}

export async function removeFromServer(frequency: number): Promise<boolean> {
  if (!bmHost) return false;
  try {
    const qs = authQs();
    const sep = qs ? '&' : '?';
    const r = await fetch(
      `http://${bmHost}/bookmarks${qs}${sep}frequency=${Math.round(frequency)}`,
      { method: 'DELETE' });
    if (!r.ok) return false;
    const arr = await r.json();
    if (Array.isArray(arr)) {
      serverBookmarks = arr.filter((s: any) => s && s.name && s.frequency)
                           .map((s: any) => ({ ...s, source: 'server' as const }));
    }
    return true;
  } catch { return false; }
}

/** Pull the server's station list. Absent/offline is fine — we degrade. */
export async function loadStations(host: string): Promise<number> {
  try {
    const r = await fetch(`http://${host}/stations`, { cache: 'no-store' });
    if (!r.ok) return 0;
    const arr = await r.json();
    stations = Array.isArray(arr) ? arr.filter(s => s && s.name && s.frequency) : [];
  } catch {
    stations = [];   // no internet on the phone, or an older shim
  }
  return stations.length;
}

export async function loadBookmarks(): Promise<UserBookmark[]> {
  bookmarks = await loadUserBookmarks();
  return bookmarks;
}

export function getBookmarks(): UserBookmark[] { return bookmarks; }

/** The server's station list — used by the VTS steward as well as search. */
export function getStations(): ServerStation[] { return stations; }

export async function addBookmark(b: Omit<UserBookmark, 'scope'>): Promise<void> {
  bookmarks = mergeBookmarks(bookmarks, [{ ...b, scope: '' }]);
  await saveUserBookmarks(bookmarks);
}

export async function removeBookmark(name: string, frequency: number): Promise<void> {
  bookmarks = bookmarks.filter(b => !(b.name === name && b.frequency === frequency));
  await saveUserBookmarks(bookmarks);
}

/** UberSDR-importable JSON — the same file the phone app exports. */
export function exportBookmarks(): string { return exportBookmarksJSON(bookmarks); }

/** Import JSON or YAML (auto-detected), merged into the existing list. */
export async function importBookmarks(text: string): Promise<number> {
  const incoming = parseBookmarksAny(text, '');
  if (!incoming.length) throw new Error('No bookmarks found in that file');
  bookmarks = mergeBookmarks(bookmarks, incoming);
  await saveUserBookmarks(bookmarks);
  return incoming.length;
}

/** "100.7", "100.7M", "7150k", "14.074 MHz" -> Hz. Null if not a frequency. */
function parseFreq(q: string): number | null {
  const m = q.trim().match(/^([\d.]+)\s*(k|khz|m|mhz|hz)?$/i);
  if (!m) return null;
  const n = parseFloat(m[1]);
  if (!isFinite(n) || n <= 0) return null;
  const unit = (m[2] || '').toLowerCase();
  if (unit.startsWith('k')) return Math.round(n * 1e3);
  if (unit.startsWith('m')) return Math.round(n * 1e6);
  if (unit === 'hz') return Math.round(n);
  // Bare number: guess by magnitude, the way a ham would read it.
  if (n < 30) return Math.round(n * 1e6);        // 14.074 -> 14.074 MHz
  if (n < 30_000) return Math.round(n * 1e3);    // 7150 -> 7150 kHz
  return Math.round(n);
}

/** The window this receiver can actually reach, or null when it can reach anything.
 *  ★★★ WHY SEARCH CARES. On a locked receiver most of EiBi is unreachable — a 2.5-10.5 MHz
 *  window cannot hear a single VHF entry — and offering results you cannot tune to is the same
 *  lie as a readout showing a frequency you are not on: the user picks one, nothing happens where
 *  they expected, and they conclude the receiver is broken rather than out of range. OWRX filters
 *  its bookmarks to the profile for exactly this reason (Stuart, 2026-08-05).
 *  ★ Null on a personal receiver, where the dongle follows the dial and everything IS reachable —
 *    the filter must never narrow a radio that is not locked. */
let tunableWindow: [number, number] | null = null;
export function setTunableWindow(w: [number, number] | null) { tunableWindow = w; }

export function search(query: string, limit = 40): SearchResult[] {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  const out: SearchResult[] = [];

  // A typed frequency is a result in its own right — the fastest way to tune.
  const hz = parseFreq(q);
  if (hz) {
    out.push({
      name: `Tune ${(hz / 1e6).toFixed(3)} MHz`,
      frequency: hz,
      source: 'band',
      detail: 'Direct entry',
    });
  }

  const hit = (s: string | undefined | null) => !!s && s.toLowerCase().includes(q);

  // 1. User bookmarks first — the user's own list outranks everything.
  for (const b of bookmarks) {
    if (hit(b.name) || hit(b.group) || hit(b.comment)) {
      out.push({
        name: b.name, frequency: b.frequency, mode: b.mode, source: 'user',
        detail: b.group || b.comment || undefined,
        bandwidthLow: b.bandwidth_low, bandwidthHigh: b.bandwidth_high,
      });
    }
  }

  // 2. SERVER bookmarks — stations this receiver has actually heard. Ranked above
  //    EiBi deliberately: EiBi lists what EXISTS, this lists what you can RECEIVE,
  //    and the latter is nearly always the more useful answer.
  for (const b of serverBookmarks) {
    if (hit(b.name)) {
      out.push({
        name: b.name, frequency: b.frequency, mode: b.mode ?? 'wfm', source: 'server',
        detail: (b as any).manual ? 'Saved on the receiver' : 'Heard by this receiver',
      });
    }
  }

  // 3. Server stations (the app's cached EiBi).
  for (const s of stations) {
    if (hit(s.name) || hit(s.group) || hit(s.comment)) {
      out.push({
        name: s.name, frequency: s.frequency, mode: s.mode,
        source: s.source === 'server' ? 'server' : 'eibi',
        detail: s.group || s.comment || undefined,
        flag: s.flag,
        itu: (s as any).itu,
        bandwidthLow: s.bandwidth_low, bandwidthHigh: s.bandwidth_high,
      });
      if (out.length > limit * 4) break;   // bound the scan; EiBi is ~10k rows
    }
  }

  // 4. Band plan — ALWAYS searched, never filtered out by the sources above.
  for (const b of BAND_PLAN) {
    if (hit(b.name) || hit(b.type) || hit(b.bandLabel)) {
      out.push({
        name: b.name,
        // Tuning to a BAND means parking in the middle of it.
        frequency: Math.round((b.lo + b.hi) / 2),
        mode: b.mode,
        source: 'band',
        detail: `${b.type} · ${(b.lo / 1e6).toFixed(3)}–${(b.hi / 1e6).toFixed(3)} MHz`,
      });
    }
  }

  // Closest-frequency-first within the same relevance is more useful than
  // alphabetical, but keep source order as the primary key.
  const rank: Record<ResultSource, number> = { user: 0, server: 1, eibi: 2, band: 3 };
  out.sort((a, b) => rank[a.source] - rank[b.source]);

  // ★★ Filter LAST, so every source is narrowed by the same rule and none can be forgotten when
  //    a fourth is added. A band that merely OVERLAPS the window is kept and re-pointed at a
  //    frequency inside it — "40m" is a reasonable thing to ask for on a receiver that covers half
  //    of it, whereas a single station outside the window is simply not available here.
  const win = tunableWindow;
  if (!win) return out.slice(0, limit);
  const [lo, hi] = win;
  const kept: SearchResult[] = [];
  for (const r of out) {
    if (r.frequency >= lo && r.frequency <= hi) { kept.push(r); continue; }
    if (r.source === 'band') {
      const m = /([\d.]+)–([\d.]+) MHz/.exec(r.detail ?? '');
      if (m) {
        const bLo = parseFloat(m[1]) * 1e6, bHi = parseFloat(m[2]) * 1e6;
        if (bHi >= lo && bLo <= hi) {
          const mid = Math.round((Math.max(bLo, lo) + Math.min(bHi, hi)) / 2);
          kept.push({ ...r, frequency: mid });
        }
      }
    }
  }
  return kept.slice(0, limit);
}

export type { UserBookmark, SDRMode };
