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

/** One MESH node as the map draws it (a MeshCore / Meshtastic node with a position). */
export interface AmMeshPoint { key: string; name: string; net: string; lat: number; lon: number; age: number; approx: boolean; what: string; live: boolean }
const okTxt = (v: unknown, n: number) => String(v ?? '').replace(/[<>&"'`\\\u0000-\u001f]/g, '').slice(0, n);
const okNum = (v: unknown, lo: number, hi: number): number | null => { const n = Number(v); return v == null || !Number.isFinite(n) ? null : Math.max(lo, Math.min(hi, n)); };
const MESH_KINDS = new Set(['meshtastic', 'meshcore']);
/** ★ The live ISM report's mesh nodes (age = since last heard) and the gallery's remembered ones (`last`, seconds), each
 *  node once (the newest wins), newest first, at most 1000. Anything not a mesh node, or with no position, is left out. */
export function amMeshPoints(live: readonly any[] | null | undefined, stored: readonly any[] | null | undefined, nowSec: number): AmMeshPoint[] {
  const by = new Map<string, AmMeshPoint>();
  const add = (d: any, age: number, isLive: boolean) => {
    if (!d || !MESH_KINDS.has(String(d.kind))) return;
    const lat = okNum(d.lat, -90, 90), lon = okNum(d.lon, -180, 180);
    if (lat == null || lon == null || (lat === 0 && lon === 0)) return;
    const key = okTxt(d.node || d.id || d.key, 40);
    if (!key) return;
    const p: AmMeshPoint = { key: `mesh:${key}`, name: okTxt(d.long || d.name || d.short || d.node || key, 40), net: d.kind === 'meshcore' ? 'MeshCore' : 'Meshtastic',
      lat, lon, age: Math.max(0, age), approx: d.approx === 1 || d.approx === true, what: okTxt(d.ntype || d.role || d.hw || '', 40), live: isLive };
    const was = by.get(p.key);
    if (!was || p.age < was.age) by.set(p.key, p);
  };
  for (const d of live ?? []) add(d, okNum(d?.age ?? d?.seen, 0, 1e9) ?? 0, true);
  for (const d of stored ?? []) add(d, Math.max(0, nowSec - (okNum(d?.last, 0, 1e11) ?? 0)), false);
  return [...by.values()].sort((a, b) => a.age - b.age).slice(0, 1000);
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

/* ★★ THE SAME RULES AS PAGE TEXT (both map pages are their own documents): the window, the layer drawing for MESH and
 *  BEACONS, the list's rows. Plain ES5; no backticks, no backslashes. The page supplies L (Leaflet or the GPU adapter),
 *  esc() and RIM. scripts/test_amateur_map.ts runs amKeepJs against amKeep. */
export const AMATEUR_JS = `
function amWinSec(win){switch(String(win||'').toLowerCase()){case '2m':return 120;case '5m':return 300;case '15m':return 900;case '30m':return 1800;
  case '1h':return 3600;case '3h':return 10800;case '24h':return 86400;case '7d':return 691200;case '30d':return 2678400;default:return 0;}}
function amKeep(ageSec,win){var w=amWinSec(win);return !w||(isFinite(ageSec)&&ageSec>=0&&ageSec<=w);}
function amAgo(s){s=Math.max(0,Math.round(s));return s<90?s+' s ago':s<5400?Math.round(s/60)+' min ago':s<172800?Math.round(s/3600)+' h ago':Math.round(s/86400)+' days ago';}
var AM_MESH_COL={MeshCore:'#ff9f43',Meshtastic:'#67ea94'};
function amMeshCard(n){return '<div class="pop"><b>'+esc(n.name)+'</b><br>'+esc(n.net)+(n.what?' &middot; '+esc(n.what):'')+'<br>'
  +(n.approx?'Position rounded to about 1 km<br>':'')+(n.live?'Heard now on ISM':'Remembered by this server')+' &middot; '+esc(amAgo(n.age))+'</div>';}
function amBeaconCard(b){return '<div class="pop"><b>'+esc(b.call)+'</b> &middot; '+esc(b.where)+'<br>NCDXF / IARU beacon &middot; heard on '+esc(b.bands.join(', '))
  +'<br>Down to '+esc(['','100 W','10 W','1 W','0.1 W'][b.best]||'')+' &middot; '+esc(amAgo(b.age))+'</div>';}
function amDrawMesh(group,nodes,win,live){group.clearLayers();var out=[];for(var i=0;i<nodes.length;i++){var n=nodes[i];if(!amKeep(n.age,win))continue;out.push(n);
  L.circleMarker([n.lat,n.lon],{radius:live?7:6,color:RIM,weight:1.2,fillColor:AM_MESH_COL[n.net]||'#67ea94',fillOpacity:n.live?0.95:0.55,label:'M',labelColor:'#000000'})
   .addTo(group).bindPopup(amMeshCard(n));}return out;}
function amDrawBeacons(group,list,win,live,me){group.clearLayers();var out=[];for(var i=0;i<list.length;i++){var b=list[i];if(!amKeep(b.age,win))continue;out.push(b);
  if(me)L.polyline([[me.lat,me.lon],[b.lat,b.lon]],{color:'#d2a8ff',weight:1,opacity:0.45,dashArray:'3 5'}).addTo(group);
  L.circleMarker([b.lat,b.lon],{radius:live?8:7,color:RIM,weight:1.2,fillColor:'#d2a8ff',fillOpacity:0.95,label:'B',labelColor:'#000000'})
   .addTo(group).bindPopup(amBeaconCard(b));}return out;}
function amSrcWords(src){var w=[];if(src&1)w.push('background decoding');if(src&2)w.push('a listener’s decoder');if(src&4)w.push('a satellite pass');return w.length?'Heard by '+w.join(' and '):'';}
function amRow(key,col,name,sub,right){return '<div class="amRow" role="button" tabindex="0" data-k="'+esc(key)+'"><i style="background:'+esc(col)+'"></i><span class="n"><b>'+esc(name)+'</b><small>'+esc(sub)+'</small></span><span class="r">'+esc(right)+'</span></div>';}
`;
