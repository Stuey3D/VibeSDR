/**
 * storedLayers.ts — ★★★ beta96 maps2: STORED SONDES AND VOR STATIONS on the AIR & SEA map (docs/v12/ONE-MAP.md: "Every other
 * layer shows what this server has STORED … faded by age"). When SONDES / the VOR map is not running, the map draws what the
 * server's gallery remembers of them: its day cards (vibeserver vibe_posgal.h — slot "sondes-<date>" / "vor-<date>", gallery
 * kinds "sondes" / "vormap"), read the way ISM's "stations received" cards are (ism.ts ismNodesParse is the pattern).
 *
 * ★★ ONE READER, TWO CLIENTS: the web's asRender and the app's airSeaTrk both use these functions (no DOM, no React Native).
 * ★★ BOUNDED: at most STORED_ROWS_MAX rows a card (the card's own bound) and in all; at most `cards` cards a kind, fetched
 *    one at a time (a live server), a card older than yesterday kept and never fetched again; the server's own HTTP, never
 *    the Worker. Sondes: the last 24 h (today's and yesterday's cards — the current and the last flights); VOR stations:
 *    the last 7 days (a station does not move; what was heard this week is what this receiver hears).
 * ★ Everything from the card is checked again here and escaped where it is drawn.
 */
import { compass8, rangeBearing, type LatLon, type TrackItem, type TrackRow } from './tracks';
import { fadeOf } from './liveMaps';

/** One row of a day card (vibe_posgal.h Row). Times in UTC seconds; null = "-" on the card. */
export interface GalPos {
  id: string; name: string; type: string; cc: string;
  lat: number; lon: number; alt: number | null; top: number | null; mhz: number | null;
  first: number; last: number;
}
export type StoredKind = 'sondes' | 'vormap';
export const STORED_ROWS_MAX = 300;
export const STORED_SPEC: Record<StoredKind, { slot: RegExp; keepSec: number; cards: number }> = {
  sondes: { slot: /^sondes-\d{4}-\d\d-\d\d$/, keepSec: 86400, cards: 2 },
  vormap: { slot: /^vor-\d{4}-\d\d-\d\d$/, keepSec: 7 * 86400, cards: 8 },
};
const DOT = ' · ';
const okTxt = (v: unknown, n: number) => String(v ?? '').replace(/[<>&"'`\\\u0000-\u001f]/g, '').trim().slice(0, n);

/** ★★ The card's table → its rows (the C++ restore's rule: 10 fields, " · " between; a damaged row costs that row). */
export function galPosParse(body: string, dayUtc: number, nowSec: number): GalPos[] {
  const out: GalPos[] = [];
  const at = typeof body === 'string' ? body.indexOf('Each, most recent first') : -1;
  if (at < 0) return out;
  const day = Math.floor(dayUtc / 86400) * 86400;
  const hms = (s: string) => { const m = /^(\d\d):(\d\d):(\d\d)$/.exec(s); return m && +m[1] < 24 && +m[2] < 60 && +m[3] < 60 ? day + +m[1] * 3600 + +m[2] * 60 + +m[3] : NaN; };
  const num = (s: string, lo: number, hi: number) => { if (s === '-') return null; const v = Number(s); return Number.isFinite(v) && v >= lo && v <= hi ? v : null; };
  for (const ln of body.slice(at).split('\n').slice(1)) {
    if (out.length >= STORED_ROWS_MAX) break;
    const f = ln.split(DOT);
    if (f.length !== 10) continue;
    const last = hms(f[0]), first = hms(f[1]);
    const pm = /^(-?\d{1,2}\.\d+), (-?\d{1,3}\.\d+)$/.exec(f[5]);
    const id = okTxt(f[2], 24);
    if (!Number.isFinite(last) || !Number.isFinite(first) || !pm || !id || id === '-') continue;
    const lat = Number(pm[1]), lon = Number(pm[2]);
    if (Math.abs(lat) > 90 || Math.abs(lon) > 180 || (lat === 0 && lon === 0)) continue;
    if (last > nowSec + 300) continue;   // ★ a clock that ran ahead: not drawn as "heard in the future"
    const dash = (s: string) => (s === '-' ? '' : s);
    out.push({ id, name: dash(okTxt(f[3], 40)), type: dash(okTxt(f[4], 24)), cc: /^[A-Z]{2}$/.test(f[9]) ? f[9] : '',
      lat, lon, alt: num(f[6], -500, 60000), top: num(f[7], -500, 60000), mhz: num(f[8], 0.01, 100000), first: Math.min(first, last), last });
  }
  return out;
}

/** ★ The cards to read: this kind's, from the day its window starts, newest first, bounded. */
export function storedCardPick<T extends { s?: string; t: number; f: string }>(items: readonly T[] | null | undefined, kind: StoredKind, nowSec: number): T[] {
  const sp = STORED_SPEC[kind], from = Math.floor((nowSec - sp.keepSec) / 86400) * 86400;
  return (items ?? []).filter((c) => !!c && typeof c.s === 'string' && sp.slot.test(c.s) && typeof c.f === 'string' && Number(c.t) >= from)
    .sort((a, b) => b.t - a.t).slice(0, sp.cards);
}
/** ★ Every card's rows as one: each id once (the newest position wins; first heard the earliest; the highest kept), inside
 *  the kind's window, newest first, at most STORED_ROWS_MAX. */
export function storedMerge(per: readonly GalPos[][], kind: StoredKind, nowSec: number): GalPos[] {
  const by = new Map<string, GalPos>();
  for (const rows of per) for (const r of rows) {
    const had = by.get(r.id);
    if (!had) { by.set(r.id, { ...r }); continue; }
    const newer = r.last >= had.last ? r : had;
    const tops = [had.top, r.top].filter((x): x is number => x != null);
    by.set(r.id, { ...newer, first: Math.min(had.first, r.first), top: tops.length ? Math.max(...tops) : null });
  }
  const since = nowSec - STORED_SPEC[kind].keepSec;
  return [...by.values()].filter((r) => r.last >= since).sort((a, b) => b.last - a.last).slice(0, STORED_ROWS_MAX);
}
/** ★ The loader (web and app alike): `list(kind)` = the gallery's items of that kind, `text(f)` = one card's body. Null while a
 *  load runs or when the gallery did not answer (keep what is drawn). */
export function storedLoader(kind: StoredKind, o: {
  list: (kind: StoredKind) => Promise<ReadonlyArray<{ s?: string; t: number; f: string }> | null>;
  text: (f: string) => Promise<string | null>;
}): (nowSec: number) => Promise<GalPos[] | null> {
  const kept = new Map<string, GalPos[]>();
  let busy = false;
  return async (nowSec) => {
    if (busy) return null;
    busy = true;
    try {
      const items = await o.list(kind);
      if (!items) return null;
      const cards = storedCardPick(items, kind, nowSec);
      const fresh = Math.floor(nowSec / 86400) * 86400 - 86400, per: GalPos[][] = [], want = new Set<string>();   // ★ yesterday and today are fetched again
      for (const c of cards) {
        want.add(c.f);
        let rows = c.t >= fresh ? undefined : kept.get(c.f);
        if (!rows) { const b = await o.text(c.f); if (b != null) { rows = galPosParse(b, c.t, nowSec); if (c.t < fresh) kept.set(c.f, rows); } }
        if (rows) per.push(rows);
      }
      for (const k of [...kept.keys()]) if (!want.has(k)) kept.delete(k);
      return storedMerge(per, kind, nowSec);
    } catch { return null; } finally { busy = false; }
  };
}

// ── drawing (the track layer's items, the lists' rows) ─────────────────────────────────────────────────────────────────
const esc = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
const hm = (t: number) => { const d = new Date(t * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
/** "40 min" · "3 h" · "2 days" — how long ago. */
export function storedAgo(sec: number): string {
  const s = Math.max(0, Math.round(sec));
  return s < 90 ? `${s} s` : s < 5400 ? `${Math.round(s / 60)} min` : s < 172800 ? `${Math.round(s / 3600)} h` : `${Math.round(s / 86400)} days`;
}
const fadeWrap = (ic: { html: string; w: number; h: number }, op: number) => (op >= 1 ? ic : { ...ic, html: `<div style="opacity:${op}">${ic.html}</div>` });
/** The stored sonde's balloon — the live layer's shape (sondes.ts sondeIcon), in grey-blue: a remembered position. */
export const STORED_SONDE_ICON = { html: '<svg width="20" height="22" viewBox="0 0 20 22" aria-hidden="true"><ellipse cx="10" cy="8" rx="6.5" ry="7.2" fill="#9cc3d8" stroke="#000" stroke-width="0.9"/>'
  + '<path d="M10 15.2v4.3" stroke="#000" stroke-width="1"/><rect x="7.6" y="19" width="4.8" height="2.6" fill="#fff" stroke="#000" stroke-width="0.7"/></svg>', w: 20, h: 22 };
/** The stored VOR's hexagon — the VOR map's shape (VORMAP_LEGEND), its rim dimmed: a remembered station. */
export const STORED_VOR_ICON = { html: '<svg width="20" height="18" viewBox="0 0 22 20" aria-hidden="true"><path d="M6 1h10l5 9-5 9H6L1 10Z" fill="rgba(0,0,0,0.55)" stroke="#c9a24a" stroke-width="2"/>'
  + '<circle cx="11" cy="10" r="2" fill="#c9a24a"/></svg>', w: 20, h: 18 };
const SONDE_COLOUR = '#9cc3d8', VOR_COLOUR = '#c9a24a';
const mFmt = (m: number | null) => (m == null ? '' : `${Math.round(m).toLocaleString('en-GB')} m`);

/** ★ The stored sondes as the map's items: key "sds:<serial>", faded by age (fadeOf), never drawn as live. */
export function storedSondeItems(rows: readonly GalPos[], nowSec: number): TrackItem[] {
  return rows.map((r) => {
    const age = Math.max(0, nowSec - r.last);
    return { key: `sds:${r.id}`, lat: r.lat, lon: r.lon, _ic: fadeWrap(STORED_SONDE_ICON, fadeOf(age, false)), _lb: r.id,
      _cd: `<b>${esc(r.id)}${r.type ? ' · ' + esc(r.type) : ''}</b><br>Remembered by this server — last heard ${esc(hm(r.last))} UTC (${esc(storedAgo(age))} ago)`
         + `${r.alt != null ? `<br>${esc(mFmt(r.alt))}${r.top != null ? ' · highest ' + esc(mFmt(r.top)) : ''}` : ''}<br>${r.lat.toFixed(4)}, ${r.lon.toFixed(4)}`
         + `${r.mhz != null ? ' · ' + r.mhz.toFixed(3) + ' MHz' : ''}<br><small>From the gallery's radiosondes card — SONDES shows them live.</small>`,
      _aria: `Radiosonde ${r.id}, remembered, last heard ${storedAgo(age)} ago`, _rk: 0 };
  });
}
/** ★ The stored VOR stations as the map's items: key "vos:<ident/channel>", faded by age. */
export function storedVorItems(rows: readonly GalPos[], nowSec: number): TrackItem[] {
  return rows.map((r) => {
    const age = Math.max(0, nowSec - r.last), ident = r.id.split('/')[0];
    return { key: `vos:${r.id}`, lat: r.lat, lon: r.lon, _ic: fadeWrap(STORED_VOR_ICON, fadeOf(age, false)), _lb: ident,
      _cd: `<b>${esc(ident)}${r.name ? ' · ' + esc(r.name) : ''}</b><br>${r.type === 'loc' ? 'Localiser' : 'VOR'}${r.mhz != null ? ' · ' + r.mhz.toFixed(2) + ' MHz' : ''}${r.cc ? ' · ' + esc(r.cc) : ''}`
         + `<br>Remembered by this server — last heard ${esc(hm(r.last))} UTC (${esc(storedAgo(age))} ago)<br><small>From the gallery's VOR stations card — the VOR MAP shows them live.</small>`,
      _aria: `${r.type === 'loc' ? 'Localiser' : 'VOR'} ${ident}${r.name ? ' ' + r.name : ''}, remembered, last heard ${storedAgo(age)} ago`, _rk: 0 };
  });
}
const rangeOf = (rx: LatLon | null, r: GalPos) => (rx ? rangeBearing(rx, r.lat, r.lon) : null);
const nm = (v: number) => `${v < 10 ? v.toFixed(1) : Math.round(v)} NM`;
/** ★ The list's rows (TrackLiveList / the web's trkRowHtml): newest first, faded words ("remembered"). */
export function storedSondeRows(rows: readonly GalPos[], nowSec: number, rx: LatLon | null): TrackRow[] {
  return rows.slice(0, STORED_ROWS_MAX).map((r) => {
    const rb = rangeOf(rx, r);
    return { key: `sds:${r.id}`, colour: SONDE_COLOUR, flag: '', name: r.id, second: [r.type, 'remembered', r.alt != null ? mFmt(r.alt) : ''].filter(Boolean).join(' · '),
      sp: r.mhz != null ? `${r.mhz.toFixed(3)}` : '', trend: 0, range: rb ? nm(rb.nm) : '', brg: rb ? compass8(rb.brg) : '', noPos: false, age: storedAgo(nowSec - r.last), em: false };
  });
}
export function storedVorRows(rows: readonly GalPos[], nowSec: number, rx: LatLon | null): TrackRow[] {
  return rows.slice(0, STORED_ROWS_MAX).map((r) => {
    const rb = rangeOf(rx, r);
    return { key: `vos:${r.id}`, colour: VOR_COLOUR, flag: '', name: r.id.split('/')[0],
      second: [`${r.mhz != null ? r.mhz.toFixed(2) + ' ' : ''}${r.type === 'loc' ? 'LOC' : 'VOR'}`, r.name, 'remembered'].filter(Boolean).join(' · '),
      sp: '', trend: 0, range: rb ? nm(rb.nm) : '', brg: rb ? compass8(rb.brg) : '', noPos: false, age: storedAgo(nowSec - r.last), em: false };
  });
}
/** The status line's words: "3 sondes remembered (24 h)" / "12 VOR stations remembered (7 days)". */
export function storedStatus(kind: StoredKind, n: number): string {
  if (!n) return '';
  return kind === 'sondes' ? `${n} ${n === 1 ? 'sonde' : 'sondes'} remembered (24 h)` : `${n} VOR ${n === 1 ? 'station' : 'stations'} remembered (7 days)`;
}
/** ★ The KEY's rows (shown only where the stored layer is present). */
export const STORED_SONDE_LEGEND = `<div>${STORED_SONDE_ICON.html.replace('width="20" height="22"', 'width="14" height="15"')} Radiosonde remembered by this server (the gallery, 24 h) — fainter as it ages</div>`;
export const STORED_VOR_LEGEND = `<div>${STORED_VOR_ICON.html.replace('width="20" height="18"', 'width="16" height="14"')} VOR / localiser remembered by this server (the gallery, 7 days) — fainter as it ages</div>`;
