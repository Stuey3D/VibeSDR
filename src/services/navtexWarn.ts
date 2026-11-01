// navtexWarn.ts — V12 2026-11-01 THE NAVTEX MAP, stages 2 and 3: the WARNINGS and the MET LABELS on the stations map
// (src/services/navtexMap.ts). The server reads them out of the gallery (android/app/src/main/cpp/vibe_navtex_warn.h,
// vibe_navtex_met.h; GET /vibeserver/gallery/navtex-warnings.json and navtex-met.json); this is what BOTH clients draw
// from them — the web client (web/client/src/main.ts, TrackView) and the app (MapOverlay kind 'navtex'). ★ ONE RULE, TWO
// READERS (scripts/test_navtex_map.ts). ★ PURE: no React Native, no DOM.
//
// ★★ The card's one-line summary is OUR wording of the class the server's lexicon matched — "Unlit light (from French:
//    FEU ETEINT)" — never presented as a translation (the full text below it has the gallery's own TRANSLATE).
// ★ Everything in a report came off the air or the gallery: every string is escaped where it meets HTML.
import { navName, navAgo } from './navtexMap';
import { NAV_HAZARD_COLOURS, navHazardMarker, navGlyphSvg, type NavHazard } from './navtexIcons';

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const str = (v: unknown, n: number): string => (typeof v === 'string' ? v.slice(0, n) : '');
const okLat = (v: unknown) => num(v) !== undefined && Math.abs(v as number) <= 90;
const okLon = (v: unknown) => num(v) !== undefined && Math.abs(v as number) <= 180;
function esc(v: string): string { return String(v).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }
const utc = (t: number) => { const d = new Date(t * 1000); const p = (n: number) => String(n).padStart(2, '0'); return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())} ${p(d.getUTCHours())}:${p(d.getUTCMinutes())}`; };
const CLASSES = ['wreck', 'navaid', 'buoy', 'structure', 'firing', 'uxo', 'cable', 'survey', 'obstruction', 'adrift', 'works', 'towing', 'sar', 'ice', 'general'];

// ── the warnings ─────────────────────────────────────────────────────────────────────────────────────────────────────
export interface NavWarn {
  key: string; id: string; khz: number; first: number; last: number; n: number; lost: number; unlit: boolean;
  expired: boolean; cancelled: boolean; cancelledBy: string; cancelAt: number; validTo: number; ufn: boolean;
  cls: string; f: string; series: string; hit: string; lang: string; pts: Array<[number, number]>; shape: '' | 'area' | 'route';
  issued: number; st: string; sc: string; iso: string; q: string;
  /** the marker's point: an area's centre, a route's middle point, else the first position */
  lat: number; lon: number;
}
export interface NavWarnReport { now: number; total: number; shown: number; receptions: number; warnings: number; live: number; indexed: number; pending: number; list: NavWarn[] }

/** A report from the wire, cut to its fields. */
export function navWarnSanitise(j: any): NavWarnReport | null {
  if (!j || typeof j !== 'object' || !Array.isArray(j.warnings)) return null;
  const list: NavWarn[] = [];
  for (const w of j.warnings.slice(0, 1000)) {
    if (!w || typeof w !== 'object' || !Array.isArray(w.pts)) continue;
    const pts = w.pts.slice(0, 24).filter((p: any) => Array.isArray(p) && okLat(p[0]) && okLon(p[1])).map((p: any) => [p[0], p[1]] as [number, number]);
    if (!pts.length) continue;
    const id = /^[A-Z]{2}\d{2}$/.test(w.id) ? w.id : '';
    if (!id) continue;
    const shape = w.shape === 'area' && pts.length >= 3 ? 'area' : w.shape === 'route' && pts.length >= 2 ? 'route' : '';
    const at = navWarnPoint(pts, shape);
    const first = num(w.first) ?? 0;
    list.push({
      key: `w:${id}:${first}`, id, khz: num(w.khz) ?? 0, first, last: num(w.last) ?? first, n: num(w.n) ?? 1, lost: Math.max(0, num(w.lost) ?? 0),
      unlit: w.unlit === 1 || w.unlit === true, expired: w.expired === 1 || w.expired === true, cancelled: w.cancelled === 1 || !!w.cancelledBy,
      cancelledBy: /^[A-Z]{2}\d{2}$/.test(w.cancelledBy) ? w.cancelledBy : '', cancelAt: num(w.cancelAt) ?? 0, validTo: num(w.validTo) ?? 0, ufn: !!w.ufn,
      cls: CLASSES.includes(w.cls) ? w.cls : 'general', f: str(w.f, 80), series: str(w.series, 40), hit: str(w.hit, 40), lang: str(w.lang, 3),
      pts, shape, issued: num(w.issued) ?? 0, st: str(w.st, 40), sc: ['confident', 'likely', 'ambiguous'].includes(w.sc) ? w.sc : '', iso: /^[A-Z]{2}$/.test(w.iso) ? w.iso : '',
      q: str(w.q, 80), lat: at[0], lon: at[1],
    });
  }
  return { now: num(j.now) ?? Math.floor(Date.now() / 1000), total: num(j.total) ?? list.length, shown: num(j.shown) ?? list.length, receptions: num(j.receptions) ?? 0,
           warnings: num(j.warnings) ?? 0, live: num(j.live) ?? 0, indexed: num(j.indexed) ?? 0, pending: num(j.pending) ?? 0, list };
}
/** Where a warning's marker sits: an area's centre (the mean of its corners), a route's middle point, else its first position. */
export function navWarnPoint(pts: Array<[number, number]>, shape: string): [number, number] {
  if (shape === 'area') return [pts.reduce((a, p) => a + p[0], 0) / pts.length, pts.reduce((a, p) => a + p[1], 0) / pts.length];
  if (shape === 'route') return pts[Math.floor((pts.length - 1) / 2)];
  return pts[0];
}
/** The icon a warning draws. */
export function navWarnHazard(w: Pick<NavWarn, 'cls'>): NavHazard {
  return w.cls === 'navaid' ? 'light' : (CLASSES.includes(w.cls) ? w.cls : 'general') as NavHazard;
}
const HAZ_WORDS: Record<string, string> = {
  wreck: 'Wreck', navaid: 'Light or navigation aid', buoy: 'Buoy or beacon', structure: 'Wind turbine or platform', firing: 'Firing or military exercise',
  uxo: 'Mine or unexploded ordnance', cable: 'Cable or pipeline', survey: 'Survey work', obstruction: 'Obstruction, shoal or anchoring',
  adrift: 'Adrift — a container, debris or a derelict', works: 'Works, dredging or diving', towing: 'A tow', sar: 'Search and rescue', ice: 'Ice', general: 'Navigational warning',
};
const UNLIT_WORDS: Record<string, string> = { navaid: 'Unlit light', buoy: 'Buoy, its light unlit', structure: 'Wind turbine or platform, its light out' };
export const NAV_LANGS: Record<string, string> = { fr: 'French', es: 'Spanish', it: 'Italian', de: 'German', nl: 'Dutch', hr: 'Croatian', el: 'Greek', pt: 'Portuguese', et: 'Estonian', pl: 'Polish' };
/** ★★ The one-line summary: our words for the class, and the message's own word when it was not English — "Unlit light
 *  (from French: FEU ETEINT)". Never called a translation. */
export function navWarnSummary(w: Pick<NavWarn, 'cls' | 'unlit' | 'hit' | 'lang'>): string {
  const words = (w.unlit && UNLIT_WORDS[w.cls]) || (w.unlit ? `${HAZ_WORDS[w.cls] ?? HAZ_WORDS.general} — a light out` : HAZ_WORDS[w.cls] ?? HAZ_WORDS.general);
  return w.hit && NAV_LANGS[w.lang] ? `${words} (from ${NAV_LANGS[w.lang]}: ${w.hit})` : words;
}
export function navWarnFaded(w: NavWarn): boolean { return w.expired || w.cancelled; }
/** "expired" / "cancelled" / "" — the tag on a faded marker. */
export function navWarnTag(w: NavWarn): string { return w.cancelled ? 'cancelled' : w.expired ? 'expired' : ''; }
/** ★ The time filter: ALL shows the expired and the cancelled, faded; a shorter range only what still stands. */
export function navWarnVisible(r: NavWarnReport | null, range: string): NavWarn[] {
  if (!r) return [];
  return range === 'all' ? r.list : r.list.filter((w) => !navWarnFaded(w));
}
export function navWarnLabel(w: NavWarn): string {
  const short = navWarnSummary({ ...w, hit: '', lang: '' }).split(' — ')[0].split(',')[0];
  const tag = navWarnTag(w);
  return `${w.id} · ${short}${tag ? ` · ${tag}` : ''}`;
}
/** The words a screen reader says for a marker. */
export function navWarnAria(w: NavWarn): string {
  const tag = navWarnTag(w);
  return `Warning ${w.id}: ${navWarnSummary(w)}${tag ? `, ${tag}` : ''}${w.st ? `, from ${navName(w.st, w.sc)}` : ''}. Press Enter for its card.`;
}
/** ★★ The WHY of one warning, from THIS message only: the word that chose its icon (its language only when this message
 *  was in that language), what its points are, and why it is faded. */
export function navWarnWhy(w: NavWarn, now: number): string[] {
  const out: string[] = [];
  if (w.hit && NAV_LANGS[w.lang]) out.push(`Its icon: this message says ${w.hit} (${NAV_LANGS[w.lang]})`);
  else if (w.cls !== 'general') out.push(`Its icon: this message's English wording names ${HAZ_WORDS[w.cls].toLowerCase()}${w.unlit ? ', with a light not working' : ''}`);
  else out.push('Its icon: no hazard word this map knows — the general warning');
  out.push(w.shape === 'area' ? `Drawn as an area: the text names an area through its ${w.pts.length} points`
    : w.shape === 'route' ? `Drawn as a line: the text names a route or line through its ${w.pts.length} points`
    : w.pts.length > 1 ? `${w.pts.length} positions, each its own point (the text names no area or route)` : 'One position');
  if (w.cancelledBy) out.push(`Cancelled by ${w.cancelledBy}`);
  else if (w.cancelled && w.cancelAt) out.push(`Cancelled at ${utc(w.cancelAt)} UTC, as it said it would be`);
  else if (w.expired) out.push(w.validTo ? `Expired: its own window ended ${utc(w.validTo)} UTC`
    : w.ufn ? 'Expired: "until further notice", but not heard for 30 days' : `Expired: not heard for ${Math.round((now - w.last) / 3600)} h (72 h without a repeat)`);
  else if (w.validTo) out.push(`Valid until ${utc(w.validTo)} UTC (its own words)`);
  else if (w.ufn) out.push('Until further notice (its own words)');
  return out;
}
const flagOf = (iso: string) => (/^[A-Z]{2}$/.test(iso) ? String.fromCodePoint(...[...iso].map((c) => 0x1f1e6 + c.charCodeAt(0) - 65)) : '');
/** A title the gallery's TRANSLATE reads its language hint from (the station's flag) — "EA45 · Niton 🇬🇧". */
export function navWarnTitle(w: NavWarn): string { return `${w.id} · ${w.st || 'NAVTEX'} ${flagOf(w.iso)}`.trim(); }
/** ★ The card. The full gallery text is fetched when it opens, into `.nvTx` (data-f = the gallery file), with the
 *  gallery's own TRANSLATE in `.nvTr` (shown only where it can translate) and its note in `.nvTrNote`. */
export function navWarnCard(w: NavWarn, now: number, wrap: 'web' | 'app' = 'web'): string {
  const h = navWarnHazard(w), faded = navWarnFaded(w);
  const c = faded ? '#9aa6b2' : NAV_HAZARD_COLOURS[h];
  const rows: Array<[string, string]> = [];
  rows.push(['Warning', [w.id, w.series, w.khz ? `${w.khz} kHz` : ''].filter(Boolean).join(' · ')]);
  if (w.issued) rows.push(['Issued', `${utc(w.issued)} UTC (its own date-time group)`]);
  rows.push(['First heard', `${utc(w.first)} UTC`]);
  rows.push(['Last heard', `${utc(w.last)} UTC (${navAgo(Math.max(0, now - w.last))})`]);
  rows.push(['Heard', `${w.n} ${w.n === 1 ? 'time' : 'times'} here`]);
  rows.push(['Station', w.st ? `${navName(w.st, w.sc)}${w.iso ? ` ${flagOf(w.iso)}` : ''}` : 'not placed']);
  rows.push(['Best copy', w.q || (w.lost ? `${w.lost} characters lost` : 'no errors')]);
  const tag = navWarnTag(w);
  const lines: string[] = [];
  lines.push(`<div class="t" style="display:flex;gap:8px;align-items:center">${navGlyphSvg(h, 20, c, w.unlit)}<b>${esc(navWarnSummary(w))}</b></div>`);
  if (tag) lines.push(`<div style="color:#ffb37a;font-weight:700;letter-spacing:.04em">${esc(tag.toUpperCase())}</div>`);
  for (const [k, v] of rows) lines.push(`<div class="r"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`);
  for (const y of navWarnWhy(w, now)) lines.push(`<div class="r"><span class="k">Why</span><span class="v">${esc(y)}</span></div>`);
  lines.push(navTextBlock(w.f, navWarnTitle(w)));
  const body = lines.join('');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:330px">${body}</div></div>` : `<div class="trkCard">${body}</div>`;
}
/** The full-text slot both cards share (the warning's message, the forecast's message). */
export function navTextBlock(file: string, title: string): string {
  return `<div class="nvTxWrap" style="margin-top:6px"><div style="display:flex;gap:6px;align-items:center;margin-bottom:4px">`
    + `<span class="k" style="flex:1">The message</span><button type="button" class="nvTr" hidden data-f="${esc(file)}" data-title="${esc(title)}">TRANSLATE</button></div>`
    + `<pre class="nvTx raw" data-f="${esc(file)}" style="white-space:pre-wrap;max-height:220px;overflow:auto;margin:0">Loading the message…</pre>`
    + `<div class="nvTrNote" hidden style="color:#ffd34d;font-size:11px;margin-top:3px"></div></div>`;
}
/** A warning on the map: its icon (faded under ALL when expired or cancelled), its shape, clustered when zoomed out. */
export function navWarnItems(list: NavWarn[], now: number, wrap: 'web' | 'app' = 'web') {
  return list.map((w) => {
    const h = navWarnHazard(w), faded = navWarnFaded(w);
    return {
      key: w.key, lat: w.lat, lon: w.lon, _ic: navHazardMarker(h, w.unlit, faded), _lb: navWarnLabel(w), _cd: navWarnCard(w, now, wrap),
      _h: w.n, _rk: (faded ? 0 : 1e5) + Math.min(w.last, 4e9) / 1e5, _cl: 'w', _aria: navWarnAria(w),
      _sh: w.shape || w.pts.length > 1 ? { kind: w.shape || 'points', pts: w.pts, colour: faded ? '#9aa6b2' : NAV_HAZARD_COLOURS[h], faded } : undefined,
      _f: w.f,
    };
  });
}
/** "Building the map from 4 515 saved messages… 42 %" while the server's backfill runs. */
export function navBuilding(r: { live: number; indexed: number; pending: number } | null): string {
  if (!r || !(r.pending > 0)) return '';
  const pct = r.live > 0 ? Math.min(99, Math.floor((100 * r.indexed) / r.live)) : 0;
  return `Building the map from ${r.live.toLocaleString('en-GB')} saved messages… ${pct} %`;
}
export function navWarnStatus(r: NavWarnReport | null, range: string): string {
  if (!r) return '';
  const vis = navWarnVisible(r, range);
  const cur = vis.filter((w) => !navWarnFaded(w)).length;
  return `${vis.length} ${vis.length === 1 ? 'warning' : 'warnings'}${range === 'all' && cur !== vis.length ? ` (${cur} current)` : ''}`
    + (r.total > r.shown ? ` · the newest ${r.shown.toLocaleString('en-GB')} of ${r.total.toLocaleString('en-GB')}` : '');
}

// ── the met labels ───────────────────────────────────────────────────────────────────────────────────────────────────
export interface NavFc { t: number; dir: string; fmin: number; fmax: number; gale: number; native: boolean; sea: number; vis: number; wx: string; f: string; txt: string; st: string; sc: string }
export interface NavArea { key: string; id: string; name: string; lat: number; lon: number; kind: string; repeats: number; fc: NavFc[]; stale: boolean }
export interface NavMetReport { now: number; stale: number; pending: number; indexed: number; areas: NavArea[] }
export function navMetSanitise(j: any): NavMetReport | null {
  if (!j || typeof j !== 'object' || !Array.isArray(j.areas)) return null;
  const now = num(j.now) ?? Math.floor(Date.now() / 1000), staleSec = num(j.stale) ?? 86400;
  const areas: NavArea[] = [];
  for (const a of j.areas.slice(0, 200)) {
    if (!a || !okLat(a.lat) || !okLon(a.lon) || !Array.isArray(a.fc) || !a.fc.length) continue;
    const id = /^[a-z0-9-]{2,40}$/.test(a.id) ? a.id : '';
    if (!id) continue;
    const fc = a.fc.slice(0, 7).filter((f: any) => f && num(f.t) !== undefined).map((f: any): NavFc => ({
      t: f.t, dir: /^[A-Z]{1,3}$/.test(f.dir) ? f.dir : '', fmin: num(f.fmin) ?? -1, fmax: num(f.fmax) ?? -1, gale: num(f.gale) ?? 0, native: !!f.native,
      sea: num(f.sea) ?? -1, vis: num(f.vis) ?? -1, wx: str(f.wx, 60), f: str(f.f, 80), txt: str(f.txt, 500), st: str(f.st, 40),
      sc: ['confident', 'likely', 'ambiguous'].includes(f.sc) ? f.sc : '',
    }));
    if (!fc.length) continue;
    areas.push({ key: `m:${id}`, id, name: str(a.name, 50), lat: a.lat, lon: a.lon, kind: str(a.kind, 1), repeats: num(a.repeats) ?? 0, fc, stale: now - fc[0].t > staleSec });
  }
  return { now, stale: staleSec, pending: num(j.pending) ?? 0, indexed: num(j.indexed) ?? 0, areas };
}
const SEA = ['calm', 'calm', 'smooth', 'slight', 'moderate', 'rough', 'very rough', 'high', 'very high', 'phenomenal'];
const VIS = ['fog', 'very poor', 'poor', 'moderate', 'good'];
const GALE: Record<number, string> = { 8: 'GALES', 9: 'SEVERE GALES', 10: 'STORM', 11: 'VIOLENT STORM', 12: 'HURRICANE FORCE' };
/** "SW 6–8" */
export function navFcWind(f: NavFc): string {
  const force = f.fmin < 0 ? '' : f.fmax > f.fmin ? `${f.fmin}–${f.fmax}` : String(f.fmin);
  return [f.dir, force].filter(Boolean).join(' ');
}
/** ★ THE LABEL: "SW 6–8 · GALES · ROUGH" (· FOG / POOR VIS when the visibility is poor or worse). A native-language gale
 *  word gives "GALE?" only — its force was not read. */
export function navFcLabel(f: NavFc): string {
  const g = f.native ? 'GALE WORD' : f.gale >= 8 ? GALE[Math.min(12, f.gale)] : '';
  const sea = f.sea >= 0 ? SEA[f.sea].toUpperCase() : '';
  const vis = f.vis === 0 ? 'FOG' : f.vis >= 0 && f.vis <= 2 ? `${VIS[f.vis].toUpperCase()} VIS` : '';
  return [navFcWind(f), g, sea, vis].filter(Boolean).join(' · ') || 'forecast';
}
export function navFcGale(f: NavFc): boolean { return f.gale >= 8; }
export function navMetVisible(r: NavMetReport | null, range: string): NavArea[] {
  if (!r) return [];
  return range === 'all' ? r.areas : r.areas.filter((a) => !a.stale);
}
/** The label as a marker: a small pill — the area's name over its forecast, gale or worse in the warning colour; an old
 *  forecast (over a day) faded. */
export function navMetIcon(a: NavArea): { html: string; w: number; h: number } {
  const f = a.fc[0], lab = navFcLabel(f), gale = navFcGale(f);
  const w = Math.round(Math.max(a.name.length * 6.0, lab.length * 6.4) + 14), h = 30;
  const bg = gale ? '#5a1414' : 'rgba(12,22,34,.88)', bd = gale ? '#ff6b5a' : '#5d8fb8', fg = gale ? '#ffd2c8' : '#cfe6ff';
  return {
    html: `<div style="width:${w}px;height:${h}px;box-sizing:border-box;border:1px solid ${bd};border-radius:6px;background:${bg};opacity:${a.stale ? 0.45 : 1};`
      + `font:600 10px/12px ui-monospace,Menlo,Consolas,monospace;color:${fg};padding:2px 6px;white-space:nowrap;overflow:hidden;text-align:center">`
      + `<div style="font-weight:500;opacity:.75;font-size:9px">${esc(a.name.toUpperCase())}${a.stale ? ' · OLD' : ''}</div>${esc(lab)}</div>`,
    w, h,
  };
}
export function navMetAria(a: NavArea): string {
  const f = a.fc[0];
  return `Sea area ${a.name}: ${navFcLabel(f)}, heard ${utc(f.t)} UTC${a.stale ? ', more than a day old' : ''}. Press Enter for the forecast.`;
}
export function navMetCard(a: NavArea, now: number, wrap: 'web' | 'app' = 'web'): string {
  const f = a.fc[0];
  const lines: string[] = [];
  lines.push(`<div class="t"><b>${esc(a.name)}</b> · ${esc(a.kind === 'i' ? 'inshore waters' : a.kind === 'n' ? 'sea area' : 'shipping forecast area')}</div>`);
  if (a.stale) lines.push(`<div style="color:#ffb37a;font-weight:700">OVER A DAY OLD — no newer forecast heard</div>`);
  const row = (k: string, v: string) => lines.push(`<div class="r"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`);
  row('Forecast', navFcLabel(f));
  if (f.fmin >= 0 || f.dir) row('Wind', `${navFcWind(f)}${f.gale >= 8 && !f.native ? ` — ${GALE[Math.min(12, f.gale)].toLowerCase()}` : ''}`);
  if (f.native) row('Gale', 'a gale word in another language — its force was not read');
  if (f.sea >= 0) row('Sea', SEA[f.sea]);
  if (f.wx) row('Weather', f.wx);
  if (f.vis >= 0) row('Visibility', VIS[f.vis]);
  row('Heard', `${utc(f.t)} UTC (${navAgo(Math.max(0, now - f.t))})${a.repeats ? ` · and ${a.repeats} repeat${a.repeats === 1 ? '' : 's'}` : ''}`);
  row('Station', f.st ? navName(f.st, f.sc) : 'not placed');
  lines.push(`<div class="raw" style="white-space:pre-wrap">${esc(f.txt)}</div>`);
  if (a.fc.length > 1) {
    lines.push('<div class="k" style="margin-top:6px">Earlier forecasts</div>');
    for (const h of a.fc.slice(1)) lines.push(`<div class="r"><span class="k">${esc(utc(h.t).slice(5))}</span><span class="v">${esc(navFcLabel(h))}${h.st ? ` · ${esc(navName(h.st, h.sc))}` : ''}</span></div>`);
  }
  lines.push(navTextBlock(f.f, `${a.name} · ${f.st || 'NAVTEX'}`));
  const body = lines.join('');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:330px">${body}</div></div>` : `<div class="trkCard">${body}</div>`;
}
export function navMetItems(areas: NavArea[], now: number, wrap: 'web' | 'app' = 'web') {
  return areas.map((a) => ({
    key: a.key, lat: a.lat, lon: a.lon, _ic: navMetIcon(a), _lb: '', _cd: navMetCard(a, now, wrap), _h: a.repeats + a.fc.length,
    // ★ overlapping labels gather (any zoom): a gale first, then the newest, then the open sea before the inshore stretches
    _rk: (navFcGale(a.fc[0]) ? 4e9 : 2e9) - (a.stale ? 1e9 : 0) + a.fc[0].t / 10 + (a.kind === 's' ? 1e8 : 0), _cl: 'm',
    _aria: navMetAria(a), _f: a.fc[0].f, _t: a.fc[0].t,
  }));
}
export function navMetStatus(r: NavMetReport | null, range: string): string {
  if (!r) return '';
  const v = navMetVisible(r, range), g = v.filter((a) => navFcGale(a.fc[0]) && !a.stale).length;
  return `${v.length} sea ${v.length === 1 ? 'area' : 'areas'}${g ? ` (${g} gale or worse)` : ''}`;
}

// ── the layer toggles ───────────────────────────────────────────────────────────────────────────────────────────────
export const NAV_LAYERS = [
  { id: 'stations', label: 'STATIONS', words: 'Show the stations received' },
  { id: 'warnings', label: 'WARNINGS', words: 'Show the navigational warnings' },
  { id: 'weather', label: 'WEATHER', words: 'Show the latest forecast for each sea area' },
];
export type NavLayers = Record<'stations' | 'warnings' | 'weather', boolean>;
export function navLayersOf(saved: string | null | undefined): NavLayers {
  const d: NavLayers = { stations: true, warnings: true, weather: true };
  try { const j = saved ? JSON.parse(saved) : null; if (j && typeof j === 'object') for (const k of Object.keys(d) as Array<keyof NavLayers>) if (typeof j[k] === 'boolean') d[k] = j[k]; } catch { /* default */ }
  return d;
}
