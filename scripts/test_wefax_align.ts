// test_wefax_align.ts — WEFAX SHIFT / SLANT and the per-chart margin finder (src/utils/wefaxAlign.ts).
// Synthetic charts: a speckled page with a few curved "isobars", plus (or not) a black margin line that drifts with
// the station's slant — the shape measured on Northwood 4610, 2026-10-04 (margin 40 px in on one chart, ~370 on another).
import { findMargin, findMarginSlant, findGutter, findChartAlign, findHeaderBar, findStrip, chartAlignStep, wefaxFormat, wefaxOffset, drawnAlign, rotateLine, wefaxPreset, MARGIN_AFTER_LINES,
         type ChartAlignState, type WefaxFormat } from '../src/utils/wefaxAlign.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const W = 1809;
let seed = 7; const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
function chart(lines: number, margin: number | null, slant: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W);
    for (let x = 0; x < W; x++) r[x] = 200 + Math.floor(rnd() * 55) - (rnd() < 0.08 ? 150 : 0);   // speckled page
    for (const c of [300, 700, 1200]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); for (let k = -1; k <= 1; k++) r[(cx + k + W) % W] = 20; } // curved isobars
    if (margin !== null) { const m = Math.round(margin + slant * y); for (let k = -3; k <= 3; k++) r[((m + k) % W + W) % W] = 0; }
    rows.push(r);
  }
  return rows;
}
// ★ The format of the station a synthetic chart is drawn for (2026-10-07: auto-align acts only on a station's own
//   format): slant 0 = DDK 7880 (white border), −0.06 = Northwood 4610 (margin).
const DDK = wefaxFormat(7878100), GYA = wefaxFormat(4608100);
const fmtOf = (slant: number): WefaxFormat => (slant ? GYA : DDK);
const near = (a: number | null, b: number, tol = 3) => a !== null && Math.abs(((a - b + W * 1.5) % W) - W / 2) <= tol;
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 42, -0.06), W, -0.06), 42), 'margin 42 px in, Northwood slant → found at 42');
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 368, -0.06), W, -0.06), 368), 'margin dead centre-ish (368) → found');
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 5, 0), W, 0), 5), 'DWD-like: margin at the edge, no slant → found at 5 (a 3 px move)');
ok(findMargin(chart(MARGIN_AFTER_LINES, null, 0), W, 0) === null, 'no margin on the chart → null (nothing is moved)');
ok(findMargin(chart(40, 42, -0.06), W, -0.06) === null, 'too few lines yet → null');

// ★ 2026-10-05 — slant MEASURED per chart (MadPsy: "depends on the frequency accuracy of the particular hardware").
{ const m = findMarginSlant(chart(300, 42, -0.09), W, -0.06, 0, 300);   // Northwood + ~16 ppm of receiver clock
  ok(!!m && near(m.col, 42) && Math.abs(m.slant + 0.09) <= 0.003, `slant measured, not preset: -0.09 found as ${m?.slant}`); }
{ const m = findMarginSlant(chart(300, 400, 0.03), W, 0, 0, 300);
  ok(!!m && near(m.col, 400) && Math.abs(m.slant - 0.03) <= 0.003, `a fast receiver clock on a straight station: +0.03 found as ${m?.slant}`); }
// ★ DDK-style: a map in a thin black FRAME on a white border (no margin). Phased → border at the edges.
function bordered(lines: number, roll: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(252);
    for (let x = 55; x <= 1745; x++) r[x] = 215 + Math.floor(rnd() * 40) - (rnd() < 0.08 ? 150 : 0);  // the map
    r[55] = r[56] = r[1744] = r[1745] = 10;                                                             // the frame
    for (const c of [500, 1100]) { const cx = Math.round(c - 0.17 * y); r[cx] = r[cx + 1] = 20; }      // meridians
    const o = new Uint8Array(W);
    for (let x = 0; x < W; x++) o[x] = r[(x + roll) % W];
    rows.push(o);
  }
  return rows;
}
ok(findChartAlign(bordered(300, 0), W, 0, 300, 150, undefined, DDK) === null, 'DDK phased: frame is NOT a margin, border already at the edge → not moved');
{ const a = findChartAlign(bordered(300, 900), W, 0, 300, 150, undefined, DDK);
  // joined mid-way: line starts 900 px late, so the border (orig 1746…54) sits at 846…963 — centre ≈ 900 → moved to the edge
  ok(!!a && Math.abs(a.shift - 900) <= 6 && a.slant === 0, `DDK joined mid-chart: the white border is cut at ${a?.shift} (≈900)`); }
ok(findGutter(chart(300, 42, -0.06), W, -0.06, 0, 300) === null, 'a Northwood-style chart has no blank band');
{ const a = findChartAlign(chart(300, 42, -0.06), W, -0.06, 300, 150, undefined, GYA);
  ok(!!a && near(a.shift, 40), 'Northwood: margin path still wins'); }
// ★ chartAlignStep: decided at 300 lines WITH CONTENT, refines at 600 only a chart it moved; phased DDK untouched.
const feed = (st: ChartAlignState, rows: Uint8Array[], slant: number, upto: number, fmt = fmtOf(slant)) => {
  let moved = 0;
  for (let y = 0; y < upto; y++) if (chartAlignStep(st, () => rows.slice(0, y + 1), W, slant, rows[y], fmt)) moved++;
  return moved;
};
{ const rows = chart(700, 42, -0.06); const st: ChartAlignState = {};
  ok(feed(st, rows, -0.06, 299) === 0 && st.al === undefined, 'nothing before 300 lines');
  const st2: ChartAlignState = {}; ok(feed(st2, rows, -0.06, 300) === 1 && !!st2.al && near(st2.al.shift, 40), 'decided at 300 → one redraw, margin to the edge');
  const st3: ChartAlignState = {}; feed(st3, rows, -0.06, 650);
  ok(st3.refined === true, 'second look taken once, by 600'); }
{ const rows = bordered(700, 0); const st: ChartAlignState = {};
  ok(feed(st, rows, 0, 700) === 0 && st.al === null, 'phased DDK: left alone at 300, still alone at 600'); }
// ★ DDK's steady tone before the chart (off air, 2026-10-05): ~270 flat grey lines, then a chart joined without
//   phasing. The grey must not hide the border, and the flat lines must not count towards the 300.
{ const tone = Array.from({ length: 270 }, () => { const r = new Uint8Array(W); for (let x = 0; x < W; x++) r[x] = 100 + Math.floor(rnd() * 20); return r; });
  const rows = [...tone, ...bordered(600, 900)]; const st: ChartAlignState = {};
  ok(feed(st, rows, 0, 300) === 0 && st.al === undefined && (st.n ?? 0) < 300, 'tone lines are not counted (no decision at line 300)');
  const st2: ChartAlignState = {}; let moved = 0;
  for (let y = 0; y < rows.length && st2.al === undefined; y++) if (chartAlignStep(st2, () => rows.slice(0, y + 1), W, 0, rows[y], DDK)) moved++;
  ok(moved === 1 && !!st2.al && Math.abs(st2.al.shift - 900) <= 6, `after the tone, the border is still found: cut at ${st2.al?.shift} (≈900)`); }
// ★★★ 2026-10-06 — Stuart's DDK 7880 charts on his RSP1A, phased, then cut by auto-align. The geometry of the UberSDR
//     copy of the 12:56 UTC chart (20261006_130727_f8c5ee8e.png, measured): ONE frame line, on the left, at 55–57; the
//     map stops at ~1695 with no right frame; blank paper 1696…1809 and 0…54 (wraps); a header of dashes on the first
//     15 lines from 45 to 1765, INTO the right border; and a near-vertical front (~180 ink) — the line the margin search
//     took at 1042 on the 13:08 chart (58 % across: the border landed 42 % across, Stuart's screenshot). Then grey
//     noise σ 45, which on the RX888 set's own charts hid the border from the old 2 %-of-lines test on 146 of 149.
const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return (s - 3) / Math.sqrt(0.5); };
function ddk1006(lines: number, roll: number, sigma: number, slant = 0): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(250);
    if (y < 15) { for (let x = 45; x <= 1765; x++) if ((x >> 3) & 1) r[x] = 30; }        // header dashes
    else {
      for (let x = 58; x <= 1695; x++) if (rnd() < 0.04) { r[x] = 40; r[x + 1] = 40; }  // text / coast, 2 px
      for (const c of [300, 700, 1300]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); r[cx] = r[cx + 1] = r[cx + 2] = 20; }
      if (y % 120 < 2) for (let x = 58; x <= 1695; x++) r[x] = 30;                       // a parallel
      const f = Math.round(1044 - 0.035 * y); for (let k = -2; k <= 3; k++) r[f + k] = 60;  // the front
    }
    r[55] = r[56] = r[57] = 10;                                                         // the ONE frame line
    if (y > 15 && y < 26) for (let x = 0; x < W; x++) if (rnd() < 0.15) r[x] = 60;       // speckle rows, full width
    const o = new Uint8Array(W), off = Math.round(roll + slant * y);
    for (let x = 0; x < W; x++) o[x] = Math.max(0, Math.min(255, Math.round(r[(((x - off) % W) + W) % W] + (sigma ? gauss() * sigma : 0))));
    rows.push(o);
  }
  return rows;
}
const signed = (s: number) => ((((s % W) + W + W / 2) % W) - W / 2);
for (const sigma of [0, 45]) {
  const st: ChartAlignState = {}; feed(st, ddk1006(700, 0, sigma), 0, 700);
  const s = st.al ? signed(st.al.shift) : 0;
  // band 1696…54 → centre 1784 = −25: content moves RIGHT ~25, so 55 px of white on the left becomes ~80, 114 → ~89
  ok(st.via === 'border' && Math.abs(s + 25) <= 8, `13:15 DDK, phased, σ${sigma}: centred on its border (shift ${s} ≈ −25), never cut`);
}
{ const st: ChartAlignState = {}; feed(st, ddk1006(700, 900, 45), 0, 700);
  ok(!!st.al && Math.abs(signed(st.al.shift - 900) + 25) <= 8, `the same chart joined 900 px late, σ45: cut in its border (${st.al?.shift} ≈ 875)`); }
// ★ The slant from the frame line (Stuart: DDK on his RSP1A has "a very slight lean backwards"; the RX888 set measures
//   +0.010…0.012 from the frame's drift, 1 px per 100 lines, on every clean DDK chart).
for (const k of [0.011, -0.03]) {
  const st: ChartAlignState = {}; feed(st, ddk1006(700, 0, 30, k), 0, 700);
  ok(!!st.al && Math.abs(st.al.slant - k) <= 0.002, `bordered chart leaning ${k}: slant measured from its frame as ${st.al?.slant}`);
}
// ★ No straight line beside the border (DDK's schedule / text pages): the slant stays the station's.
{ const rows = Array.from({ length: 400 }, () => { const r = new Uint8Array(W).fill(250);
    for (let x = 100; x <= 1700; x++) if (rnd() < 0.05) { r[x] = 30; r[x + 1] = 30; } return r; });
  const a = findChartAlign(rows, W, 0, 400, 200, undefined, DDK);
  ok(a === null || a.slant === 0, `a text page: no slant invented (${a?.slant})`); }

// ★★★ 2026-10-07 — Stuart's DDK 7880 on the Pi 500's Airspy HF+ (RC24 iPhone): the North Sea SST chart, joined ~670
//     lines in, ROLLED with its wide white margins joined in the middle of the picture, and leaning BACKWARDS
//     +0.11 px/line (its frame line, measured on the finished 11:10 screenshot; the same chart on the UberSDR's RX888,
//     20261007_100515_90f67e54.png, leans +0.010). Its geometry, from that RX888 copy: a 4 px frame at 257 and 1563;
//     blank paper 0…139 and 1691…1808; "60°N"/"56°N"/"52°N" labels in the margins beside the frame every ~440 lines;
//     meridians inside leaning −0.06…+0.14; land stipple; under the map a legend in big text from 245 to 1580.
function sst(lines: number, roll: number, slant: number, sigma: number, mapLines = lines): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(250);
    if (y < mapLines) {
      for (let x = 261; x < 1563; x++) if (rnd() < 0.05) r[x] = r[x + 1] = 30;                     // stipple, coasts
      for (const [c, k] of [[400, -0.06], [700, 0.0], [1000, 0.07], [1300, 0.14]] as const) {      // meridians
        const cx = Math.round(c + k * y); if (cx > 262 && cx < 1560) r[cx] = r[cx + 1] = r[cx + 2] = 15;
      }
      if (y % 220 < 3) for (let x = 257; x <= 1566; x++) r[x] = 20;                              // parallels
      if (y % 440 < 30) for (const x0 of [140, 1580]) for (let x = x0; x < x0 + 110; x++) if (rnd() < 0.3) r[x] = 20; // labels
      for (const f of [257, 1563]) for (let k = 0; k < 4; k++) r[f + k] = 0;                      // the frame
    } else if ((y - mapLines) % 70 < 30 && y - mapLines < 300) {
      for (let x = 245; x < 1580; x++) if (rnd() < 0.25) r[x] = r[x + 1] = 25;                    // the legend
    }
    const o = new Uint8Array(W), off = Math.round(roll + slant * y);
    for (let x = 0; x < W; x++) o[x] = Math.max(0, Math.min(255, Math.round(r[(((x - off) % W) + W) % W] + (sigma ? gauss() * sigma : 0))));
    rows.push(o);
  }
  return rows;
}
const cutIn = (shift: number | undefined, roll: number) => shift !== undefined && Math.abs(signed(shift - roll - 8)) <= 25;
for (const sigma of [0, 45]) {
  const roll = 1220, st: ChartAlignState = {};
  feed(st, sst(700, roll, 0.11, sigma), 0, 700);
  ok(!!st.al && st.via === 'border' && cutIn(st.al.shift, roll),
     `SST rolled, wide margins + labels, σ${sigma}: cut in its white band (shift ${st.al?.shift} ≈ ${roll + 8})`);
  ok(st.slant !== undefined && Math.abs(st.slant - 0.11) <= 0.01,
     `SST leaning +0.11 (beyond the old ±0.05 search), σ${sigma}: slant measured from the frame as ${st.slant}`);
}
// ★★ WHY THE CLIENTS CENTRE THE SEARCH ON THE STATION'S SLANT, never a saved one: centred on a stale −0.06 (a slant
//    saved on another radio, or Northwood's), the search ends at +0.09 and stops there, short of the chart's +0.11.
{ const st: ChartAlignState = {}; feed(st, sst(700, 1220, 0.11, 30), -0.06, 700, DDK);
  ok(st.slant !== undefined && st.slant < 0.1, `centred on −0.06 the search cannot reach +0.11 (stops at ${st.slant})`); }
// ★★ A phased chart (band already at the ends) measures its slant too — reported in st.slant, nothing moved.
{ const st: ChartAlignState = {}; let moved = 0; const rows = sst(700, -8, 0.05, 30);
  for (let y = 0; y < rows.length; y++) if (chartAlignStep(st, () => rows.slice(0, y + 1), W, 0, rows[y], DDK)) moved++;
  ok(st.atEdge === true && (st.al === null || st.al.shift === 0) && st.slant !== undefined && Math.abs(st.slant - 0.05) <= 0.01,
     `phased SST leaning +0.05: left where it is, slant measured (${st.slant}), shift ${st.al?.shift ?? 0}`); }
// ★★ Joined near the end: ~230 lines of map + legend, then white paper — never 300 lines with content. Looked at when
//    the chart ends (CHART_END_FLAT featureless lines). ★ 2026-10-07: and then cut in its band, or LEFT ALONE — the
//    band beside the legend is bounded by latitude labels here, not by the frame, so nothing straight confirms it as
//    a border (findChartAlign: conservative, worldwide); it is never cut anywhere else.
{ const rows = sst(500, 900, 0.01, 30, 100), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length);
  ok((st.n ?? 0) < 300 && st.al !== undefined && (st.al === null || cutIn(st.al.shift, 900)),
     `joined at the end (${st.n} lines with content): looked at when it ended, cut in its band or left (${st.al?.shift ?? 'left'})`); }
// ★★ A first look that found nothing is taken again at 600 — once.
{ const st: ChartAlignState = { al: null, n: 599 }; let calls = 0;
  const rows = sst(600, 1220, 0, 0);
  const moved = chartAlignStep(st, () => { calls++; return rows; }, W, 0, rows[599], DDK);
  ok(moved && calls === 1 && cutIn(st.al?.shift, 1220) && st.retried === true, `nothing at 300 → looked again at 600 and cut (${st.al?.shift})`);
  ok(!chartAlignStep(st, () => { calls++; return rows; }, W, 0, rows[599], DDK) && calls === 1, '…and only once'); }
{ const st: ChartAlignState = { al: null, atEdge: true, n: 599 }; let calls = 0;
  chartAlignStep(st, () => { calls++; return sst(600, 1220, 0, 0); }, W, 0, sst(1, 0, 0, 0)[0], DDK);
  ok(calls === 0, 'a phased chart (border centred at the first look) is never looked at again'); }

// ★★ No signal at all (Northwood 4610, 20261006_154718: 3000 lines of noise): nothing is ever moved — the second look
//    used to find a "border" in it at −0.153 once the border search was widened.
{ const rows = Array.from({ length: 700 }, () => { const r = new Uint8Array(W); for (let x = 0; x < W; x++) r[x] = Math.floor(rnd() * 256); return r; });
  const st: ChartAlignState = {}; ok(feed(st, rows, -0.06, 700) === 0 && !st.al, `pure noise: never moved (${JSON.stringify(st.al)})`); }

// ★★★ 2026-10-07 14:59 BST — Stuart's DDK 7880 on the HF+ (RC25 Mac), joined near the END of a chart, no phasing: drawn
//     leaning forward (~+0.05 applied to a chart whose true slant is +0.01). On a late join the lines beside the white
//     band are a legend box, a text box, a coastline: dark enough on average over the stretch they cover, but they
//     cover only part of the look. A slant away from the station's is believed only from a line that runs the whole way
//     down AND beats the band at the station's own slant.
function lateJoin(lines: number, roll: number, sigma: number, boxSlant: number, frame: boolean): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(250);
    for (let x = 175; x < 1690; x++) if (rnd() < 0.05) r[x] = r[x + 1] = 30;                         // map
    for (const c of [500, 1100]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); r[cx] = r[cx + 1] = 20; }
    if (frame) r[170] = r[171] = r[172] = 10;                                                         // a whole frame
    // the "icon_tkb" box beside the band: its edge leans boxSlant, solid, over the first 120 lines only (40 % of the
    // first look, 20 % of the second)
    if (y < 120) { const bx = Math.round(165 + boxSlant * y); for (let k = 0; k < 4; k++) r[bx + k] = 0; }
    const o = new Uint8Array(W), off = Math.round(roll + 0.01 * y);
    for (let x = 0; x < W; x++) o[x] = Math.max(0, Math.min(255, Math.round(r[(((x - off) % W) + W) % W] + (sigma ? gauss() * sigma : 0))));
    rows.push(o);
  }
  return rows;
}
for (const sigma of [0, 45]) {
  const st: ChartAlignState = {}; feed(st, lateJoin(700, 1300, sigma, 0.06, false), 0, 700);
  const k = st.slant ?? 0;
  ok(Math.abs(k - 0.01) <= 0.015 && (!st.al || Math.abs(st.al.slant - 0.01) <= 0.015),
     `late join, a box edge over 120 lines leaning +0.07, σ${sigma}: slant not taken from it (${st.slant}, drawn ${st.al?.slant})`);
}
for (const sigma of [0, 45]) {
  const st: ChartAlignState = {}; feed(st, lateJoin(700, 1300, sigma, 0.06, true), 0, 700);
  ok(st.slant !== undefined && Math.abs(st.slant - 0.01) <= 0.004, `…with a frame the whole way down, σ${sigma}: its slant measured (${st.slant})`);
}
// ★★ …and the same for a leaning chart: the +0.11 SST chart's frame runs the whole height, so it is still measured
//    (the two SST cases above); a chart that leans +0.11 with NO whole line beside its band keeps the station's.
{ const st: ChartAlignState = {}; feed(st, sst(700, 1220, 0.11, 30).map((r) => r), 0, 700);
  ok(st.slant !== undefined && Math.abs(st.slant - 0.11) <= 0.01, `SST +0.11 still measured with the whole-line rule (${st.slant})`); }
// ★★ Never a sign-flipped slant: Northwood (station −0.06) joined late, nothing straight but a short box edge at +0.06.
{ const st: ChartAlignState = {}; feed(st, lateJoin(700, 1300, 30, 0.13, false).map((r, y) => {
    const o = new Uint8Array(W), off = Math.round(-0.07 * y); for (let x = 0; x < W; x++) o[x] = r[(((x - off) % W) + W) % W]; return o; }), -0.06, 700);
  ok(st.slant === undefined || Math.abs(st.slant + 0.06) <= 0.015, `Northwood late join: no +0.06 from a short edge (${st.slant})`); }

// ★★ THE HEADER BAR (Stuart, 2026-10-07: "notice the black bar at the top with the white cutouts, that is how it is
//    aligned"): ~15 lines of black with ONE ~90 px white gap — split across the ends on a phased chart.
function withBar(rows: Uint8Array[], gapCentre: number): Uint8Array[] {
  const bar = Array.from({ length: 15 }, () => { const r = new Uint8Array(W).fill(8);
    for (let k = -45; k < 45; k++) r[((gapCentre + k) % W + W) % W] = 250; return r; });
  return [...bar, ...rows];
}
{ // phased: the gap at the ends, the map NOT centred in its border (55 px left, 114 right) — never moved
  const rows = withBar(ddk1006(700, 0, 30), 0), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length);
  ok(st.atEdge === true && (!st.al || st.al.shift === 0),
     `phased DDK with its header bar: never moved (shift ${st.al?.shift ?? 0}, atEdge ${st.atEdge})`);
  ok(findHeaderBar(rows, W, 0) === 0, `…the bar's gap found at the line's ends (${findHeaderBar(rows, W, 0)})`); }
{ // phasing missed, the start caught: everything rolled 700 px — cut exactly at the gap, not the band's centre (−25)
  const rows = withBar(ddk1006(700, 0, 30), 0).map((r) => { const o = new Uint8Array(W); for (let x = 0; x < W; x++) o[x] = r[(((x - 700) % W) + W) % W]; return o; });
  const st: ChartAlignState = {}; feed(st, rows, 0, rows.length);
  ok(!!st.al && Math.abs(st.al.shift - 700) <= 2, `rolled DDK joined before its bar: cut at the gap (${st.al?.shift} ≈ 700)`); }
{ // Northwood sends a bar too, its gap 43 px left of the margin — not the line start: the margin still decides
  const rows = withBar(chart(700, 400, -0.06), 357), st: ChartAlignState = {};
  feed(st, rows, -0.06, rows.length);
  ok(st.via === 'margin' && !!st.al && near(st.al.shift, 398, 4), `Northwood with its bar: cut at the margin (${st.al?.shift} ≈ 398), not the gap`); }

// ★★★ CONSERVATIVE, WORLDWIDE (2026-10-07, scripts/wefax-world): a chart is moved only in a format it positively is.
const ANY = wefaxFormat(9108100);                                   // NMF Boston: neither a border nor a margin station
ok(!ANY.border && !ANY.margin && DDK.border && !DDK.margin && GYA.margin && !GYA.border, 'formats: NMF none, DDK border, GYA margin');
ok(wefaxPreset(11088100).slant === 0 && wefaxPreset(11084600).slant === -0.06,
   'KVM70 11090 (dial 11088.1) no longer takes Northwood 11086.5\'s −0.06; Northwood\'s own dial does');
// an open-sea band on an edge-to-edge chart from a station that sends no white border: never cut as one
{ const rows = bordered(700, 900), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length, ANY);
  ok(!st.al, `a blank band on a non-border station: left as received (${JSON.stringify(st.al)})`); }
// the black margin strip (NWS/JMH/HLL2/XSG/JFX): an 81 px black strip right of a 1728 px picture
function stripped(lines: number, roll: number, slant: number, sigma: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(245);
    for (let x = 0; x < 1728; x++) if (rnd() < 0.05) r[x] = r[(x + 1) % W] = 30;
    for (const c of [400, 1000]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); r[cx] = r[cx + 1] = 20; }
    for (let x = 1728; x < W; x++) r[x] = 8;
    const o = new Uint8Array(W), off = Math.round(roll + slant * y);
    for (let x = 0; x < W; x++) o[x] = Math.max(0, Math.min(255, Math.round(r[(((x - off) % W) + W) % W] + (sigma ? gauss() * sigma : 0))));
    rows.push(o);
  }
  return rows;
}
{ const st: ChartAlignState = {}; feed(st, stripped(700, 0, 0, 30), 0, 700, ANY);
  ok(st.via === 'strip' && !st.al, `phased chart with a black strip at the line's end: left as received (${JSON.stringify(st.al)})`); }
{ const st: ChartAlignState = {}; feed(st, stripped(700, 600, 0, 30), 0, 700, ANY);
  const seam = st.al ? (((st.al.shift - 600) % W) + W) % W : -1;
  ok(st.via === 'strip' && seam >= 1728 && seam < W, `rolled 600 px: the seam put inside the strip (${seam}, strip 1728…1808)`); }
{ const st: ChartAlignState = {}; feed(st, stripped(700, 600, 0.16, 30), 0, 700, ANY);
  ok(st.slant !== undefined && Math.abs(st.slant - 0.16) <= 0.01, `a strip chart leaning 0.16 (CBV): its slant measured from the strip (${st.slant})`); }
{ const rows = stripped(300, 0, 0, 0).map((r) => r.map((v) => (v === 8 ? 90 : v)));   // a dark GREY band, not black
  ok(findStrip(rows, W, 0, 0, 300) === null, 'a dark grey band (a satellite\'s sea) is not a strip'); }
// a phased DDK text page (schedule, Stuart's HF+ 15:04): header bar at the ends, no frame — never moved, no slant
{ const page = Array.from({ length: 700 }, (_, y) => { const r = new Uint8Array(W).fill(250);
    if (y % 28 < 12) for (let x = 190; x < 190 + 400 + (y % 7) * 150; x++) if (rnd() < 0.35) r[x] = r[x + 1] = 20;
    return r; });
  const rows = withBar(page, 0), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length);
  ok(!st.al && st.slant === undefined, `a phased text page with its header bar: drawn exactly as received (${JSON.stringify(st.al)}, slant ${st.slant})`); }

// ★★★ 2026-10-07 — Stuart's HF+, DDK 7880, 18:21 (wefax_2026-10-07T18-21-12.png, not committed: DWD's): a ROTATED
//     surface analysis joined just after its start — no header bar. The geometry measured on it: the frame each side
//     a pair of thin GREY lines (pixels ~100–190: dark by a pair under 320 on only 2–3 % of lines), the right pair at
//     x ≈ 44–50 with the ICEBERG legend box just inside it at x 5–60, the white border x ≈ 52–167, the left pair at
//     x ≈ 168–176, a slant of +0.010, and from line ~400 a dotted interference column in the border (x ≈ 100, 11 %).
//     Truth (DDK's phased geometry, UberSDR copy 20261007_180537: left frame ≈ 55, right ≈ 1741–1746): a shift of
//     ≈ 110. RC26 left it rolled.
function rotated18(lines: number, roll: number, k: number, pairs: 'both' | 'left' = 'both'): Uint8Array[] {
  const rows: Uint8Array[] = [];
  const grey = (m: number, d: number) => Math.max(0, Math.min(255, m + Math.round((rnd() - 0.5) * 2 * d)));
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W);
    for (let x = 0; x < W; x++) r[x] = 250 - (rnd() < 0.004 ? 120 : 0);                           // paper, a few specks
    for (let x = 75; x <= 1660; x++) if (rnd() < 0.06) r[x] = r[x + 1] = 30;                       // the map
    for (const c of [400, 900, 1400]) { const cx = Math.round(c + 90 * Math.sin((y + c) / 80)); r[cx] = r[cx + 1] = 20; }
    if (y > lines / 2) for (let x = 1690; x <= 1730; x++) if (rnd() < 0.08) r[x] = r[x + 1] = 30;  // the legend's text
    if (y > lines / 2) { r[1688] = r[1689] = r[1732] = r[1733] = 40; }                             // …and its box
    // one-pixel grey lines: the left pair dark (by a pair under 320) on ~20 % of lines, the right pair on ~4 %
    for (const c of [55, 61]) r[c] = grey(120, 100);
    if (pairs === 'both') for (const c of [1740, 1746]) r[c] = grey(160, 100);
    if (y > 400 && y % 9 < 2) r[1790] = r[1791] = 40;                                              // interference dots
    const o = new Uint8Array(W), off = Math.round(roll + k * y);
    for (let x = 0; x < W; x++) o[x] = r[(((x - off) % W) + W) % W];
    rows.push(o);
  }
  return rows;
}
{ const rows = rotated18(1300, 110, 0.01), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length);
  ok(!!st.al && Math.abs(st.al.shift - 110) <= 6 && st.via === 'border',
     `DDK 18:21 (rotated, late join, grey frame pairs): cut in its border at ${st.al?.shift} (≈110), via ${st.via}`);
  ok(st.slant !== undefined && Math.abs(st.slant - 0.01) <= 0.003, `…drawn at its own slant, +0.010 (${st.slant})`); }
{ const rows = rotated18(1300, 110, 0.01, 'left'), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length);
  ok(!st.al, `…but a grey frame on ONE side only is not enough: drawn as received (${JSON.stringify(st.al)})`); }
{ const rows = rotated18(1300, 110, 0.01), st: ChartAlignState = {};
  feed(st, rows, 0, rows.length, wefaxFormat(12748100));
  ok(!st.al, `…and on a station that sends no white border (NMF 12750), drawn as received (${JSON.stringify(st.al)})`); }

// ★ RAW (Stuart, 2026-10-06): drawn through drawnAlign, every line comes out exactly as received — whatever the
//   correction underneath — and RAW off gives that correction back untouched.
{ const corr = { shift: 1775, slant: 0.011 };
  const rows = ddk1006(50, 300, 0);
  let same = true;
  for (let y = 0; y < rows.length; y++) {
    const o = new Uint8Array(W); rotateLine(rows[y], o, W, wefaxOffset(drawnAlign(corr, true), y, W));
    for (let x = 0; x < W; x++) if (o[x] !== rows[y][x]) { same = false; break; }
  }
  ok(same, 'RAW: every line drawn byte-for-byte as received (shift 0, slant 0)');
  ok(drawnAlign(corr, false) === corr && wefaxOffset(drawnAlign(corr, false), 1000, W) === wefaxOffset(corr, 1000, W),
     'RAW off: the correction underneath is drawn unchanged'); }

// The arithmetic the canvases use
ok(wefaxOffset({ shift: 40, slant: -0.06 }, 0, W) === 40 && wefaxOffset({ shift: 40, slant: -0.06 }, 1000, W) === W - 20, 'offset wraps');
ok(wefaxPreset(4608100).slant === -0.06 && wefaxPreset(4608100).shift === 0, 'Northwood preset = slant only');
ok(wefaxPreset(7878100).slant === 0, 'DWD 7880 = no correction');
console.log(`wefaxAlign: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
