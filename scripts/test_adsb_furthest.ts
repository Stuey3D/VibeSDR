/**
 * ★★ THE FURTHEST AIRCRAFT KEY (Stuart, 2026-10-24: "We could put a Furthest aircraft here") — ONE RULE, TWO READERS:
 * the web map's adsbFurthest (web/client/src/adsb.ts) and the app map page's adsbFar (src/components/MapOverlay.tsx,
 * run here from its own source against a stand-in button). Both must name the same aircraft and the same distance.
 *
 * Run: npx tsx scripts/test_adsb_furthest.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { adsbFurthest, adsbFurthestHtml, adsbLongestTracked, adsbNewest, adsbChipHtml, adsbImageBase } from '../web/client/src/adsb.ts';

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
// ★★ TRAILS AND LOGOS FROM THE RADIO, NOT THE FRONT DOOR (2026-10-25: the door answered /adsb/trail/… with its own page)
{
  const at = (href: string) => { const u = new URL(href); (globalThis as any).location = { protocol: u.protocol, origin: u.origin, pathname: u.pathname }; return adsbImageBase(); };
  ok(at('http://192.168.86.13:48000/r/59a1a7d0/') === 'http://192.168.86.13:48000/r/59a1a7d0/adsb/', 'behind a front door: /r/<id>/adsb/');
  ok(at('http://192.168.86.13:48001/') === 'http://192.168.86.13:48001/adsb/', 'a radio on its own port: /adsb/');
  ok(at('https://x.vibeserver.vibesdr.net/r/ab%20c/?pin=1') === 'https://x.vibeserver.vibesdr.net/r/ab%20c/adsb/', 'the id as the address bar has it, query left off');
  delete (globalThis as any).location;
}
// ★ THE LIST HEADER'S OTHER TWO CHIPS (2026-10-25): FURTHEST TRACKED and NEWEST
{
  // ★ every aircraft ON THE MAP unless it says nopos — a chip only names one it can show (see onMap in adsb.ts)
  const rep = (ac: any[]) => ({ aircraft: ac.map((a) => (a.nopos ? a : { lat: 52.3, lon: -0.9, ...a })) } as any);
  {
    const off = adsbLongestTracked(rep([{ hex: 'aaaaaa', flight: 'BAW1', tracked_nm: 40 }, { hex: 'bbbbbb', flight: 'BAW2', tracked_nm: 157, nopos: true }]));
    ok(!!off && off.hex === 'aaaaaa', `tracked: the longest WITHOUT a position (157 NM, heard, no fix in 60 s) is not offered — it could not be selected (${JSON.stringify(off)})`);
    const s0 = new Map<string, number>();
    adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }]), s0, 1000);
    const nn = adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }, { hex: 'cccccc', flight: 'EZY9', nopos: true }]), s0, 2000);
    ok(nn === null, `newest: one with a name but no position yet waits (${JSON.stringify(nn)})`);
  }
  const lg = adsbLongestTracked(rep([{ hex: 'aaaaaa', tracked_nm: 12 }, { hex: 'bbbbbb', flight: 'BAW12 ', tracked_nm: 140.6 }, { hex: 'cccccc', tracked_nm: 0.4 }]));
  ok(!!lg && lg.hex === 'bbbbbb' && lg.name === 'BAW12' && Math.round(lg.nm) === 141, `tracked: the longest (${JSON.stringify(lg)})`);
  ok(adsbLongestTracked(rep([{ hex: 'aaaaaa', tracked_nm: 0.5 }])) === null, 'tracked: under 1 NM is not a track');
  ok(adsbChipHtml('LONGEST TRACKED', '141 NM', 'BAW12') === '<span class="k">LONGEST TRACKED</span> 141 NM<span class="n"> · BAW12</span>', 'tracked: the chip text');
  ok(adsbChipHtml('NEWEST', '', '<b>') === '<span class="k">NEWEST</span><span class="n"> · &#60;b&#62;</span>', 'newest: the chip text, escaped');
  const seen = new Map<string, number>();
  ok(adsbNewest(rep([{ hex: 'aaaaaa' }, { hex: 'bbbbbb' }]), seen, 1000) === null, 'newest: the first report is "already there", not new');
  const n1 = adsbNewest(rep([{ hex: 'aaaaaa' }, { hex: 'bbbbbb' }, { hex: 'cccccc', flight: 'EZY1' }]), seen, 5000);
  ok(!!n1 && n1.hex === 'cccccc' && n1.name === 'EZY1' && n1.ageS === 0, `newest: one arrives (${JSON.stringify(n1)})`);
  const n2 = adsbNewest(rep([{ hex: 'aaaaaa' }, { hex: 'cccccc', flight: 'EZY1' }, { hex: 'dddddd', flight: 'RYR2' }]), seen, 9000);
  ok(!!n2 && n2.hex === 'dddddd', 'newest: a later arrival takes over');
  ok(!seen.has('bbbbbb'), 'newest: one that left is forgotten');
  const n3 = adsbNewest(rep([{ hex: 'aaaaaa' }, { hex: 'cccccc', flight: 'EZY1' }]), seen, 12000);
  ok(!!n3 && n3.hex === 'cccccc' && n3.ageS === 7, `newest: the newest leaves → the one before it, with its age (${JSON.stringify(n3)})`);
  ok(adsbNewest(null, seen, 13000) === null && seen.size === 0, 'newest: leaving ADS-B forgets them all');
  // ★ nameless arrivals (address only) are skipped until they have a callsign or a registration
  const s2 = new Map<string, number>();
  adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }]), s2, 1000);
  const q1 = adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }, { hex: '408000' }]), s2, 2000);
  ok(q1 === null, `newest: an address-only arrival is not shown (${JSON.stringify(q1)})`);
  const q2 = adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }, { hex: '408000' }, { hex: 'bbbbbb', r: 'G-ZXIH' }]), s2, 3000);
  ok(!!q2 && q2.hex === 'bbbbbb' && q2.name === 'G-ZXIH', `newest: a registration is a name (${JSON.stringify(q2)})`);
  const q3 = adsbNewest(rep([{ hex: 'aaaaaa', flight: 'BAW1' }, { hex: '408000', flight: 'EZY62ZF' }, { hex: 'bbbbbb', r: 'G-ZXIH' }]), s2, 4000);
  ok(!!q3 && q3.hex === 'bbbbbb', 'newest: the nameless one, now named, keeps its earlier arrival (not newer than G-ZXIH)');
  const t2 = adsbLongestTracked(rep([{ hex: 'cccccc', r: 'G-ABCD', tracked_nm: 9 }]));
  ok(!!t2 && t2.name === 'G-ABCD', 'tracked: no callsign → its registration');
}
console.log(fails ? `\n${fails} FAILED, ${passes} passed` : `adsb furthest: all ${passes} passed`);
process.exit(fails ? 1 : 0);
