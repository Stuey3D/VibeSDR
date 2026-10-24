/**
 * ★★ THE FURTHEST AIRCRAFT KEY (Stuart, 2026-10-24: "We could put a Furthest aircraft here") — ONE RULE, TWO READERS:
 * the web map's adsbFurthest (web/client/src/adsb.ts) and the app map page's adsbFar (src/components/MapOverlay.tsx,
 * run here from its own source against a stand-in button). Both must name the same aircraft and the same distance.
 *
 * Run: npx tsx scripts/test_adsb_furthest.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { adsbFurthest, adsbFurthestHtml } from '../web/client/src/adsb.ts';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) { passes++; return; } fails++; console.error(`FAIL ${what}`); };

const rx = { lat: 52.6, lon: -0.6 };
const ac = [
  { hex: '400001', flight: 'NEAR1 ', lat: 52.7, lon: -0.5 },
  { hex: '~2a0b1c', flight: '', lat: 55.1, lon: -3.9 },        // ★ the furthest, non-ICAO, no callsign
  { hex: '400003', flight: 'MID', lat: 53.5, lon: 0.4 },
  { hex: '400004', flight: 'NOPOS' },                           // unlocated: never the furthest
];
const rep = { type: 'adsb', aircraft: ac } as never;
const f = adsbFurthest(rep, rx);
ok(!!f && f.hex === '~2a0b1c', 'web: the furthest located aircraft is picked');
ok(!!f && f.name === '~2A0B1C', 'web: no callsign → its address');
ok(!!f && Math.round(f.nm) > 150 && Math.round(f.nm) < 250, `web: a sane distance (${f && Math.round(f.nm)} NM)`);
ok(adsbFurthest(rep, null) === null, 'web: no receiver position → no key');
ok(adsbFurthest({ type: 'adsb', aircraft: [ac[3]] } as never, rx) === null, 'web: nothing located → no key');
ok(/FURTHEST<\/span> \d+ NM<span class="n"> · ~2A0B1C<\/span>/.test(adsbFurthestHtml(f)), 'web: the key reads "FURTHEST n NM · name"');
ok(!/<b>/.test(adsbFurthestHtml({ nm: 1, name: '<b>' })), 'web: a callsign is escaped');

// ── the app's page, from its own source ──
const src = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
const fn = src.match(/\n  var farHex='';\n  function adsbFar\(a\)\{[\s\S]*?\n  \}\n/);
ok(!!fn, 'app: adsbFar found in MapOverlay.tsx');
if (fn) {
  const b = { hidden: true, innerHTML: '' };
  const document = { getElementById: (id: string) => (id === 'far' ? b : null) };
  const run = new Function('document', 'RX_LAT', 'RX_LON', 'a', `${fn[0]}\nadsbFar(a); return farHex;`);
  const hex = run(document, rx.lat, rx.lon, ac);
  ok(hex === f?.hex, `app: the same aircraft as the web (${hex})`);
  ok(!b.hidden && b.innerHTML.includes(`${Math.round(f!.nm)} NM`), 'app: the same distance, shown');
  run(document, 0, 0, ac);
  ok(b.hidden, 'app: no receiver position → hidden');
}
console.log(fails ? `\n${fails} FAILED, ${passes} passed` : `adsb furthest: all ${passes} passed`);
process.exit(fails ? 1 : 0);
