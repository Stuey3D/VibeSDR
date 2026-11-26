/* test_amateur_map.ts — ★★★ beta96 stage 7, THE AMATEUR MAP (src/utils/amateurMap.ts, docs/v12/ONE-MAP.md).
 *
 * Stuart's rule (11-26): the map shows ANY station heard in the HEARD IN window — background decoding's and every
 * listener's decoders' alike — and APRS, MESH and BEACONS follow the SAME window; the live layer is only emphasised.
 * The store side is test-station-store (one background FT8 spot and one listener CW skimmer spot both in 1 h, both gone
 * two hours later, both in 24 h). This is the map side: the window rule (TS and the page text alike), a background and a
 * listener spot both shown inside the window and gone outside it, the same for APRS, MESH and BEACONS, the live layer,
 * the MESH and BEACONS points, the tabs, and the two map pages wired to it.
 *   npx tsx scripts/test_amateur_map.ts
 */
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { amKeep, amWinSec, amLiveOf, amMeshPoints, amBeaconPoints, amTabs, amSrcWords, AMATEUR_JS, AM_SRC_BACKGROUND, AM_SRC_LIVE } from '../src/utils/amateurMap';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { if (c) pass++; else { fail++; console.log(`FAIL ${what}`); } };
const eq = (a: unknown, b: unknown, what: string) => ok(JSON.stringify(a) === JSON.stringify(b), `${what}: ${JSON.stringify(a)} ≠ ${JSON.stringify(b)}`);

/* ── the window ── */
eq(['1h', '24h', '7d', '30d', 'all'].map(amWinSec), [3600, 86400, 8 * 86400, 31 * 86400, 0], 'the windows (7D / 30D = today and the N days before)');
eq(['2m', '15m', 'ALL'].map(amWinSec), [120, 900, 0], 'the app\'s live-only windows');

/* ── the page text is the same rule ── */
const ctx: Record<string, unknown> = { esc: (v: unknown) => String(v), RIM: '#111', L: null };
vm.createContext(ctx);
vm.runInContext(AMATEUR_JS, ctx);
const keepJs = ctx.amKeep as (a: number, w: string) => boolean;
for (const w of ['2m', '15m', '1h', '24h', '7d', '30d', 'all', 'ALL', ''])
  for (const a of [0, 60, 119, 121, 3599, 3601, 86399, 86401, 8 * 86400 + 1, 31 * 86400 + 1, 1e9, -1, NaN])
    eq(keepJs(a, w), amKeep(a, w), `page text = TS: ${a} s in ${w || '(none)'}`);
ok(!/`/.test(AMATEUR_JS) && !/\\/.test(AMATEUR_JS), 'the page text has no backticks or backslashes (it sits in template literals)');

/* ── ★★ a BACKGROUND and a LISTENER spot: both inside the window, both gone outside it ── */
{
  const now = 1_764_000_000;
  // the window page's stored stations: { src, timestamp (ms) } — background FT8 heard 30 min ago, a listener's CW skimmer 40 min ago
  const spots = [{ call: 'G4BGD', src: AM_SRC_BACKGROUND, t: now - 1800 }, { call: 'DL1LIS', src: AM_SRC_LIVE, t: now - 2400 }];
  const shown = (atNow: number, win: string) => spots.filter((s) => amKeep(atNow - s.t, win)).map((s) => s.call);
  eq(shown(now, '1h'), ['G4BGD', 'DL1LIS'], 'SPOTS: background and listener both shown in 1 H');
  eq(shown(now + 7200, '1h'), [], 'SPOTS: both gone two hours later (1 H)');
  eq(shown(now + 7200, '24h'), ['G4BGD', 'DL1LIS'], 'SPOTS: both still in 24 H');
  eq([amSrcWords(AM_SRC_BACKGROUND), amSrcWords(AM_SRC_LIVE), amSrcWords(3)],
     ['Heard by background decoding', 'Heard by a listener’s decoder', 'Heard by background decoding and a listener’s decoder'], 'who heard it, in words');
  // APRS: the box's stations carry `age` (since last heard); one decoded in the background session, one by this listener
  const aprs = [{ call: 'G4ABC-9', age: 600 }, { call: 'MB7UH', age: 3000 }];
  const aprsIn = (dt: number, win: string) => aprs.filter((a) => amKeep(a.age + dt, win)).map((a) => a.call);
  eq(aprsIn(0, '1h'), ['G4ABC-9', 'MB7UH'], 'APRS: both in 1 H');
  eq(aprsIn(7200, '1h'), [], 'APRS: both gone two hours later (1 H) — the SAME window as the spots');
  eq(aprsIn(7200, '24h'), ['G4ABC-9', 'MB7UH'], 'APRS: both in 24 H');
}

/* ── the live layer ── */
eq(['spots', 'cw', 'psk', 'aprs', 'ncdxf', 'navtex', null].map((d) => amLiveOf(d)), ['spots', 'spots', 'spots', 'aprs', 'beacons', '', ''], 'the live layer from the decoder');
eq(amLiveOf('aprs', true), 'mesh', 'ISM mode: MESH is live');

/* ── MESH ── */
{
  const now = 1_764_000_000;
  const live = [{ kind: 'meshcore', node: 'ab12', name: 'Hill repeater', lat: 51.5, lon: -1.2, age: 30, ntype: 'repeater' },
                { kind: 'meshtastic', node: '!0f', long: 'Bob mobile', lat: 51.6, lon: -1.1, age: 5, approx: 1 },
                { kind: 'lorawan', node: 'x', lat: 1, lon: 1, age: 1 },                       // not mesh
                { kind: 'meshcore', node: 'nopos', age: 1 }];                                  // no position
  const stored = [{ kind: 'meshcore', node: 'ab12', name: 'Hill repeater', lat: 51.5, lon: -1.2, last: now - 7200 },   // the live one wins
                  { kind: 'meshtastic', node: '!aa', long: 'Old <b>', lat: 52, lon: 0, last: now - 3 * 86400 }];
  const m = amMeshPoints(live, stored, now);
  eq(m.map((x) => `${x.key}:${x.net}:${x.live}:${x.age}`), ['mesh:!0f:Meshtastic:true:5', 'mesh:ab12:MeshCore:true:30', `mesh:!aa:Meshtastic:false:${3 * 86400}`], 'MESH: live + remembered, once each, newest first');
  eq(m[2].name, 'Old b', 'MESH: a name is cleaned');
  eq(m.filter((x) => amKeep(x.age, '24h')).length, 2, 'MESH follows the window (the 3-day-old node out of 24 H)');
  eq(m.filter((x) => amKeep(x.age, '7d')).length, 3, '…and in 7 D');
}

/* ── BEACONS ── */
{
  const r = { beacons: [{ call: '4U1UN', where: 'New York', lat: 40.75, lon: -73.97 }, { call: 'OH2B', where: 'Lohja', lat: 60.32, lon: 24.13 }, { call: 'CS3B', where: 'Madeira', lat: 32.82, lon: -17.2 }],
              cells: [{ beacon: 1, band: 0, heardAgo: 120, steps: 3 }, { beacon: 1, band: 2, heardAgo: 4000, steps: 1 }, { beacon: 2, band: 0, heardAgo: -1, steps: 0 }] };
  const b = amBeaconPoints(r, ['20 m', '17 m', '15 m', '12 m', '10 m']);
  eq(b.map((x) => [x.call, x.age, x.bands.join('+'), x.best]), [['OH2B', 120, '20 m+15 m', 3]], 'BEACONS: only those heard, the newest heard, bands and the deepest step');
  eq(b.filter((x) => amKeep(x.age, '1h')).length, 1, 'BEACONS follow the window');
  eq(amBeaconPoints(null, []), [], 'BEACONS: no report, nothing');
}

/* ── the tabs ── */
eq(amTabs({ spots: 3, aprs: 0, mesh: 2 }, '', ''), ['spots', 'mesh'], 'tabs: those with something');
eq(amTabs({}, 'beacons', 'aprs'), ['aprs', 'beacons'], 'tabs: the live one and the one opened on, even empty');

/* ── the pages ── */
{
  const main = readFileSync('web/client/src/main.ts', 'utf8');
  ok(/<title>VibeSDR — Amateur Map<\/title>/.test(main) && /<span id="amTitle">AMATEUR MAP<\/span>/.test(main), 'web: the window is the AMATEUR MAP');
  ok(/\$\{AMATEUR_JS\}/.test(main) && /function aprsIn\(\) \{ return aprs\.filter\(\(a\) => amKeep\(a\.age, win\)\); \}/.test(main), 'web: APRS through the window rule');
  ok(/for \(const a of aprsIn\(\)\) \{/.test(main) && /const aw = aprsIn\(\);/.test(main), 'web: APRS drawn and counted from the window');
  ok(/amDrawMesh\(meshLayer, mesh, win/.test(main) && /amDrawBeacons\(beaconLayer, beacons, win/.test(main), 'web: MESH and BEACONS drawn through the window');
  ok(/ws\.hidden = !\(srvOk \|\| aprs\.length \|\| mesh\.length \|\| beacons\.length\);/.test(main), 'web: the window keys stand for every layer');
  ok(/class="live" title="Live now/.test(main), 'web: the live layer\'s key marked');
  ok(/id="amTabs" role="tablist"/.test(main) && /function amList\(\)/.test(main), 'web: the tabs');
  const mo = readFileSync('src/components/MapOverlay.tsx', 'utf8');
  ok(/AMATEUR_JS/.test(mo), 'app: the page carries the same rules');
}

console.log(`test_amateur_map: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
