/**
 * Edge chips anchor to the PHYSICAL screen edge (src/components/edgeChipGeometry.ts, Stuart 2026-10-06, RC15:
 * "still got issues with the server chips not anchoring to the edge of the screen when it is rotated").
 *
 * Proves, for portrait, both landscapes, an iPad and a Mac-style window resize:
 *   • the card and the tab are both at right 0 — flush with the glass, never at the safe-area inset;
 *   • the content is padded in by the inset (clear of the island / notch / corner);
 *   • the tucked card and the hidden tab are wholly past the edge, from the SAME geometry;
 *   • a resize / rotation gives exactly the geometry a fresh layout at that size gives (no stale state);
 *   • (2026-10-06, RC16 "huge when collapsed") only the ISLAND's side is padded by the inset; the other side gets
 *     only what the rounded corner takes; unknown keeps the full inset; the tab is never wider than the open card;
 *   • and EdgeChip + SDRScreen actually use it (source checks: no `right: insets.right` path left).
 *
 * Run: node --no-warnings scripts/test_edge_chip.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { edgeChipGeometry, cornerIntrusion, islandSideFor, EDGE_TAB_W, EDGE_TAB_H, DISPLAY_CORNER_R,
         type EdgeInsets } from '../src/components/edgeChipGeometry.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}

const ins = (top: number, right: number, bottom: number, left: number): EdgeInsets => ({ top, right, bottom, left });
const CASES: { name: string; windowW: number; insets: EdgeInsets }[] = [
  { name: 'iPhone 17 Pro portrait',                       windowW: 402, insets: ins(62, 0, 34, 0) },
  // iOS reports the inset on BOTH sides in landscape, whichever way round the island is.
  { name: 'iPhone 17 Pro landscape-left (island right)', windowW: 874, insets: ins(0, 62, 21, 62) },
  { name: 'iPhone 17 Pro landscape-right (island left)', windowW: 874, insets: ins(0, 62, 21, 62) },
  { name: 'iPhone 15 landscape (the 43 pt screenshot)',  windowW: 852, insets: ins(0, 47, 21, 47) },
  { name: 'iPad full screen',                             windowW: 1194, insets: ins(24, 0, 20, 0) },
  { name: 'iPad split view (narrow)',                     windowW: 320, insets: ins(24, 0, 20, 0) },
  { name: 'Mac window',                                   windowW: 1000, insets: ins(28, 0, 0, 0) },
];

for (const c of CASES) {
  for (const cardW of [140, 210]) {
    const g = edgeChipGeometry({ windowW: c.windowW, insets: c.insets, cardW });
    console.log(`${c.name}, card ${cardW}:`);
    ok(g.right === 0, 'card and tab anchored at the physical edge (right 0)');
    ok(g.padRight === c.insets.right, `content padded by the inset (${g.padRight} = ${c.insets.right})`);
    ok(g.tabW === EDGE_TAB_W + c.insets.right, 'tab reaches the glass: glyph width + inset');
    ok(g.tabOff > g.tabW, 'hidden tab is wholly past the edge');
    ok(g.cardOff > Math.min(cardW, g.maxCardW), 'tucked card is wholly past the edge');
    ok(g.maxCardW <= c.windowW - c.insets.left, 'an open card can never run off the far side');
    // The card's right edge in window coordinates is windowW - right: on the glass.
    ok(c.windowW - g.right === c.windowW, 'card right edge = window right edge');
  }
}

// ★ A rotation / resize is a fresh evaluation: the same inputs give the same answer as a cold layout, and the
//   portrait → landscape → portrait round trip comes back exactly.
console.log('rotation / resize:');
const p0 = edgeChipGeometry({ windowW: 402, insets: ins(62, 0, 34, 0), cardW: 150 });
const l  = edgeChipGeometry({ windowW: 874, insets: ins(0, 62, 21, 62), cardW: 150 + 62 });
const p1 = edgeChipGeometry({ windowW: 402, insets: ins(62, 0, 34, 0), cardW: 150 });
ok(JSON.stringify(p0) === JSON.stringify(p1), 'portrait → landscape → portrait returns the same geometry');
ok(l.right === p0.right, 'the anchor does not move with the rotation (right 0 both ways)');
ok(l.cardOff === 150 + 62 + 12, 'landscape slide distance follows the re-measured (padded) card');
const macWide = edgeChipGeometry({ windowW: 1400, insets: ins(28, 0, 0, 0), cardW: 180 });
const macNarrow = edgeChipGeometry({ windowW: 300, insets: ins(28, 0, 0, 0), cardW: 400 });
ok(macWide.right === 0 && macNarrow.right === 0, 'Mac window resize keeps the chip on the edge');
ok(macNarrow.cardOff <= 300 - 8 + 12, 'a window narrower than the card clamps it, and tucks it by the clamped width');

// ★★★ ONLY THE ISLAND'S SIDE IS PADDED (Stuart, 2026-10-06, RC16: "the health and timer pills are huge when
//   collapsed, bigger than when they are open"). iOS gives the landscape inset to BOTH sides.
console.log('island side (RC16 "huge when collapsed"):');
ok(islandSideFor('landscapeRight') === 'left', 'landscapeRight (Home button on the right) ⇒ island on the LEFT');
ok(islandSideFor('landscapeLeft') === 'right', 'landscapeLeft (Home button on the left) ⇒ island on the RIGHT');
ok(islandSideFor('portrait') === 'none' && islandSideFor('portraitUpsideDown') === 'none', 'portrait ⇒ no island on a long edge');
ok(islandSideFor('unknown') === null && islandSideFor(undefined) === null, 'unknown / not yet in ⇒ null');

const LAND = { windowW: 874, windowH: 402, insets: ins(0, 62, 21, 62) };   // iPhone 17 Pro landscape
// The chips' real rows in Stuart's screenshots: health ~72 pt down, time ~113 pt down; 46 = the stack's top.
for (const top of [46, 72, 113]) {
  for (const cardW of [60, 140, 210]) {
    const away = edgeChipGeometry({ ...LAND, top, cardW, cardH: 64, islandSide: 'left' });
    const near = edgeChipGeometry({ ...LAND, top, cardW, cardH: 64, islandSide: 'right' });
    const unk  = edgeChipGeometry({ ...LAND, top, cardW, cardH: 64, islandSide: null });
    console.log(`landscape, top ${top}, card ${cardW}:`);
    ok(away.right === 0 && near.right === 0 && unk.right === 0, 'all three flush at right 0');
    ok(near.padRight === 62, 'island on this edge: the full inset');
    ok(unk.padRight === 62, "side unknown: the full inset (today's safe behaviour)");
    ok(away.padRight <= 8, `island on the far edge: only the corner allowance (${away.padRight} pt)`);
    ok(away.padRight === cornerIntrusion(top, 64, 402), "that allowance is the corner measured over the chip's rows");
    for (const g of [away, near, unk]) {
      ok(g.tabW <= Math.max(EDGE_TAB_W, Math.min(cardW, g.maxCardW)), `collapsed tab (${g.tabW}) never wider than the open card (${cardW})`);
      ok(g.tabOff > g.tabW && g.cardOff > Math.min(cardW, g.maxCardW), 'tab and card still slide wholly past the edge');
    }
    ok(away.tabW === Math.max(EDGE_TAB_W, Math.min(EDGE_TAB_W + away.padRight, cardW)), 'island-away tab is a compact pill (glyphs + corner)');
  }
}
console.log('corner:');
ok(cornerIntrusion(72, EDGE_TAB_H, 402) === 0, 'rows from 72 pt down miss even the 62 pt corner');
ok(cornerIntrusion(46, EDGE_TAB_H, 402) === Math.ceil(DISPLAY_CORNER_R - Math.sqrt(DISPLAY_CORNER_R ** 2 - 16 ** 2)), 'a row 46 pt down loses R − √(R² − 16²)');
ok(cornerIntrusion(0, 20, 402) === DISPLAY_CORNER_R, 'the very top row loses the whole radius');
ok(cornerIntrusion(330, 60, 402) > 0, 'the BOTTOM corner counts too');
console.log('rotation between the landscapes:');
const toL = edgeChipGeometry({ ...LAND, top: 72, cardW: 150 + 62, cardH: 64, islandSide: 'right' });
const toR = edgeChipGeometry({ ...LAND, top: 72, cardW: 150, cardH: 64, islandSide: 'left' });
ok(toL.cardOff !== toR.cardOff && toL.tabW !== toR.tabW, 'the 180° flip moves cardOff and tabW (feeds the re-place effect)');
console.log('portrait / iPad / Android with the side known or not:');
const port = edgeChipGeometry({ windowW: 402, windowH: 874, insets: ins(62, 0, 34, 0), top: 130, cardW: 150, islandSide: 'none' });
ok(port.padRight === 0 && port.tabW === EDGE_TAB_W, 'portrait: no padding, glyph-wide tab');
const pad = edgeChipGeometry({ windowW: 1194, windowH: 834, insets: ins(24, 0, 20, 0), top: 70, cardW: 150, islandSide: 'left' });
ok(pad.padRight === 0 && pad.tabW === EDGE_TAB_W, 'iPad (no inset): unchanged whichever way round');
const padNull = edgeChipGeometry({ windowW: 1194, windowH: 834, insets: ins(24, 0, 20, 0), top: 70, cardW: 150, islandSide: null });
ok(padNull.padRight === 0, 'iPad, side unknown: unchanged');
const android = edgeChipGeometry({ windowW: 900, windowH: 412, insets: ins(0, 0, 0, 48), top: 72, cardW: 150, islandSide: null });
ok(android.padRight === 0, 'Android: one-sided cutout inset used as given');
const androidR = edgeChipGeometry({ windowW: 900, windowH: 412, insets: ins(0, 48, 0, 0), top: 72, cardW: 150, islandSide: null });
ok(androidR.padRight === 48, 'Android, cutout on the right: the inset it reports');

// ★ Source checks: the caller no longer feeds the safe-area inset in as the anchor.
console.log('source:');
const chip = readFileSync(new URL('../src/components/EdgeChip.tsx', import.meta.url), 'utf8');
const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
ok(chip.includes('edgeChipGeometry('), 'EdgeChip reads edgeChipGeometry');
ok(!/right:\s*insets\.right/.test(chip), 'EdgeChip has no right: insets.right');
ok(!/const edgeRight\s*=/.test(sdr), 'SDRScreen no longer computes edgeRight');
ok(!/<EdgeChip[^>]*\bright=/.test(sdr), 'no EdgeChip call site passes right=');
ok(/useIslandSide\(\)/.test(chip) && /islandSide \}\)/.test(chip), 'EdgeChip feeds the island side into the geometry');
ok(/paddingRight:\s*geo\.tabW - EDGE_TAB_W/.test(chip), 'the tab pads only what its width allows (never wider than the card)');

if (fails) { console.log(`\n✗ ${fails} failed`); process.exit(1); }
console.log('\n✓ edge chips anchor to the physical edge');
