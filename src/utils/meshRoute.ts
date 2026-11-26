/**
 * meshRoute.ts — ★★ beta96 THE MESH MAP's shared rules (Stuart, 2026-11-26), ONE copy for the web client and the app
 * (docs/v12/MESH-ROUTE.md):
 *
 *   1. NAMES. A node's advertised name is a stranger's bytes: shown as printable text only (control, private-use,
 *      unpaired-surrogate and replacement characters dropped; emoji and every script kept), 40 characters at most. A
 *      node with no usable name shows its short key id, as an unnamed node always has. "t::1" (the Pi 500's 334 km node,
 *      key 8d76bb29, a repeater on the north Cornwall coast) is a REAL name and stays exactly that.
 *   2. PLAUSIBILITY. One LoRa hop spans ~500 km at the very most (a tropospheric duct); a fix further than 500 km per
 *      hop (first-hand = one hop of air), or 3000 km when the hop count is unknown, is a bad fix: not plotted, never "the
 *      furthest". The server's card applies the same rule (vibe_ism_gallery.h meshPlausibleKm).
 *   3. FURTHEST. The headline is the furthest node heard FIRST-HAND (hops 0 at least once: the aerial's own reach); the
 *      furthest heard only through the mesh is said beside it: "furthest direct: N-Ise repeater NN29 16 km · via mesh:
 *      t::1 334 km". In km or miles: the ISM list's KM/H ↔ MPH switch (one preference drives the list and the maps).
 *   4. THE ROUTE. A MeshCore flood packet carries the relays it passed through (each the first 1-3 bytes of a
 *      repeater's public key, first relay first). Each is matched to the repeaters and room servers this receiver has
 *      heard advertise: one match → placed; several → "one of these" (dimmed); none → an unknown repeater (a dashed
 *      gap). meshRoute() also lays out the drawing — every joining segment and hop badge with the tick it lights on —
 *      and MESH_ROUTE_JS plays it on a map, ~300 ms a hop: node → relay 1 → … → this receiver.
 *  ★★★ PRIVACY: only repeaters and room servers (which advertise themselves to be found) are ever placed on a route; a
 *      person's companion node is never a candidate and keeps its rounded position.
 */

export const MESH_HOP_KM = 500, MESH_UNKNOWN_KM = 3000;
export const MESH_ROUTE_MAX = 64, MESH_CANDS_MAX = 6;

/** A name as it may be shown, or '' when nothing usable is left (the caller then shows the node's id). */
export function meshName(v: unknown, max = 40): string {
  if (typeof v !== 'string') return '';
  // control (C0/C1), private use, lone surrogates, U+FFFD; format characters except the emoji joiner (U+200D)
  let t = v.replace(/[\p{Cc}\p{Co}\p{Cs}�]/gu, '').replace(/\p{Cf}/gu, (c) => (c === '‍' ? c : ''));
  t = t.replace(/\s+/g, ' ').trim();
  const cps = Array.from(t);
  if (cps.length > max) t = cps.slice(0, max).join('').replace(/‍+$/, '').trim();
  return /[\p{L}\p{N}\p{S}\p{P}]/u.test(t) ? t : '';
}

/** Is this position believable for a node `km` away heard by `hops` hops (undefined/negative = unknown)? */
export function meshPlausible(km: number, hops: number | undefined): boolean {
  if (!Number.isFinite(km)) return false;
  return hops !== undefined && hops >= 0 ? km <= MESH_HOP_KM * (hops + 1) : km <= MESH_UNKNOWN_KM;
}

/** Great-circle km between two points. */
export function meshKm(a: { lat: number; lon: number }, lat: number, lon: number): number {
  const r = Math.PI / 180, p1 = a.lat * r, p2 = lat * r, dl = (lon - a.lon) * r;
  const h = Math.sin((p2 - p1) / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return 12742 * Math.asin(Math.min(1, Math.sqrt(h)));
}

/** "16 km" / "9.9 km" — or in miles ("10 mi"). Land distances: km or miles, never nautical. */
export function meshDist(km: number, miles = false): string {
  const v = miles ? km / 1.609344 : km;
  return `${v < 10 ? v.toFixed(1) : Math.round(v)} ${miles ? 'mi' : 'km'}`;
}

/** Heard first-hand at least once: the newest packet's hop count is 0, or the server says it ever was (`direct`). */
export function meshIsDirect(d: { hops?: number; direct?: number }): boolean { return d.hops === 0 || d.direct === 1; }
/** The hop count the plausibility rule judges by: 0 when ever heard first-hand. */
export function meshHopsForRule(d: { hops?: number; direct?: number }): number | undefined { return meshIsDirect(d) ? 0 : d.hops; }

export interface MeshFar { name: string; km: number; hops?: number }
/** The furthest first-hand node and the furthest heard only via the mesh, each plausible. */
export function meshFurthest<T extends { lat?: number; lon?: number; hops?: number; direct?: number }>(
  nodes: readonly T[], rx: { lat: number; lon: number } | null, label: (d: T) => string,
): { direct: MeshFar | null; mesh: MeshFar | null } {
  let direct: MeshFar | null = null, mesh: MeshFar | null = null;
  if (!rx) return { direct, mesh };
  for (const d of nodes) {
    if (d.lat === undefined || d.lon === undefined) continue;
    const km = meshKm(rx, d.lat, d.lon);
    const dir = meshIsDirect(d);
    if (!meshPlausible(km, dir ? 0 : d.hops)) continue;
    if (dir) { if (!direct || km > direct.km) direct = { name: label(d), km, hops: 0 }; }
    else if (!mesh || km > mesh.km) mesh = { name: label(d), km, hops: d.hops };
  }
  return { direct, mesh };
}
/** " · furthest direct: N-Ise repeater NN29 16 km · via mesh: t::1 334 km" ('' when nothing is placed). The via-mesh
 *  half only when it is further than the direct one (otherwise it says nothing the headline did not). */
export function meshFarText(f: { direct: MeshFar | null; mesh: MeshFar | null }, miles = false): string {
  let o = '';
  if (f.direct) o += ` · furthest direct: ${f.direct.name} ${meshDist(f.direct.km, miles)}`;
  if (f.mesh && (!f.direct || f.mesh.km > f.direct.km)) o += ` · ${f.direct ? '' : 'furthest '}via mesh: ${f.mesh.name} ${meshDist(f.mesh.km, miles)}`;
  return o;
}

/** A relays field from the wire or a card ("5a 9f 1c") → hex groups, or undefined. Bounded: 64 hops of 1-3 bytes. */
export function meshRelays(v: unknown): string[] | undefined {
  if (typeof v !== 'string' || v.length > 64 * 7) return undefined;
  const out: string[] = [];
  for (const g of v.trim().toLowerCase().split(/\s+/)) {
    if (!/^(?:[0-9a-f]{2}){1,3}$/.test(g)) return undefined;
    out.push(g);
    if (out.length >= MESH_ROUTE_MAX) break;
  }
  return out.length ? out : undefined;
}

/** A repeater or room server the route may place: its MeshCore id (the key's first bytes, hex), name, position. */
export interface MeshKnown { id: string; name: string; lat: number; lon: number }
type LL = [number, number];
/** One hop of the route: p a placed relay · a ambiguous (candidates, nearest the previous point first) · u unknown. */
export type MeshHop =
  | { k: 'p'; i: number; h: string; p: LL; n: string }
  | { k: 'a'; i: number; h: string; c: LL[]; n: string[]; more: number }
  | { k: 'u'; i: number; h: string };
/** A joining line, lit at tick `t` (hop i lights at tick i; the receiver at hops + 1): from a towards b (the arrowhead's
 *  way — towards the receiver). `dim` = to or from an ambiguous candidate; `dash` = it bridges unknown relays. */
export interface MeshSeg { t: number; a: LL; b: LL; dim: boolean; dash: boolean }
/** A hop's numbered badge, lit at tick `t`; `dim` = one candidate of several. */
export interface MeshMark { t: number; p: LL; i: number; dim: boolean }
export interface MeshRoute {
  o: LL; rx: LL | null; hops: MeshHop[]; seg: MeshSeg[]; marks: MeshMark[]; ticks: number;
  placed: number; ambiguous: number; unknown: number; words: string[];
}

/** "route: 13 hops, 9 placed, 2 ambiguous, 2 unknown" */
export function meshRouteHead(n: number, placed: number, ambiguous: number, unknown: number): string {
  const parts = [`${placed} placed`];
  if (ambiguous) parts.push(`${ambiguous} ambiguous`);
  if (unknown) parts.push(`${unknown} unknown`);
  return `route: ${n} ${n === 1 ? 'hop' : 'hops'}, ${parts.join(', ')}`;
}

/** Match each relay to the known repeaters and lay out the drawing. `node.node` = the node's own id (never its own
 *  relay). Null when the node carries no route (heard first-hand, a direct-routed packet, Meshtastic). */
export function meshRoute(node: { lat: number; lon: number; relays?: string[]; node?: string },
                          known: readonly MeshKnown[], rx: { lat: number; lon: number } | null): MeshRoute | null {
  const rel = node.relays;
  if (!rel || !rel.length || !Number.isFinite(node.lat) || !Number.isFinite(node.lon)) return null;
  const self = (node.node || '').toLowerCase();
  const o: LL = [node.lat, node.lon];
  const hops: MeshHop[] = [], seg: MeshSeg[] = [], marks: MeshMark[] = [];
  let placed = 0, ambiguous = 0, unknown = 0;
  const words: string[] = [];
  // the points the next line starts from (several after an ambiguous hop), and whether unknown relays lie between
  let anchors: LL[] = [o], anchorsDim = false, gap = false;
  const join = (t: number, to: LL, toDim: boolean) => {
    for (const a of anchors) seg.push({ t, a, b: to, dim: anchorsDim || toDim, dash: gap });
  };
  const list = rel.slice(0, MESH_ROUTE_MAX);
  list.forEach((h, j) => {
    const i = j + 1;
    const cands = known.filter((k) => k.id.toLowerCase().startsWith(h) && k.id.toLowerCase() !== self);
    if (cands.length === 1) {
      const c = cands[0], p: LL = [c.lat, c.lon];
      hops.push({ k: 'p', i, h, p, n: c.name }); placed++;
      join(i, p, false); marks.push({ t: i, p, i, dim: false });
      anchors = [p]; anchorsDim = false; gap = false;
      words.push(`${i} · ${h} · ${c.name}`);
    } else if (cands.length > 1) {
      const from = anchors[0];
      const near = [...cands].sort((a, b) => meshKm({ lat: from[0], lon: from[1] }, a.lat, a.lon) - meshKm({ lat: from[0], lon: from[1] }, b.lat, b.lon))
        .slice(0, MESH_CANDS_MAX);
      const c: LL[] = near.map((x) => [x.lat, x.lon]);
      hops.push({ k: 'a', i, h, c, n: near.map((x) => x.name), more: cands.length - near.length }); ambiguous++;
      for (const p of c) { join(i, p, true); marks.push({ t: i, p, i, dim: true }); }
      anchors = c; anchorsDim = true; gap = false;
      words.push(`${i} · ${h} · one of ${near.map((x) => x.name).join(', ')}${cands.length > near.length ? ` (+${cands.length - near.length} more)` : ''}`);
    } else {
      hops.push({ k: 'u', i, h }); unknown++; gap = true;
      words.push(`${i} · ${h} · unknown repeater 0x${h}`);
    }
  });
  const ticks = list.length + 1;
  const rxp: LL | null = rx ? [rx.lat, rx.lon] : null;
  if (rxp) join(ticks, rxp, false);
  return { o, rx: rxp, hops, seg, marks, ticks, placed, ambiguous, unknown,
           words: [meshRouteHead(list.length, placed, ambiguous, unknown), ...words] };
}

/* ★★ THE DRAWING, AS PAGE TEXT — one copy for every map that shows a route: the web's track view (tracks.ts runs it),
 *  the app's WebView map page (MapOverlay carries it) and the AMATEUR map's pages (beside AMATEUR_JS). Written against
 *  the slice of Leaflet that vibemapgl-compat.js also answers (polyline + dashArray, marker + divIcon). ES5; no
 *  backticks, no backslashes, no template holes, no closing script tag.
 *    var h = meshRouteDraw(map, L, route);   // route = meshRoute(...) — lights tick by tick (300 ms), then holds
 *    h.clear();                              // gone
 *  JOINING LINES (Stuart 11-26): node → relay 1 → … → receiver, each segment as its hop lights: solid between placed
 *  hops; dimmed to and from ambiguous candidates; dashed across unknown relays; a small arrowhead mid-segment pointing
 *  towards the receiver. Nothing from the air reaches HTML here: badges are numbers, the rest geometry. */
export const MESH_ROUTE_JS = `
function meshRouteDraw(map,L,rt){
  var layers=[],timers=[],dead=false,COL='#ffd34d';
  var reduce=false;try{reduce=!!(window.matchMedia&&window.matchMedia('(prefers-reduced-motion: reduce)').matches);}catch(e){}
  var STEP=reduce?0:300;
  function num(v){v=Number(v);return isFinite(v)?v:0;}
  function ll(p){return (p&&p.length===2)?[num(p[0]),num(p[1])]:null;}
  function add(l){if(dead)return l;try{l.addTo(map);layers.push(l);}catch(e){}return l;}
  function merc(p){var r=Math.PI/180;return [p[1]*r,Math.log(Math.tan(Math.PI/4+Math.max(-1.5,Math.min(1.5,p[0]*r))/2))];}
  function arrow(a,b,dim){var A=merc(a),B=merc(b);var ang=Math.atan2(-(B[1]-A[1]),B[0]-A[0])*180/Math.PI;
    var html='<svg width="12" height="12" viewBox="0 0 12 12" style="display:block;transform:rotate('+ang.toFixed(1)+'deg);opacity:'+(dim?0.5:1)+'"><path d="M2 2 L10 6 L2 10 Z" fill="'+COL+'" stroke="#1a1200" stroke-width="1"/></svg>';
    add(L.marker([(a[0]+b[0])/2,(a[1]+b[1])/2],{icon:L.divIcon({className:'vmr-ar',html:html,iconSize:[12,12],iconAnchor:[6,6]}),interactive:false,keyboard:false}));}
  function seg(s){var a=ll(s.a),b=ll(s.b);if(!a||!b)return;var st={color:COL,weight:s.dim?2:3,opacity:s.dim?0.45:0.95,fill:false};if(s.dash)st.dashArray='6 6';
    add(L.polyline([a,b],st));arrow(a,b,!!s.dim);}
  function badge(m){var p=ll(m.p);if(!p)return;var html='<span style="display:block;min-width:16px;height:16px;padding:0 3px;box-sizing:border-box;border-radius:8px;background:'+COL+';color:#111;font:700 10px/16px ui-monospace,Menlo,Consolas,monospace;text-align:center;opacity:'+(m.dim?0.55:1)+';box-shadow:0 0 0 1.5px #0b1420">'+String(Math.round(num(m.i)))+'</span>';
    add(L.marker(p,{icon:L.divIcon({className:'vmr-bd',html:html,iconSize:[18,16],iconAnchor:[9,24]}),interactive:false,keyboard:false}));}
  var segs=(rt&&rt.seg)||[],marks=(rt&&rt.marks)||[],n=Math.min(70,Math.max(1,Math.round(num(rt&&rt.ticks))));
  function tick(t){if(dead)return;var i;for(i=0;i<segs.length&&i<600;i++)if(segs[i].t===t)seg(segs[i]);for(i=0;i<marks.length&&i<400;i++)if(marks[i].t===t)badge(marks[i]);}
  for(var t=1;t<=n;t++)(function(t){if(!STEP||t===1)tick(t);else timers.push(setTimeout(function(){tick(t);},(t-1)*STEP));})(t);
  return {clear:function(){dead=true;for(var q=0;q<timers.length;q++)clearTimeout(timers[q]);timers=[];
    for(var k=0;k<layers.length;k++){try{layers[k].remove();}catch(e){}}layers=[];}};
}
`;
