/**
 * Signal-meter rules (src/constants/meters.ts) — one deck height, the LED table and the squelch ring,
 * the edge LED's partial brightness (Φ, the σ window, the eye filter, steady-LED hysteresis) and the
 * edgewise needles' ballistics.
 *
 * Brief: docs/BRIEF-faceplates.md §4.1–§4.5, acceptance §13.5 / §13.7 / §13.11.
 *
 * Run: node --no-warnings scripts/test_faceplate_meters.ts   (run-tests.sh does)
 */
import {
  portraitDeck, compactKeyHitSlop, sqlClosedOf, type MeterKind,
} from '../src/constants/meters.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (what: string, got: number, want: number, tol: number) =>
  ok(`${what} (got ${got.toFixed(4)}, want ${want} ± ${tol})`, Math.abs(got - want) <= tol);

// ── §4.1 ONE DECK HEIGHT ─────────────────────────────────────────────────────
const METERS: MeterKind[] = ['bar', 'vu', 'edge'];
for (const scale of [0.82, 1, 1.1, 1.45]) {
  const r = (n: number) => Math.round(n * scale);
  const rowGap = r(7);
  for (const tablet of [false, true]) {
    // ★★★ Silver / black: every meter × shared is ONE height.
    const metal = new Set<number>();
    for (const meter of METERS) for (const shared of [false, true]) {
      const d = portraitDeck({ cap: true, meter, shared, tablet, rowGap, r });
      metal.add(d.blockH);
      eq(`metal ${meter} shared=${shared} @${scale}: block = display + gap + keys`, d.displayH + rowGap + d.keySlot, d.blockH);
      if (d.compact) {
        eq(`metal ${meter} shared=${shared} @${scale}: the column adds up`,
           d.bannerH + d.bannerGap + d.freqH + d.meterGap + d.housingH, d.displayH);
        ok(`metal ${meter} shared=${shared} @${scale}: the window is never squeezed away`, d.freqH >= r(30));
      }
    }
    eq(`metal @${scale} tablet=${tablet}: one deck height across meter × shared`, metal.size, 1);
    // Default chassis: LED / analogue one height; the bar is TODAY's (§4.2 / §13.1).
    const def = new Set<number>();
    for (const meter of ['vu', 'edge'] as const) for (const shared of [false, true])
      def.add(portraitDeck({ cap: false, meter, shared, tablet, rowGap, r }).blockH);
    eq(`default @${scale} tablet=${tablet}: LED / analogue one height across meter × shared`, def.size, 1);
    const bar = portraitDeck({ cap: false, meter: 'bar', shared: false, tablet, rowGap, r });
    eq(`default bar @${scale}: today's bar`, bar.displayH, r(tablet ? 62 : 40));
    eq(`default bar @${scale}: today's key`, bar.keySlot, r(44));
    eq(`default bar shared @${scale}: the banner is inside the bar, no change`,
       portraitDeck({ cap: false, meter: 'bar', shared: true, tablet, rowGap, r }).blockH, bar.blockH);
  }
}
// The §4.1 table falls OUT of the fixed block at scale 1 — it is not typed in anywhere.
{
  const r = (n: number) => n, rowGap = 7;
  const d = (meter: MeterKind, shared: boolean) => portraitDeck({ cap: true, meter, shared, tablet: false, rowGap, r });
  eq('§4.1 LED VU window 48', d('vu', false).freqH, 48);
  eq('§4.1 LED VU shared window 38', d('vu', true).freqH, 38);
  eq('§4.1 analogue window 46', d('edge', false).freqH, 46);
  eq('§4.1 analogue shared window 35', d('edge', true).freqH, 35);
  eq('§4.1 keys 44 / shared 34', [d('vu', false).keySlot, d('vu', true).keySlot, d('edge', true).keySlot], [44, 34, 34]);
  eq('§4.1 bar keys 58 on metal', d('bar', false).keySlot, 58);
  eq('metal bar frame = the mockup\'s 72', d('bar', true).displayH, 72);
  eq('§4.5 analogue housing 34 (28 window)', d('edge', false).housingH, 34);
  eq('legends 88 % / shared 80 %', [d('vu', false).legendScale, d('vu', true).legendScale], [0.88, 0.8]);
  eq('the bar\'s legends are untouched', d('bar', false).legendScale, 1);
}
// ★ §4.1 TRAP: 34 pt keys reach a 44 pt target through the gaps, without overlapping a neighbour.
{
  const h = compactKeyHitSlop(34, 7, 8);
  ok('34 pt key + slop reaches 44 pt', h.reach >= 44);
  ok('side slop stays inside half the column gap', h.left <= 4 && h.right <= 4);
  ok('downward slop stays inside half the row gap (the drums are below)', h.bottom <= 3.5);
}
// The one SQL rule the bar, the mode box and the meters share.
eq('squelch off (−1): never closed', sqlClosedOf(-1, true, 0), false);
eq('gate verdict wins over geometry', sqlClosedOf(0.5, false, 0.1), false);
eq('no verdict: below the line = closed', sqlClosedOf(0.5, undefined, 0.4), true);
eq('no verdict: above the line = open', sqlClosedOf(0.5, undefined, 0.6), false);

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate meters: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
