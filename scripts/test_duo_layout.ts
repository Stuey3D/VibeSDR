/**
 * iPhone Duo layout rules (src/utils/duoGeometry.ts, Stuart 2026-11-25) — window rules and the reserved-region
 * helpers, proven with the simulator's real numbers (folded front screen 466 × 678 pt; camera [400,29 37×37] and
 * the status column [382,0 84×174] reported by UIView -reservedRegionsOfKind:; inner screen 669 × 951 pt).
 *
 * Run: node --no-warnings scripts/test_duo_layout.ts   (run-tests.sh does)
 */
import { edgeChipGeometry } from '../src/components/edgeChipGeometry.ts';
import { isFoldedFrontWindow, isFrontScreenWindow, phonePortraitScale, clearBelow, rightLimitAt, deckClearance,
         avoidRects, NO_REGIONS, type ReservedRegions } from '../src/utils/duoGeometry.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}

// ── The folded front screen, told by the window ──────────────────────────────────────────────
ok(isFoldedFrontWindow(466, 678, 466, 678), 'Duo folded, portrait: the front screen');
ok(!isFoldedFrontWindow(678, 466, 466, 678), 'Duo folded, landscape: no grab bar (portrait only)');
ok(isFrontScreenWindow(678, 466, 466, 678), '…but it is still the front screen for the ratio');
ok(!isFrontScreenWindow(669, 951, 669, 951), 'Duo open portrait: not the front screen');
ok(!isFrontScreenWindow(951, 669, 669, 951), 'Duo open landscape: not the front screen');
ok(!isFrontScreenWindow(475, 669, 951, 669), 'Duo open, Split View share of the same shape: not the front screen');
for (const [w, h, name] of [[402, 874, 'iPhone 17 Pro'], [440, 956, 'iPhone 17 Pro Max'], [375, 667, 'iPhone SE'],
                            [414, 736, 'iPhone 8 Plus'], [320, 568, 'SE Display Zoom']] as const) {
  ok(!isFrontScreenWindow(w, h, w, h) && !isFrontScreenWindow(h, w, w, h), `${name}: never the front screen`);
}
for (const [w, h, name] of [[744, 1133, 'iPad mini'], [820, 1180, 'iPad Air 11'], [1032, 1376, 'iPad Pro 13']] as const) {
  ok(!isFrontScreenWindow(w, h, w, h), `${name}: never the front screen`);
}
ok(!isFrontScreenWindow(590, 820, 820, 1180), 'iPad Split View half (590 × 820): not the whole screen');
ok(!isFrontScreenWindow(500, 700, 1032, 1376), 'iPad Stage Manager window 500 × 700: not the whole screen');

// ── The deck's scale: only short windows change ─────────────────────────────────────────────────
const old = (W: number) => Math.max(0.75, Math.min(1.45, W / 390));
for (const [w, h, name] of [[402, 874, 'iPhone 17 Pro'], [440, 956, 'Pro Max'], [375, 667, 'SE'], [414, 736, '8 Plus'],
                            [320, 568, 'SE Zoom'], [390, 844, 'iPhone 14']] as const) {
  ok(phonePortraitScale(w, h) === old(w), `${name}: deck scale unchanged (${old(w).toFixed(3)})`);
}
const front = phonePortraitScale(466, 678);
ok(Math.abs(front / old(466) - 0.873) < 0.01, `front screen: deck at ${(front / old(466) * 100).toFixed(0)} % of before (${front.toFixed(3)})`);

// ── Reserved regions ─────────────────────────────────────────────────────────────────────────────
const FOLDED: ReservedRegions = { ...NO_REGIONS, occlusion: [{ x: 400, y: 29, w: 37, h: 37 }, { x: 382, y: 0, w: 84, h: 174 }],
                                  hinge: { status: 'closed', angleDeg: 0 } };
ok(clearBelow(FOLDED, 466 - 180, 466, 678) === 174, 'right-hand stack starts below the status column (174)');
ok(clearBelow(FOLDED, 12, 140, 678) === 0, 'the Servers chip corner is clear');
ok(rightLimitAt(FOLDED, 0, 64, 466) === 382, "the receiver's name stops at the column's left edge (382)");
ok(rightLimitAt(NO_REGIONS, 0, 64, 466) === Infinity, 'no regions: no limit (every other device)');
const dc = deckClearance(FOLDED, 400, 466, 678);
ok(dc.bottom === 0, 'folded portrait: the deck is clear of the top-right column');
// Folded landscape, camera bottom-right (Stuart: "camera hole fouls the controls").
const LAND: ReservedRegions = { ...NO_REGIONS, occlusion: [{ x: 612, y: 400, w: 37, h: 37 }] };
const dl = deckClearance(LAND, 330, 678, 466);
ok(dl.bottom === 466 - 400 + 6 && dl.left === 0 && dl.right === 0, `camera bottom-right LIFTS the deck as a block (${dl.bottom}), no inset`);
const LANDL: ReservedRegions = { ...NO_REGIONS, occlusion: [{ x: 29, y: 400, w: 37, h: 37 }] };
ok(deckClearance(LANDL, 330, 678, 466).bottom === 72, 'camera bottom-left lifts it the same');
ok(deckClearance({ ...NO_REGIONS, occlusion: [{ x: 612, y: 20, w: 37, h: 37 }] }, 330, 678, 466).right === 0,
   'camera top-right (over the spectrum) leaves the deck alone');
ok(rightLimitAt({ ...NO_REGIONS, statusBar: [{ x: 0, y: 0, w: 402, h: 62 }] }, 40, 100, 402) === Infinity,
   'an iPhone status band is not a corner: the name is not moved');
ok(avoidRects({ ...NO_REGIONS, statusBar: [{ x: 0, y: 0, w: 10, h: 10 }] }).length === 1, 'the status bar frame is avoided too');

// ── Edge tabs on the front screen, landscape (Stuart: "+ ‹" ~3x wider with the camera bottom-right) ──────────────
{
  const ins = { top: 0, right: 84, bottom: 21, left: 0 };
  const camBR = [{ x: 612, y: 400, w: 37, h: 37 }];
  const g = edgeChipGeometry({ windowW: 678, windowH: 466, insets: ins, cardW: 0, cardH: 40, top: 60, islandSide: 'right', rects: camBR });
  const old = edgeChipGeometry({ windowW: 678, windowH: 466, insets: ins, cardW: 0, cardH: 40, top: 60, islandSide: 'right' });
  ok(g.padRight < old.padRight && g.padRight <= 62, `camera bottom-right, tab at the top: corner only (${g.padRight}, was ${old.padRight})`);
  const g2 = edgeChipGeometry({ windowW: 678, windowH: 466, insets: ins, cardW: 0, cardH: 40, top: 390, islandSide: 'right', rects: camBR });
  ok(g2.padRight >= 678 - 612, `a tab in the camera's rows clears it (${g2.padRight})`);
  const ip = edgeChipGeometry({ windowW: 874, windowH: 402, insets: { top: 0, right: 62, bottom: 21, left: 62 }, cardW: 0, cardH: 40, top: 60, islandSide: 'right' });
  ok(ip.padRight === 62, 'ordinary iPhone landscape (no rects): unchanged, full island inset');
}

// ── Split View shares on the open screen (landscape only on the Duo). UIKit gives the rects in the WINDOW's own
//    coordinates (native converts the status bar frame from the scene), so a share containing the pill avoids it and
//    one that doesn't, doesn't. Inner landscape 951 × 669, halves ~475 wide; pill assumed at the screen's top-right.
{
  const RIGHT_HALF: ReservedRegions = { ...NO_REGIONS, statusBar: [{ x: 475 - 90, y: 0, w: 90, h: 40 }], hinge: { status: 'fullyOpen', angleDeg: 180 } };
  ok(isFrontScreenWindow(475, 669, 951, 669) === false, 'a share is never the front screen');
  ok(rightLimitAt(RIGHT_HALF, 0, 64, 475) === 385, 'right-hand share: the name stops left of the pill');
  ok(clearBelow(RIGHT_HALF, 475 - 180, 475, 669) === 40, 'right-hand share: the stack starts below the pill');
  const tab = edgeChipGeometry({ windowW: 475, windowH: 669, insets: { top: 0, right: 0, bottom: 21, left: 0 }, cardW: 0, cardH: 40, top: 10, rects: RIGHT_HALF.statusBar });
  ok(tab.padRight >= 90, `right-hand share: a tab in the pill's rows clears it (${tab.padRight})`);
  const LEFT_HALF: ReservedRegions = { ...NO_REGIONS, hinge: { status: 'fullyOpen', angleDeg: 180 } };
  ok(rightLimitAt(LEFT_HALF, 0, 64, 475) === Infinity && clearBelow(LEFT_HALF, 295, 475, 669) === 0,
     'left-hand share (no pill in it): nothing moves');
  // A rect reported outside the window (another share's) never moves anything.
  const OUTSIDE: ReservedRegions = { ...NO_REGIONS, statusBar: [{ x: 500, y: 0, w: 90, h: 40 }] };
  ok(clearBelow(OUTSIDE, 295, 475, 669) === 0, "a rect beyond the window's right edge is ignored");
}

if (fails) { console.log(`\nduo layout: ${fails} FAILED`); process.exit(1); }
console.log('\nduo layout: all passed');
