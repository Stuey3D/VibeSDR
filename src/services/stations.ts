/**
 * stations.ts — VTS station/bookmark engine + search (skin parity).
 *
 * Server sources:
 *   GET /api/bookmarks — UberSDR: static config bookmarks AUGMENTED with currently
 *     active EiBi broadcast schedule entries (station names, group "EiBi").
 *     Refresh periodically — the EiBi active set changes through the day.
 *   GET /bookmarks     — VIBESERVER: the stations its shim has learned from RDS as people tune.
 *     A different server with a different path; fetchBookmarks asks for both.
 *   GET /api/bands     — amateur band list for the search ("Band Plan" rows).
 *   GET /api/noisefloor/latest — per-band ft8_snr for ham band conditions.
 *
 * Lookup rules (verbatim from the reference skin):
 *   VTS_ON_HZ  99      within ±99 Hz of a bookmark = "on tune"
 *   VTS_MAX_KHZ 150    nearest bookmark beyond 150 kHz = no station shown
 */

export const VTS_ON_HZ = 99;
export const VTS_MAX_KHZ = 150;

export interface ServerBookmark {
  name:            string;
  frequency:       number;   // Hz
  mode?:           string;
  group?:          string;
  comment?:        string;
  bandwidth_low?:  number;
  bandwidth_high?: number;
  repeater?:       boolean;   // OWRX: from the repeater database (vs a plain bookmark)
  flag?:           string;    // emoji flag of the transmitter country (EiBi ITU)
  itu?:            string;    // EiBi transmitter-country code (raw, col 4)
  source?:         'eibi' | 'server' | 'user';   // origin — drives the VTS source icon
}

export interface ServerBand {
  label:  string;
  start:  number;  // Hz
  end:    number;  // Hz
  group?: string;
  mode?:  string;
}

/**
 * ★★★ TWO SERVERS, TWO PATHS, AND THE APP ONLY KNEW ONE. UberSDR serves `/api/bookmarks`;
 *     VIBESERVER serves `/bookmarks` — the stations its shim has LEARNED FROM RDS as people tune,
 *     which is the list the browser has always shown. Connecting the app to a VibeServer asked for
 *     a path that server does not have, got a 404, and showed nothing: "the rds confirmed stations
 *     saving is not working on the app, it is on the web client" (Stuart, 2026-08-15, with the
 *     browser listing thirty stations beside an empty app).
 * ★★ BOTH ARE TRIED, rather than detecting which server this is. Each path exists on exactly one
 *    of them, so the other simply 404s — and asking is cheaper and more honest than keeping a
 *    server-type flag in step with reality. A failure on either side is not an error as long as
 *    one answered; only both failing is.
 * ★ Merged and de-duplicated by frequency and name, because a server could one day serve both and
 *   a list with everything twice is worse than either half.
 */
export async function fetchBookmarks(baseUrl: string): Promise<ServerBookmark[]> {
  const base = baseUrl.replace(/\/+$/, '');
  const one = async (path: string): Promise<ServerBookmark[]> => {
    const res = await fetch(`${base}${path}`);
    if (!res.ok) throw new Error(`bookmarks HTTP ${res.status}`);
    const data = await res.json();
    const list = Array.isArray(data) ? data : (data?.bookmarks ?? []);
    return (list as ServerBookmark[]).filter(
      (b) => b && typeof b.frequency === 'number' && b.frequency > 0 && !!b.name,
    );
  };
  const results = await Promise.allSettled([one('/api/bookmarks'), one('/bookmarks')]);
  const ok = results.filter((r) => r.status === 'fulfilled') as PromiseFulfilledResult<ServerBookmark[]>[];
  if (!ok.length) {
    const first = results[0] as PromiseRejectedResult;
    throw first.reason instanceof Error ? first.reason : new Error('bookmarks unavailable');
  }
  const seen = new Set<string>();
  const merged: ServerBookmark[] = [];
  for (const r of ok) {
    for (const b of r.value) {
      const k = `${Math.round(b.frequency)}|${b.name.trim().toLowerCase()}`;
      if (seen.has(k)) continue;
      seen.add(k);
      merged.push(b);
    }
  }
  return merged;
}

/** Server UI config — spectrum backdrop + station-ID overlay settings. */
export interface ServerUiConfig {
  spectrum_bg_image?:   string;   // url/path; empty = none
  spectrum_bg_opacity?: number;   // 0–1, server default 0.30
  station_id_overlay?:  boolean;  // default true
  station_id_color?:    string;   // #rrggbb, default #ffffff
}

export async function fetchUiConfig(baseUrl: string): Promise<ServerUiConfig | null> {
  try {
    const res = await fetch(`${baseUrl.replace(/\/+$/, '')}/api/ui-config`);
    if (!res.ok) return null;
    return await res.json() as ServerUiConfig;
  } catch {
    return null;
  }
}

export interface ReceiverInfo {
  callsign?: string;
  name?:     string;
  location?: string;
  /** UberSDR server software version — /api/description top-level `version`. */
  serverVersion?: string;
}

export async function fetchReceiverInfo(baseUrl: string): Promise<ReceiverInfo | null> {
  try {
    const res = await fetch(`${baseUrl.replace(/\/+$/, '')}/api/description`);
    if (!res.ok) return null;
    const d = await res.json() as { receiver?: ReceiverInfo; version?: string };
    if (!d?.receiver && !d?.version) return null;
    return { ...(d.receiver ?? {}), serverVersion: d.version };
  } catch {
    return null;
  }
}

export async function fetchBands(baseUrl: string): Promise<ServerBand[]> {
  const res = await fetch(`${baseUrl.replace(/\/+$/, '')}/api/bands`);
  if (!res.ok) throw new Error(`bands HTTP ${res.status}`);
  const data = await res.json();
  return Array.isArray(data)
    ? (data as ServerBand[]).filter((b) => b && typeof b.start === 'number')
    : [];
}

// ── ITU region from receiver longitude (skin _deriveItuRegion) ──────────────

export function deriveItuRegion(lon: number | null | undefined): number {
  if (lon === null || lon === undefined) return 0;
  if (lon < -30) return 2;
  if (lon < 60) return 1;
  return 3;
}

// ── Nearest / next-bookmark lookups (skin findNearest / findNextBookmark) ──

export interface NearestStation {
  hz:     number;
  name:   string;
  mode:   string | null;
  offset: number;  // curHz - bookmark hz
  flag?:   string;
  source?: 'eibi' | 'server' | 'user';
}

export function findNearest(bms: ServerBookmark[], curHz: number): NearestStation | null {
  let best: NearestStation | null = null;
  let bestDist = Infinity;
  for (const bm of bms) {
    const d = Math.abs(bm.frequency - curHz);
    if (d < bestDist) {
      bestDist = d;
      best = { hz: bm.frequency, name: bm.name, mode: bm.mode ?? null, offset: curHz - bm.frequency, flag: bm.flag, source: bm.source };
    }
  }
  if (best && bestDist > VTS_MAX_KHZ * 1000) return null;
  return best;
}

export function findNextBookmark(
  bms: ServerBookmark[], curHz: number, direction: 'left' | 'right',
): ServerBookmark | null {
  if (!bms.length) return null;
  const sorted = [...bms].sort((a, b) => a.frequency - b.frequency);
  if (direction === 'left') {
    for (let i = sorted.length - 1; i >= 0; i--) {
      if (sorted[i].frequency < curHz - VTS_ON_HZ) return sorted[i];
    }
  } else {
    for (const bm of sorted) {
      if (bm.frequency > curHz + VTS_ON_HZ) return bm;
    }
  }
  return null;
}

export function fmtOffset(hz: number): string {
  const abs = Math.abs(hz);
  if (abs === 0) return '';
  if (abs >= 1000) {
    const k = hz / 1000;
    return (hz > 0 ? '+' : '') + k.toFixed(1) + 'kHz';
  }
  return (hz > 0 ? '+' : '') + hz + 'Hz';
}

// ── Band-condition cache (ham bands, /api/noisefloor/latest ft8_snr) ────────

let _snrCache: Record<string, number> = {};
let _snrCacheTime = 0;
let _snrBase = '';
const SNR_TTL = 60_000;

export function refreshBandSnr(baseUrl: string): void {
  if (baseUrl === _snrBase && Date.now() - _snrCacheTime <= SNR_TTL) return;
  // Instance changed → drop the previous server's conditions IMMEDIATELY. The
  // cache is module-global and the fetch below is async (and 404s on servers
  // without /api/noisefloor/latest, e.g. OpenWebRX), so without this clear the
  // OLD instance's band SNR leaks onto the new one (showed UberSDR "Conditions:
  // Fair" while connected to OWRX, which supplies no conditions at all).
  if (baseUrl !== _snrBase) _snrCache = {};
  _snrBase = baseUrl;
  _snrCacheTime = Date.now();  // debounce concurrent refreshes
  fetch(`${baseUrl.replace(/\/+$/, '')}/api/noisefloor/latest`)
    .then((r) => (r.ok ? r.json() : null))
    .then((data: Record<string, { ft8_snr?: number }> | null) => {
      if (!data) return;
      const cache: Record<string, number> = {};
      for (const band of Object.keys(data)) {
        const snr = data[band]?.ft8_snr;
        if (typeof snr === 'number' && snr > 0) cache[band] = Math.round(snr);
      }
      _snrCache = cache;
    })
    .catch(() => {});
}

export function getBandSnrDb(baseUrl: string, bandLabel?: string): number | null {
  if (!bandLabel) return null;
  refreshBandSnr(baseUrl);
  const v = _snrCache[bandLabel];
  return typeof v === 'number' ? v : null;
}

export function propCondition(snr: number | null): string | null {
  if (snr === null || snr === undefined) return null;
  if (snr >= 30) return `Excellent (${snr} dB)`;
  if (snr >= 20) return `Good (${snr} dB)`;
  if (snr >= 6) return `Fair (${snr} dB)`;
  return `Poor (${snr} dB)`;
}

export function fmtBandFreq(hz: number): string {
  if (hz >= 1_000_000) {
    const mhz = hz / 1_000_000;
    return (mhz === Math.floor(mhz) ? mhz.toFixed(0) : mhz.toFixed(mhz < 10 ? 3 : 1)) + ' MHz';
  }
  return Math.round(hz / 1000) + ' kHz';
}

// ── Search (skin lsv-mp-bm-input scoring, verbatim) ─────────────────────────

export function fmtFreq(hz: number): string {
  if (!hz || hz <= 0) return '—';
  const k = hz / 1000;
  if (k >= 1000) return (k / 1000).toFixed(3) + ' MHz';
  if (k >= 1) return k.toFixed(k < 100 ? 2 : 1) + ' kHz';
  return hz + ' Hz';
}

export function fmtRange(lo: number, hi: number): string {
  if (!lo && !hi) return '—';
  if (!hi || lo === hi) return fmtFreq(lo);
  const loK = lo / 1000, hiK = hi / 1000;
  if (loK >= 1000 && hiK >= 1000) {
    const fm = (v: number) => v.toFixed(3).replace(/\.?0+$/, '');
    return fm(loK / 1000) + '–' + fm(hiK / 1000) + ' MHz';
  }
  if (loK >= 1 && hiK < 1000) return Math.round(loK) + '–' + Math.round(hiK) + ' kHz';
  return fmtFreq(lo) + '–' + fmtFreq(hi);
}

export function grpAbbr(g?: string): string {
  const m: Record<string, string> = {
    amateur: 'HAM', broadcast: 'BCST', aviation: 'AVI', marine: 'MAR', maritime: 'MAR',
    weather: 'WX', military: 'MIL', navigation: 'NAV', 'am broadcast': 'AM',
    'fm broadcast': 'FM', shortwave: 'SW', 'short wave': 'SW', cb: 'CB',
  };
  if (!g) return '—';
  const k = g.toLowerCase().trim();
  return m[k] || m[k.split(/\s+/)[0]] || g.trim().substring(0, 4).toUpperCase();
}

function scoreBm(name: string, q: string): number {
  const n = name.toLowerCase(), lq = q.toLowerCase().trim();
  if (!lq) return 0;
  if (n.indexOf(lq) !== -1) return 3;
  const nw = n.split(/[\s\-\/,()+]+/).filter(Boolean);
  for (const w of nw) if (w.indexOf(lq) !== -1) return 2;
  for (const w of nw) if (w.indexOf(lq) === 0) return 1;
  return 0;
}

function scoreBand(band: ServerBand, q: string): number {
  const lq = q.toLowerCase().trim();
  const lbl = (band.label || '').toLowerCase();
  const grp = (band.group || '').toLowerCase();
  const abb = grpAbbr(band.group).toLowerCase();
  if (lbl.indexOf(lq) !== -1) return 3;
  if (grp === lq || abb === lq) return 3;
  if (grp.indexOf(lq) !== -1 || abb.indexOf(lq) !== -1) return 2;
  const nw = lbl.split(/[\s\-\/,()+]+/).filter(Boolean);
  for (const w of nw) if (w.indexOf(lq) !== -1) return 2;
  const num = parseFloat(lq);
  if (!isNaN(num)) {
    for (const cand of [num, num * 1e3, num * 1e6]) {
      if (cand >= (band.start || 0) && cand <= (band.end || 0)) return 1;
    }
  }
  return 0;
}

export interface SearchResult {
  isBand: boolean;
  bm?:    ServerBookmark;
  band?:  ServerBand;
}

const BAND_GENERIC_RE = /^(band|bands|band\s*plan|bandplan)$/i;

export function searchStations(
  bms: ServerBookmark[], bands: ServerBand[], query: string, limit = 200,
): SearchResult[] {
  const q = query.trim();
  if (!q) return [];
  // ★ A Dutch (and most European) keyboard types a COMMA for the decimal point, so
  // "100,5" would never match the "100.5" we format. Used ONLY for the frequency
  // comparison below — names can legitimately contain a comma ("Radio 1, Hilversum"),
  // so the query itself is never rewritten.
  const qFreq = q.includes(',') && !q.includes('.') ? q.replace(/,/g, '.') : q;
  const isGenBand = BAND_GENERIC_RE.test(q);

  const scoredBms: Array<{ bm: ServerBookmark; s: number }> = [];
  if (!isGenBand) {
    for (const bm of bms) {
      let s = scoreBm(bm.name || '', q);
      if (!s && fmtFreq(bm.frequency).toLowerCase().indexOf(qFreq.toLowerCase()) !== -1) s = 1;
      if (s > 0) scoredBms.push({ bm, s });
    }
    scoredBms.sort((a, b) => b.s - a.s || (a.bm.frequency || 0) - (b.bm.frequency || 0));
  }

  const scoredBands: Array<{ band: ServerBand; s: number }> = [];
  if (isGenBand) {
    for (const band of bands) scoredBands.push({ band, s: 3 });
    scoredBands.sort((a, b) => (a.band.start || 0) - (b.band.start || 0));
  } else {
    for (const band of bands) {
      const s = scoreBand(band, q);
      if (s > 0) scoredBands.push({ band, s });
    }
    scoredBands.sort((a, b) => b.s - a.s || (a.band.start || 0) - (b.band.start || 0));
  }

  const out: SearchResult[] = [];
  for (const { bm } of scoredBms) {
    if (out.length >= limit) break;
    out.push({ isBand: false, bm });
  }
  for (const { band } of scoredBands) {
    if (out.length >= limit) break;
    out.push({ isBand: true, band });
  }
  return out;
}
