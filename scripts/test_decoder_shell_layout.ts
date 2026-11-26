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
import { bigOffered, cornerClearRight, independentRowPanel, LIST_DECODER_SPLIT, shellPlacement, type ListDecoder,
         type ShellAvoid } from '../src/utils/decoderShellLayout.ts';
import { linkedContentH, linkedEndPad, linkedMaxY, linkedReveal, linkedRowWhere, linkedSeam, linkedSnap, linkedSnapOffsets,
         type RowBox } from '../src/utils/linkedListScroll.ts';
import { halfOpenSplit, NO_REGIONS, type ReservedRegions } from '../src/utils/duoGeometry.ts';
import { advRdsPlotLayout, advRdsPlotsWidth } from '../src/utils/advRdsPlotLayout.ts';

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
  const sp = halfOpenSplit(R, fold, H, col, { top: 0, bottomInset: 21 })!;
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
    ok(u.top === 8, `upper panel runs to the top edge (y ${u.top}) — the Servers chip tucks into its tab instead`);
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

// ── 4. The LOWER panel's content is never wider than the panel (Stuart, half-open, Advanced RDS BIG: "labels getting
//    cut off" — the plots were laid out for a box, 330 pt, in a ~296 pt panel). Panel width from halfOpenSplit (the
//    left column), minus the frame's border, the metal window's side margins (0 on glass, 8 on metal) and the body's
//    padding (12 left, 12 scroll lane right). ────────────────────────────────────────────────────────────────────
{
  // 624 = the narrowest window that puts the box beside the deck (half the width − 12 >= 300).
  for (const W of [669, 624, 640, 744, 820]) {
    const H = Math.round(W * 1.42), fold = { x: 0, y: Math.round(H * 0.48), w: W, h: 40 };
    const sp = halfOpenSplit({ ...NO_REGIONS, division: [fold] }, fold, H, { x0: 8, x1: W - (Math.ceil(W / 2) + 4) },
                             { top: 0, bottomInset: 21 })!;
    const panelW = sp.x1 - sp.x0 - 2;
    for (const inset of [0, 8]) {
      const body = panelW - 2 * inset;
      const contentW = body - 12 - 12;
      const l = advRdsPlotLayout(contentW);
      ok(advRdsPlotsWidth(l) <= contentW,
         `${W} pt window, ${inset ? 'metal' : 'glass'}: Advanced RDS plots ${advRdsPlotsWidth(l)} pt in ${contentW} pt (${l.stacked ? 'stacked' : 'side by side'}, trace ${l.eyeW})`);
      // Spots, compact (the lower panel): time 36 + band 34 + mode 35 + SNR 30 + distance 48 (+4) + call margin 6, five
      // 4 pt gaps, 10 + 10 row padding, no country column; the callsign takes the rest — at least 48 pt (six characters; a longer one ends in an ellipsis, never clipped).
      const spotsFixed = 36 + 34 + 35 + 30 + 48 + 4 + 6 + 5 * 4 + 20;
      ok(body - spotsFixed >= 48, `${W} pt, ${inset ? 'metal' : 'glass'}: spots table fits, callsign ${body - spotsFixed} pt`);
      // ADS-B: flag 22 + altitude 78 + distance 82, three 8 pt gaps, 10 + 10 padding; the callsign takes the rest.
      const adsbFixed = 22 + 78 + 82 + 3 * 8 + 20;
      ok(body - adsbFixed >= 48, `${W} pt, ${inset ? 'metal' : 'glass'}: ADS-B table fits, callsign ${body - adsbFixed} pt`);
    }
  }
  const box = advRdsPlotLayout(null);
  ok(box.constSize === 120 && box.plotW === 180 && box.eyeW === 310 && !box.stacked, 'every other layout: the plots exactly as before');
}

// ── 5. beta96: EVERY list decoder splits across the crease in half-open BIG (Stuart 2026-11-26: "the duo needs the layout
//    we just built with decoder list split across the crease"), and nowhere else changes. ─────────────────────────────
{
  const W = 669, H = 951, fold = { x: 0, y: 455, w: W, h: 41 };
  const R: ReservedRegions = { ...NO_REGIONS, division: [fold], hinge: { status: 'partiallyOpen', angleDeg: 110 } };
  const col = { x0: 8, x1: W - (Math.ceil(W / 2) + 4) };
  const sp = halfOpenSplit(R, fold, H, col, { top: 0, bottomInset: 21 })!;
  const split: ShellAvoid = { rects: [], windowW: W, frame: { left: 8, right: Math.ceil(W / 2) + 4, bottom: 29,
    maxHeight: 400, split: { ...sp, windowH: H } } };
  const decoders = Object.keys(LIST_DECODER_SPLIT) as ListDecoder[];
  ok(decoders.length === 9 && ['cw', 'psk', 'rtty', 'jtty', 'dsc', 'ncdxf', 'balloon', 'aprs', 'scanner'].every((d) => d in LIST_DECODER_SPLIT),
     'a half-open plan for every list decoder: CW, PSK, RTTY, JT-TTY, DSC, NCDXF, balloons, APRS, the scanner');
  for (const d of decoders) {
    const plan = LIST_DECODER_SPLIT[d];
    // BIG offered half-open even where BIG would gain nothing over SMALL (the beta95 rule hid it for these).
    ok(bigOffered(true, 300, 300), `${d}: BIG offered half-open`);
    const big = shellPlacement(split, { tall: true, bottom: 300, maxWidth: 760, clearRight: 8 });
    ok(big.kind === 'split', `${d}: BIG is two panels (${plan.scroll}) — upper: ${plan.upper}; lower: ${plan.lower}`);
    if (big.kind === 'split') {
      const ub = H - big.upper.wrap.bottom, lt = H - big.lower.wrap.bottom - big.lower.maxHeight;
      ok(ub <= fold.y - 8 && lt >= fold.y + fold.h + 8, `${d}: nothing on the crease (upper ends y ${ub}, lower starts y ${lt})`);
    }
    // No hinge: exactly as before — one box, and BIG only where it gains 40 pt.
    for (const [name, Wn] of WINDOWS) {
      const one = shellPlacement({ rects: [], windowW: Wn }, { tall: true, bottom: 180, maxWidth: 760, maxHeight: 520, clearRight: 8 });
      if (one.kind !== 'single' || one.wrap.bottom !== 180 || one.maxHeight !== 520) ok(false, `${d} on ${name}: unchanged`);
    }
    ok(!bigOffered(false, 230, 200) && bigOffered(false, 240, 200), `${d}: no hinge — BIG only where it gains 40 pt (as before)`);
  }
  ok(LIST_DECODER_SPLIT.dsc.scroll === 'linked', 'DSC (one list: keys, then the calls) scrolls as ONE list across the fold');
  for (const d of ['cw', 'psk', 'rtty', 'jtty', 'ncdxf', 'balloon', 'aprs', 'scanner'] as ListDecoder[])
    ok(LIST_DECODER_SPLIT[d].scroll === 'independent', `${d}: live view above, list below — each panel scrolls on its own`);
  // Independent: pad focus crosses the fold from the last signal to the first spot.
  ok(independentRowPanel(4, 5) === 'upper' && independentRowPanel(5, 5) === 'lower' && independentRowPanel(0, 0) === 'lower',
     'independent split: rows 0…n-1 (signals) above the fold, n… (spots) below; no signals = all below');
}

// ── 6. The linked list: ONE list continuing across the fold (utils/linkedListScroll). ─────────────────────────────
{
  const col = (hs: number[]): RowBox[] => { let t = 0; return hs.map((h) => { const r = { top: t, h }; t += h; return r; }); };
  // Duo 669 x 951 half-open: the panels' inside heights less the header (46) — upper ~355, lower ~380 (uneven).
  const upperH = 355, lowerH = 380;
  // DSC: the channel keys block (78), then calls of 44 / 61 / 78 pt (one, two, three text lines).
  const rows = col([78, 61, 44, 78, 61, 61, 44, 78, 61, 44, 61, 78, 44, 61, 61, 78, 44, 61, 61, 44]);
  const contentH = linkedContentH(rows);
  const seam = linkedSeam(rows, upperH);
  ok(seam <= upperH && rows.some((r) => r.top + r.h === seam) && !rows.some((r) => r.top + r.h > seam && r.top + r.h <= upperH),
     `seam ${seam} pt: the last row boundary that fits the upper panel (${upperH}) — no row cut at the fold at the top`);
  const maxY = linkedMaxY(rows, seam, lowerH);
  const pad = linkedEndPad(rows, seam, lowerH, maxY);
  ok(maxY >= contentH - seam - lowerH && pad === maxY - (contentH - seam - lowerH) && pad < Math.max(...rows.map((r) => r.h)),
     `scrolls ${maxY} pt — the last row in the lower panel, ${pad} pt blank under it (less than a row) so the fold stays on a boundary`);
  const offs = linkedSnapOffsets(rows, seam, maxY);
  ok(offs[0] === 0 && offs[offs.length - 1] === maxY, 'rest offsets run from the top to the end');
  // The lower panel shows the rows that FOLLOW the upper's last row: at every rest offset, the row starting at the fold.
  let cont = true, noCut = true;
  for (const y of offs) {
    const upperRows = rows.filter((r) => linkedRowWhere(r, y, seam, lowerH) === 'upper');
    const lowerRows = rows.filter((r) => linkedRowWhere(r, y, seam, lowerH) === 'lower');
    const lastU = upperRows[upperRows.length - 1], firstL = lowerRows[0];
    if (lastU && firstL && rows.indexOf(firstL) !== rows.indexOf(lastU) + 1) cont = false;
    if (rows.some((r) => linkedRowWhere(r, y, seam, lowerH) === 'fold')) noCut = false;
  }
  ok(cont, 'at rest the lower panel continues with the row after the upper panel\'s last (one list, no gap, no repeat)');
  ok(noCut, `no row straddles the fold at any rest offset (all ${offs.length} of them, the end included)`);
  // Both panels share ONE offset: a drag of d moves every row in both panels by d (linked, nothing jumps).
  ok([0, 7, 33, 120].every((d) => rows.every((r) => (r.top - (40 + d)) - (r.top - 40) === -d)), 'one offset moves both panels together');
  // Mid-drag a row may straddle; when the finger lifts, it settles whole on one side.
  const mid = 0 + 23;
  const straddler = rows.find((r) => linkedRowWhere(r, mid, seam, lowerH) === 'fold');
  const rest = linkedSnap(mid, offs);
  ok(!!straddler && ['upper', 'lower', 'above'].includes(linkedRowWhere(straddler, rest, seam, lowerH)),
     `a row across the fold mid-drag (y ${mid}) settles whole on one side (rest at ${rest})`);
  // Uneven panels: a tall upper with a short lower, and the reverse.
  for (const [u, l] of [[480, 160], [140, 520]]) {
    const s2 = linkedSeam(rows, u), m2 = linkedMaxY(rows, s2, l), o2 = linkedSnapOffsets(rows, s2, m2);
    ok(s2 <= u && o2.every((y) => !rows.some((r) => linkedRowWhere(r, y, s2, l) === 'fold')),
       `uneven panels ${u} / ${l}: seam ${s2}, no row cut at the fold at rest`);
  }
  // A first row taller than the upper panel: the whole panel, cut as any list cuts a row too tall for it.
  ok(linkedSeam(col([400, 44]), 355) === 355, 'a first row taller than the upper panel: the panel\'s whole height');
  ok(linkedSeam([], 355) === 355 && linkedMaxY([], 355, 380) === 0, 'no rows yet: nothing to scroll');
  // A short list: it all fits — no scrolling, the lower shows what the upper could not.
  const short = col([78, 61, 44, 78, 61, 61, 44]);
  const ss = linkedSeam(short, upperH);
  ok(linkedMaxY(short, ss, lowerH) === 0, 'a short list fits across the two panels without scrolling');

  // Pad / D-pad focus walking down the list across the fold: never moves while the row is whole in either panel; when
  // it must move, the row comes into view whole, and the rest offset keeps the fold on a boundary.
  let y = 0, moves = 0, okAll = true, crossedStill = false;
  for (let i = 0; i < rows.length; i++) {
    const before = linkedRowWhere(rows[i], y, seam, lowerH);
    const ny = linkedReveal(rows, i, y, seam, lowerH, offs);
    const after = linkedRowWhere(rows[i], ny, seam, lowerH);
    if (after !== 'upper' && after !== 'lower') okAll = false;
    if ((before === 'upper' || before === 'lower') && ny !== y) okAll = false;
    if (i > 0 && ny === y && linkedRowWhere(rows[i - 1], y, seam, lowerH) === 'upper' && after === 'lower') crossedStill = true;
    if (ny !== y) moves++;
    if (!offs.includes(ny)) okAll = false;
    y = ny;
  }
  ok(okAll, `focus down all ${rows.length} rows: each shown whole, at a rest offset; ${moves} scrolls`);
  ok(crossedStill, 'focus steps from the upper panel\'s last row to the lower panel\'s first WITHOUT scrolling');
  let up = true;
  for (let i = rows.length - 1; i >= 0; i--) {
    const ny = linkedReveal(rows, i, y, seam, lowerH, offs);
    const w = linkedRowWhere(rows[i], ny, seam, lowerH);
    if (w !== 'upper' && w !== 'lower') up = false;
    y = ny;
  }
  ok(up && y === 0, 'focus back up to the first row: each shown whole, ending at the top');
}

if (fails) { console.log(`\ndecoder shell layout: ${fails} FAILED`); process.exit(1); }
console.log('\ndecoder shell layout: all passed');
