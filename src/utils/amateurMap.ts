/**
 * amateurMap.ts — ★★★ V12 beta96 STAGE 7 (docs/v12/ONE-MAP.md): THE AMATEUR MAP, grown from the DIGI MODES MAP — digital
 * spots (FT8/FT4/WSPR/JS8 …, CW and RTTY skimmer, PSK), APRS, MeshCore / Meshtastic nodes (MESH) and the NCDXF / IARU
 * beacons (BEACONS), with tabs SPOTS / APRS / MESH / BEACONS.
 *
 * ★★★ STUART'S RULE (11-26, via the coordinator): the map shows ANY station heard in the HEARD IN window — background
 *     decoding's and every listener's decoders' alike — not only what the current mode decodes. APRS, MESH and BEACONS
 *     follow the SAME window as the spots. The live mode's layer is only EMPHASISED (its key first, with the red dot).
 *     The station store already holds both kinds of spot (vibe_station_store.h origin kBackground = 1 / kLive = 2, `src`
 *     in stations.json); this module holds the window rule every layer obeys, the live layer, and the shapes of the
 *     MESH and BEACONS points.
 * ★★ ONE RULE, THREE READERS: the web's map window (main.ts spotsMapHtml), the app's map page (MapOverlay kind 'digi') and
 *    the tests (scripts/test_amateur_map.ts) — the TS functions here, and AMATEUR_JS, the same rules as page text
 *    (plain ES5, no backticks, no backslashes: it sits inside both pages' template literals).
 */

/** ★ The HEARD IN window in seconds (0 = everything kept). The spots map's own ids — '7d' / '30d' are "today and the N
 *  days before" (the store's day rows), hence 8 and 31 days; the app's live-only ids ('2m' … ) too. */
export function amWinSec(win: string): number {
  switch (String(win || '').toLowerCase()) {
    case '2m': return 120;
    case '5m': return 300;
    case '15m': return 900;
    case '30m': return 1800;
    case '1h': return 3600;
    case '3h': return 3 * 3600;
    case '24h': return 86400;
    case '7d': return 8 * 86400;
    case '30d': return 31 * 86400;
    default: return 0;   // 'all' / 'ALL'
  }
}
/** ★★ THE RULE: a station last heard `ageSec` ago is on the map in window `win` — whoever's decoder heard it. */
export function amKeep(ageSec: number, win: string): boolean {
  const w = amWinSec(win);
  return !w || (Number.isFinite(ageSec) && ageSec >= 0 && ageSec <= w);
}

/** The layers and the store's origin bits (vibe_station_store.h Origin). */
import { meshName, meshKm, meshDist, meshPlausible, meshHopsForRule, meshRelays, meshRoute, MESH_ROUTE_JS, MESH_ROUTE_BTN_STYLE, type MeshKnown, type MeshRoute } from './meshRoute';

export type AmLayer = 'spots' | 'aprs' | 'mesh' | 'beacons';
export const AM_SRC_BACKGROUND = 1;
export const AM_SRC_LIVE = 2;
export const AM_SRC_SATELLITE = 4;
/** ★ Who heard a stored station, in words (its card and its row) — from the store's `src` bits. */
export function amSrcWords(src: number): string {
  const b = !!(src & AM_SRC_BACKGROUND), l = !!(src & AM_SRC_LIVE), s = !!(src & AM_SRC_SATELLITE);
  const w = [b ? 'background decoding' : '', l ? 'a listener’s decoder' : '', s ? 'a satellite pass' : ''].filter(Boolean);
  return w.length ? `Heard by ${w.join(' and ')}` : '';
}
/** ★ The layer the receiver's current decoder feeds (the one emphasised) — '' when none of the amateur map's. */
export function amLiveOf(dec: string | null | undefined, ismOn = false): AmLayer | '' {
  if (ismOn) return 'mesh';
  switch (String(dec || '')) {
    case 'ft8': case 'ft4': case 'wspr': case 'js8': case 'spots': case 'cw': case 'rttyskim': case 'psk': case 'jtty': return 'spots';
    case 'aprs': return 'aprs';
    case 'ncdxf': return 'beacons';
    default: return '';
  }
}

/** One MESH node as the map draws it (a MeshCore / Meshtastic node with a position). ★★ beta96 (docs/v12/MESH-ROUTE.md):
 *  `dist` its distance from the receiver in the ISM list's km / miles ('' when the receiver is not known); `rt` its route
 *  (meshRoute — MESH_ROUTE_JS draws it when the node is tapped) and `rw` the route card's lines. */
export interface AmMeshPoint { key: string; name: string; net: string; lat: number; lon: number; age: number; approx: boolean; what: string; live: boolean;
                               dist?: string; km?: number; rt?: MeshRoute | null; rw?: string[] }
/** ★★ beta96 a node's NAME is kept as it was advertised (meshName: printable text — "t::1" stays "t::1"); the pages
 *  escape it where it meets HTML. Other fields keep the old strict filter. */
const okName = (v: unknown, n: number) => meshName(v, n);
const okTxt = (v: unknown, n: number) => String(v ?? '').replace(/[<>&"'`\\\u0000-\u001f]/g, '').slice(0, n);
const okNum = (v: unknown, lo: number, hi: number): number | null => { const n = Number(v); return v == null || !Number.isFinite(n) ? null : Math.max(lo, Math.min(hi, n)); };
const MESH_KINDS = new Set(['meshtastic', 'meshcore']);
/** ★ The live ISM report's mesh nodes (age = since last heard) and the gallery's remembered ones (`last`, seconds), each
 *  node once (the newest wins), newest first, at most 1000. Anything not a mesh node, or with no position, is left out. */
export function amMeshPoints(live: readonly any[] | null | undefined, stored: readonly any[] | null | undefined, nowSec: number,
                             opt: { rx?: { lat: number; lon: number } | null; miles?: boolean } = {}): AmMeshPoint[] {
  const rx = opt.rx && Number.isFinite(opt.rx.lat) && Number.isFinite(opt.rx.lon) ? opt.rx : null;
  const by = new Map<string, { p: AmMeshPoint; d: any }>();
  const add = (d: any, age: number, isLive: boolean) => {
    if (!d || !MESH_KINDS.has(String(d.kind))) return;
    const lat = okNum(d.lat, -90, 90), lon = okNum(d.lon, -180, 180);
    if (lat == null || lon == null || (lat === 0 && lon === 0)) return;
    const key = okTxt(d.node || d.id || d.key, 40);
    if (!key) return;
    // ★★ beta96 an implausible fix (meshRoute.ts meshPlausible: beyond 500 km a hop) is never drawn
    const km = rx ? meshKm(rx, lat, lon) : undefined;
    if (km !== undefined && !meshPlausible(km, meshHopsForRule(d))) return;
    const p: AmMeshPoint = { key: `mesh:${key}`, name: okName(d.long, 40) || okName(d.name, 40) || okName(d.short, 40) || okTxt(d.node || key, 40), net: d.kind === 'meshcore' ? 'MeshCore' : 'Meshtastic',
      lat, lon, age: Math.max(0, age), approx: d.approx === 1 || d.approx === true, what: okTxt(d.ntype || d.role || d.hw || '', 40), live: isLive };
    if (km !== undefined) { p.km = km; p.dist = meshDist(km, !!opt.miles); }
    const was = by.get(p.key);
    if (!was || p.age < was.p.age) by.set(p.key, { p, d });
  };
  for (const d of live ?? []) add(d, okNum(d?.age ?? d?.seen, 0, 1e9) ?? 0, true);
  for (const d of stored ?? []) add(d, Math.max(0, nowSec - (okNum(d?.last, 0, 1e11) ?? 0)), false);
  const out = [...by.values()].sort((a, b) => a.p.age - b.p.age).slice(0, 1000);
  // ★★ beta96 THE ROUTES: each MeshCore node's relays, matched to the repeaters and room servers on this map (never a
  //   companion — a person's node is never a route's stop)
  const known: MeshKnown[] = [];
  for (const { p, d } of out) {
    const id = String(d.node || '');
    if (d.kind === 'meshcore' && (d.ntype === 'repeater' || d.ntype === 'room') && /^[0-9a-fA-F]{2,16}$/.test(id)) known.push({ id: id.toLowerCase(), name: p.name, lat: p.lat, lon: p.lon });
  }
  for (const { p, d } of out) {
    if (d.kind !== 'meshcore') continue;
    const rel = Array.isArray(d.relays) ? meshRelays(d.relays.join(' ')) : meshRelays(d.relays);
    const rt = rel ? meshRoute({ lat: p.lat, lon: p.lon, relays: rel, node: String(d.node || '') }, known, rx) : null;
    if (rt) { p.rt = rt; p.rw = rt.words; }
  }
  return out.map((x) => x.p);
}

/* ★★★ beta96 maps2 MESH REMEMBERS EVERY ISM BAND (Stuart's 11-26 decision): the AMATEUR map's remembered nodes are every band's
 *  "stations received" day cards (vibe_ism_gallery.h, slot "ismnodes-<band>-<date>") over the HEARD IN window — not only the
 *  band ISM last used. BOUNDED: at most AM_MESH_CARDS_MAX cards (the newest days first), fetched ONE AT A TIME (a live
 *  server); a card older than yesterday never changes, so it is kept and not fetched again (today's and yesterday's are —
 *  yesterday's is written once more when its day rolls over); at most
 *  AM_MESH_NODES_MAX nodes. ISM's own live map keeps its band's last 24 h (ismStored) as before. */
export const AM_MESH_CARDS_MAX = 16;
export const AM_MESH_NODES_MAX = 1000;
const ISM_NODES_SLOT = /^ismnodes-[^-]+-\d{4}-\d\d-\d\d$/;
/** The window's days, in seconds back from now ('all' = the gallery's 31 days, as ISM's gallery map). */
function amSpanSec(win: string): number { return amWinSec(win) || 31 * 86400; }
/** ★ The day cards to read for window `win`: every band's, from the day the window starts, newest first, bounded. */
export function amMeshCardPick<T extends { s?: string; t: number; f: string }>(items: readonly T[] | null | undefined, win: string, nowSec: number, max = AM_MESH_CARDS_MAX): T[] {
  const from = Math.floor((nowSec - amSpanSec(win)) / 86400) * 86400;
  return (items ?? []).filter((c) => !!c && typeof c.s === 'string' && ISM_NODES_SLOT.test(c.s) && typeof c.f === 'string' && Number(c.t) >= from)
    .sort((a, b) => b.t - a.t).slice(0, Math.max(0, max));
}
/** ★ The loader (web and app alike): `list` = the gallery's ISM items, `card` = one card's nodes (ismNodesParse), `merge` =
 *  ismGalMerge. Returns null while a load is running or when the gallery did not answer (keep what is drawn). */
export function amMeshLoader<N extends { last: number }>(o: {
  list: () => Promise<ReadonlyArray<{ s?: string; t: number; f: string }> | null>;
  card: (f: string, t: number) => Promise<N[] | null>;
  merge: (per: N[][], nowSec: number) => N[];
}): (win: string, nowSec: number) => Promise<N[] | null> {
  const kept = new Map<string, N[]>();   // ★ past days' cards (they never change) — only those still picked
  let busy = false;
  return async (win, nowSec) => {
    if (busy) return null;
    busy = true;
    try {
      const items = await o.list();
      if (!items) return null;
      const cards = amMeshCardPick(items, win, nowSec);
      const fresh = Math.floor(nowSec / 86400) * 86400 - 86400, per: N[][] = [], want = new Set<string>();   // ★ yesterday and today
      for (const c of cards) {
        want.add(c.f);
        let n = c.t >= fresh ? undefined : kept.get(c.f);
        if (!n) { n = (await o.card(c.f, c.t)) ?? undefined; if (n && c.t < fresh) kept.set(c.f, n); }
        if (n) per.push(n);
      }
      for (const k of [...kept.keys()]) if (!want.has(k)) kept.delete(k);
      const since = nowSec - amSpanSec(win);
      return o.merge(per, nowSec).filter((n) => n.last >= since).sort((a, b) => b.last - a.last).slice(0, AM_MESH_NODES_MAX);
    } catch { return null; } finally { busy = false; }
  };
}

/** One NCDXF / IARU beacon heard, at its published site. */
export interface AmBeaconPoint { key: string; call: string; where: string; lat: number; lon: number; age: number; bands: string[]; best: number }
/** ★ The tracker's report → the beacons heard on any band (age = the most recent heard), with the bands heard on and the
 *  deepest power step reached (1–4). A beacon never heard is not drawn (no dead markers). */
export function amBeaconPoints(r: { beacons?: readonly { call: string; where: string; lat: number; lon: number }[]; cells?: readonly { beacon: number; band: number; heardAgo: number; steps: number }[] } | null | undefined,
                               bandLabels: readonly string[]): AmBeaconPoint[] {
  if (!r || !Array.isArray(r.beacons) || !Array.isArray(r.cells)) return [];
  const out: AmBeaconPoint[] = [];
  r.beacons.forEach((b, i) => {
    const lat = okNum(b?.lat, -90, 90), lon = okNum(b?.lon, -180, 180);
    if (lat == null || lon == null) return;
    const heard = (r.cells ?? []).filter((c) => c && c.beacon === i && Number.isFinite(c.heardAgo) && c.heardAgo >= 0);
    if (!heard.length) return;
    out.push({ key: `bcn:${okTxt(b.call, 12)}`, call: okTxt(b.call, 12), where: okTxt(b.where, 60), lat, lon,
      age: Math.min(...heard.map((c) => c.heardAgo)), bands: heard.map((c) => okTxt(bandLabels[c.band] ?? '', 8)).filter(Boolean),
      best: Math.max(...heard.map((c) => Math.max(0, Math.min(4, Number(c.steps) || 0)))) });
  });
  return out.sort((a, b) => a.age - b.age);
}

/** ★ The tabs drawn, in order: those with something in the window, the live layer's, the one opened on. */
export const AM_TABS: readonly AmLayer[] = ['spots', 'aprs', 'mesh', 'beacons'];
export const AM_TAB_LABEL: Record<AmLayer, string> = { spots: 'SPOTS', aprs: 'APRS', mesh: 'MESH', beacons: 'BEACONS' };
export function amTabs(have: Partial<Record<AmLayer, number>>, live: AmLayer | '', opened: AmLayer | ''): AmLayer[] {
  return AM_TABS.filter((t) => (have[t] ?? 0) > 0 || t === live || t === opened);
}

/** ★ The tab shown: the viewer's pick while it is still offered, else the live layer's, else the one opened on, else the
 *  first (the web window's amList rule — one rule for both). */
export function amTabNow(tabs: readonly AmLayer[], pick: AmLayer | '' | null | undefined, live: AmLayer | '', opened: AmLayer | ''): AmLayer {
  if (pick && tabs.includes(pick)) return pick;
  if (live && tabs.includes(live)) return live;
  if (opened && tabs.includes(opened)) return opened;
  return tabs[0] ?? 'spots';
}

/* ★★★ beta96 maps2 THE APP's LIST, IN TABS (TrackLiveList over the 'digi' page — React Native, so a pad walks it): the same
 *  rows as the web window's amList (main.ts) — a spot says who heard it (the store's src: background decoding / a
 *  listener's decoder; one not yet stored is this app's own decoder's), APRS its symbol and comment, MESH its network,
 *  BEACONS its site and bands — every one through the HEARD IN window (amKeep), at most AM_ROWS_MAX a tab. */
export const AM_ROWS_MAX = 300;
/** A spot as the list reads it (the app's SpotRow: time in ms; `src` set on a station from the store). */
export interface AmSpotIn { call: string; mode: string; band: string; time: number; distKm?: number; country?: string; src?: number }
/** An APRS station as the list reads it (digiMapShared.ts DigiAprsPoint: age = since last heard, s). */
export interface AmAprsIn { key: string; call: string; age: number; km: number | null; symName?: string; comment?: string }
export interface AmListIn { spots?: readonly AmSpotIn[] | null; aprs?: readonly AmAprsIn[] | null; mesh?: readonly AmMeshPoint[] | null; beacons?: readonly AmBeaconPoint[] | null }
/** One row (services/tracks.ts TrackRow's shape — TrackLiveList draws it). */
export interface AmRow { key: string; colour: string; flag: string; name: string; second: string; sp: string; trend: 0; range: string; brg: string; noPos: false; age: string; em: false }
/** "45 s" · "12 min" · "3 h" · "2 days" — the row's age (the list adds "heard … ago" for a screen reader). */
export function amAgoShort(sec: number): string {
  const s = Math.max(0, Math.round(Number.isFinite(sec) ? sec : 0));
  return s < 90 ? `${s} s` : s < 5400 ? `${Math.round(s / 60)} min` : s < 172800 ? `${Math.round(s / 3600)} h` : `${Math.round(s / 86400)} days`;
}
/** ★ A spot's key — the map page's own key(s) (call|band|MODE), prefixed, so a row finds its marker. */
export function amSpotKey(call: string, band: string, mode: string): string { return `spot:${call}|${band}|${String(mode || '').toUpperCase()}`; }
const kmTxt = (km: number | null | undefined) => (km != null && Number.isFinite(km) && km >= 0 ? `${Math.round(km)} km` : '');
/** The spots in the window, each station-band-mode once (the newest wins), newest first. */
function spotsIn(spots: readonly AmSpotIn[] | null | undefined, win: string, nowMs: number): AmSpotIn[] {
  const by = new Map<string, AmSpotIn>();
  for (const s of spots ?? []) {
    if (!s || !s.call || !amKeep((nowMs - s.time) / 1000, win)) continue;
    const k = amSpotKey(s.call, s.band, s.mode), was = by.get(k);
    if (!was || s.time > was.time || (s.time === was.time && s.src != null && was.src == null)) by.set(k, s);
  }
  return [...by.values()].sort((a, b) => b.time - a.time);
}
/** ★ How many of each layer are in the window (the tabs' counts). */
export function amCountsOf(d: AmListIn, win: string, nowMs: number): Record<AmLayer, number> {
  return {
    spots: spotsIn(d.spots, win, nowMs).length,
    aprs: (d.aprs ?? []).filter((a) => amKeep(a.age, win)).length,
    mesh: (d.mesh ?? []).filter((x) => amKeep(x.age, win)).length,
    beacons: (d.beacons ?? []).filter((x) => amKeep(x.age, win)).length,
  };
}
/** ★★ The rows of one tab. `colourOf` is the mode's colour (digiModes.ts digiModeStyle — the marker's). */
export function amListRows(tab: AmLayer, d: AmListIn, win: string, nowMs: number, colourOf: (mode: string) => string): AmRow[] {
  const row = (key: string, colour: string, name: string, second: string, range: string, ageSec: number): AmRow =>
    ({ key, colour, flag: '', name, second, sp: '', trend: 0, range, brg: '', noPos: false, age: amAgoShort(ageSec), em: false });
  if (tab === 'spots') {
    return spotsIn(d.spots, win, nowMs).slice(0, AM_ROWS_MAX).map((s) => row(amSpotKey(s.call, s.band, s.mode), colourOf(s.mode), s.call,
      [String(s.mode || '').toUpperCase(), s.band, s.country, s.src != null ? amSrcWords(s.src) : 'Heard by this app\u2019s decoder'].filter(Boolean).join(' \u00b7 '),
      kmTxt(s.distKm), (nowMs - s.time) / 1000));
  }
  if (tab === 'aprs') {
    return (d.aprs ?? []).filter((a) => amKeep(a.age, win)).slice(0, AM_ROWS_MAX).map((a) => row(`aprs:${a.key}`, '#88CCEE', a.call,
      [a.symName || 'APRS', a.comment].filter(Boolean).join(' \u00b7 '), kmTxt(a.km), a.age));
  }
  if (tab === 'mesh') {
    return (d.mesh ?? []).filter((x) => amKeep(x.age, win)).slice(0, AM_ROWS_MAX).map((x) => row(x.key, x.net === 'MeshCore' ? '#ff9f43' : '#67ea94', x.name,
      [x.net, x.what, x.live ? 'heard now' : 'remembered', x.rt ? `${x.rt.hops.length} hops` : ''].filter(Boolean).join(' \u00b7 '), x.dist || '', x.age));
  }
  return (d.beacons ?? []).filter((x) => amKeep(x.age, win)).slice(0, AM_ROWS_MAX).map((x) => row(x.key, '#d2a8ff', x.call,
    `${x.where} \u00b7 ${x.bands.join(' ')}`, '', x.age));
}
/** ★ What an empty tab says (the web window's words). */
export function amEmptyWords(tab: AmLayer): string {
  return tab === 'mesh' ? 'No mesh node with a position in this window — ISM mode hears MeshCore and Meshtastic.'
    : tab === 'beacons' ? 'No NCDXF beacon heard in this window — the NCDXF decoder tracks them.'
    : tab === 'aprs' ? 'No APRS station with a position in this window.' : 'No station with a locator in this window.';
}

/* ★★ THE SAME RULES AS PAGE TEXT (both map pages are their own documents): the window, the layer drawing for MESH and
 *  BEACONS (each marker kept in AM_MK by its key, so a list row can open it), the list's rows. Plain ES5; no backticks, no backslashes. The page supplies L (Leaflet or the GPU adapter),
 *  esc() and RIM. scripts/test_amateur_map.ts runs amKeepJs against amKeep. */
export const AMATEUR_JS = MESH_ROUTE_JS + (`
function amWinSec(win){switch(String(win||'').toLowerCase()){case '2m':return 120;case '5m':return 300;case '15m':return 900;case '30m':return 1800;
  case '1h':return 3600;case '3h':return 10800;case '24h':return 86400;case '7d':return 691200;case '30d':return 2678400;default:return 0;}}
function amKeep(ageSec,win){var w=amWinSec(win);return !w||(isFinite(ageSec)&&ageSec>=0&&ageSec<=w);}
function amAgo(s){s=Math.max(0,Math.round(s));return s<90?s+' s ago':s<5400?Math.round(s/60)+' min ago':s<172800?Math.round(s/3600)+' h ago':Math.round(s/86400)+' days ago';}
var AM_MESH_COL={MeshCore:'#ff9f43',Meshtastic:'#67ea94'};
function amMeshCard(n){var rw='';if(n.rw&&n.rw.length)for(var i=0;i<n.rw.length&&i<70;i++)rw+='<br>'+esc(n.rw[i]);
  return '<div class="pop"><b>'+esc(n.name)+'</b><br>'+esc(n.net)+(n.what?' &middot; '+esc(n.what):'')+(n.dist?' &middot; '+esc(n.dist):'')+'<br>'
  +(n.approx?'Position rounded to about 1 km<br>':'')+(n.live?'Heard now on ISM':'Remembered by this server')+' &middot; '+esc(amAgo(n.age))+rw
  +(n.rt?'<div class="vmrRow"><button type="button" class="vmrBtn" data-amk="'+esc(n.key)+'" aria-pressed="'+(AM_RT_KEY===n.key)+'" style="'+AM_RT_BTN_STYLE+'">'+(AM_RT_KEY===n.key?'HIDE ROUTE':'ROUTE')+'</button></div>':'')+'</div>';}
/* ★★ beta96 THE ROUTE (meshRoute.ts MESH_ROUTE_JS, carried beside this — Stuart 11-26): a tap on a mesh node opens its card
 *  and zooms in to it; the card's ROUTE (only on a node whose packet carried relays) lights the repeaters it came through,
 *  hop by hop, and fits the view to the whole path; ROUTE again, the card closing or a tap on the empty map clears it.
 *  A node chosen HOLDS THE VIEW (the page's auto fit, digiFit, pauses until FIT, or the card closes). The page calls
 *  amRouteBind() once its map exists. amMeshExtra() cleans what React Native / the opener sent for it. */
var AM_RT=null,AM_RT_KEY='',AM_HELD=false,AM_SEL_AT=0,AM_NODES={};
var AM_RT_BTN_STYLE='__AM_RT_BTN_STYLE__';
function amFitOf(){try{return (typeof digiFit!=='undefined'&&digiFit&&digiFit.state)?digiFit:null;}catch(e){return null;}}
function amHold(){var f=amFitOf();try{var s=f&&f.state();if(s&&s.on){if(!s.paused)f.pause();AM_HELD=true;}}catch(e){}}
function amRelease(){if(!AM_HELD)return;AM_HELD=false;var f=amFitOf();try{var s=f&&f.state();if(s&&s.on&&s.paused)f.resume();}catch(e){}}
function amRouteBtns(){var bs=document.querySelectorAll('.vmrBtn[data-amk]');for(var i=0;i<bs.length;i++){var on=!!AM_RT_KEY&&bs[i].getAttribute('data-amk')===AM_RT_KEY;
  bs[i].setAttribute('aria-pressed',String(on));bs[i].textContent=on?'HIDE ROUTE':'ROUTE';}}
function amRouteShow(n){var was=AM_RT_KEY;try{if(AM_RT)AM_RT.clear();}catch(e){}AM_RT=null;AM_RT_KEY='';
  if(n&&n.rt&&was!==n.key&&typeof meshRouteDraw==='function'){try{AM_RT=meshRouteDraw(map,L,n.rt);AM_RT_KEY=n.key;amHold();}catch(e){console.error('mesh route',e);}}
  amRouteBtns();
  try{if(window.ReactNativeWebView&&was!==AM_RT_KEY)window.ReactNativeWebView.postMessage(JSON.stringify({t:'amRoute',key:AM_RT_KEY||null}));}catch(e){}}
/* the app's list ROUTE key (a pad cannot reach the card's): toggles the chosen node's route */
if(typeof window!=='undefined')window.__amRoute=function(k){var n=AM_NODES[String(k)];if(n){AM_SEL_AT=Date.now();amRouteShow(n);}};
function amMeshPick(n){AM_SEL_AT=Date.now();if(AM_RT_KEY&&AM_RT_KEY!==n.key)amRouteShow(null);amHold();
  try{map.flyTo([n.lat,n.lon],Math.max(map.getZoom(),9),{duration:0.8});}catch(e){}}
function amCardClosed(){if(Date.now()-AM_SEL_AT<400)return;if(AM_RT)amRouteShow(null);amRelease();}
function amRouteBind(){try{map.on('click',amCardClosed);}catch(e){}try{map.on('popupclose',amCardClosed);}catch(e){}
  document.addEventListener('click',function(ev){var t=ev.target,b=t&&t.closest?t.closest('.vmrBtn[data-amk]'):null;if(!b)return;
    ev.preventDefault();ev.stopPropagation();AM_SEL_AT=Date.now();var n=AM_NODES[b.getAttribute('data-amk')];if(n)amRouteShow(n);},true);}
function amMeshExtra(x,o){o.dist=String(x.dist||'').slice(0,16);o.rt=x.rt&&typeof x.rt==='object'&&Array.isArray(x.rt.seg)?x.rt:null;
  o.rw=Array.isArray(x.rw)?x.rw.slice(0,70).map(function(w){return String(w).slice(0,300);}):[];return o;}
function amBeaconCard(b){return '<div class="pop"><b>'+esc(b.call)+'</b> &middot; '+esc(b.where)+'<br>NCDXF / IARU beacon &middot; heard on '+esc(b.bands.join(', '))
  +'<br>Down to '+esc(['','100 W','10 W','1 W','0.1 W'][b.best]||'')+' &middot; '+esc(amAgo(b.age))+'</div>';}
var AM_MK={};
function amMkClear(pre){for(var k in AM_MK)if(k.indexOf(pre)===0)delete AM_MK[k];}
function amDrawMesh(group,nodes,win,live){group.clearLayers();amMkClear('mesh:');AM_NODES={};var out=[];for(var i=0;i<nodes.length;i++){var n=nodes[i];if(!amKeep(n.age,win))continue;out.push(n);
  AM_MK[n.key]=L.circleMarker([n.lat,n.lon],{radius:live?7:6,color:RIM,weight:1.2,fillColor:AM_MESH_COL[n.net]||'#67ea94',fillOpacity:n.live?0.95:0.55,label:'M',labelColor:'#000000'})
   .addTo(group).bindPopup(amMeshCard(n));
  AM_NODES[n.key]=n;if(AM_MK[n.key].on)(function(n){AM_MK[n.key].on('click',function(){amMeshPick(n);});})(n);}
  if(AM_RT_KEY&&!out.some(function(x){return x.key===AM_RT_KEY;}))amRouteShow(null);return out;}
function amDrawBeacons(group,list,win,live,me){group.clearLayers();amMkClear('bcn:');var out=[];for(var i=0;i<list.length;i++){var b=list[i];if(!amKeep(b.age,win))continue;out.push(b);
  if(me)L.polyline([[me.lat,me.lon],[b.lat,b.lon]],{color:'#d2a8ff',weight:1,opacity:0.45,dashArray:'3 5'}).addTo(group);
  AM_MK[b.key]=L.circleMarker([b.lat,b.lon],{radius:live?8:7,color:RIM,weight:1.2,fillColor:'#d2a8ff',fillOpacity:0.95,label:'B',labelColor:'#000000'})
   .addTo(group).bindPopup(amBeaconCard(b));}return out;}
function amSrcWords(src){var w=[];if(src&1)w.push('background decoding');if(src&2)w.push('a listener’s decoder');if(src&4)w.push('a satellite pass');return w.length?'Heard by '+w.join(' and '):'';}
function amRow(key,col,name,sub,right){return '<div class="amRow" role="button" tabindex="0" data-k="'+esc(key)+'"><i style="background:'+esc(col)+'"></i><span class="n"><b>'+esc(name)+'</b><small>'+esc(sub)+'</small></span><span class="r">'+esc(right)+'</span></div>';}
`).replace('__AM_RT_BTN_STYLE__', MESH_ROUTE_BTN_STYLE);
