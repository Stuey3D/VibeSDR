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
  /** ★ the WHY: messages named outright, messages inside the letter's schedule slot, stations sharing the letter and
   *  band, the nearest of them to the receiver */
  outright: number; inSlot: number; share: number; near: Array<{ name: string; iso: string; km: number }>;
  /** ★ how its other messages were placed (vibe_navtex_map.h): text / series / link / run / neighbour / dx / night → count */
  ev: Record<string, number>;
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
      outright: num(s.outright) ?? 0, inSlot: num(s.inSlot) ?? 0, share: num(s.share) ?? 0,
      ev: (() => { const o: Record<string, number> = {}; if (s.ev && typeof s.ev === 'object') for (const k of ['text', 'series', 'link', 'run', 'neighbour', 'dx', 'night']) if (num(s.ev[k])) o[k] = s.ev[k]; return o; })(),
      near: Array.isArray(s.near) ? s.near.slice(0, 3).filter((x: any) => x && num(x.km) !== undefined).map((x: any) => ({ name: str(x.name, 40), iso: str(x.iso, 2), km: x.km })) : [],
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
/** ★★ LATEST WINS (Stuart, 2026-11-01): the filter picks WHICH stations, warnings and sea areas are shown, by when they were
 *  last heard; each is drawn as its latest message says — never a cancelled, replaced or expired warning, never an old
 *  forecast. The hint must not promise them under ALL. */
export const NAV_RANGE_HINT = 'The filter picks what is shown by when it was last heard; each is drawn as its latest message says. '
  + 'Cancelled, replaced and expired warnings and forecasts over a day old are not drawn (their messages stay in the gallery). '
  + 'ALL is every NAVTEX message still in the gallery — the oldest go when it is full.';
export function navRangeOf(saved: string | null | undefined): string { return NAV_RANGES.some((r) => r.id === saved) ? (saved as string) : 'all'; }
/** The query's `since` (seconds back; 0 = everything). */
export function navSince(range: string): number { return NAV_RANGES.find((r) => r.id === range)?.sec ?? 0; }

// ── how a station looks ──
/** The band's colour: 518 kHz (international, English) cyan, 490 kHz (national language) violet, 4209.5 kHz (HF) amber. */
export const NAV_BAND_COLOURS: Record<string, string> = { '518': '#4fc3f7', '490': '#c792ea', '4209.5': '#ffb74d', '424': '#9fe870' };
export function navColour(s: NavStation): string { return NAV_BAND_COLOURS[String(s.khz[0] ?? 518)] || '#4fc3f7'; }
const kmTxt = (km: number) => (km < 10 ? km.toFixed(1) : Math.round(km).toLocaleString('en-GB'));
function escN(v: string): string { return v.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }
/** ★ Fades over a week since last heard (to 0.45) — a station heard last month is still drawn, quieter. */
export function navAgeOpacity(seen: number): number {
  if (!(seen > 3600)) return 1;
  return Math.max(0.45, 1 - 0.55 * Math.min(1, (seen - 3600) / (7 * 86400)));
}
// ── ★★ CO-SITED STATIONS ARE ONE SITE (Stuart's screenshot: Niton E/K/I/T, Oostende B/V/T, Cullercoats G/U drawn on top of
//    each other). Every letter at one place — the same name within ~2 km — is ONE marker ("Niton · E K I T"), one card
//    listing each letter with its band and counts, one line from the receiver. A "?" station stays on its own.
export interface NavSite {
  key: string; name: string; iso: string; country: string; lat: number; lon: number; km?: number;
  letters: NavStation[];            // ★ most messages first
  khz: number[]; first: number; last: number; n: number; uniq: number; seen: number;
  conf: 'confident' | 'likely' | 'ambiguous';   // ★ the surest of its letters
}
export function navSites(r: NavReport | null): NavSite[] {
  const by = new Map<string, NavSite>();
  const near = (a: NavSite, s: NavStation) => Math.abs(a.lat - s.lat) < 0.5 && Math.abs(a.lon - s.lon) < 0.8;   // ★ ~50 km: one site
  for (const s of r?.stations ?? []) {
    let k = s.conf === 'ambiguous' ? `?${s.key}` : `${s.name}|${s.lat.toFixed(2)}|${s.lon.toFixed(2)}`;
    if (s.conf !== 'ambiguous') for (const [k2, g2] of by) if (!k2.startsWith('?') && g2.name === s.name && near(g2, s)) { k = k2; break; }
    let g = by.get(k);
    if (!g) {
      g = { key: k.slice(0, 40), name: s.name, iso: s.iso, country: s.country, lat: s.lat, lon: s.lon, km: s.km, letters: [], khz: [],
            first: s.first, last: s.last, n: 0, uniq: 0, seen: s.seen, conf: s.conf };
      by.set(k, g);
    }
    g.letters.push(s);
    for (const f of s.khz) if (!g.khz.includes(f)) g.khz.push(f);
    g.first = Math.min(g.first, s.first); g.last = Math.max(g.last, s.last); g.seen = Math.min(g.seen, s.seen);
    g.n += s.n; g.uniq += s.uniq;
    if (s.conf === 'confident' || (s.conf === 'likely' && g.conf === 'ambiguous')) g.conf = s.conf;
  }
  const out = [...by.values()];
  for (const g of out) { g.letters.sort((a, b) => b.n - a.n); g.khz.sort((a, b) => b - a === 0 ? 0 : (a === 518 ? -1 : b === 518 ? 1 : a - b)); }
  return out;
}
const lettersOf = (g: NavSite) => g.letters.map((s) => s.b1).join(' ');
export function navSiteColour(g: NavSite): string { return NAV_BAND_COLOURS[String(g.khz[0] ?? 518)] || '#4fc3f7'; }
/** The marker: a transmitting mast on a disc in the band's colour (split when the site sends on two bands), its letters
 *  beside it; LIKELY has a dashed rim, AMBIGUOUS a "?" badge; fades over a week since last heard. */
export function navIcon(g: NavSite): { html: string; w: number; h: number } {
  const c1 = navSiteColour(g), c2 = g.khz.length > 1 ? (NAV_BAND_COLOURS[String(g.khz[1])] || c1) : c1;
  const op = navAgeOpacity(g.seen).toFixed(2);
  const rim = g.conf === 'likely' ? ' stroke-dasharray="2.5 2"' : '';
  const disc = c1 === c2
    ? `<circle cx="13" cy="13" r="11" fill="${c1}" stroke="#0b1420" stroke-width="1.4"${rim}/>`
    : `<path d="M13 2 A11 11 0 0 0 13 24 Z" fill="${c1}"/><path d="M13 2 A11 11 0 0 1 13 24 Z" fill="${c2}"/><circle cx="13" cy="13" r="11" fill="none" stroke="#0b1420" stroke-width="1.4"${rim}/>`;
  const mast = '<path d="M13 7 L9.5 19 M13 7 L16.5 19 M10.6 15 H15.4" stroke="#0b1420" stroke-width="1.6" fill="none" stroke-linecap="round"/>'
    + '<path d="M9 6 a5 5 0 0 0 0 5 M17 6 a5 5 0 0 1 0 5" stroke="#0b1420" stroke-width="1.2" fill="none" stroke-linecap="round"/>';
  const q = g.conf === 'ambiguous' ? '<circle cx="22" cy="22" r="5.2" fill="#ffd34d" stroke="#0b1420" stroke-width="1"/><text x="22" y="25" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="8" font-weight="700" fill="#111">?</text>' : '';
  return { html: `<svg width="28" height="28" viewBox="0 0 28 28" style="opacity:${op}" aria-hidden="true">${disc}${mast}${q}</svg>`, w: 28, h: 28 };
}
/** ★★ Stuart's words (2026-11-01): confident → the plain name; likely → "Suspected <name>"; ambiguous → "<name>?" with
 *  its candidates in the card. */
export function navName(name: string, conf: string): string { return conf === 'likely' ? `Suspected ${name}` : conf === 'ambiguous' ? `${name}?` : name; }
/** "Niton · E K I T · 195 km" */
export function navLabel(g: NavSite): string {
  return `${navName(g.name, g.conf)} · ${lettersOf(g)}${g.km !== undefined ? ` · ${kmTxt(g.km)} km` : ''}`;
}
/** Who keeps its name when labels collide: the most different messages, then the most recent. */
export function navRank(g: NavSite): number { return g.uniq * 1e6 - Math.min(g.seen, 999999); }
const CONF_WORDS: Record<string, string> = {
  confident: 'named by its messages',
  likely: 'suspected — the station label could not decide; the evidence below points here',
  ambiguous: 'not certain — the letter and frequency fit more than one station',
};
const utc = (t: number) => { const d = new Date(t * 1000); const p = (n: number) => String(n).padStart(2, '0'); return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())} ${p(d.getUTCHours())}:${p(d.getUTCMinutes())}`; };
export function navAgo(sec: number): string {
  if (!(sec >= 60)) return 'just now';
  if (sec < 3600) return `${Math.round(sec / 60)} min ago`;
  if (sec < 86400) return `${Math.round(sec / 3600)} h ago`;
  return `${Math.round(sec / 86400)} days ago`;
}
/** The schedule slot of a letter: "04:50" (the first of its six a day). */
export function navSlot(b1: string): string {
  const i = b1.charCodeAt(0) - 65;
  if (!(i >= 0 && i < 24)) return '';
  const m = i * 10;
  return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
}
const pct = (a: number, b: number) => (b > 0 ? Math.round((100 * a) / b) : 0);
/** ★★ The WHY of one letter (Stuart's request: the reasons behind confident / likely / ambiguous): how it was named, how
 *  many stations share the letter and band and which are nearest, the schedule slot, the night path; for a "?" the
 *  candidates with shares from distance alone (the resolver's own prior, 1 / (1 + (d / 800 km)²)). */
export function navWhy(s: NavStation): string[] {
  // ★★ The evidence, strongest first, in plain words; counts of its messages — never an internal score. Each line says
  //    what the MESSAGES showed (no language is claimed for a station: a French authority sends English on 518).
  const out: string[] = [];
  const band = s.khz[0];
  const of = (k: number) => `${k} of ${s.n} ${s.n === 1 ? 'message' : 'messages'}`;
  if (s.outright) out.push(`Named by the message itself or its station label: ${of(s.outright)}`);
  if (s.ev.series) out.push(`Its warning service is named in the text (e.g. AVURNAV BREST, ESTONIAN NAV WARN): ${of(s.ev.series)}`);
  if (s.ev.link) out.push(`The same warning number as a message from this station on another band: ${of(s.ev.link)}`);
  if (s.ev.run) out.push(`Part of the same broadcast run as a message naming it: ${of(s.ev.run)}`);
  if (s.ev.neighbour) out.push(`Its header was lost; sent straight after a message naming this station: ${of(s.ev.neighbour)}`);
  if (s.ev.text) out.push(`Positions, names or language in its own text point here: ${of(s.ev.text)}`);
  if (s.folded) out.push(`Settled because only ${s.name} has been heard named on letter ${s.b1}: ${of(s.folded)}`);
  if (s.ev.dx) out.push(`The only station on letter ${s.b1}, ${band} kHz, within night-time reach: ${of(s.ev.dx)}`);
  if (s.ev.night) out.push(`The other candidate was out of reach in daylight: ${of(s.ev.night)}`);
  if (s.n) out.push(`Ended inside ${s.b1}’s schedule slot (${navSlot(s.b1)} UTC, every 4 h): ${of(s.inSlot)}`);
  if (s.near.length) out.push(`Nearest ${s.b1} station on ${band} kHz: ${s.near[0].name}, ${kmTxt(s.near[0].km)} km`
    + (s.near[1] ? ` (next ${s.near[1].name}, ${kmTxt(s.near[1].km)} km)` : '') + (s.share > 1 ? ` — ${s.share} stations share this letter` : ''));
  if (s.night) out.push(`Heard with the path in darkness: ${of(s.night)}`);
  if (s.cands && s.cands.length > 1) out.push('Could be ' + s.cands.map((c) => `${c.name}${c.km !== undefined ? ` (${kmTxt(c.km)} km)` : ''}`).join(' or '));
  return out;
}
/** ★★ The card (Stuart, 2026-11-01: tidied — it was a wall of text). THE ESSENTIALS FIRST: the name, its letters and
 *  bands, the distance, last heard, how many messages, and the top three reasons it is placed here. EVERYTHING ELSE —
 *  first heard, each letter's own counts, kinds, newest ids and full why — under a "More" disclosure (a <details>, so it
 *  works the same in the web page and the app's map). */
export function navCard(g: NavSite, wrap: 'web' | 'app' = 'web'): string {
  const lines: string[] = [];
  const bands = g.khz.map((k) => `${k}`).join(' + ');
  lines.push(`<div class="t"><b>${escN(navName(g.name, g.conf))}</b>${g.country ? `, ${escN(g.country)}` : ''}</div>`);
  lines.push(`<div>${escN(lettersOf(g))} · ${escN(bands)} kHz${g.km !== undefined ? ` · ${escN(kmTxt(g.km))} km` : ''}</div>`);
  lines.push(`<div>Last heard ${navAgo(g.seen)} · ${g.n} ${g.n === 1 ? 'message' : 'messages'}${g.uniq !== g.n ? ` (${g.uniq} different)` : ''}</div>`);
  // ★ the top reasons — the busiest letter's strongest evidence (navWhy is strongest first)
  const top = g.letters[0] ? navWhy(g.letters[0]).slice(0, 3) : [];
  if (top.length) lines.push(`<div class="why" style="margin-top:4px">${top.map((w) => `· ${escN(w)}`).join('<br>')}</div>`);
  const more: string[] = [];
  more.push(`First heard ${utc(g.first)} UTC · last heard ${utc(g.last)} UTC`);
  for (const s of g.letters) {
    const kinds = Object.entries(s.subj).sort((a, b) => b[1] - a[1]).map(([k, n]) => `${navtexSubject(k) ?? k} ${n}`);
    more.push(`<b>${escN(s.b1)}</b> · ${s.khz.map((k) => `${k} kHz`).join(' + ')} · ${s.n} received, ${s.uniq} different`
      + `${s.conf !== 'confident' ? ` · ${escN(CONF_WORDS[s.conf])}` : ''}`);
    for (const w of navWhy(s)) more.push('&nbsp;&nbsp;· ' + escN(w));
    if (kinds.length) more.push('&nbsp;&nbsp;' + escN(kinds.join(' · ')));
    // ★ each id once (a warning is repeated every slot until cancelled): its newest reception
    const seenIds = new Set<string>();
    const fresh = s.recent.filter((r) => (seenIds.has(r.id) ? false : (seenIds.add(r.id), true)));
    if (fresh.length) more.push('&nbsp;&nbsp;Newest: ' + fresh.slice(0, 6).map((r) => `${escN(r.id)} ${utc(r.t).slice(5)}`).join(' · '));
  }
  lines.push(`<details class="navMore" style="margin-top:6px"><summary style="cursor:pointer;font-weight:700">More</summary><div style="margin-top:4px">${more.join('<br>')}</div></details>`);
  const body = lines.join('');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="pop">${body}</div>`;
}
/** The list (the web's TrackView rows / the app's TrackLiveList rows): nearest first, one row a site. */
export function navRows(r: NavReport): Array<{ g: NavSite; second: string; range: string }> {
  return navSites(r).sort((a, b) => (a.km ?? 1e9) - (b.km ?? 1e9)).map((g) => ({
    g, range: g.km !== undefined ? `${kmTxt(g.km)} km` : '',
    second: [`${lettersOf(g)} · ${g.khz.join('+')} kHz`, `${g.uniq} msg${g.uniq === 1 ? '' : 's'}`, g.conf === 'confident' ? '' : g.conf].filter(Boolean).join(' · '),
  }));
}
export function navStatus(r: NavReport | null): string {
  if (!r) return 'Reading the gallery…';
  const sites = navSites(r);
  const far = sites.reduce<NavSite | null>((b, s) => (s.km !== undefined && (!b || s.km > (b.km ?? 0)) ? s : b), null);
  return `${sites.length} ${sites.length === 1 ? 'site' : 'sites'} · ${r.stations.length} ${r.stations.length === 1 ? 'station' : 'stations'} · ${r.messages} messages`
    + (far ? ` · furthest ${far.name} ${kmTxt(far.km!)} km` : '') + (r.unplaced ? ` · ${r.unplaced} not placed` : '');
}
export const NAV_EMPTY = 'No NAVTEX message in the gallery for this time — the map fills as messages arrive (a listener’s decoder or background decoding).';

/** ★ The app's pins and rows (MapOverlay kind 'navtex' / TrackLiveList) — one a site. */
export function navItems(r: NavReport | null, wrap: 'web' | 'app' = 'app') {
  return navSites(r).map((g) => ({ key: g.key, lat: g.lat, lon: g.lon, _ic: navIcon(g), _lb: navLabel(g), _cd: navCard(g, wrap), _h: g.n,
                                   // ★ stations outrank the warnings and the met labels when names collide
                                   _rk: 1e12 + navRank(g), _arc: true,
                                   _aria: `Station ${navLabel(g)}, ${g.n} messages, last heard ${navAgo(g.seen)}. Press Enter for its card.` }));
}
const ageShort = (sec: number) => (sec < 3600 ? `${Math.round(sec / 60)} min` : sec < 86400 ? `${Math.round(sec / 3600)} h` : `${Math.round(sec / 86400)} d`);
export function navListRows(r: NavReport | null) {
  if (!r) return [];
  return navRows(r).map(({ g, second, range }) => ({
    key: g.key, colour: navSiteColour(g), flag: '', name: navName(g.name, g.conf), second, sp: '', trend: 0 as const,
    range, brg: '', noPos: false, age: ageShort(g.seen), em: false,
  }));
}
export function navRowAge(sec: number): string { return ageShort(sec); }
