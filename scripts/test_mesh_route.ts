// test_mesh_route.ts — ★★ beta96 THE MESH MAP's shared rules (src/utils/meshRoute.ts, docs/v12/MESH-ROUTE.md), Stuart 11-26:
//   names kept as advertised ("t::1") and only unprintable ones cleaned · furthest DIRECT vs VIA MESH (implausible fixes
//   never count) · the flood path as MeshCore's packet format lays it out · relays matched to advertised repeaters
//   (unique / ambiguous / unknown) · the ordered drawing data (segments, badges, ticks) · MESH_ROUTE_JS played on a fake
//   map · km ↔ miles on every map surface (one preference) · the web and app copies agree.
//   npx tsx scripts/test_mesh_route.ts
import vm from 'node:vm';
import { readFileSync } from 'node:fs';
import * as web from '../web/client/src/ism.ts';
import * as app from '../src/services/ism.ts';
import { meshName, meshPlausible, meshKm, meshDist, meshFurthest, meshFarText, meshRelays, meshRoute, MESH_ROUTE_JS,
         meshSortRows, meshSortOffered, meshSortParse, type MeshKnown } from '../src/utils/meshRoute.ts';
import { amMeshPoints, amListRows } from '../src/utils/amateurMap.ts';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`FAIL ${what}\n     got  ${g}\n     want ${w}`); }
};
const ok = (what: string, c: boolean) => eq(what, c, true);

// ── 1. NAMES ──────────────────────────────────────────────────────────────────────────────────────────────────────────
eq('t::1 is a real name and stays exactly that', meshName('t::1'), 't::1');
eq('an emoji name is kept', meshName('Woolwich Sun Bear 🐻'), 'Woolwich Sun Bear 🐻');
eq('punctuation kept', meshName("Syston Repeater | Brist"), 'Syston Repeater | Brist');
eq('quotes and angle brackets kept (escaped where shown)', meshName('Bob\'s <relay>'), 'Bob\'s <relay>');
eq('a family emoji (ZWJ) kept whole', meshName('Home 👨‍👩‍👧'), 'Home 👨‍👩‍👧');
eq('control characters dropped', meshName('Hill\u0000\u0007 Rptr\u001b'), 'Hill Rptr');
eq('private-use and U+FFFD dropped', meshName('X�Y'), 'XY');
eq('binary rubbish → nothing usable', meshName('\u0001\u0002�'), '');
eq('whitespace only → nothing usable', meshName('  ​ '), '');
eq('over-long names cut to 40 characters', Array.from(meshName('A'.repeat(90))).length, 40);
eq('cut by characters, never mid-emoji', meshName('🐻'.repeat(50)), '🐻'.repeat(40));
eq('not a string', meshName(42), '');
{
  const card = 'Receiver:     52.3125, -0.8750\n\nEach node, most recent first (last heard · first heard · times heard · heard by · network · type · name · id · position · fewest hops · SNR dB · MHz):\n'
    + '16:00:00 · 15:00:00 · 1× · live · MeshCore · repeater · \u0001\u0002 · 8d76bb29 · 50.592710, -4.822830 · 13 · -7.0 · 869.619\n'
    + '16:00:00 · 15:00:00 · 1× · live · MeshCore · repeater · t::1 · 8d76bb30 · 50.592710, -4.822830 · 13 · -7.0 · 869.619\n';
  const w = web.ismNodesParse(card, 0, 60000), a = app.ismNodesParse(card, 0, 60000);
  eq('an unprintable name shows the short key id (web)', w.nodes[0].name, '8d76bb29');
  eq('…and t::1 stays t::1', w.nodes[1].name, 't::1');
  eq('…the app the same', JSON.stringify(a), JSON.stringify(w));
  eq('the map label of a live node with a binary long name falls back to its short name, then its id',
     [web.ismMapLabel({ ...w.nodes[0], long: '\u0001', short: '', name: '' }), app.ismMapLabel({ ...a.nodes[0], long: '\u0001', short: 'AB', name: '' })], ['8d76bb29', 'AB']);
}

// ── 2. PLAUSIBILITY + FURTHEST DIRECT / VIA MESH ───────────────────────────────────────────────────────────────────────
const RX = { lat: 52.3125, lon: -0.875 };   // the Pi 500 (its card's Receiver line)
type N = { key: string; kind: string; name: string; long?: string; node: string; ntype: string; lat: number; lon: number; hops?: number; direct?: number; relays?: string[] };
const dev = (n: N): app.IsmDevice => ({ make: 'MeshCore', model: '', line: '', freq: 869.618e6, level: -99, snr: -5, seen: 60, age: 600, count: 2, ...n, long: n.long ?? n.name } as app.IsmDevice);
const nise = dev({ key: 'c:92771c35', kind: 'meshcore', name: 'N-Ise repeater NN29', node: '92771c35', ntype: 'repeater', lat: 52.271268, lon: -0.645521, hops: 0 });
const t1 = dev({ key: 'c:8d76bb29', kind: 'meshcore', name: 't::1', node: '8d76bb29', ntype: 'repeater', lat: 50.59271, lon: -4.82283, hops: 13 });
const typo = dev({ key: 'c:0bad0bad', kind: 'meshcore', name: 'Typo', node: '0bad0bad', ntype: 'repeater', lat: -33.9, lon: 151.2, hops: 1 });
const relayedOnce = dev({ key: 'c:11223344', kind: 'meshcore', name: 'Daventry', node: '11223344', ntype: 'repeater', lat: 52.28, lon: -0.80, hops: 2, direct: 1 });
eq('N-Ise is 16 km', Math.round(meshKm(RX, nise.lat!, nise.lon!)), 16);
eq('t::1 is 334 km', Math.round(meshKm(RX, t1.lat!, t1.lon!)), 334);
ok('334 km at 13 hops is plausible', meshPlausible(334, 13));
ok('Sydney at 1 hop is not', !meshPlausible(meshKm(RX, typo.lat!, typo.lon!), 1));
ok('600 km first-hand is not', !meshPlausible(600, 0));
ok('2900 km with hops unknown is; 3100 is not', meshPlausible(2900, undefined) && !meshPlausible(3100, undefined));
const far = meshFurthest([nise, t1, typo, relayedOnce], RX, (d) => d.name);
eq('furthest DIRECT = N-Ise 16 km (Daventry, ever first-hand, is direct too but nearer)', [far.direct?.name, Math.round(far.direct!.km)], ['N-Ise repeater NN29', 16]);
eq('furthest VIA MESH = t::1 334 km; Sydney never counts', [far.mesh?.name, Math.round(far.mesh!.km)], ['t::1', 334]);
eq('the header, km', meshFarText(far), ' · furthest direct: N-Ise repeater NN29 16 km · via mesh: t::1 334 km');
eq('the header, miles', meshFarText(far, true), ' · furthest direct: N-Ise repeater NN29 10 mi · via mesh: t::1 207 mi');
eq('only via mesh heard → "furthest via mesh:"', meshFarText(meshFurthest([t1], RX, (d) => d.name)), ' · furthest via mesh: t::1 334 km');
eq('via mesh nearer than direct → said once', meshFarText({ direct: { name: 'A', km: 50 }, mesh: { name: 'B', km: 20 } }), ' · furthest direct: A 50 km');
eq('no receiver → nothing', meshFarText(meshFurthest([nise, t1], null, (d) => d.name)), '');
const rep = { band: '868', devices: [nise, t1, typo, relayedOnce] } as unknown as app.IsmReport;
eq('the live map status (app)', app.ismMapStatus(rep, RX), '3 on the map · 3 repeaters · furthest direct: N-Ise repeater NN29 16 km · via mesh: t::1 334 km');
eq('…web the same', web.ismMapStatus(rep as unknown as web.IsmReport, RX), app.ismMapStatus(rep, RX));
eq('Sydney is not plotted (app items)', app.ismMapItems(rep, RX).map((i) => i.key), ['c:92771c35', 'c:8d76bb29', 'c:11223344']);
ok('…its card says why', app.ismLoraDetail(typo, RX).some((l) => /position implausible \(not plotted\)/.test(l)));
ok('a node ever heard first-hand says so', app.ismLoraDetail(relayedOnce, RX)[0].includes('2 hops (also heard first-hand)'));

// ── 3. THE PATH, AS MESHCORE'S PACKET FORMAT LAYS IT OUT ──────────────────────────────────────────────────────────────
// packet_format.md: header (bits 0-1 route type: 0 TRANSPORT_FLOOD, 1 FLOOD, 2 DIRECT, 3 TRANSPORT_DIRECT; bits 2-5
// payload type, 4 = ADVERT; bits 6-7 version) · [4 transport-code bytes for the TRANSPORT_ types] · path_len (bits 0-5
// the hop count, bits 6-7 the hash size − 1) · path (hop count × hash size bytes, each relay appending its hash) · payload.
// This reader states that layout for the fixtures; the server's own (vibe_meshcore.h) is tested on the same bytes by
// test-lora-nodes. What the server sends — "5a 9f 1c" — is what meshRelays must take.
function packet(route: number, hashSize: number, path: number[], payload = [0xaa, 0xbb]): Uint8Array {
  const hops = path.length / hashSize;
  return Uint8Array.from([(4 << 2) | route, ...(route === 0 || route === 3 ? [1, 2, 3, 4] : []), ((hashSize - 1) << 6) | hops, ...path, ...payload]);
}
function relaysOf(b: Uint8Array): string {
  const route = b[0] & 3; let i = 1;
  if (route === 0 || route === 3) i += 4;
  const hops = b[i] & 63, hs = (b[i] >> 6) + 1; i++;
  if (hs === 4 || hops * hs > 64 || i + hops * hs > b.length) return 'bad';
  if (route === 2 || route === 3) return '';   // a DIRECT packet's path is the route still to go, not one travelled
  const out: string[] = [];
  for (let h = 0; h < hops; h++) out.push(Array.from(b.slice(i + h * hs, i + (h + 1) * hs), (x) => x.toString(16).padStart(2, '0')).join(''));
  return out.join(' ');
}
eq('FLOOD, 1-byte hashes', relaysOf(packet(1, 1, [0x5a, 0x9f, 0x1c])), '5a 9f 1c');
eq('TRANSPORT_FLOOD (transport codes skipped)', relaysOf(packet(0, 1, [0x5a, 0x9f])), '5a 9f');
eq('2-byte hashes', relaysOf(packet(1, 2, [0xab, 0xcd, 0x01, 0x02])), 'abcd 0102');
eq('3-byte hashes', relaysOf(packet(1, 3, [0xab, 0xcd, 0xef])), 'abcdef');
eq('DIRECT: no route travelled', relaysOf(packet(2, 1, [0x5a, 0x9f])), '');
eq('first-hand: no relays', relaysOf(packet(1, 1, [])), '');
eq('meshRelays takes the server\'s field', meshRelays(relaysOf(packet(1, 1, [0x5a, 0x9f, 0x1c]))), ['5a', '9f', '1c']);
eq('…upper case folded', meshRelays('5A 9F'), ['5a', '9f']);
eq('…mixed widths (as written)', meshRelays('abcd 01'), ['abcd', '01']);
eq('…odd digits refused', meshRelays('5a 9'), undefined);
eq('…4-byte hash refused', meshRelays('01020304'), undefined);
eq('…non-hex refused', meshRelays('5a zz'), undefined);
eq('…empty = none', meshRelays(''), undefined);
eq('…capped at 64 hops', meshRelays(Array(64).fill('7f').join(' '))?.length, 64);
eq('…over-long text refused', meshRelays('7f '.repeat(200)), undefined);
eq('the wire (ismSanitise) carries relays and direct', (() => {
  const r = app.ismSanitise({ type: 'ism', band: '868', devices: [{ key: 'k', kind: 'meshcore', name: 'X', line: '', freq: 869.6e6, level: -90, snr: 3, seen: 1, age: 1, count: 1,
                                                       node: '8d76bb29', relays: '5a 9f 1c', direct: 1, hops: 3 }] });
  const d = r?.devices[0]; return [d?.relays, d?.direct];
})(), [['5a', '9f', '1c'], 1]);

// ── 4. MATCHING + THE ORDERED DRAWING ─────────────────────────────────────────────────────────────────────────────────
// 13 relays like t::1's: 9 that one advertised repeater answers, 2 that two repeaters share a first byte for (ambiguous),
// 2 that nothing heard answers (unknown). Positions: a chain from Cornwall towards the receiver.
const ids = ['a1', 'b2', 'c3', 'd4', 'e5', 'f6', '17', '28', '39'];
const known: MeshKnown[] = ids.map((h, i) => ({ id: `${h}00ff00`, name: `R${i + 1}`, lat: 50.7 + i * 0.15, lon: -4.6 + i * 0.4 }));
known.push({ id: '4a000001', name: 'Twin A', lat: 51.2, lon: -3.0 }, { id: '4a000002', name: 'Twin B', lat: 51.9, lon: -1.0 });
known.push({ id: '5b000001', name: 'Pair 1', lat: 51.3, lon: -2.6 }, { id: '5b000002', name: 'Pair 2', lat: 52.0, lon: -2.0 });
const path13 = ['a1', 'b2', '4a', 'c3', '0e', 'd4', 'e5', '5b', 'f6', '17', '0f', '28', '39'];
const rt = meshRoute({ lat: t1.lat!, lon: t1.lon!, relays: path13, node: '8d76bb29' }, known, RX)!;
eq('the route card head', rt.words[0], 'route: 13 hops, 9 placed, 2 ambiguous, 2 unknown');
eq('the hops, in order', rt.hops.map((h) => `${h.i}${h.k}`), path13.map((_, j) => `${j + 1}${['4a', '5b'].includes(path13[j]) ? 'a' : ['0e', '0f'].includes(path13[j]) ? 'u' : 'p'}`));
eq('…placed names', rt.hops.filter((h) => h.k === 'p').map((h) => (h as { n: string }).n), ['R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9']);
eq('…an unknown hop says its byte', rt.words[5], '5 · 0e · unknown repeater 0x0e');
eq('…an ambiguous hop names its candidates, nearest the previous stop first', rt.words[3], '3 · 4a · one of Twin A, Twin B');
// segments: the order, the styles
eq('segments light in order (tick never goes back)', rt.seg.every((s, i) => i === 0 || s.t >= rt.seg[i - 1].t), true);
eq('first segment: the node → R1, solid, lit at tick 1', [rt.seg[0].t, rt.seg[0].a, rt.seg[0].b, rt.seg[0].dim, rt.seg[0].dash],
   [1, [t1.lat, t1.lon], [known[0].lat, known[0].lon], false, false]);
const at = (t: number) => rt.seg.filter((s) => s.t === t);
eq('tick 3 (ambiguous 4a): a dimmed line from R2 to EACH candidate', at(3).map((s) => [s.a, s.b, s.dim, s.dash]),
   [[[known[1].lat, known[1].lon], [51.2, -3.0], true, false], [[known[1].lat, known[1].lon], [51.9, -1.0], true, false]]);
eq('tick 4 (R3): dimmed from each candidate (still uncertain), solid after', at(4).map((s) => s.dim), [true, true]);
eq('tick 5 (unknown 0e): nothing drawn yet', at(5).length, 0);
eq('tick 6 (R4): ONE dashed line across the unknown hop', at(6).map((s) => [s.a, s.b, s.dim, s.dash]), [[[known[2].lat, known[2].lon], [known[3].lat, known[3].lon], false, true]]);
eq('tick 7 (R5): solid again', at(7).map((s) => [s.dim, s.dash]), [[false, false]]);
eq('tick 12 (R8, after unknown 0f): dashed', at(12).map((s) => s.dash), [true]);
eq('the receiver lights last (tick 14), solid from R9', at(14).map((s) => [s.a, s.b, s.dim, s.dash]), [[[known[8].lat, known[8].lon], [RX.lat, RX.lon], false, false]]);
eq('ticks = hops + 1', rt.ticks, 14);
eq('badges: one per placed hop, one per candidate (dimmed), none for unknown', [rt.marks.filter((m) => !m.dim).length, rt.marks.filter((m) => m.dim).length], [9, 4]);
eq('…numbered by hop', rt.marks.map((m) => m.i), [1, 2, 3, 3, 4, 6, 7, 8, 8, 9, 10, 12, 13]);
eq('total segments (ticks 1…14: 1 1 2 2 0 1 1 2 2 1 0 1 1 1)', rt.seg.length, 16);
eq('a node is never its own relay', meshRoute({ lat: 50, lon: -4, relays: ['8d'], node: '8d76bb29' }, [{ id: '8d76bb29', name: 'self', lat: 50, lon: -4 }], RX)!.words[0], 'route: 1 hop, 0 placed, 1 unknown');
eq('no relays → no route', meshRoute({ lat: 50, lon: -4, relays: [] }, known, RX), null);
eq('no receiver → no last line', meshRoute({ lat: 50, lon: -4, relays: ['a1'] }, known, null)!.seg.length, 1);
{ // ambiguous candidates capped (6) with "+N more"
  const many: MeshKnown[] = Array.from({ length: 9 }, (_, i) => ({ id: `77${i}0`, name: `M${i}`, lat: 51 + i * 0.1, lon: -1 }));
  const r = meshRoute({ lat: 51, lon: -1, relays: ['77'] }, many, RX)!;
  ok('candidates capped at 6, "+3 more"', /one of M0, M1, M2, M3, M4, M5 \(\+3 more\)$/.test(r.words[1]) && r.marks.length === 6);
}
// privacy: a companion with a matching key is never a route stop (ismRouteKnown takes repeaters and room servers only)
{
  const comp = dev({ key: 'c:a1ffffff', kind: 'meshcore', name: 'Alice', node: 'a1ffffff', ntype: 'companion', lat: 52.24, lon: -0.89 });
  const room = dev({ key: 'c:b2eeeeee', kind: 'meshcore', name: 'Room', node: 'b2eeeeee', ntype: 'room', lat: 52.1, lon: -1.0 });
  const kn = app.ismRouteKnown([comp, room, nise, typo], RX);
  eq('known stops: repeaters and room servers, plotted ones only (no companion, no Sydney)', kn.map((k) => k.name).sort(), ['N-Ise repeater NN29', 'Room']);
  eq('…web the same', web.ismRouteKnown([comp, room, nise, typo] as unknown as web.IsmDevice[], RX), kn);
}

// ── 5. MESH_ROUTE_JS, PLAYED ON A FAKE MAP ────────────────────────────────────────────────────────────────────────────
{
  const added: Array<{ kind: string; st?: any; html?: string; removed: boolean }> = [];
  const timers: Array<{ at: number; fn: () => void; id: number; dead: boolean }> = [];
  let tid = 0;
  const layer = (kind: string, st?: any, html?: string) => { const l = { kind, st, html, removed: false };
    return { addTo() { added.push(l); return this; }, remove() { l.removed = true; } }; };
  const L = { polyline: (_p: unknown, st: any) => layer(st.dashArray ? 'dash' : st.opacity < 0.9 ? 'dim' : 'line', st),
              marker: (_p: unknown, o: any) => layer(o.icon.options.className, undefined, o.icon.options.html), divIcon: (o: any) => ({ options: o }) };
  const sb: any = { window: {}, Math, Number, String, isFinite, console,
                    setTimeout: (fn: () => void, ms: number) => { const t = { at: ms, fn, id: ++tid, dead: false }; timers.push(t); return t.id; },
                    clearTimeout: (id: number) => { const t = timers.find((x) => x.id === id); if (t) t.dead = true; } };
  vm.runInNewContext(MESH_ROUTE_JS + '\nthis.meshRouteDraw = meshRouteDraw;', sb);
  const h = sb.meshRouteDraw({}, L, JSON.parse(JSON.stringify(rt)));
  eq('page text: tick 1 drawn at once (the first line + R1 badge + its arrow)', added.map((a) => a.kind), ['line', 'vmr-ar', 'vmr-bd']);
  eq('the rest wait, one tick each 300 ms', timers.map((t) => t.at), Array.from({ length: 13 }, (_, i) => (i + 1) * 300));
  for (const t of timers.slice(0, 5)) t.fn();   // ticks 2-6
  eq('by tick 6: the dashed bridge across the unknown hop is drawn', added.filter((a) => a.kind === 'dash').length, 1);
  const c = h; c.clear();
  eq('clear(): every layer removed', added.every((a) => a.removed), true);
  eq('…and the ticks still to come cancelled', timers.slice(5).every((t) => t.dead), true);
  const n0 = added.length; for (const t of timers.slice(5)) t.fn();
  eq('…a late tick draws nothing', added.length, n0);
  added.length = 0; timers.length = 0;
  sb.meshRouteDraw({}, L, JSON.parse(JSON.stringify(rt)));
  for (const t of timers) t.fn();
  eq('played through: one polyline per segment', added.filter((a) => ['line', 'dim', 'dash'].includes(a.kind)).length, rt.seg.length);
  eq('…an arrowhead per segment, pointing on (towards the receiver)', added.filter((a) => a.kind === 'vmr-ar').length, rt.seg.length);
  eq('…a badge per mark, its hop number', added.filter((a) => a.kind === 'vmr-bd').map((a) => Number(/>(\d+)</.exec(a.html!)![1])), rt.marks.map((m) => m.i));
  eq('…dashed lines = segments across unknown hops', added.filter((a) => a.kind === 'dash').length, rt.seg.filter((s) => s.dash).length);
  ok('no backtick, backslash or closing script tag in the page text', !/[`\\]|<\/script/i.test(MESH_ROUTE_JS));
  // reduced motion: everything at once
  added.length = 0; timers.length = 0;
  sb.window.matchMedia = () => ({ matches: true });
  sb.meshRouteDraw({}, L, JSON.parse(JSON.stringify(rt)));
  eq('reduced motion: drawn at once, no timers', [timers.length, added.filter((a) => a.kind === 'vmr-bd').length], [0, rt.marks.length]);
  // ★★ THE VIEW FITS THE WHOLE PATH when the route is shown (Stuart 11-26): a map that can fly to bounds is asked to, once,
  //    a moment after the press (so it follows the layer's own fly to the node), with the route's fit points
  sb.window.matchMedia = () => ({ matches: false });
  added.length = 0; timers.length = 0;
  const flown: any[] = [];
  const L2 = { ...L, latLngBounds: (pts: any) => ({ pts }) };
  sb.meshRouteDraw({ flyToBounds: (b: any, o: any) => { flown.push({ b, o }); } }, L2, JSON.parse(JSON.stringify(rt)));
  const fitT = timers.find((t) => t.at === 60);
  ok('a fit is scheduled 60 ms after the press', !!fitT && flown.length === 0);
  fitT?.fn();
  eq('…to the route\'s fit points, padded, never past zoom 11', [flown.length, JSON.stringify(flown[0]?.b.pts) === JSON.stringify(rt.fit), flown[0]?.o.maxZoom, flown[0]?.o.paddingTopLeft], [1, true, 11, { x: 48, y: 48 }]);
}
// ── 5b. WHICH POINTS THE VIEW FITS ─────────────────────────────────────────────────────────────────────────────────────
{
  const has = (pts: Array<[number, number]>, p: [number, number]) => pts.some((q) => q[0] === p[0] && q[1] === p[1]);
  ok('the fit holds the node, every placed hop and the receiver', has(rt.fit, rt.o) && rt.hops.every((h) => h.k !== 'p' || has(rt.fit, h.p)) && has(rt.fit, [RX.lat, RX.lon]));
  ok('…and the ambiguous candidates here (they sit inside the path\'s box)', rt.hops.every((h) => h.k !== 'a' || h.c.every((c) => has(rt.fit, c))));
  // a candidate far away would blow the view up: left out, the placed hops kept
  const far2: MeshKnown[] = [{ id: 'a1000000', name: 'Near', lat: 52.0, lon: -1.0 }, { id: '77000001', name: 'Here', lat: 52.1, lon: -0.9 }, { id: '77000002', name: 'Shetland', lat: 60.2, lon: -1.2 }];
  const r2 = meshRoute({ lat: 51.9, lon: -1.1, relays: ['a1', '77'] }, far2, RX)!;
  ok('a far candidate (Shetland) is left out of the fit', !has(r2.fit, [60.2, -1.2]) && has(r2.fit, [52.0, -1.0]) && has(r2.fit, [RX.lat, RX.lon]) && has(r2.fit, [51.9, -1.1]));
  ok('…but still drawn (dimmed) — only the view ignores it', r2.marks.some((m) => m.p[0] === 60.2 && m.dim));
}
// ── 5c. THE CARD's ROUTE BUTTON — only on a node with a path (no dead control) ────────────────────────────────────────
{
  const rtT1 = meshRoute({ lat: t1.lat!, lon: t1.lon!, relays: ['92'], node: '8d76bb29' }, app.ismRouteKnown([nise], RX), RX);
  const withBtn = app.ismMapCard(t1, RX, false, rtT1), noBtn = app.ismMapCard(nise, RX, false, null);
  ok('a node with a path: a ROUTE button keyed to it (app card)', /<button type="button" class="vmrBtn" data-rtk="c:8d76bb29" aria-pressed="false"[^>]*>ROUTE<\/button>/.test(withBtn));
  ok('a node without one: no button', !/vmrBtn/.test(noBtn));
  ok('the web card the same', /class="vmrBtn" data-rtk="c:8d76bb29"/.test(web.ismMapCard(t1 as unknown as web.IsmDevice, RX, false, rtT1)) && !/vmrBtn/.test(web.ismMapCard(nise as unknown as web.IsmDevice, RX)));
  ok('the key is escaped in the attribute', /data-rtk="c:a&quot;b"/.test(app.ismMapCard({ ...t1, key: 'c:a"b' }, RX, false, rtT1)));
}
{
}

// ── 6. THE CARDS: ROUTES SECTION ROUND TRIP, ROUTE CARD ───────────────────────────────────────────────────────────────
{
  const card = 'Receiver:     52.3125, -0.8750\n\nEach node, most recent first (last heard · first heard · times heard · heard by · network · type · name · id · position · fewest hops · SNR dB · MHz):\n'
    + '16:14:11 · 16:14:11 · 1× · live · MeshCore · repeater · MW3ASG Repeater · 3252aed0 · 51.533397, -3.094019 · 3 · -10.5 · 869.618\n'
    + '16:12:37 · 16:12:35 · 2× · live · MeshCore · repeater · Syston Repeater | Brist · 70504539 · 51.467898, -2.505826 · 0 · -6.7 · 869.619\n'
    + '16:11:35 · 16:11:27 · 2× · live · MeshCore · repeater · N-Ise repeater NN29 · 92771c35 · 52.271268, -0.645521 · 0 · -6.3 · 869.618\n'
    + '\nRoutes heard (id · the relays of its newest packet, first relay first; each the first byte(s) of a repeater\'s key):\n'
    + '3252aed0 · 70 0e 92\nnope · zz\n';
  const g = web.ismNodesParse(card, 0, 60000), ga = app.ismNodesParse(card, 0, 60000);
  eq('the Routes section read back onto its node', g.nodes[0].relays, ['70', '0e', '92']);
  eq('…app the same', JSON.stringify(ga), JSON.stringify(g));
  const items = app.ismGalItems(ga.nodes, ga.rx);
  const it = items.find((i) => i.key === g.nodes[0].key)!;
  eq('the gallery map item carries its route', it._rt?.words[0], 'route: 3 hops, 2 placed, 1 unknown');
  ok('…its card says it', /route: 3 hops, 2 placed, 1 unknown<br>1 · 70 · Syston Repeater \| Brist<br>2 · 0e · unknown repeater 0x0e<br>3 · 92 · N-Ise repeater NN29/.test(it._cd));
  ok('the web gallery card too', /route: 3 hops, 2 placed, 1 unknown/.test(web.ismGalCard(g.nodes[0], g.rx, false, web.ismRouteOf(g.nodes[0], web.ismRouteKnown(g.nodes, g.rx), g.rx))));
  eq('gallery status: furthest direct and via mesh', app.ismGalStatus(ga.nodes, ga.rx), '3 nodes heard · 3 repeaters · furthest direct: Syston Repeater | Brist 146 km · via mesh: MW3ASG Repeater 175 km');
  eq('…web the same', web.ismGalStatus(g.nodes, g.rx), app.ismGalStatus(ga.nodes, ga.rx));
  // merging days: the fewest hops, the newer day's route
  const older = { ...ga.nodes[0], last: ga.nodes[0].last - 86400, first: ga.nodes[0].first - 86400, hops: 0, relays: ['11'] };
  const m = app.ismGalMerge([[older], [ga.nodes[0]]], 60000)[0];
  eq('merged days: fewest hops 0, the newer route', [m.hops, m.relays], [0, ['70', '0e', '92']]);
}

// ── 7. KM ↔ MILES on every mesh surface (one preference) — never NM ───────────────────────────────────────────────────
eq('meshDist', [meshDist(16.09), meshDist(16.09, true), meshDist(5), meshDist(5, true)], ['16 km', '10.0 mi', '5.0 km', '3.1 mi']);
{
  const st = app.ismMapStatus(rep, RX, true);
  ok('live status in miles', / 10 mi · via mesh: t::1 207 mi$/.test(st) && !/km|NM/.test(st));
  eq('…web the same', web.ismMapStatus(rep as unknown as web.IsmReport, RX, true), st);
  eq('app NODES rows in miles, nearest first', app.ismMapRows(rep, RX, true).map((r) => r.range), ['3.9 mi', '10 mi', '207 mi']);
  ok('web NODES rows in miles', (() => { const h = web.ismMapRows(rep as unknown as web.IsmReport, RX, true); return /10 mi <span class="brg">/.test(h) && /207 mi/.test(h) && !/ km /.test(h); })());
  ok('the card in miles (app + web)', /10 mi [NESW]/.test(app.ismMapCard(nise, RX, true)) && /10 mi [NESW]/.test(web.ismMapCard(nise as unknown as web.IsmDevice, RX, true)));
  ok('…and in km by default', /16 km [NESW]/.test(app.ismMapCard(nise, RX)));
  ok('the ISM list detail follows the same switch', app.ismLoraDetail(nise, RX, true, true).some((l) => /10 mi/.test(l)));
  // the stored preference: the ISM list's mph flag IS the maps' miles
  eq('app: the stored ISM prefs drive it', [app.parseIsmPrefs('{"mph":true}').units.mph, app.parseIsmPrefs('{}').units.mph], [true, false]);
  let heard: boolean | null = null; const off = app.ismMilesOn((m) => { heard = m; });
  app.ismMilesSet(true); eq('app: the maps hear the switch', [heard, app.ismMilesGet()], [true, true]);
  off(); app.ismMilesSet(false); eq('…until they stop listening', heard, true);
  // the AMATEUR map's MESH tab
  const pts = amMeshPoints([nise, t1, typo].map((d) => ({ ...d, age: 60 })), [], 0, { rx: RX, miles: true });
  eq('AMATEUR MESH: miles, Sydney left out', pts.map((p) => [p.name, p.dist]), [['N-Ise repeater NN29', '10 mi'], ['t::1', '207 mi']]);
  eq('…its list rows show them', amListRows('mesh', { mesh: pts }, 'all', 0, () => '#fff').map((r) => r.range), ['10 mi', '207 mi']);
  const ptsKm = amMeshPoints([nise], [], 0, { rx: RX });
  eq('…km by default', ptsKm[0].dist, '16 km');
  const withRt = amMeshPoints([{ ...t1, age: 60, relays: ['92'] }, { ...nise, age: 60 }], [], 0, { rx: RX });
  eq('…a MeshCore node carries its route, matched to the repeaters on the map', withRt.find((p) => p.name === 't::1')?.rw?.slice(0, 2), ['route: 1 hop, 1 placed', '1 · 92 · N-Ise repeater NN29']);
}

// ── 8. THE APP's MAP PAGES carry it (MapOverlay.tsx is page text: read as source) ─────────────────────────────────────
{
  const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
  const am = readFileSync(new URL('../src/utils/amateurMap.ts', import.meta.url), 'utf8');
  ok('app ISM page: MESH_ROUTE_JS carried on the ism page', mo.includes("${kind === 'ism' ? MESH_ROUTE_JS : ''}"));
  ok('app ISM page: a tap opens the card and ZOOMS IN (the layer\'s focus), and clears another node\'s route — it never draws one',
     mo.includes("if(KIND==='ism'){if(trkRtKey&&String(k)!==trkRtKey)trkRoute(null);") && mo.includes("if(k!=null)setTimeout(function(){try{if(trkLayer)trkLayer.focus(String(k));}catch(e){}},0);}"));
  ok('app ISM page: the card\'s ROUTE key toggles it', mo.includes("rb.addEventListener('click',function(ev){ev.stopPropagation();trkRoute(rb.getAttribute('data-rtk'));});"));
  ok('app: the pad\'s ROUTE key in the list, only for a node with a route', /route=\{kind === 'ism' && trkSel && trk\.items\.some\(\(it\) => it\.key === trkSel && !!\(it as \{ _rt\?: unknown \}\)\._rt\)/.test(mo)
     && readFileSync(new URL('../src/components/TrackLiveList.tsx', import.meta.url), 'utf8').includes("{open && !!p.route && ("));
  ok('app AMATEUR: the pad\'s ROUTE key for a chosen mesh node with a route', mo.includes('route={amSel && amateur?.mesh.some((x) => x.key === amSel && !!x.rt)'));
  ok('app ISM page: each item\'s _rt kept by key', /trkRt\[String\(a\[r0\]\.key\)\]=a\[r0\]\._rt/.test(mo));
  ok('AMATEUR pages: MESH_ROUTE_JS rides with AMATEUR_JS', /export const AMATEUR_JS = MESH_ROUTE_JS \+/.test(am));
  ok('AMATEUR pages: a mesh marker tap opens its card and zooms in (amMeshPick) — no route yet', am.includes("AM_MK[n.key].on('click',function(){amMeshPick(n);})") && mo.includes('amRouteBind();'));
  ok('AMATEUR: the card\'s ROUTE only on a node with a path', am.includes("+(n.rt?'<div class=\"vmrRow\"><button type=\"button\" class=\"vmrBtn\" data-amk=\"'+esc(n.key)"));
  ok('AMATEUR: a chosen node holds the auto fit; the card closing (or the empty map) clears the route and resumes it',
     /function amCardClosed\(\)\{if\(Date\.now\(\)-AM_SEL_AT<400\)return;if\(AM_RT\)amRouteShow\(null\);amRelease\(\);\}/.test(am) && /map\.on\('popupclose',amCardClosed\)/.test(am));
  ok('app: the maps follow the ISM list\'s units', readFileSync(new URL('../src/components/IsmPanel.tsx', import.meta.url), 'utf8').includes('ismMilesSet(u.mph)'));
}

// ── 9. THE NODES LIST's ORDER — NEAREST | LATEST (Stuart 11-26: "meshcore list needs to filter by nearest first and latest
//       first"). Ties: nearest then latest. No receiver position: LATEST, and no key (never a NEAREST that does nothing). ──
{
  const A = { ...nise, key: 'A', seen: 300 };                     // 16 km, heard 5 min ago
  const B = { ...relayedOnce, key: 'B', seen: 30 };               //  6 km, 30 s
  const C = { ...t1, key: 'C', seen: 5 };                         // 334 km, 5 s
  const D = { ...nise, key: 'D', name: 'N-Ise twin', long: 'N-Ise twin', seen: 10 };   // 16 km (A's spot), 10 s
  const E = { ...t1, key: 'E', name: 't::2', long: 't::2', seen: 30 };                // 334 km (C's spot), 30 s (B's age)
  const r = { band: '868', devices: [A, B, C, D, E] } as unknown as app.IsmReport;
  const keys = (rows: { key: string }[]) => rows.map((x) => x.key);
  const webKeys = (h: string) => Array.from(h.matchAll(/data-key="([^"]+)"/g)).map((m) => m[1]);
  const NEAR = ['B', 'D', 'A', 'C', 'E'], LATE = ['C', 'D', 'B', 'E', 'A'];
  eq('NEAREST (the default): by distance; a tie on distance → the most recently heard first', keys(app.ismMapRows(r, RX)), NEAR);
  eq('…asked for by name the same', keys(app.ismMapRows(r, RX, false, 'nearest')), NEAR);
  eq('LATEST: the most recently heard first; a tie on age → the nearest first', keys(app.ismMapRows(r, RX, false, 'latest')), LATE);
  eq('web NEAREST = app', webKeys(web.ismMapRows(r as unknown as web.IsmReport, RX)), NEAR);
  eq('web LATEST = app', webKeys(web.ismMapRows(r as unknown as web.IsmReport, RX, false, 'latest')), LATE);
  eq('no receiver position: LATEST whatever is picked (app)', [keys(app.ismMapRows(r, null, false, 'nearest')), keys(app.ismMapRows(r, null, false, 'latest'))], [['C', 'D', 'B', 'E', 'A'], ['C', 'D', 'B', 'E', 'A']]);
  eq('…web the same', webKeys(web.ismMapRows(r as unknown as web.IsmReport, null, false, 'nearest')), ['C', 'D', 'B', 'E', 'A']);
  eq('…no distance shown', app.ismMapRows(r, null).every((x) => x.range === ''), true);
  eq('the key: offered with a position, the pick kept', [meshSortOffered('nearest', RX), meshSortOffered('latest', RX)], ['nearest', 'latest']);
  eq('the key: NOT offered without one (no dead NEAREST)', [meshSortOffered('nearest', null), meshSortOffered('latest', undefined)], [null, null]);
  eq('stored value: only "latest" changes it; anything else is NEAREST', [meshSortParse('latest'), meshSortParse('nearest'), meshSortParse(null), meshSortParse('furthest')], ['latest', 'nearest', 'nearest', 'nearest']);
  eq('the order never changes what is on the map: same rows, same status', [keys(app.ismMapRows(r, RX, false, 'latest')).sort(), app.ismMapStatus(r, RX)], [NEAR.slice().sort(), app.ismMapStatus(r, RX)]);
  eq('…nor the map items (their own order)', keys(app.ismMapItems(r, RX)), ['A', 'B', 'C', 'D', 'E']);
  // a range unknown in a row (never in the lists — a plotted node has a position — but the rule is total): last in NEAREST
  eq('meshSortRows: a row with no range sorts last under NEAREST', meshSortRows([{ d: { seen: 1 }, k: null, n: 'x' }, { d: { seen: 9 }, k: { km: 5 }, n: 'y' }], 'nearest', RX).map((x) => x.n), ['y', 'x']);
  // THE GALLERY's map (its nodes carry seen = now − last heard)
  const g = [A, B, C, D, E].map((d) => ({ ...d, first: 0, last: 0, origin: 'live' })) as unknown as app.IsmGalNode[];
  eq('gallery NEAREST (app)', keys(app.ismGalRows(g, RX)), NEAR);
  eq('gallery LATEST (app)', keys(app.ismGalRows(g, RX, false, 'latest')), LATE);
  eq('gallery web = app (both orders)', [webKeys(web.ismGalRows(g as unknown as web.IsmGalNode[], RX)), webKeys(web.ismGalRows(g as unknown as web.IsmGalNode[], RX, false, 'latest'))], [NEAR, LATE]);
  eq('gallery without a position: LATEST', keys(app.ismGalRows(g, null, false, 'nearest')), ['C', 'D', 'B', 'E', 'A']);
  // the wiring (source): the web's maps give the key (the AIR & SEA chip) only with a position, remembered per viewer
  const mt = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  const tk = readFileSync(new URL('../web/client/src/tracks.ts', import.meta.url), 'utf8') + readFileSync(new URL('../web/client/src/mapChrome.ts', import.meta.url), 'utf8');
  ok('web live map: the key + its order', mt.includes("ismMapView.setSort(meshSortOffered(meshSort, myPos()), (v) => meshSortSet(v, ismMapRender), 'nodes');") && mt.includes('ismMapRows(r, myPos(), mi, meshSort)'));
  ok('web gallery map: the key + its order', mt.includes("ismGalView?.setSort(meshSortOffered(meshSort, rx), (v) => meshSortSet(v, draw), 'nodes');") && mt.includes('ismGalRows(nodes, rx, mi, meshSort)'));
  ok('web: remembered in localStorage (lsGet / lsSet — try/catch)', mt.includes('let meshSort: MeshSort = meshSortParse(lsGet(MESH_SORT_KEY));') && mt.includes('lsSet(MESH_SORT_KEY, meshSort)'));
  ok('web: the chip names its rows for a screen reader', tk.includes("setSort(sort: string | null, onSort: (s: string) => void, what = 'aircraft')") && tk.includes('host.setAttribute(\'aria-label\', `Sort the ${what}`);'));
  ok('web AIR & SEA keeps its chip (aircraft)', /v\.setSort\(tab === 'aircraft' && !adsbOn \? asSort : null, /.test(mt));
}

console.log(`test_mesh_route: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
