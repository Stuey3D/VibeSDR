/**
 * Edge chips anchor to the PHYSICAL screen edge (src/components/edgeChipGeometry.ts, Stuart 2026-10-06, RC15:
 * "still got issues with the server chips not anchoring to the edge of the screen when it is rotated").
 *
 * Proves, for portrait, both landscapes, an iPad and a Mac-style window resize:
 *   • the card and the tab are both at right 0 — flush with the glass, never at the safe-area inset;
 *   • the content is padded in by the inset (clear of the island / notch / corner);
 *   • the tucked card and the hidden tab are wholly past the edge, from the SAME geometry;
 *   • a resize / rotation gives exactly the geometry a fresh layout at that size gives (no stale state);
 *   • and EdgeChip + SDRScreen actually use it (source checks: no `right: insets.right` path left).
 *
 * Run: node --no-warnings scripts/test_edge_chip.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { edgeChipGeometry, EDGE_TAB_W, type EdgeInsets } from '../src/components/edgeChipGeometry.ts';

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

// ★ Source checks: the caller no longer feeds the safe-area inset in as the anchor.
console.log('source:');
const chip = readFileSync(new URL('../src/components/EdgeChip.tsx', import.meta.url), 'utf8');
const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
ok(chip.includes('edgeChipGeometry('), 'EdgeChip reads edgeChipGeometry');
ok(!/right:\s*insets\.right/.test(chip), 'EdgeChip has no right: insets.right');
ok(!/const edgeRight\s*=/.test(sdr), 'SDRScreen no longer computes edgeRight');
ok(!/<EdgeChip[^>]*\bright=/.test(sdr), 'no EdgeChip call site passes right=');

if (fails) { console.log(`\n✗ ${fails} failed`); process.exit(1); }
console.log('\n✓ edge chips anchor to the physical edge');
