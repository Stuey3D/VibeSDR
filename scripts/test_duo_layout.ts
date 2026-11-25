/**
 * iPhone Duo layout rules (src/utils/duoGeometry.ts, Stuart 2026-11-25) — window rules and the reserved-region
 * helpers, proven with the simulator's real numbers (folded front screen 466 × 678 pt; camera [400,29 37×37] and
 * the status column [382,0 84×174] reported by UIView -reservedRegionsOfKind:; inner screen 669 × 951 pt).
 *
 * Run: node --no-warnings scripts/test_duo_layout.ts   (run-tests.sh does)
 */
import { edgeChipGeometry } from '../src/components/edgeChipGeometry.ts';
import { namePlacement } from '../src/utils/duoGeometry.ts';
import { isFoldedFrontWindow, isFrontScreenWindow, phonePortraitScale, clearBelow, rightLimitAt, deckClearance,
         avoidRects, NO_REGIONS, halfOpenSplit, serversChipPlacement, type ReservedRegions, type RegionRect } from '../src/utils/duoGeometry.ts';

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
ok(Math.abs(front / old(466) - 0.681) < 0.01, `front screen: deck at ${(front / old(466) * 100).toFixed(0)} % of 11.0.16's (${front.toFixed(3)})`);
ok(front >= 0.75, 'front screen: never below the 0.75 floor');

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

// ── The receiver's name (Stuart: "name still displays under the system status pill", right-hand share) ─────────────
{
  // A share reports its status bar across its whole width: both lines of the name drop below it and it fades.
  const SHARE_BAND: ReservedRegions = { ...NO_REGIONS, statusBar: [{ x: 0, y: 0, w: 469, h: 32 }], hinge: { status: 'fullyOpen', angleDeg: 180 } };
  const n = namePlacement(SHARE_BAND, 0, 64, 469, 28);
  ok(n.minTop >= 36 && n.right === 28 && n.crowded, `share: the name (both lines) starts below the status band (top ${n.minTop}) and fades`);
  // Folded front screen: the status column at the right — the name ends >= 8 pt left of it, both lines, and fades.
  const f = namePlacement(FOLDED, 0, 64, 466, 28);
  ok(466 - f.right <= 382 - 8 && f.minTop === 0 && f.crowded, `folded: the name's right edge is at x ${466 - f.right}, 8+ pt left of the column (382)`);
  // A pill given as a corner rect in a share: right edge clears it.
  const p = namePlacement({ ...NO_REGIONS, statusBar: [{ x: 380, y: 0, w: 89, h: 60 }] }, 0, 64, 469, 28);
  ok(469 - p.right <= 380 - 8, `share with a corner pill rect: the name ends at x ${469 - p.right}`);
  // Nothing there: unchanged, no fade.
  const z = namePlacement(NO_REGIONS, 0, 64, 469, 28);
  ok(z.right === 28 && z.minTop === 0 && !z.crowded, 'no rects: the name is where it always was, and does not fade');
}

// ── HALF-OPEN: BIG as two panels either side of the fold (Stuart: "dont cover over the crease … treat the decoder box as
//    2 halves"). Inner portrait 669 × 951; the fold as UIKit's division region across the middle; the box in the LEFT
//    column (x 8 … 669 − (335 + 4) = 330), the deck in the right. Pill + camera top-RIGHT. ───────────────────────────
{
  const W = 669, H = 951, COL = { x0: 8, x1: W - (Math.ceil(W / 2) + 4) };
  const hit = (a: RegionRect, x0: number, x1: number, y0: number, y1: number) =>
    a.x < x1 && x0 < a.x + a.w && a.y < y1 && y0 < a.y + a.h;
  const pill = { x: 560, y: 0, w: 109, h: 40 }, cam = { x: 600, y: 8, w: 37, h: 37 };
  for (const fold of [{ x: 0, y: 455, w: W, h: 41 }, { x: 0, y: 470, w: W, h: 12 }, { x: 0, y: 400, w: W, h: 60 }]) {
    const R: ReservedRegions = { ...NO_REGIONS, division: [fold], statusBar: [pill], occlusion: [cam],
                                 hinge: { status: 'partiallyOpen', angleDeg: 110 } };
    const sp = halfOpenSplit(R, fold, H, COL, { top: 0, bottomInset: 21 });
    ok(!!sp, `fold at ${fold.y}+${fold.h}: a split is offered`);
    if (!sp) continue;
    ok(!hit(fold, sp.x0, sp.x1, sp.upper.top, sp.upper.bottom) && !hit(fold, sp.x0, sp.x1, sp.lower.top, sp.lower.bottom),
       `fold at ${fold.y}+${fold.h}: neither panel touches the division rect (upper ends ${sp.upper.bottom}, lower starts ${sp.lower.top})`);
    ok(sp.upper.bottom <= fold.y - 8 && sp.lower.top >= fold.y + fold.h + 8, '…with an 8 pt gap each side (the waterfall shows through)');
    ok(sp.upper.top === 8, `the pill and camera are top-RIGHT: the upper panel reaches the top edge (${sp.upper.top})`);
    ok(sp.lower.bottom === H - 21 - 8, `the lower panel sits on the home-indicator line (${sp.lower.bottom})`);
    ok(sp.x1 <= W / 2 && sp.x0 >= 8, `both panels in the left column (${sp.x0}…${sp.x1}), the deck's half untouched`);
    ok(avoidRects(R).every((r) => !hit(r, sp.x0, sp.x1, sp.upper.top, sp.upper.bottom) && !hit(r, sp.x0, sp.x1, sp.lower.top, sp.lower.bottom)),
       'no reserved region falls on either panel');
  }
  const fold = { x: 0, y: 455, w: W, h: 41 };
  // A rect over the column's top (an active inner camera top-left; a share's band) pushes the upper panel's top down.
  const camTL: ReservedRegions = { ...NO_REGIONS, division: [fold], occlusion: [{ x: 20, y: 10, w: 40, h: 40 }] };
  const a = halfOpenSplit(camTL, fold, H, COL, { top: 0, bottomInset: 21 })!;
  ok(a.upper.top === 58, `a camera over the column's top: the upper panel starts below it (${a.upper.top})`);
  const band: ReservedRegions = { ...NO_REGIONS, division: [fold], statusBar: [{ x: 0, y: 0, w: W, h: 44 }] };
  ok(halfOpenSplit(band, fold, H, COL, { top: 0, bottomInset: 21 })!.upper.top === 52, 'a full-width status band: below it');
  // One below the fold lifts the lower panel's bottom.
  const camBL: ReservedRegions = { ...NO_REGIONS, division: [fold], occlusion: [{ x: 20, y: 880, w: 40, h: 40 }] };
  ok(halfOpenSplit(camBL, fold, H, COL, { top: 0, bottomInset: 21 })!.lower.bottom === 872, 'a rect low in the column lifts the lower panel');
  // A rect in the deck's half moves nothing.
  const right: ReservedRegions = { ...NO_REGIONS, division: [fold], occlusion: [{ x: 500, y: 600, w: 40, h: 40 }] };
  const r0 = halfOpenSplit(right, fold, H, COL, { top: 0, bottomInset: 21 })!;
  ok(r0.upper.top === 8 && r0.lower.bottom === H - 29, "a rect in the deck's half leaves both panels alone");
  // No room for two panels (a fold near the top): no split — the box stays below the fold and BIG is not offered.
  ok(halfOpenSplit(NO_REGIONS, { x: 0, y: 80, w: W, h: 30 }, H, COL, { top: 0, bottomInset: 21 }) === null,
     'a fold too near the top: no split (null)');
}

// ── The Servers button: the ordinary left margin in every pose, DROPPED below a camera / status rect that would meet it
//    (Stuart, folded landscape, camera top-left: "it's avoiding the camera by coming in too far, so just do what we did
//    with the health chips and drop it lower"). Full bleed (the Duo): margin 12, top 46. Chip ~120 x 38. ──────────────
{
  const place = (R: ReservedRegions, W: number, H: number, base = { left: 12, top: 46 }) => serversChipPlacement(R, base, 120, 38, W, H);
  const meets = (p: { left: number; top: number }, r: RegionRect) =>
    r.x < p.left + 120 && p.left < r.x + r.w && r.y < p.top + 38 && p.top < r.y + r.h;
  const poses: [string, ReservedRegions, number, number, number][] = [
    // name, regions, window W, H, expected top
    ['folded portrait (camera + status column top-right)', FOLDED, 466, 678, 46],
    ['folded landscape, camera TOP-LEFT', { ...NO_REGIONS, occlusion: [{ x: 29, y: 29, w: 37, h: 37 }] }, 678, 466, 29 + 37 + 8],
    ['folded landscape, camera top-left with the status column beside it',
      { ...NO_REGIONS, occlusion: [{ x: 29, y: 29, w: 37, h: 37 }], statusBar: [{ x: 0, y: 0, w: 84, h: 174 }] }, 678, 466, 174 + 8],
    ['folded landscape, camera bottom-right', LAND, 678, 466, 46],
    ['folded landscape, camera bottom-left', LANDL, 678, 466, 46],
    ['open portrait (pill top-right)', { ...NO_REGIONS, statusBar: [{ x: 560, y: 0, w: 109, h: 40 }], hinge: { status: 'fullyOpen', angleDeg: 180 } }, 669, 951, 46],
    ['open landscape (pill top-right)', { ...NO_REGIONS, statusBar: [{ x: 860, y: 0, w: 91, h: 34 }], hinge: { status: 'fullyOpen', angleDeg: 180 } }, 951, 669, 46],
    ['half-open portrait (pill top-right, the fold)', { ...NO_REGIONS, statusBar: [{ x: 560, y: 0, w: 109, h: 40 }],
      division: [{ x: 0, y: 455, w: 669, h: 41 }], hinge: { status: 'partiallyOpen', angleDeg: 110 } }, 669, 951, 46],
    ['Split View share with a full-width status band', { ...NO_REGIONS, statusBar: [{ x: 0, y: 0, w: 469, h: 32 }] }, 469, 669, 46],
  ];
  for (const [name, R, W, H, top] of poses) {
    const p = place(R, W, H);
    ok(p.left === 12, `Servers button, Duo ${name}: x = the ordinary margin (12)`);
    ok(p.top === top, `…top ${p.top} (expected ${top})`);
    ok(avoidRects(R).every((r) => !meets(p, r)), '…clear of every reserved rect');
  }
  // Ordinary iPhones in landscape (no rects): the caller's inset margin, untouched — the Dynamic Island side.
  const islandLeft = serversChipPlacement(NO_REGIONS, { left: 62 + 8, top: 0 + 46 }, 120, 38, 874, 402);
  ok(islandLeft.left === 70 && islandLeft.top === 46, 'iPhone landscape, Dynamic Island on the left: inset + 8 (70), top 46 — as before');
  const islandRight = serversChipPlacement(NO_REGIONS, { left: 12, top: 46 }, 120, 38, 874, 402);
  ok(islandRight.left === 12 && islandRight.top === 46, 'iPhone landscape, Dynamic Island on the right: 12, top 46 — as before');
  const portrait = serversChipPlacement(NO_REGIONS, { left: 12, top: 62 + 46 }, 120, 38, 402, 874);
  ok(portrait.left === 12 && portrait.top === 108, 'iPhone portrait: 12, under the status row — as before');
}

if (fails) { console.log(`\nduo layout: ${fails} FAILED`); process.exit(1); }
console.log('\nduo layout: all passed');
