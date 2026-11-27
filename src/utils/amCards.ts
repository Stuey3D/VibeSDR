/**
 * amCards.ts — ★★★ beta97 THE AMATEUR (DIGI MODES / SPOTS) MAP ON THE MAP CARDS in the app (the web's b97-maps-port): the
 * pure parts — the page's statistics (digiMapStats.ts digiStats, posted by the page) checked and turned into the STATS
 * card's sections (summary, range, modes, bands, countries), and the LIST's one sort key (latest ⇄ nearest). Pure —
 * scripts/test_am_cards.ts.
 */
import { amSpotKey } from './amateurMap';

interface StatItem { call: string; band: string; mode: string; km: number | null; country: string; t: number }
export interface AmStatsIn {
  n: number; nCountries: number; nBands: number;
  closest: StatItem | null; farthest: StatItem | null; latest: StatItem | null; rarest: [string, number] | null;
  modes: Array<[string, number]>; bands: Array<[string, number]>; countries: Array<[string, number]>;
  bearing: Array<{ d: string; n: number; pct: number }> | null;
  aprs: { call: string; km: number } | null; aprsN: number;
}
const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, n = 40) => (typeof v === 'string' ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, n) : '');
const pairs = (v: unknown, max: number): Array<[string, number]> => (Array.isArray(v) ? v : []).slice(0, max)
  .filter((x) => Array.isArray(x) && typeof x[0] === 'string').map((x) => [str(x[0], 40), Math.max(0, num(x[1]))] as [string, number]);
const item = (v: unknown): StatItem | null => {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const call = str(o.call, 16); if (!call) return null;
  return { call, band: str(o.band, 8), mode: str(o.mode, 12), km: o.km == null ? null : num(o.km, NaN), country: str(o.country, 40), t: num(o.t) };
};
/** The page's message, trusted for nothing: numbers are numbers, strings bounded, lists capped. null = not stats. */
export function amStatsClean(m: unknown): AmStatsIn | null {
  if (!m || typeof m !== 'object' || (m as { t?: unknown }).t !== 'amStats') return null;
  const o = m as Record<string, unknown>;
  const r = Array.isArray(o.rarest) && typeof o.rarest[0] === 'string' ? [str(o.rarest[0]), num(o.rarest[1])] as [string, number] : null;
  const a = o.aprs && typeof o.aprs === 'object' && str((o.aprs as Record<string, unknown>).call, 16) ? { call: str((o.aprs as Record<string, unknown>).call, 16), km: num((o.aprs as Record<string, unknown>).km) } : null;
  return { n: Math.max(0, num(o.n)), nCountries: Math.max(0, num(o.nCountries)), nBands: Math.max(0, num(o.nBands)),
           closest: item(o.closest), farthest: item(o.farthest), latest: item(o.latest), rarest: r,
           modes: pairs(o.modes, 8), bands: pairs(o.bands, 10), countries: pairs(o.countries, 8),
           bearing: Array.isArray(o.bearing) ? (o.bearing as unknown[]).slice(0, 8).map((x) => ({ d: str((x as { d?: unknown }).d, 3), n: num((x as { n?: unknown }).n), pct: num((x as { pct?: unknown }).pct) })) : null,
           aprs: a, aprsN: Math.max(0, num(o.aprsN)) };
}
const nf = (n: number) => Math.round(n).toLocaleString('en-GB');
/** The STATS card: one section per subject (its tabs); a station's row finds it on the map (its spot key). */
export function amStatsSections(st: AmStatsIn | null): Array<{ mode: string; rows: Array<{ key: string; label: string; value: string; name: string }> }> {
  if (!st || !st.n) return [];
  const sk = (x: StatItem | null) => (x ? amSpotKey(x.call, x.band, x.mode) : '');
  const who = (x: StatItem) => [x.call, x.mode, x.band, x.country].filter(Boolean).join(' · ');
  const summary = [{ key: '', label: 'STATIONS', value: nf(st.n), name: `${nf(st.nCountries)} countries · ${nf(st.nBands)} bands` }];
  if (st.latest) summary.push({ key: sk(st.latest), label: 'LATEST', value: st.latest.mode, name: who(st.latest) });
  if (st.rarest) summary.push({ key: '', label: 'RAREST COUNTRY', value: `${st.rarest[1]}`, name: st.rarest[0] });
  const range: Array<{ key: string; label: string; value: string; name: string }> = [];
  if (st.farthest) range.push({ key: sk(st.farthest), label: 'FURTHEST', value: st.farthest.km != null && Number.isFinite(st.farthest.km) ? `${nf(st.farthest.km)} km` : '', name: who(st.farthest) });
  if (st.closest) range.push({ key: sk(st.closest), label: 'CLOSEST', value: st.closest.km != null && Number.isFinite(st.closest.km) ? `${nf(st.closest.km)} km` : '', name: who(st.closest) });
  if (st.aprs) range.push({ key: '', label: 'APRS FURTHEST', value: `${nf(st.aprs.km)} km`, name: st.aprs.call });
  for (const b of st.bearing ?? []) if (b.n) range.push({ key: '', label: `BEARING ${b.d}`, value: `${b.pct}%`, name: `${nf(b.n)} stations` });
  const list = (rows: Array<[string, number]>) => rows.map(([k, n]) => ({ key: '', label: k, value: nf(n), name: `${Math.round((n / st.n) * 100)}% of the stations` }));
  const out = [{ mode: 'SUMMARY', rows: summary }];
  if (range.length) out.push({ mode: 'RANGE', rows: range });
  if (st.modes.length) out.push({ mode: 'MODES', rows: list(st.modes) });
  if (st.bands.length) out.push({ mode: 'BANDS', rows: list(st.bands) });
  if (st.countries.length) out.push({ mode: 'COUNTRIES', rows: list(st.countries) });
  return out;
}
/** The LIST's one sort key: latest (the rows' own order — newest first) ⇄ nearest (by the row's distance; none last). */
export function amSortRows<T extends { range: string }>(rows: T[], sort: 'latest' | 'nearest' | string): T[] {
  if (sort !== 'nearest') return rows;
  const d = (r: T) => { const m = /([\d.,]+)\s*(km|mi|NM)?/i.exec(r.range || ''); return m ? parseFloat(m[1].replace(/,/g, '')) : Infinity; };
  return rows.map((r, i) => ({ r, i, k: d(r) })).sort((a, b) => a.k - b.k || a.i - b.i).map((x) => x.r);
}
