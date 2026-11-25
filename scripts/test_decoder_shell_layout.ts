/**
 * Where a decoder box goes (src/utils/decoderShellLayout.ts — DecoderShell's placement, pure). Two promises:
 *  1. NO HINGE, NO DIVISION RECT (every iPhone, iPad, Mac window, Android, and the Duo flat or folded): the box is
 *     placed EXACTLY as before the half-open split work — checked against the pre-split formula (DecoderShell at
 *     35bcd1653: wrap { bottom } + { right: clearRight } when > 8; the frame's maxWidth / maxHeight as given), over a
 *     grid of windows, offsets, caps, BIG / SMALL and corner clearances.
 *  2. HALF-OPEN (the fold as a division rect, partially open): SMALL is one box below the fold beside the deck; BIG is
 *     two panels, neither touching the fold, both in the left column.
 * Run: node --no-warnings scripts/test_decoder_shell_layout.ts   (run-tests.sh does)
 */
import { cornerClearRight, shellPlacement, type ShellAvoid } from '../src/utils/decoderShellLayout.ts';
import { halfOpenSplit, NO_REGIONS, type ReservedRegions } from '../src/utils/duoGeometry.ts';

let fails = 0, n = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}

// The pre-split rule, transcribed from DecoderShell at 35bcd1653 for a context with no frame.
function legacy(p: { bottom: number; maxWidth: number; maxHeight?: number; clearRight: number }) {
  const wrap: Record<string, number> = { bottom: p.bottom };
  if (p.clearRight > 8) wrap.right = p.clearRight;
  return { wrap, maxWidth: p.maxWidth, maxHeight: p.maxHeight };
}

// ── 1. No hinge / no division rect ──────────────────────────────────────────────────────────────
const WINDOWS: [string, number][] = [['iPhone 17 Pro', 402], ['iPhone SE', 375], ['Pro Max', 440], ['SE Display Zoom', 320],
  ['iPad mini', 744], ['iPad Pro 13', 1032], ['Mac window', 1300], ['Duo flat (inner)', 669], ['Duo folded (front)', 466],
  ['Android Moto G35', 393], ['Galaxy Tab A9', 601]];
let same = 0;
for (const [name, W] of WINDOWS) {
  let allSame = true;
  for (const tall of [false, true]) for (const bottom of [96, 180, 312]) for (const maxWidth of [560, 600, 760, 800])
  for (const maxHeight of [undefined, 240, 520]) for (const clearRight of [8, 60, 92]) {
    // A corner-rect context (the folded front screen's camera / status column) carries rects, never a frame.
    const avoid: ShellAvoid = clearRight > 8 ? { rects: [{ x: W - 84, y: 0, w: 84, h: 174 }], windowW: W } : { rects: [], windowW: W };
    const got = shellPlacement(avoid, { tall, bottom, maxWidth, maxHeight, clearRight });
    const want = legacy({ bottom, maxWidth, maxHeight, clearRight });
    n++;
    const eq = got.kind === 'single' && JSON.stringify(got.wrap) === JSON.stringify(want.wrap)
      && got.maxWidth === want.maxWidth && got.maxHeight === want.maxHeight;
    if (eq) same++; else allSame = false;
  }
  ok(allSame, `${name}: every placement identical to before (SMALL and BIG, all offsets, caps and corner clearances)`);
}
ok(same === n, `${same} / ${n} placements identical with no hinge`);
// A Duo flat open (fullyOpen) has no division rect, so SDRScreen gives no frame: covered by the grid above.

// ── 2. Half-open ──────────────────────────────────────────────────────────────────────────────────
{
  const W = 669, H = 951, fold = { x: 0, y: 455, w: W, h: 41 };
  const R: ReservedRegions = { ...NO_REGIONS, division: [fold], statusBar: [{ x: 560, y: 0, w: 109, h: 40 }],
                               hinge: { status: 'partiallyOpen', angleDeg: 110 } };
  const col = { x0: 8, x1: W - (Math.ceil(W / 2) + 4) };
  const sp = halfOpenSplit(R, fold, H, col, { top: 46 + 40, bottomInset: 21 })!;
  const frameBottom = 21 + 8, halfTop = Math.ceil(fold.y + fold.h) + 8;
  const avoid: ShellAvoid = { rects: [], windowW: W, frame: { left: 8, right: Math.ceil(W / 2) + 4, bottom: frameBottom,
    maxHeight: H - halfTop - frameBottom, split: { ...sp, windowH: H } } };
  const small = shellPlacement(avoid, { tall: false, bottom: 300, maxWidth: 760, maxHeight: 400, clearRight: 8 });
  ok(small.kind === 'single', 'half-open SMALL: one box');
  if (small.kind === 'single') {
    const top = H - small.wrap.bottom - (small.maxHeight ?? 0);
    ok(top >= fold.y + fold.h, `…wholly below the fold (its top can reach y ${top}, the fold ends ${fold.y + fold.h})`);
    ok(small.wrap.left === 8 && W - (small.wrap.right ?? 0) <= W / 2, '…in the left column, beside the deck');
  }
  const big = shellPlacement(avoid, { tall: true, bottom: 300, maxWidth: 760, maxHeight: 400, clearRight: 8 });
  ok(big.kind === 'split', 'half-open BIG: two panels');
  if (big.kind === 'split') {
    const span = (p: { wrap: { bottom: number }; maxHeight: number }) => ({ top: H - p.wrap.bottom - p.maxHeight, bottom: H - p.wrap.bottom });
    const u = span(big.upper), l = span(big.lower);
    ok(u.bottom <= fold.y - 8, `upper panel ends ${fold.y - u.bottom} pt above the fold (y ${u.bottom})`);
    ok(l.top >= fold.y + fold.h + 8, `lower panel starts ${l.top - fold.y - fold.h} pt below it (y ${l.top})`);
    ok(u.top >= 86 + 8, `upper panel starts below the Servers chip (y ${u.top})`);
    ok(l.bottom === H - frameBottom, 'lower panel on the bottom line, like SMALL');
    ok(big.upper.wrap.left === 8 && big.upper.wrap.right === W - col.x1 && big.lower.wrap.right === W - col.x1,
       'both panels the left column\'s width');
    ok(big.upper.maxHeight > 300 && big.lower.maxHeight > 350, `room in each (${big.upper.maxHeight} / ${big.lower.maxHeight} pt)`);
  }
  // Half-open with the box stacked over the deck (a share too narrow for side by side): no split offered → one box.
  const stacked: ShellAvoid = { rects: [], windowW: 560, frame: { left: 8, right: 8, bottom: 200, maxHeight: 180 } };
  const st = shellPlacement(stacked, { tall: true, bottom: 300, maxWidth: 760, clearRight: 8 });
  ok(st.kind === 'single' && st.maxHeight === 180, 'half-open, stacked over the deck: BIG stays one box below the fold');
}

// ── 3. Corner rects: move the box only when the CENTRED box would reach one (Stuart, open inner screen, landscape, DAB
//    BIG: "inner screen doesn't need the shift to the left in big mode" — the pill at ~96 % of the width). ────────────
{
  const centredBox = (W: number, maxW: number, clear: number) => {
    const wrapL = 8, wrapR = W - clear, boxW = Math.min(maxW, wrapR - wrapL);
    const l = wrapL + (wrapR - wrapL - boxW) / 2; return { l, r: l + boxW };
  };
  // Open inner screen, landscape 951 x 669: the status pill top-right, the edge tabs' rect at the right edge.
  const W = 951;
  const pill = { x: 900, y: 0, w: 51, h: 34 }, tabs = { x: W - 60, y: 60, w: 60, h: 96 };
  for (const [name, maxW] of [['DAB', 560], ['decoder box', 760], ['Advanced RDS (two columns)', 800], ['Advanced RDS', 600]] as const) {
    const big = cornerClearRight([pill, tabs], W, { y: 8, h: 600 }, maxW);
    const small = cornerClearRight([pill, tabs], W, { y: 380, h: 240 }, maxW);
    const b = centredBox(W, maxW, big);
    ok(big === 8 && small === 8, `inner landscape, ${name} (${maxW} pt): BIG and SMALL stay centred, as on an iPad`);
    ok(Math.abs((b.l + b.r) / 2 - W / 2) < 0.5, `…BIG's centre is the window's (${Math.round(b.l / W * 100)} %–${Math.round(b.r / W * 100)} % of the width)`);
  }
  // Folded front screen, portrait 466 x 678: the status column [382,0 84x174] — BIG rises beside it and still clears it.
  const col = { x: 382, y: 0, w: 84, h: 174 };
  for (const [name, maxW] of [['DAB', 560], ['decoder box', 760], ['Advanced RDS', 600]] as const) {
    const big = cornerClearRight([col], 466, { y: 20, h: 560 }, maxW);
    const b = centredBox(466, maxW, big);
    ok(big === 466 - 382 + 8 && b.r <= 382 - 8, `folded portrait BIG, ${name}: right edge ${b.r} clears the column (382) by 8+`);
    ok(cornerClearRight([col], 466, { y: 300, h: 240 }, maxW) === 8, `folded portrait SMALL below the column, ${name}: centred`);
  }
  ok(cornerClearRight([], 402, { y: 0, h: 800 }, 760) === 8, 'no rects (every iPhone / iPad): never moved');
  // A rect the centred box would come within 8 pt of (not overlap) still moves it.
  ok(cornerClearRight([{ x: 870, y: 0, w: 60, h: 40 }], W, { y: 0, h: 500 }, 800) === W - 870 + 8,
     'a rect within 8 pt of the centred box still moves it');
}

if (fails) { console.log(`\ndecoder shell layout: ${fails} FAILED`); process.exit(1); }
console.log('\ndecoder shell layout: all passed');
