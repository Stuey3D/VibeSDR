// navtexMap.ts — V12 2026-11-01 THE NAVTEX MAP, stage 1: the STATIONS RECEIVED (Stuart: "it would be cool to be able to
// open a map and see which navtex stations you've received"). The server folds this gallery's NAVTEX items per station
// (android/app/src/main/cpp/vibe_navtex_map.h, GET /vibeserver/gallery/navtex-stations.json); this is what both clients
// draw from it — the web client (web/client/src/main.ts, TrackView) and the app (MapOverlay kind 'navtex',
// TrackLiveList). ★ ONE RULE, TWO READERS (scripts/test_navtex_map.ts). ★ PURE: no React Native, no DOM.
//
// ★★ The station is the one the gallery's own title names (the label every client already shows — one resolver). Where
//    that label could not decide, the server says "likely" (the time of day, or the history, settled it) or "ambiguous"
//    (a "?" here, its candidates in the card).
// ★ Everything in a report came off the air or the gallery: every string is escaped where it meets HTML.
import { navtexSubject } from '../utils/navtex';

export interface NavCand { name: string; iso: string; lat: number; lon: number; km?: number }
export interface NavRecent { t: number; id: string; f: string }
export interface NavStation {
  key: string; b1: string; name: string; iso: string; country: string; lat: number; lon: number;
  conf: 'confident' | 'likely' | 'ambiguous'; km?: number; khz: number[];
  first: number; last: number; n: number; uniq: number; night: number; folded: number;
  subj: Record<string, number>; cands?: NavCand[]; recent: NavRecent[];
  /** seconds since last heard (from the report's `now`) */
  seen: number;
}
export interface NavReport { rx: { lat: number; lon: number } | null; now: number; messages: number; unplaced: number; oldest: number; stations: NavStation[] }

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const str = (v: unknown, n: number): string => (typeof v === 'string' ? v.slice(0, n) : '');
const okLat = (v: unknown) => num(v) !== undefined && Math.abs(v as number) <= 90;
const okLon = (v: unknown) => num(v) !== undefined && Math.abs(v as number) <= 180;

/** A report from the wire, cut to its fields (a hostile or broken server must not put an object where a number goes). */
export function navSanitise(j: any): NavReport | null {
  if (!j || typeof j !== 'object' || !Array.isArray(j.stations)) return null;
  const now = num(j.now) ?? Math.floor(Date.now() / 1000);
  const stations: NavStation[] = [];
  for (const s of j.stations.slice(0, 400)) {
    if (!s || typeof s !== 'object' || !okLat(s.lat) || !okLon(s.lon)) continue;
    const b1 = /^[A-Z]$/.test(s.b1) ? s.b1 : '';
    if (!b1) continue;
    const conf = s.conf === 'confident' || s.conf === 'likely' || s.conf === 'ambiguous' ? s.conf : 'ambiguous';
    const subj: Record<string, number> = {};
    if (s.subj && typeof s.subj === 'object') for (const k of Object.keys(s.subj).slice(0, 26)) if (/^[A-Z]$/.test(k) && num(s.subj[k]) !== undefined) subj[k] = s.subj[k];
    const cands = Array.isArray(s.cands) ? s.cands.slice(0, 4).filter((c: any) => c && okLat(c.lat) && okLon(c.lon))
      .map((c: any) => ({ name: str(c.name, 40), iso: str(c.iso, 2), lat: c.lat, lon: c.lon, km: num(c.km) })) : undefined;
    const recent = Array.isArray(s.recent) ? s.recent.slice(0, 12).filter((r: any) => r && num(r.t) !== undefined)
      .map((r: any) => ({ t: r.t, id: str(r.id, 4), f: str(r.f, 128) })) : [];
    const name = str(s.name, 40), last = num(s.last) ?? 0;
    stations.push({
      key: `${b1}|${conf === 'ambiguous' ? '?' : ''}${name}`, b1, name, iso: str(s.iso, 2), country: str(s.country, 40), lat: s.lat, lon: s.lon, conf,
      km: num(s.km), khz: Array.isArray(s.khz) ? s.khz.filter((k: any) => num(k) !== undefined).slice(0, 4) : [],
      first: num(s.first) ?? 0, last, n: num(s.n) ?? 0, uniq: num(s.uniq) ?? 0, night: num(s.night) ?? 0, folded: num(s.folded) ?? 0,
      subj, cands: cands && cands.length > 1 ? cands : undefined, recent, seen: Math.max(0, now - last),
    });
  }
  const rx = j.rx && okLat(j.rx.lat) && okLon(j.rx.lon) ? { lat: j.rx.lat, lon: j.rx.lon } : null;
  return { rx, now, messages: num(j.messages) ?? 0, unplaced: num(j.unplaced) ?? 0, oldest: num(j.oldest) ?? 0, stations };
}

// ── the time filter (the ISM map's ranges; this view is the RECEIVED-STATIONS one, so ALL is the default) ──
export const NAV_RANGES = [
  { id: '1h', label: '1 H', words: 'Heard in the last hour', sec: 3600 },
  { id: '24h', label: '24 H', words: 'Heard in the last 24 hours', sec: 86400 },
  { id: '7d', label: '7 DAYS', words: 'Heard in the last 7 days', sec: 7 * 86400 },
  { id: '30d', label: '30 DAYS', words: 'Heard in the last 30 days', sec: 30 * 86400 },
  { id: 'all', label: 'ALL', words: 'Every message still in the gallery', sec: 0 },
];
export const NAV_RANGE_HINT = 'ALL is every NAVTEX message still in the gallery — the oldest go when it is full.';
export function navRangeOf(saved: string | null | undefined): string { return NAV_RANGES.some((r) => r.id === saved) ? (saved as string) : 'all'; }
/** The query's `since` (seconds back; 0 = everything). */
export function navSince(range: string): number { return NAV_RANGES.find((r) => r.id === range)?.sec ?? 0; }

// ── how a station looks ──
/** The band's colour: 518 kHz (international, English) cyan, 490 kHz (national language) violet, 4209.5 kHz (HF) amber. */
export const NAV_BAND_COLOURS: Record<string, string> = { '518': '#4fc3f7', '490': '#c792ea', '4209.5': '#ffb74d', '424': '#9fe870' };
export function navColour(s: NavStation): string { return NAV_BAND_COLOURS[String(s.khz[0] ?? 518)] || '#4fc3f7'; }
function escN(v: string): string { return v.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }
/** ★ Fades over a week since last heard (to 0.45) — a station heard last month is still drawn, quieter. */
export function navAgeOpacity(seen: number): number {
  if (!(seen > 3600)) return 1;
  return Math.max(0.45, 1 - 0.55 * Math.min(1, (seen - 3600) / (7 * 86400)));
}
/** The marker: a transmitting mast on a disc in the band's colour, the station's letter beside it; LIKELY has a dashed
 *  rim, AMBIGUOUS a "?" badge. Two bands heard = a split disc (both colours). */
export function navIcon(s: NavStation): { html: string; w: number; h: number } {
  const c1 = navColour(s), c2 = s.khz.length > 1 ? (NAV_BAND_COLOURS[String(s.khz[1])] || c1) : c1;
  const op = navAgeOpacity(s.seen).toFixed(2);
  const rim = s.conf === 'likely' ? ' stroke-dasharray="2.5 2"' : '';
  const disc = c1 === c2
    ? `<circle cx="13" cy="13" r="11" fill="${c1}" stroke="#0b1420" stroke-width="1.4"${rim}/>`
    : `<path d="M13 2 A11 11 0 0 0 13 24 Z" fill="${c1}"/><path d="M13 2 A11 11 0 0 1 13 24 Z" fill="${c2}"/><circle cx="13" cy="13" r="11" fill="none" stroke="#0b1420" stroke-width="1.4"${rim}/>`;
  const mast = '<path d="M13 7 L9.5 19 M13 7 L16.5 19 M10.6 15 H15.4" stroke="#0b1420" stroke-width="1.6" fill="none" stroke-linecap="round"/>'
    + '<path d="M9 6 a5 5 0 0 0 0 5 M17 6 a5 5 0 0 1 0 5" stroke="#0b1420" stroke-width="1.2" fill="none" stroke-linecap="round"/>';
  const letter = `<text x="25" y="9" font-family="ui-monospace,Menlo,monospace" font-size="9.5" font-weight="700" fill="#e8f4ff" stroke="#0b1420" stroke-width="2.4" paint-order="stroke">${escN(s.b1)}</text>`;
  const q = s.conf === 'ambiguous' ? '<circle cx="22" cy="22" r="5.2" fill="#ffd34d" stroke="#0b1420" stroke-width="1"/><text x="22" y="25" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="8" font-weight="700" fill="#111">?</text>' : '';
  return { html: `<svg width="32" height="28" viewBox="0 0 32 28" style="opacity:${op}" aria-hidden="true">${disc}${mast}${letter}${q}</svg>`, w: 32, h: 28 };
}
const kmTxt = (km: number) => (km < 10 ? km.toFixed(1) : Math.round(km).toLocaleString('en-GB'));
export function navLabel(s: NavStation): string {
  return `${s.b1} · ${s.name}${s.conf === 'ambiguous' ? '?' : ''}${s.km !== undefined ? ` · ${kmTxt(s.km)} km` : ''}`;
}
/** Who keeps its name when labels collide: the most messages, then the most recent. */
export function navRank(s: NavStation): number { return s.uniq * 1e6 - Math.min(s.seen, 999999); }
const CONF_WORDS: Record<string, string> = {
  confident: 'named by its messages',
  likely: 'likely — the label could not decide; the time of day, the message text or the station’s other messages point here',
  ambiguous: 'not certain — the letter and frequency fit more than one station',
};
const utc = (t: number) => { const d = new Date(t * 1000); const p = (n: number) => String(n).padStart(2, '0'); return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())} ${p(d.getUTCHours())}:${p(d.getUTCMinutes())}`; };
export function navAgo(sec: number): string {
  if (!(sec >= 60)) return 'just now';
  if (sec < 3600) return `${Math.round(sec / 60)} min ago`;
  if (sec < 86400) return `${Math.round(sec / 3600)} h ago`;
  return `${Math.round(sec / 86400)} days ago`;
}
/** The card: name, country and letter, the bands, how sure, first / last heard, how many messages (and how many
 *  different), the kinds of message, the newest ids. */
export function navCard(s: NavStation, wrap: 'web' | 'app' = 'web'): string {
  const kinds = Object.entries(s.subj).sort((a, b) => b[1] - a[1]).map(([k, n]) => `${navtexSubject(k) ?? k} ${n}`);
  const lines: string[] = [];
  lines.push(`<b>${escN(s.name)}${s.conf === 'ambiguous' ? '?' : ''}</b>, ${escN(s.country)} · station ${escN(s.b1)}`);
  lines.push(`${s.khz.map((k) => `${k} kHz`).join(' + ')}${s.km !== undefined ? ` · ${escN(kmTxt(s.km))} km from the receiver` : ''}`);
  lines.push(escN(CONF_WORDS[s.conf]));
  if (s.cands) lines.push('Could be: ' + s.cands.map((c) => `${escN(c.name)}${c.km !== undefined ? ` (${escN(kmTxt(c.km))} km)` : ''}`).join(' or '));
  lines.push(`first heard ${utc(s.first)} · last heard ${utc(s.last)} UTC (${navAgo(s.seen)})`);
  lines.push(`${s.n} ${s.n === 1 ? 'message' : 'messages'} received · ${s.uniq} different${s.folded ? ` · ${s.folded} settled from the label’s pair` : ''}`
    + `${s.night ? ` · ${s.night} after dark on the path` : ''}`);
  if (kinds.length) lines.push(escN(kinds.join(' · ')));
  if (s.recent.length) lines.push('Newest: ' + s.recent.slice(0, 8).map((r) => `${escN(r.id)} ${utc(r.t).slice(5)}`).join(' · '));
  const body = lines.join('<br>');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="pop">${body}</div>`;
}
/** The list (the web's TrackView rows / the app's TrackLiveList rows): nearest first. */
export function navRows(r: NavReport): Array<{ s: NavStation; second: string; range: string }> {
  return [...r.stations].sort((a, b) => (a.km ?? 1e9) - (b.km ?? 1e9)).map((s) => ({
    s, range: s.km !== undefined ? `${kmTxt(s.km)} km` : '',
    second: [`${s.b1} · ${s.khz.join('+')} kHz`, `${s.uniq} msg${s.uniq === 1 ? '' : 's'}`, s.conf === 'confident' ? '' : s.conf].filter(Boolean).join(' · '),
  }));
}
export function navStatus(r: NavReport | null): string {
  if (!r) return 'Reading the gallery…';
  const far = r.stations.reduce<NavStation | null>((b, s) => (s.km !== undefined && (!b || s.km > (b.km ?? 0)) ? s : b), null);
  return `${r.stations.length} ${r.stations.length === 1 ? 'station' : 'stations'} · ${r.messages} messages`
    + (far ? ` · furthest ${far.name} ${kmTxt(far.km!)} km` : '') + (r.unplaced ? ` · ${r.unplaced} not placed` : '');
}
export const NAV_EMPTY = 'No NAVTEX message in the gallery for this time — the map fills as messages arrive (a listener’s decoder or background decoding).';

/** ★ The app's pins and rows (MapOverlay kind 'navtex' / TrackLiveList). */
export function navItems(r: NavReport | null) {
  return (r?.stations ?? []).map((s) => ({ key: s.key, lat: s.lat, lon: s.lon, _ic: navIcon(s), _lb: navLabel(s), _cd: navCard(s, 'app'), _h: s.n, _rk: navRank(s), _arc: true }));
}
export function navListRows(r: NavReport | null) {
  if (!r) return [];
  return navRows(r).map(({ s, second, range }) => ({
    key: s.key, colour: navColour(s), flag: '', name: `${s.b1} · ${s.name}${s.conf === 'ambiguous' ? '?' : ''}`, second, sp: '', trend: 0 as const,
    range, brg: '', noPos: false, age: s.seen < 3600 ? `${Math.round(s.seen / 60)} min` : s.seen < 86400 ? `${Math.round(s.seen / 3600)} h` : `${Math.round(s.seen / 86400)} d`, em: false,
  }));
}
