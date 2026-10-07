/**
 * wefaxAlign — SHIFT and SLANT for a WEFAX chart, applied as each line is DRAWN (app + web; no server cost).
 *
 * ★★★ WHY (Stuart, 2026-10-04): Northwood (GYA) charts come out crooked on every SDR, UberSDR included, while
 *     DWD's are straight — and Northwood's black margin sits "slightly to the right of the left edge causing wrap
 *     around". Measured on Northwood 4610 through Stuart's HF+: the margin starts ~40 px in and walks LEFT about
 *     0.06 px per line — a ~33 ppm line-rate offset at the transmitter plus a phasing offset. Both are a steady
 *     sideways shift that grows with the line number, so one formula undoes both:
 *         line y is moved LEFT by  shift + slant × y   (circularly — what falls off the left reappears on the right)
 * ★ An automatic margin tracker was prototyped first and dropped: on charts with no margin it locked onto map
 *   features as strong as a real margin. A remembered per-frequency setting is predictable.
 */
export interface WefaxAlign { shift: number; slant: number }
export const WEFAX_ALIGN_ZERO: WefaxAlign = { shift: 0, slant: 0 };

/** Northwood (GYA) carrier frequencies, kHz. */
const NORTHWOOD_KHZ = [2618.5, 4610, 8040, 11086.5];
/** ★ Measured 2026-10-04 (see the header). SLANT only: the margin's position differs chart to chart (~40 px in on
 *  some, dead centre on others), so the SHIFT is found per chart by findMargin, never preset. */
export const NORTHWOOD_ALIGN: WefaxAlign = { shift: 0, slant: -0.06 };

/** The station preset for a dial frequency, or zero. The dial in USB sits 1.9 kHz below the carrier (the published
 *  "assigned" frequency); a dial showing the carrier itself matches too.
 *  ★★ Within PRESET_KHZ of either, not 5 kHz of the carrier (2026-10-07, scripts/wefax-world): KVM70 Honolulu's
 *  11090 kHz (dial 11088.1) is 3.5 kHz from Northwood's 11086.5, so every KVM70 chart there was drawn with
 *  Northwood's −0.06 slant — and, with the margin search Northwood's alone, searched for a margin it does not send. */
export function wefaxPreset(dialHz: number): WefaxAlign {
  const khz = dialHz / 1000;
  return nearStation(khz * 1000, NORTHWOOD_KHZ) ? { ...NORTHWOOD_ALIGN } : { ...WEFAX_ALIGN_ZERO };
}
/** ★ How close (kHz) a dial must be to a station's to take its preset — see wefaxPreset. */
const PRESET_KHZ = 0.5;

/** ★★★ Which chart formats auto-align may act on for a station (2026-10-07 — see findChartAlign, CONSERVATIVE,
 *  WORLDWIDE). `margin`: Northwood's thin black margin line (only GYA sends one; a NOAA polar chart's full-height
 *  meridian is the same 4–5 px solid line). `border`: the white border round a framed map, cut by the frame beside
 *  it (DDK and SVJ4 are the only stations documented with white margins, docs/WEFAX-WORLD-STATIONS.md; NOAA's open
 *  sea between a grid meridian and an isobar is as wide and as straight-sided). Every station also gets the black
 *  margin STRIP (the world's commonest format, recognised by content) and DDK's header bar, which says itself where
 *  the line starts. */
export interface WefaxFormat { margin: boolean; border: boolean }
/** Assigned (centre) frequencies, kHz, of the stations whose charts sit on white paper with a framed border:
 *  DDK/DDH3/DDK6 Pinneberg, SVJ4 Athens (published 4481/8105 are its dials: centre 1.9 kHz higher). */
const BORDER_KHZ = [3855, 7880, 13882.5, 4482.9, 8106.9];
const nearStation = (dialHz: number, list: number[]) => {
  const khz = dialHz / 1000;
  return list.some((c) => Math.abs(khz + 1.9 - c) <= PRESET_KHZ || Math.abs(khz - c) <= PRESET_KHZ);
};
export function wefaxFormat(dialHz: number): WefaxFormat {
  return { margin: nearStation(dialHz, NORTHWOOD_KHZ), border: nearStation(dialHz, BORDER_KHZ) };
}
/** Where a listener's own setting is remembered: per frequency, to the nearest kHz. */
export function wefaxAlignKey(dialHz: number): string { return `wefaxAlign:${Math.round(dialHz / 1000)}`; }
export function parseAlign(v: unknown): WefaxAlign | null {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const s = Number(o.shift), k = Number(o.slant);
  if (!Number.isFinite(s) || !Number.isFinite(k)) return null;
  return { shift: Math.max(-2000, Math.min(2000, s)), slant: Math.max(-2, Math.min(2, k)) };
}
/** The whole-pixel left shift for line `y` of a `width`-pixel chart, 0 ≤ result < width. */
export function wefaxOffset(a: WefaxAlign, y: number, width: number): number {
  if (!width) return 0;
  const o = Math.round(a.shift + a.slant * y);
  return ((o % width) + width) % width;
}
/** ★★ RAW (Stuart, 2026-10-06: "a No Correct or RAW button shows the image without any correction at all"): the
 *  geometry a chart is DRAWN with — `a` (auto-align / manual shift, measured / saved / station slant), or with RAW on,
 *  none at all: every line exactly as received. Geometry only — the paper/ink levels (wefaxCrisp) are rendering,
 *  not correction, and stay. Both clients draw through this, so RAW means the same thing in each. */
export function drawnAlign(a: WefaxAlign, raw: boolean): WefaxAlign { return raw ? WEFAX_ALIGN_ZERO : a; }
/** Copy `src` (one line) into `dst` moved left by `off` pixels, wrapping. */
export function rotateLine(src: ArrayLike<number>, dst: Uint8Array | number[], width: number, off: number): void {
  for (let x = 0; x < width; x++) dst[x] = src[(x + off) % width] ?? 0;
}
/** Steps for the adjust keys. ★ The margin keys step 1 px (the 5 px pair went 2026-10-06 — see DecoderPanel's
 *  'ONLY THE KEYS IN USE'); held, a key steps ×5 (useHoldRepeat). SHIFT_STEP is no longer used by either client.
 *  ★ SLANT_STEP 0.005 → 0.001 (2026-10-06): the slants now measured are 0.007–0.012 on the RX888's DDK charts, so
 *    0.005 a tap could not land on one; a HELD key moves 5× a step once it is going (useHoldRepeat), so the coarse
 *    range is still quick. 0.001 is 1.3 px over a 1300-line chart. */
export const SHIFT_STEP = 5;
export const SLANT_STEP = 0.001;

/** ★ Lines a chart must have before its margin is looked for, and how many are summed. */
export const MARGIN_AFTER_LINES = 150;

/**
 * ★★★ FIND THE CHART'S MARGIN — once per chart, every station (Stuart, 2026-10-04: a noob sits on Northwood, presses
 *     WEFAX and gets a clean chart; "same with DDK"). The black margin line is the one feature that runs EXACTLY down
 *     the page once the station's slant is taken out, so summing each column's darkness along the slant over
 *     MARGIN_AFTER_LINES lines makes it one sharp peak wherever it sits — Northwood puts it ~40 px in on some charts
 *     and dead centre (~370) on others; map lines, curved or diagonal, smear out. Measured on two real Northwood
 *     charts: margin strength 53–97 against ~20 for the 30th-best column, at the same column in every window.
 * Returns the raw column of the margin AT LINE 0 (so `shift = column − 2` puts it at the left edge), or null when no
 * line is clearly a margin — then nothing is moved (a chart without one is never shifted).
 * `rows[y]` = line y as received (greyscale), any missing rows skipped.
 */
export function findMargin(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                           fromRow = 0, count = MARGIN_AFTER_LINES): number | null {
  const m = marginPeak(rows, width, slant, fromRow, count);
  return m && m.ok ? m.col : null;
}

/** ★ Below this much average ink (of 255) the paper beside a line is blank — see 'A FRAME, NOT A MARGIN'. DDK's
 *  border reads ~1; Northwood's chart beside its margin 20–50. */
const BLANK_INK = 6;
/** ★ The least average ink (of 255) a margin column carries — see 'A MARGIN IS SOLID'. */
const MARGIN_INK = 140;

/** The strongest margin-like column at one slant, its strength, and whether it passes findMargin's test. */
function marginPeak(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                    fromRow: number, count: number, minRows = count / 2): { col: number; strength: number; ok: boolean } | null {
  const W = width;
  if (!W) return null;
  const acc = new Float64Array(W);
  let n = 0;
  for (let y = fromRow; y < fromRow + count && y < rows.length; y++) {
    const r = rows[y];
    if (!r) continue;
    const off = ((Math.round(slant * y) % W) + W) % W;
    for (let x = 0; x < W; x++) acc[x] += 255 - (r[(x + off) % W] ?? 255);
    n++;
  }
  if (n < minRows) return null;
  const sm = new Float64Array(W);
  for (let x = 0; x < W; x++) {
    let a = 0;
    for (let k = -2; k <= 2; k++) a += acc[((x + k) % W + W) % W];
    sm[x] = a / 5 / n;
  }
  // Narrow: darker than BOTH sides 25–40 px away (the edge of a dark band is lighter on one side only).
  const narrow = new Float64Array(W);
  for (let x = 0; x < W; x++) {
    let l = 0, r = 0;
    for (let k = 25; k <= 40; k++) { r += sm[(x + k) % W]; l += sm[((x - k) % W + W) % W]; }
    narrow[x] = sm[x] - Math.max(l, r) / 16;
  }
  let best = 0;
  for (let x = 1; x < W; x++) if (narrow[x] > narrow[best]) best = x;
  const sorted = Array.from(narrow).sort((a, b) => b - a);
  const ref = sorted[Math.min(29, sorted.length - 1)];
  // ★★★ A FRAME, NOT A MARGIN (Stuart, 2026-10-05: "DDK likes to have white borders around their faxes which is
  //     throwing our auto mode into disarray. Northwood perfect however"). DWD/DDK draw the map inside a thin black
  //     FRAME on a white border, already where it belongs; the frame line is as straight and narrow as Northwood's
  //     margin and was moved to the left edge, wrapping the border round. Measured on 60 DDK charts from Stuart's
  //     RX888: beyond the frame is BLANK PAPER — 254 average over 600 lines. Northwood's margin has chart on both
  //     sides (205–235 on 60 charts). So a line with empty paper beside it is a frame: never a margin.
  const side = (dir: number) => {
    let a = 0;
    for (let k = 8; k <= 40; k++) a += sm[((best + dir * k) % W + W) % W];
    return a / 33;                               // average INK beyond the line (sm is ink), 0 = blank paper
  };
  const framed = Math.min(side(-1), side(1)) < BLANK_INK;
  // ★★ A MARGIN IS SOLID (60 Northwood + 60 DDK charts, RX888, 2026-10-05): Northwood's margin averages 153–248 ink
  //    (dark on 64–100 % of lines); the straight map lines a slant search can also line up — DDK's meridians and
  //    frame — average 75–120. The darkest column within ±2 must clear MARGIN_INK.
  let ink = 0;
  for (let k = -2; k <= 2; k++) ink = Math.max(ink, acc[((best + k) % W + W) % W] / n);
  return { col: best, strength: narrow[best],
           ok: !framed && ink >= MARGIN_INK && narrow[best] >= 40 && narrow[best] >= 2 * Math.max(ref, 1) };
}

/** ★ How far from the station's own slant a chart's slant is searched, px per line: ±0.05 is ±27 ppm at 1809 px —
 *  the RECEIVER's clock error on anything with a TCXO, and most crystals. A wider search lines up straight MAP lines
 *  instead (DDK's meridians at −0.17…−0.18 on the RX888 set). */
export const SLANT_SEARCH = 0.05;
/** ★ How far from the station's slant Northwood's MARGIN slant is searched — see findMarginSlant. */
const MARGIN_SLANT_SEARCH = 0.04;

/**
 * ★★★ THE SLANT, MEASURED ON EACH CHART (MadPsy, 2026-10-05: "that fix depends on the frequency accuracy of the
 *     particular hardware … sample clock error which even changes with temperature"; Stuart: "make sure it isn't
 *     tied to a specific radio"). A chart's slant is the TRANSMITTER's line-rate error plus the RECEIVER's
 *     sample-clock error. Measured 2026-10-05 on 60 Northwood charts from Stuart's RX888: −0.062…−0.067 px/line,
 *     the same as his HF+ (−0.06) — so Northwood's slant IS the transmitter's and the preset holds across radios.
 *     The receiver's share is small on a TCXO radio but not zero on every radio, so the preset is only the centre
 *     of the search: the margin line is the one thing that runs exactly straight down the page once the slant is
 *     right, so the slant whose margin peak is SHARPEST is this chart's slant on this radio.
 * Steps of 0.005, then 0.001 around the best. Null when no slant shows a margin.
 */
export function findMarginSlant(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, centre: number,
                                fromRow = 0, count = MARGIN_AFTER_LINES, minRows = count / 2): { col: number; slant: number } | null {
  let best: { col: number; strength: number; slant: number } | null = null;
  const tryAt = (k: number) => {
    const m = marginPeak(rows, width, k, fromRow, count, minRows);
    if (m && m.ok && (!best || m.strength > best.strength)) best = { col: m.col, strength: m.strength, slant: k };
  };
  // ★★ ±MARGIN_SLANT_SEARCH, not ±SLANT_SEARCH (2026-10-07): on a Northwood dial the margin search lined up a straight
  //    meridian of an edge-to-edge map at −0.015 (0.045 off the station's) — as solid as a margin. 89 Northwood charts
  //    (UberSDR archive, RX888) measure −0.061…−0.093: within 0.035 of −0.06.
  for (let i = -8; i <= 8; i++) tryAt(centre + i * MARGIN_SLANT_SEARCH / 8);
  if (!best) return null;
  const c = (best as { slant: number }).slant;
  for (let i = -4; i <= 4; i++) if (i) tryAt(Math.round((c + i * 0.001) * 1000) / 1000);
  const b = best as { col: number; slant: number };
  return { col: b.col, slant: Math.round(b.slant * 1000) / 1000 };
}

/** ★ How much more often (share of lines) the median column is dark than a noisy chart's lightest — see bandOf. */
const NOISY_CONTRAST = 0.05;
/** ★ The narrowest blank border findGutter accepts, px. DDK's is ~90 on the RX888 set (left + right border, joined). */
const GUTTER_MIN = 24;
/** ★ The share of lines a column may be dark on and still be blank paper (noise specks) — see findGutter. */
const GUTTER_SPECKS = 0.02;
/** ★ How far (px a side) and over how inked columns a found band is grown to the border's true edges — bandOf. */
const GUTTER_GROW = 120;
const GUTTER_EDGE = 0.09;
/** ★ The narrowest band accepted against a noisy chart's own paper (see 'A NOISY CHART'S PAPER'), px. */
const GUTTER_NOISY_MIN = 48;
/** ★ The narrowest a blank band may be, once grown (bandOf), to count as a border — see 'A BORDER IS WIDE', px. */
const BORDER_MIN = 90;
/** ★ A border centred this close to the line's ends (px) is left exactly where it is — a phased chart. */
const CENTRE_DEADBAND = 8;

/** A blank band: its centre column and width, how much of it touches the line's two ends (`edge`; 0 = the line's
 *  first and last pixels both carry ink), the slant it was measured at, and how sharp the lines bounding it are
 *  (`sharp`: the average ink, of 255, of the darkest column within 12 px beyond either end — the sharper side). */
export interface BlankBand { col: number; len: number; edge: number; slant: number; sharp: number;
  /** ★ The column (at line 0, like `col`) of that sharpest line beside the band (2026-10-07 — see lineCover). */
  line: number }

/** Per column at one slant: lines on which it is dark (see 'INK IS TWO PIXELS'), and its total ink. */
function columnProfile(rows: ReadonlyArray<ArrayLike<number> | undefined>, W: number, slant: number,
                       fromRow: number, count: number, rowStep: number) {
  const dark = new Uint32Array(W), ink = new Float64Array(W);
  let n = 0;
  for (let y = fromRow; y < fromRow + count && y < rows.length; y += rowStep) {
    const r = rows[y];
    if (!r) continue;
    const off = ((Math.round(slant * y) % W) + W) % W;
    // ★★★ INK IS TWO PIXELS, NOT ONE (Stuart's RSP1A on DDK 7880, 2026-10-06 — the chart that was phased perfectly
    //     and then cut 45 % across). One dark pixel was ink, so on a receiver noisier than the RX888 the white border
    //     filled with specks: grey noise of σ 45 added to the RX888 set's own charts puts 1.2–1.8 % of border pixels
    //     below 160, the 2 % allowance stopped covering it, and the border vanished on 146 of 149 daytime DDK charts —
    //     after which the margin search took a straight map line (a front, 183 ink) for Northwood's margin. Noise is
    //     single pixels; everything drawn on a chart (lines, text, frame) is at least two wide. A PAIR whose average
    //     is dark is ink: σ 45 then darkens ≤ 0.2 % of a border, and every RX888 DDK chart whose border is found clean
    //     has it found with σ 45 added too (bandOf, 171 charts).
    let prev = r[(W - 1 + off) % W] ?? 255, last = -1;   // `last`: the column most recently counted on this line
    for (let x = 0; x < W; x++) {
      const v = r[(x + off) % W] ?? 255;
      ink[x] += 255 - v;
      if (prev + v < 320) {                              // both of the pair are ink — each counted once per line
        if (last !== x - 1) dark[(x - 1 + W) % W]++;
        dark[x]++; last = x;
      }
      prev = v;
    }
    n++;
  }
  return { dark, ink, n };
}

/** The widest blank band in a column profile (see findGutter), or null. */
function bandOf(p: { dark: Uint32Array; ink: Float64Array; n: number }, W: number, slant: number): BlankBand | null {
  const { dark, ink, n } = p;
  // ★★ A FEW SPECKS DO NOT INK A COLUMN (120 RX888 charts, 2026-10-05): requiring NO dark pixel on ANY line let
  //    noise specks hide DDK's border on half its charts, the frame then passed for a margin and the chart was cut
  //    inside the map. Dark on ≤ GUTTER_SPECKS of lines still counts as blank.
  const inked = new Uint8Array(W);
  // Longest circular run of columns dark on ≤ `tol` lines.
  const longest = (tol: number) => {
    for (let x = 0; x < W; x++) inked[x] = dark[x] > tol ? 1 : 0;
    let start = -1;
    for (let x = 0; x < W; x++) if (inked[x]) { start = x; break; }
    if (start < 0) return null;                    // nothing drawn at all
    let bLen = 0, bAt = 0, len = 0;
    for (let i = 1; i <= W; i++) {
      const x = (start + i) % W;
      if (!inked[x]) { len++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } }
      else len = 0;
    }
    return { len: bLen, at: bAt };
  };
  let run = longest(GUTTER_SPECKS * n);
  if (!run) return null;
  let growTol = GUTTER_EDGE;
  if (run.len < GUTTER_MIN) {
    // ★★ A NOISY CHART'S PAPER (DDK, RX888 set, 2026-10-06 — 3 of 171 charts, on air not added): its blank border
    //    is dark on 3–5 % of lines even counted in pairs, so no column is under 2 % and the margin search took the
    //    frame for a margin — on a rolled chart the right cut by luck, on a phased one the left border cut off.
    //    Measured against the chart's OWN paper (the lightest 15 columns): within GUTTER_SPECKS of it is blank, and
    //    a band found that way must be GUTTER_NOISY_MIN wide before it is grown (and BORDER_MIN after, as any band).
    let floor = Infinity;
    for (let x = 0; x < W; x++) {
      let a = 0;
      for (let k = -7; k <= 7; k++) a += dark[(x + k + W) % W];
      floor = Math.min(floor, a / 15);
    }
    if (floor <= GUTTER_SPECKS * n / 2) return null;
    // ★★ …and only when the chart HAS paper lighter than its picture (2026-10-07). On no signal at all (Northwood
    //    4610, 20261006_154718: 3028 lines of pure noise) every column is equally dark, a "band" turns up somewhere at
    //    most slants by chance, and the wider border-slant search found one at −0.153 by line 600. The median column
    //    must be inked on clearly more lines than the lightest: by 0.008–0.030 of lines on that noise, ≥ 0.068 on every
    //    RX888 DDK chart (σ 45 added) whose border the noisy-paper path decides.
    const sorted = Array.from(dark).sort((a, b) => a - b);
    if (sorted[W >> 1] - floor < NOISY_CONTRAST * n) return null;
    run = longest(floor + GUTTER_SPECKS * n);
    if (!run || run.len < GUTTER_NOISY_MIN) return null;
    growTol = Math.max(GUTTER_EDGE, floor / n + 0.05);
  }
  let bestLen = run.len, bestAt = run.at;
  // ★★ THE BORDER'S TRUE WIDTH (Stuart, 2026-10-06: "55 left 110 right then we just do 82 left 83 right of white").
  //    DDK's header — the dashes under the black bar — runs ~70 px INTO the right-hand border, inking those columns
  //    on ~5 % of lines, so the 2 % band stopped short there and its centre sat ~35 px off the border's. The band is
  //    grown each way over columns dark on ≤ GUTTER_EDGE of lines (the header's 5 % at the 300-line look, 6–7 %
  //    with a few rows of speckle across it; a frame line is 60–85 %; a map's ragged edge passes 9 % within 2–4 px
  //    of its last column, so the grown edge sits at most that far into the map), at most GUTTER_GROW px a side.
  for (let k = 0; k < GUTTER_GROW && dark[(bestAt - 1 + W) % W] <= growTol * n && bestLen < W; k++) {
    bestAt = (bestAt - 1 + W) % W; bestLen++;
  }
  for (let k = 0; k < GUTTER_GROW && dark[(bestAt + bestLen) % W] <= growTol * n && bestLen < W; k++) bestLen++;
  // ★★ A BORDER IS WIDE; A LIGHT PATCH OF MAP IS NOT (2026-10-06). Counting ink in pairs stopped single noise specks
  //    inking a column — and with them the specks that had kept Northwood's open sea from reading as blank: on one
  //    of 60 charts a 64 px patch beside the margin passed for a border and the chart was moved 57 px off its
  //    margin. Grown, DDK's border measures 105–329 px on all 171 RX888 charts (σ 45 of added noise included);
  //    nothing on 60 Northwood charts reaches 65.
  if (bestLen < BORDER_MIN) return null;
  for (let x = 0; x < W; x++) inked[x] = 1;
  for (let i = 0; i < bestLen; i++) inked[(bestAt + i) % W] = 0;   // `edge` below is the grown band's own
  let edge = 0;
  for (let x = 0; x < W && !inked[x]; x++) edge++;
  for (let x = W - 1; x >= 0 && !inked[x]; x--) edge++;
  let pl = 0, pr = 0, cl = 0, cr = 0;
  for (let k = 1; k <= 12; k++) {
    const xl = ((bestAt - k) % W + W) % W, xr = (bestAt + bestLen - 1 + k) % W;
    if (ink[xl] > pl) { pl = ink[xl]; cl = xl; }
    if (ink[xr] > pr) { pr = ink[xr]; cr = xr; }
  }
  return { col: (bestAt + Math.floor(bestLen / 2)) % W, len: bestLen, edge, slant, sharp: Math.max(pl, pr) / n,
           line: pl >= pr ? cl : cr };
}

/**
 * ★★★ NO MARGIN? FIND THE BLANK BORDER (Stuart, 2026-10-05, joined a DDK chart half way: "chart is wrapped around on
 *     itself"). DWD/DDK send no black margin — the map sits in a frame on WHITE paper. Joined without phasing, the
 *     line starts anywhere and that white border lands mid-picture. It is as findable as a margin: a band of
 *     columns with NO ink on ANY line (map whites are crossed by grid lines and text within a few lines; a border
 *     never is). The widest such band, ≥ GUTTER_MIN, is where the lines' ends meet.
 *     Returns the band (see BlankBand), or null when there is none.
 */
export function findGutter(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                           fromRow = 0, count = MARGIN_AFTER_LINES, minRows = count / 2, rowStep = 1): BlankBand | null {
  if (!width) return null;
  const p = columnProfile(rows, width, slant, fromRow, count, rowStep);
  if (p.n < minRows / rowStep) return null;
  return bandOf(p, width, slant);
}

/**
 * ★★★ A BORDERED CHART'S SLANT, MEASURED (Stuart, 2026-10-06, DDK on his RSP1A: "it does have a very slight lean
 *     backwards compared to Northwood's previous forward lean that we fixed"). The slant was only ever measured from
 *     a Northwood margin, so a DDK chart was drawn at the station's slant (0) whatever the receiver's clock did.
 *     The lines that BOUND the border — DDK's frame, the map's edge — run straight down the page only at the
 *     chart's true slant; at any other they smear across columns and their darkest column fades (frame line 145
 *     ink at the true slant, 102 at 0.01 off, 54 at 0.025 off on an RX888 chart). The band's WIDTH is no guide:
 *     the header's dashes and the map's ragged edge set it, and it stayed 88 px from −0.05 to +0.03.
 *     So: the slant whose border is bounded most SHARPLY. Searched ±BORDER_SLANT_SEARCH (2026-10-07; was
 *     ±SLANT_SEARCH) around the station's own in steps of 0.005 (0.01 beyond ±SLANT_SEARCH) on every 2nd line, then
 *     0.001 around the best on every line.
 */
export function findBorder(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, centre: number,
                           fromRow = 0, count = MARGIN_AFTER_LINES, minRows = count / 2): BlankBand | null {
  let best: BlankBand | null = null;
  const tryAt = (k: number, step: number) => {
    const g = findGutter(rows, width, Math.round(k * 1000) / 1000, fromRow, count, minRows, step);
    // a tie (a line too short to tell 0.005 apart) goes to the slant nearer the station's own
    if (g && (!best || g.sharp > best.sharp + 0.5
              || (g.sharp > best.sharp - 0.5 && Math.abs(g.slant - centre) < Math.abs(best.slant - centre)))) best = g;
  };
  // ★★ ±BORDER_SLANT_SEARCH, not ±SLANT_SEARCH (2026-10-07) — see BORDER_SLANT_SEARCH.
  //    Steps of 0.005 within ±SLANT_SEARCH as before, 0.01 beyond (the fine pass below covers ±0.006).
  for (let i = -10; i <= 10; i++) tryAt(centre + i * SLANT_SEARCH / 10, 2);
  const near = best as BlankBand | null;
  for (let k = SLANT_SEARCH + 0.01; k <= BORDER_SLANT_SEARCH + 1e-9; k += 0.01) { tryAt(centre + k, 2); tryAt(centre - k, 2); }
  // ★★ …but a slant beyond ±SLANT_SEARCH must WIN CLEARLY: WIDE_SLANT_WIN × the sharpest within it. A frame line at
  //    its true slant does (254 against ≤ 82 on the HF+ chart); the legend under a map does not — on a join that
  //    saw only the map's last 100 lines and the legend, letters lined up at −0.11 for a chart leaning +0.02.
  if (near && best && (best as BlankBand).sharp < WIDE_SLANT_WIN * near.sharp) best = near;
  if (!best) return null;
  const c = (best as BlankBand).slant;
  // ★ Fine: a frame line 2–3 px wide reads equally sharp over a few thousandths of slant (a PLATEAU), so the middle of
  //   the plateau is taken, not its first or its nearest-to-station end (that read −0.026 for a −0.030 lean).
  const fine: BlankBand[] = [];
  for (let i = -6; i <= 6; i++) {
    const g = findGutter(rows, width, Math.round((c + i * 0.001) * 1000) / 1000, fromRow, count, minRows);
    if (g) fine.push(g);
  }
  best = null;
  if (fine.length) {
    const top = Math.max(...fine.map((g) => g.sharp));
    const flat = fine.filter((g) => g.sharp >= top * 0.99);
    best = flat[Math.floor((flat.length - 1) / 2)];
  }
  // ★★ NO STRAIGHT LINE, NO SLANT (DDK's schedule and text pages, 2026-10-06): with no frame and only ragged text
  //    beside the border, nothing sharpens at any slant and the "best" one was noise — ±0.05 on 30 of 171 charts.
  //    A measured slant counts only when a line beside the border is solid (BORDER_LINE_INK); else the station's.
  const b = best as BlankBand | null;
  // ★★★ …and a slant away from the station's only from a line that runs the WHOLE way down (2026-10-07) — see
  //     SLANT_TRUST. Short of that, the band is taken at the station's slant below.
  //     ★ …and it must be clearly SHARPER (NEAR_WIN) than the sharpest band within ±SLANT_TRUST, looked at on every
  //       line. The coarse pass looks at every 2nd line, and on a noisy chart the band comes and goes from one slant
  //       to the next (σ 60: none at +0.005, the frame 155 ink at +0.010) — it picked −0.036 at 86 ink where the
  //       station's 0 read 124. And a THICK dark edge (the Norwegian ice chart's frame + panel border + logo box,
  //       ~12 px, 20260930_155456) stays dark in some column at a slant 0.04 off: +0.053 at 149 ink against 85 at the
  //       station's 0 — but 0.015 reads as sharp.
  //     ★ …and its band must not have shrunk to a fraction (BAND_KEEP) of that one. Northwood joined for its last 400
  //       lines (20261005_161119, _181119) has a 262–270 px white band at its −0.06 and a diagonal meridian beside a
  //       97–103 px one at −0.184/−0.198 — solid on 6–7 of 8 stretches, 133–151 ink against 23–25. A real frame's band
  //       at its true slant kept ≥ 0.68 of the near band's width on all 143 leaning (+0.11) DDK late joins.
  let close: BlankBand | null = null;
  for (let i = -3; i <= 3; i++) {
    const g = findGutter(rows, width, Math.round((centre + i * SLANT_TRUST / 3) * 1000) / 1000, fromRow, count, minRows);
    if (g && (!close || g.sharp > close.sharp + 0.5)) close = g;
  }
  if (b && b.sharp >= BORDER_LINE_INK
      && (Math.abs(b.slant - centre) <= SLANT_TRUST + 1e-9
          || ((!close || (b.sharp > NEAR_WIN * close.sharp && b.len >= BAND_KEEP * close.len)) && slantJustified(rows, width, b.slant, centre, b.line, fromRow, count)))) return b;
  // ★ Else the sharpest band within ±SLANT_TRUST when a solid line bounds it, else the band AT the station's slant, or
  //   none: a band that only appears at another slant, with no straight line to say that slant is real, was a
  //   Northwood sea patch lined up into a 114 px "border" (1 of 60).
  if (close && close.sharp >= BORDER_LINE_INK) return close;
  return findGutter(rows, width, centre, fromRow, count, minRows);
}
/** ★ How much sharper a border must be beyond ±SLANT_TRUST than the sharpest within it — see findBorder. */
const NEAR_WIN = 1.15;
/** ★ The least share of the near-station band's width a band beyond ±SLANT_TRUST must keep — see findBorder. */
const BAND_KEEP = 0.6;
/** ★★ How far from the station's slant a BORDERED chart's slant is searched, px per line (2026-10-07). Stuart's DDK
 *  7880 on the Pi 500's Airspy HF+ came out leaning +0.11 px/line (frame line measured on the finished chart,
 *  11:10 BST) against +0.010 for the same chart on the UberSDR's RX888 — and ±SLANT_SEARCH (±0.05) could not reach
 *  it: the "best" slant was the search's own end (0.056), which leaves the chart leaning half as much. A border's
 *  slant is only believed from a SOLID line right beside the band (BORDER_LINE_INK), and the frame line is the
 *  sharpest such line at its true slant by a wide margin (254 ink at 0.11–0.13 against ≤ 82 anywhere in ±0.05 on
 *  the rebuilt chart), so the wider search does not line up the meridians inside the map the way the margin search
 *  would. ±0.15 is ±83 ppm at 1809 px. */
export const BORDER_SLANT_SEARCH = 0.15;
/** ★ How much sharper a border must be beyond ±SLANT_SEARCH than within it to be believed there — see findBorder. */
const WIDE_SLANT_WIN = 1.2;
/** ★ How dark (average ink of 255) the darkest column beside a border must be to measure a slant from it. DDK's
 *  frame line reads ~145 at its true slant; text and ragged map edges beside a border 7–40. */
const BORDER_LINE_INK = 80;

/**
 * ★★★ A SLANT AWAY FROM THE STATION'S NEEDS A LINE THAT RUNS THE WHOLE WAY DOWN (Stuart, 2026-10-07 14:59 BST, DDK
 *     7880 on the Pi 500's HF+, RC25 Mac: "previous caught the end of a chart and still slanted; this time it looks
 *     like Northwood uncorrected"). The chart, joined late without phasing, was drawn leaning FORWARD ~0.04 px/line
 *     (the "icon_tkb" box edge walked 18 px left over 445 lines of the screenshot) — so ~+0.05 applied to a chart
 *     whose true slant is +0.01 (the UberSDR's copy of the same transmission, 20261007_135527_b3ad8373.png, and
 *     Stuart's own phased copy at 15:03: "no slant, or a very slight backwards lean"). Its white border was left
 *     ~220 px right of the cut, so the looks had not cut it in its band either. Replayed through chartAlignStep (the RX888 copy's last 720–950 lines, rolled, σ 50–65 of noise,
 *     180 joins) the border look measured +0.031/+0.032 (5) and −0.021/−0.036 (2); the last 400–900 lines of 30
 *     archive charts gave Northwood −0.184, −0.198, −0.091 (true −0.064) and DDK +0.031 (true +0.010); a box edge
 *     solid over 120 lines beside the band gives +0.05 (test_wefax_align). Each was a line dark enough on average
 *     (BORDER_LINE_INK) over the stretch it covers — a box edge, the legend, a coastline — but not the whole look,
 *     or a frame lined up at a slant where the coarse pass (every 2nd line) happened to see the band and the true
 *     slant did not. A frame line at its true slant is dark the whole way down; 0.03 off, it has drifted 9 px over
 *     300 lines and covers the middle only.
 *     So beyond ±SLANT_TRUST of the station's slant: the line's column (±2 px) must be dark in LINE_COVER of
 *     LINE_SEGMENTS equal stretches of the lines looked at, and (findBorder) the band must be sharper there than at
 *     the station's own slant; else the station's slant is used.
 *     The +0.11 HF+ chart of this morning is still measured: its frame runs the whole height at +0.11.
 */
function slantJustified(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number, centre: number,
                        col: number, fromRow: number, count: number): boolean {
  if (Math.abs(slant - centre) <= SLANT_TRUST + 1e-9) return true;
  return lineCover(rows, width, slant, col, fromRow, count) >= LINE_COVER;
}
/** ★ How far from the station's slant a measured slant is believed on its sharpness alone, px per line. DDK on the
 *  RX888 measures +0.007…+0.013 and on the HF+ ≈ +0.01; Northwood −0.062…−0.067 against its −0.06. */
export const SLANT_TRUST = 0.015;
/** ★ The stretches a line is checked over, the share of them it must be dark in, and the share of a stretch's lines
 *  it must be dark on to count — see slantJustified. ★ 0.15, not ½: DDK's frame reads ~145 ink, dark on ~55 % of
 *  lines and patchily, so ½ failed real frames at their true slant on 24 of 146 leaning (+0.11) late-join looks;
 *  at 0.15, 5 — and none of the 7 wrong wide slants the RC25 search proposed on the late-join set got through on
 *  it that the other tests did not stop (a wrong slant's stretches away from the crossing read ~0). */
const LINE_SEGMENTS = 8;
const LINE_COVER = 0.75;
const LINE_SEG_DARK = 0.15;
/** The share of LINE_SEGMENTS stretches of rows in which column `col` (±2 px, at `slant` — the same columns as
 *  columnProfile) is dark on at least LINE_SEG_DARK of the rows more than the columns 7 px to either side of it:
 *  dark = a PAIR of pixels averaging under 128 (see 'INK IS TWO PIXELS'). A frame or margin at its true slant covers every stretch; a box edge, a legend or a slant that is
 *  wrong covers some. */
export function lineCover(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number, col: number,
                          fromRow = 0, count = rows.length): number {
  const s = lineStretches(rows, width, slant, col, fromRow, count);
  return s ? s.solid.filter(Boolean).length / LINE_SEGMENTS : 0;
}
function lineStretches(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number, col: number,
                       fromRow: number, count: number): { solid: boolean[]; per: number } | null {
  const W = width, ys: number[] = [];
  for (let y = fromRow; y < fromRow + count && y < rows.length; y++) if (rows[y]) ys.push(y);
  const per = Math.floor(ys.length / LINE_SEGMENTS);
  if (!W || per < 4) return null;
  // dark at `c` (±2 px) on line y: a pair of pixels averaging under 128
  const darkAt = (r: ArrayLike<number>, c: number, off: number) => {
    for (let k = -2; k <= 1; k++) {
      const x = (((c + k + off) % W) + W) % W;
      if ((r[x] ?? 255) + (r[(x + 1) % W] ?? 255) < 256) return 1;
    }
    return 0;
  };
  const solid: boolean[] = [];
  for (let s = 0; s < LINE_SEGMENTS; s++) {
    let dark = 0, left = 0, right = 0;
    for (let i = s * per; i < (s + 1) * per; i++) {
      const y = ys[i], r = rows[y] as ArrayLike<number>;
      const off = ((Math.round(slant * y) % W) + W) % W;
      dark += darkAt(r, col, off); left += darkAt(r, col - 7, off); right += darkAt(r, col + 7, off);
    }
    // ★ …and clearly darker than the columns 7 px to EITHER side: a slant that has drifted the column into a map's
    //   stipple or text finds as much ink beside it as on it (a box edge beside a band, lined up at its own slant for
    //   120 lines and then wandering into the map, read as solid against the band side alone).
    solid.push(dark >= per * LINE_SEG_DARK && dark >= Math.max(left, right) + per * LINE_SEG_DARK);
  }
  return { solid, per };
}

/**
 * ★★ DWD/DDK'S HEADER BAR (Stuart, 2026-10-07: "notice the black bar at the top with the white cutouts, that is how
 *    it is aligned"). Every DDK chart opens with ~15 lines of black across the whole line but for ONE white gap,
 *    split across the line's two ends when the chart is phased. Measured on the UberSDR archive (RX888, 161 DDK charts
 *    with a clean bar): the gap is 83–98 px wide and centred −12…+11 px from the line's ends (median 0); Stuart's
 *    phased HF+ copy of 14:08 UTC, 90 px centred at −2. So when the bar is received its gap IS the line start:
 *    - centred within BAR_EDGE px of the ends → the chart is phased: it is never moved (Stuart: "I did actually get
 *      that bar yesterday and it looked exactly like it did today until our 'fix'" — 2026-10-06 14:15, a phased chart
 *      the border search then cut);
 *    - anywhere else → a chart whose phasing was missed but whose start was caught: cut exactly at the gap.
 *    ★ Only at the TOP: DDK's closing bar (14:08 copy, lines 1315–1335) is black edge to edge, no gap, and the
 *      UberSDR archive stops before it — a late join has no bar to go by.
 * Returns the gap's centre at line 0 (columnProfile's columns at `slant`), or null when no bar was received.
 */
export function findHeaderBar(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                              upto = BAR_LOOK_LINES): number | null {
  const W = width;
  if (W < 200) return null;
  const centres: number[] = [];
  const m = new Float64Array(W);
  for (let y = 0; y < rows.length && y < upto; y++) {
    const r = rows[y];
    if (!r) continue;
    // 5-px means: single noise specks neither break the bar nor fill the gap
    let a = 0;
    for (let k = -2; k <= 2; k++) a += r[(k + W) % W] ?? 255;
    for (let x = 0; x < W; x++) { m[x] = a / 5; a += (r[(x + 3) % W] ?? 255) - (r[(x - 2 + W) % W] ?? 255); }
    let dark = 0;
    for (let x = 0; x < W; x++) if (m[x] < 128) dark++;
    if (dark < BAR_DARK * W) { if (centres.length >= BAR_MIN_ROWS) break; centres.length = 0; continue; }
    // the longest circular run of light columns — the gap — and nothing else light of any size
    let start = -1;
    for (let x = 0; x < W; x++) if (m[x] < 128) { start = x; break; }
    if (start < 0) continue;
    let bLen = 0, bAt = 0, len = 0, light = 0;
    for (let i = 1; i <= W; i++) {
      const x = (start + i) % W;
      if (m[x] >= 128) { len++; light++; if (len > bLen) { bLen = len; bAt = (x - len + 1 + W) % W; } } else len = 0;
    }
    if (bLen < BAR_GAP[0] || bLen > BAR_GAP[1] || light > bLen + 20) continue;
    const off = Math.round(slant * y);
    centres.push((((bAt + bLen / 2 - off) % W) + W) % W);
  }
  if (centres.length < BAR_MIN_ROWS) return null;
  // the rows must agree on where the gap is (circularly)
  const ref = centres[0];
  const d = centres.map((c) => ((c - ref + W * 1.5) % W) - W / 2).sort((p, q) => p - q);
  if (d[d.length - 1] - d[0] > 8) return null;
  return Math.round(((ref + d[d.length >> 1]) % W + W) % W);
}
/** ★ The header bar: lines looked through for it, how many it must span, how much of a line is black, the gap's
 *  width (px), and how close to the line's ends its centre sits on a phased chart (px) — see findHeaderBar. */
const BAR_LOOK_LINES = 400;
const BAR_MIN_ROWS = 6;
const BAR_DARK = 0.85;
const BAR_GAP: [number, number] = [60, 130];
const BAR_EDGE = 16;

/** ★ When a chart's shift is decided, and when it is checked once more (redrawn only if the answer changed).
 *  Measured on 120 RX888 charts: 150 lines agreed with the whole chart on 42/60 Northwood, 300 on 58/60. */
export const ALIGN_CHECK_LINES = [300, 600];

/**
 * ★★★ ONE CALL FOR EVERY STATION: the margin (Northwood-style), else the blank border (DDK-style), else nothing.
 * `shift` moves the found column to the left edge (wefaxOffset), `slant` is this chart's — measured from the margin
 * or the border, else the station's own. `out.atEdge` is set when the chart was left where it was because its
 * border is already centred on the line's ends — chartAlignStep then keeps that chart's shift at 0 for good.
 * `out.via` says which way the answer was found; passed in as 'margin', the border search is skipped (see
 * chartAlignStep's second look).
 */
export function findChartAlign(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, stationSlant: number,
                               count: number, minRows = count / 2,
                               out?: AlignLookOut, fmt: WefaxFormat = defaultFormat(stationSlant)): WefaxAlign | null {
  // ★★★ CONSERVATIVE, WORLDWIDE (Stuart, 2026-10-07: "this is a worldwide app"). A chart is moved only when it is
  //     positively one of the formats below, on strong evidence; anything else is drawn exactly as received — a wrong
  //     cut through the map is far worse than none. scripts/wefax-world/harness.ts runs every format we know of.
  //     1. DDK-style white BORDER — a blank band confirmed by DDK's header bar (any station), or on a white-border
  //        station (fmt.border) bounded by a straight solid line, the frame, down the look (see bandConfirmed);
  //     2. a black margin STRIP (NWS, JMH, HLL2, XSG, JFX — the commonest on air; see findStrip);
  //     3. Northwood's thin MARGIN line — only on a station that sends one (fmt.margin): a NOAA polar chart's
  //        full-height meridian is the same 4–5 px solid line and cannot be told from it by content.
  //     See WefaxFormat for why 1 and 3 go by the station.
  // ★★ A BORDERED CHART NEVER HAS ITS FRAME TAKEN FOR A MARGIN: a blank band means a DDK-style chart, and its
  //    frame and meridians are as straight as a margin (5 of 60 DDK charts were mis-moved by the margin search
  //    when it ran first). The margin search runs only when there is no blank band at all.
  // ★ out.fixedSlant: look at the station's slant only, measure none (an early look — see chartAlignStep).
  const fixed = !!out?.fixedSlant, kind = out?.via;
  // ★★ A PHASED chart (left where it is) is drawn at a measured slant only when it is a real lean, beyond
  //    ±SLANT_TRUST (Stuart, 2026-10-07, his HF+ copies of DDK's schedule pages: "straight, not moved … or a very
  //    slight backwards lean that I would say is well within tolerance. Don't try to correct that small native lean
  //    on phased charts"). The +0.11 HF+ chart of the morning is still corrected.
  const phased = (k: number): WefaxAlign | null => {
    if (out) out.atEdge = true;
    if (Math.abs(k - stationSlant) <= SLANT_TRUST + 1e-9) return null;
    if (out) out.measured = k;
    return { shift: 0, slant: k };
  };
  const g0 = kind === 'margin' || kind === 'strip' ? null
           : fixed ? findGutter(rows, width, stationSlant, 0, count, minRows) : findBorder(rows, width, stationSlant, 0, count, minRows);
  // ★★ The header bar, when received, says where the line starts — before the band does (see findHeaderBar). The
  //    slant is still the border look's (measured, else the station's).
  //    ★ Only when its gap lies IN the blank band: Northwood sends a bar too, but its gap sits 43 px left of the
  //      margin, mid-chart (42–44 px on all 22 Northwood charts with a bar, UberSDR archive) — not the line start.
  //      DDK's line start is inside its white border, so on a bordered chart the two agree.
  const bar = g0 ? findHeaderBar(rows, width, stationSlant) : null;
  if (g0 && bar !== null && Math.abs(((bar - g0.col + width * 1.5) % width) - width / 2) <= g0.len / 2 + 10) {
    // the slant: measured only from a frame down the whole look (a text page's straight column edges are not one)
    const k = !fixed && bandConfirmed(rows, width, g0, count) ? g0.slant : stationSlant;
    if (out) out.via = 'border';
    if (Math.min(bar, width - bar) < BAR_EDGE) return phased(k);
    if (out && k !== stationSlant) out.measured = k;
    return { shift: bar, slant: k };
  }
  const g = g0 && fmt.border && bandConfirmed(rows, width, g0, count) ? g0 : null;
  if (g) {
    if (out) out.via = 'border';
    // ★★★ A BORDERED CHART IS CENTRED ON ITS BORDER (Stuart, 2026-10-06: "if there is a clear white border either
    //     side, our auto align just needs to centre the image on it — 55 left 110 right then we just do 82 left 83
    //     right of white"). The band's centre goes to the line's ends: equal white either side, wherever the band
    //     was — split across the edge (phased, a little off) or mid-picture (joined late). A chart already within
    //     CENTRE_DEADBAND px of centred is left exactly as received ("it was already aligned and we broke the
    //     alignment"). ★ Content decides, never the phasing signal: Northwood is phased on every chart on Stuart's
    //     UberSDR and still arrives with its margin 35–46 px in, or ~475 px in.
    const fromEdge = Math.min(g.col, width - g.col);
    if (fromEdge < CENTRE_DEADBAND) return phased(fixed ? stationSlant : g.slant);
    if (out && !fixed) out.measured = g.slant;
    return { shift: g.col, slant: g.slant };
  }
  // 2. a black strip
  if (kind !== 'margin') {
    const f = findStripSlant(rows, width, stationSlant, fixed, 0, count, minRows);
    if (f) {
      const sp = f.strip, k = f.slant;
      if (out) out.via = 'strip';
      // the seam goes STRIP_SEAM px inside the strip's far edge — where a phased chart has it: the picture starts at
      // the line's left end, the strip fills its right end. A strip already over the line's ends, or whose far edge
      // is within STRIP_EDGE of them, is left where it is (only a real lean is drawn — see `phased`).
      const seam = (sp.at + sp.len - STRIP_SEAM) % width;
      if (Math.min(seam, width - seam) <= STRIP_EDGE || (((width - sp.at) % width) < sp.len)) return phased(k);
      if (out && k !== stationSlant) out.measured = k;
      return { shift: seam, slant: k };
    }
  }
  if (out) out.via = 'margin';
  if (!fmt.margin) return null;
  // 3. Northwood's margin
  if (fixed) {
    const col = findMargin(rows, width, stationSlant, 0, count);
    return col === null ? null : { shift: col - 2, slant: stationSlant };
  }
  const m = findMarginSlant(rows, width, stationSlant, 0, count, minRows);
  // ★★ The margin's slant, too, is believed away from the station's only when the margin runs the whole way down
  //    (2026-10-07 — see slantJustified); else the margin is looked for at the station's slant.
  if (m && !slantJustified(rows, width, m.slant, stationSlant, m.col, 0, count)) {
    const col = findMargin(rows, width, stationSlant, 0, count);
    return col === null ? null : { shift: col - 2, slant: stationSlant };
  }
  if (m && out) out.measured = m.slant;
  return m ? { shift: m.col - 2, slant: m.slant } : null;
}

/** ★ The format assumed when a caller gives none: Northwood's margin when its slant is Northwood's, never a border
 *  (the clients and the harness pass wefaxFormat(dial)). */
function defaultFormat(stationSlant: number): WefaxFormat { return { margin: stationSlant !== 0, border: false }; }

/**
 * ★★★ A BLANK BAND IS DDK'S BORDER ONLY WHEN A STRAIGHT LINE BOUNDS IT (2026-10-07, scripts/wefax-world). NOAA's
 *     edge-to-edge analyses have open-sea column bands as wide as DDK's border (PYAA12 x≈1137, between two curved
 *     isobars), a Northwood chart's bottom has blank patches, and the paper after DDK's closing bar is blank right
 *     across: every one was cut as a border. DDK's border is bounded by its frame, solid and straight down the whole
 *     look (BORDER_LINE_INK, lineCover); curved isobars, coasts and text are not. (A band DDK's header bar confirms
 *     needs no frame — findChartAlign.)
 */
function bandConfirmed(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, g: BlankBand, count: number): boolean {
  // ★ …down the WHOLE look (LINE_COVER), not a long stretch of it: the left edge of the times column on DDK's schedule
  //   page (Stuart's HF+, 15:04) is straight for ~150 lines and confirmed a "border" through the text on a late join.
  //   A chart joined so near its end that the frame is above the legend only is left as received.
  return g.sharp >= BORDER_LINE_INK && lineCover(rows, width, g.slant, g.line, 0, count) >= LINE_COVER;
}

/** ★ A black margin strip: its first column and width at line 0. */
export interface Strip { at: number; len: number }
/**
 * ★★★ THE BLACK MARGIN STRIP (2026-10-07, docs/WEFAX-WORLD-STATIONS.md): NWS (NMF, NMG, NMC, NOJ, KVM70), JMH,
 *     HLL2, XSG and JFX send the 1728-px picture with the rest of the 1809-px line BLACK — ~81 px, every line. The
 *     margin search took it for a margin and its slant search ran to its end (±0.054: a wide strip's sides smear
 *     into a sharper peak at a wrong slant). Recognised as what it is: a run of STRIP_MIN…STRIP_MAX columns dark on
 *     STRIP_SOLID of lines at the station's slant, with picture (mostly light) either side — a black-paper chart's
 *     own background is not a strip. Its slant is measured from it (findStripSlant).
 */
export function findStrip(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                          fromRow = 0, count = MARGIN_AFTER_LINES, minRows = count / 2): Strip | null {
  const W = width;
  if (!W) return null;
  const p = columnProfile(rows, W, slant, fromRow, count, 1);
  if (p.n < minRows || p.n < 20) return null;
  const solid = new Uint8Array(W);
  let any = -1;
  for (let x = 0; x < W; x++) { solid[x] = p.dark[x] >= STRIP_SOLID * p.n ? 1 : 0; if (!solid[x]) any = x; }
  if (any < 0) return null;                                   // every column solid: no picture at all
  let best: Strip | null = null, len = 0;
  for (let i = 1; i <= W; i++) {
    const x = (any + i) % W;
    if (solid[x]) { len++; continue; }
    if (len >= STRIP_MIN && len <= STRIP_MAX && (!best || len > best.len)) best = { at: (x - len + W) % W, len };
    len = 0;
  }
  if (!best) return null;
  // picture either side: the 40 columns beyond each end average light (ink of 255, 0 = white)
  const side = (from: number, dir: number) => {
    let a = 0;
    for (let k = 4; k < 44; k++) a += p.ink[(((from + dir * k) % W) + W) % W];
    return a / 40 / p.n;
  };
  const b = best as Strip;
  if (side(b.at, -1) > STRIP_SIDE_INK || side(b.at + b.len - 1, 1) > STRIP_SIDE_INK) return null;
  // ★ …and BLACK, not dark: a strip is the transmitter's black level, line after line (ink ~247 of 255; ~240 under
  //   σ 30 of noise). A satellite image's dark sea is dark on 90 % of lines too, but grey — and lined up at a
  //   slant it made a "strip" that cut NOAA's GOES image and slanted the synthetic ones ±0.09…0.17.
  let ink = 0;
  for (let i = 0; i < b.len; i++) ink += p.ink[(b.at + i) % W];
  if (ink / b.len / p.n < STRIP_INK) return null;
  return b;
}
/**
 * ★★ A STRIP'S SLANT, MEASURED: its two edges are straight and solid the whole way down, so it is WIDEST (solid
 *    columns at STRIP_SOLID) at the chart's true slant and loses |error| × lines px at any other — 81 px phased, 17 px
 *    at 0.16 off over 400 lines (CBV/CBM's ~0.16 px/line, the largest of any station). Searched ±BORDER_SLANT_SEARCH
 *    (and to ±0.17, CBV) in steps of 0.01, then 0.002; a slant beyond ±SLANT_TRUST counts only when it widens the
 *    strip by STRIP_GAIN px over the station's own. `fixed`: the station's slant only.
 */
function findStripSlant(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, centre: number, fixed: boolean,
                        fromRow: number, count: number, minRows: number): { strip: Strip; slant: number } | null {
  const at = (k: number) => findStrip(rows, width, Math.round(k * 1000) / 1000, fromRow, count, minRows);
  const s0 = at(centre);
  if (fixed) return s0 ? { strip: s0, slant: centre } : null;
  let best: { strip: Strip; slant: number } | null = s0 ? { strip: s0, slant: centre } : null;
  const tryAt = (k: number) => {
    const s = at(k);
    if (s && (!best || s.len > best.strip.len || (s.len === best.strip.len && Math.abs(k - centre) < Math.abs(best.slant - centre))))
      best = { strip: s, slant: Math.round(k * 1000) / 1000 };
  };
  for (let i = 1; i <= 17; i++) { tryAt(centre + i * 0.01); tryAt(centre - i * 0.01); }
  if (!best) return null;
  const c = (best as { slant: number }).slant;
  for (let i = -4; i <= 4; i++) if (i) tryAt(c + i * 0.002);
  const b = best as { strip: Strip; slant: number };
  if (Math.abs(b.slant - centre) > SLANT_TRUST + 1e-9 && (!s0 ? b.strip.len < STRIP_MIN + STRIP_GAIN : b.strip.len < s0.len + STRIP_GAIN))
    return s0 ? { strip: s0, slant: centre } : null;
  return b;
}
/** ★ The strip: narrowest/widest (px), share of lines its columns are dark on, the most ink the picture beside it may
 *  average (of 255), how far inside its far edge the seam goes (px), how close (px) its far edge may sit to the line's
 *  ends and still be left alone, and the width (px) a slant away from the station's must gain — see findStrip. */
const STRIP_MIN = 40;
const STRIP_MAX = 200;
const STRIP_SOLID = 0.9;
const STRIP_SIDE_INK = 128;
const STRIP_SEAM = 4;
const STRIP_EDGE = 24;
const STRIP_GAIN = 8;
/** ★ The least average ink (of 255) across a strip — black, not a dark grey: see findStrip. */
const STRIP_INK = 225;

/** What findChartAlign found besides its answer. */
export interface AlignLookOut {
  /** The border was already centred on the line's ends — the chart was left where it was. */
  atEdge?: boolean;
  /** Which way the answer was found; passed in as 'margin' or 'strip', the border search is skipped (and as
   *  'margin', the strip search too). */
  via?: 'border' | 'strip' | 'margin';
  /** ★★ The chart's slant as MEASURED from a solid line — the margin, or the frame beside the border — or undefined
   *  when nothing on the chart could measure it (2026-10-07). Set even when nothing is moved: a phased chart left
   *  where it was still has a slant of its own, and the clients draw THAT, not a slant saved on some other day on
   *  some other radio (see ChartAlignState.slant). */
  measured?: number;
  /** In: look at the station's slant only and measure none. */
  fixedSlant?: boolean;
}

/** Per-station slant (px per line); the shift comes from findMargin per chart. Northwood −0.06, everything else 0. */
export function stationSlant(dialHz: number): number { return wefaxPreset(dialHz).slant; }

/** ★ One chart's automatic alignment: `al` undefined = not looked yet, null = looked and nothing to move; `n` = lines
 *  with content seen so far (see FLAT_ROW). */
export interface ChartAlignState { al?: WefaxAlign | null; refined?: boolean; n?: number;
  /** ★ The first look found the blank border already centred on the line's ends: its shift stays 0 for good. */
  atEdge?: boolean;
  /** ★ How the first look decided — by the blank border, a black strip or the margin; the second look keeps to it. */
  via?: 'border' | 'strip' | 'margin';
  /** ★★ THIS chart's slant as measured from a solid line (AlignLookOut.measured), undefined until one is. The
   *  clients draw it in preference to a SAVED slant (2026-10-07): a slant is the transmitter's line rate plus the
   *  RECEIVER's clock, so one saved at 7878 kHz on the RSP1A yesterday is wrong on the HF+ today. */
  slant?: number;
  /** ★ Consecutive featureless lines just received — the chart has ended (see CHART_END_FLAT). */
  flat?: number;
  /** ★ A first look that found nothing has been taken again (see chartAlignStep). */
  retried?: boolean }
/** ★★ A CHART JOINED NEAR ITS END NEVER REACHED THE FIRST LOOK (2026-10-07): ALIGN_CHECK_LINES[0] lines with
 *  content, and the bottom ~600 lines of DDK's North Sea SST chart (the map's last ~200, then the legend and white
 *  paper) have ~230. When this many featureless lines follow at least MARGIN_AFTER_LINES with content, the chart has
 *  ended (DDK's white tail, a stop) and it is looked at with what it has. 40 lines = 20 s at 120 lpm. */
export const CHART_END_FLAT = 40;

/** ★★ A FEATURELESS LINE TELLS THE ALIGNMENT NOTHING (DDK 7880 off air, Stuart's RX888, 2026-10-05). DDK sends a
 *  steady tone for ~2 minutes before the chart; a listener who tunes in during it gets ~270 lines of flat grey at the
 *  top. Grey is ink to findGutter, so those lines filled every column and DDK's white border could never be found.
 *  Measured as the spread of 8-px block averages across a line: the tone 12.7–14 (p50–p99); chart lines with
 *  content 18–35 (p5–p50); the only chart lines flatter are blank white ones, which carry no layout either.
 *  Lines under FLAT_ROW are skipped, and the decision points count only lines with content. */
export const FLAT_ROW = 16;
export function rowIsFlat(r: ArrayLike<number>, width: number): boolean {
  const B = Math.floor(width / 8);
  if (B < 2) return true;
  let s = 0, ss = 0;
  for (let j = 0; j < B; j++) {
    let a = 0;
    for (let k = 0; k < 8; k++) a += r[j * 8 + k] ?? 0;
    a /= 8; s += a; ss += a * a;
  }
  const m = s / B;
  return Math.sqrt(Math.max(0, ss / B - m * m)) < FLAT_ROW;
}

/**
 * ★ The per-chart alignment as each line arrives, for both clients: decided once the chart has ALIGN_CHECK_LINES[0]
 * lines WITH CONTENT, checked once more at [1]. `row` is the line just received (it is counted here, so a dropped row
 * cannot skip either point). Updates `st` and returns true when the chart must be redrawn. `getRows` is called only
 * at those points.
 * `stationSlant` is the STATION's slant (wefaxPreset) — the centre of every slant search. ★★ Never a saved or a
 * previous chart's slant (2026-10-07): centred on a stale −0.06, a DDK chart leaning +0.05 was measured at −0.005,
 * the end of a search that could not reach it.
 * The second look only REFINES a chart the first one moved: on two phased DDK charts (RX888 set) the white border
 * had collected enough noise specks by line 600 to hide it, and the frame then passed for a margin — so a chart
 * left alone at the first look stays alone.
 */
export function chartAlignStep(st: ChartAlignState, getRows: () => ReadonlyArray<ArrayLike<number> | undefined>,
                               width: number, stationSlant: number, row: ArrayLike<number>,
                               fmt: WefaxFormat = defaultFormat(stationSlant)): boolean {
  if (!rowIsFlat(row, width)) { st.n = (st.n ?? 0) + 1; st.flat = 0; }
  else st.flat = (st.flat ?? 0) + 1;
  const n = st.n ?? 0;
  const look = (via?: 'border' | 'strip' | 'margin', fixedSlant = false) => {
    const rows = getRows();
    const out: AlignLookOut = { via, fixedSlant };
    // every row so far, featureless ones blanked (their y still sets the slant offset); half must have content
    const a = findChartAlign(rows.map((r) => (r && !rowIsFlat(r, width) ? r : undefined)), width, stationSlant,
                             rows.length, n / 2, out, fmt);
    return { a, atEdge: !!out.atEdge, via: out.via, measured: out.measured };
  };
  if (st.al === undefined) {
    // ★★ `>=`, not `===` (2026-10-07): an exact count is one chance — a line the client handled without calling here
    //    (ALIGN dragging, a re-render) and the chart was never looked at. Ended early: look with what there is.
    const ended = n >= MARGIN_AFTER_LINES && (st.flat ?? 0) === CHART_END_FLAT;
    if (n < ALIGN_CHECK_LINES[0] && !ended) return false;
    // ★★ An EARLY look (fewer than ALIGN_CHECK_LINES[0] lines) finds the band at the station's slant and measures
    //    none: on the SST chart's last ~230 lines the legend's letters, not the frame, decided the slant search
    //    (−0.08 and +0.13 for a chart leaning +0.02). The chart is cut; the slant waits for a look with more lines.
    //    "Ended" may also be a blank strip between a map and its legend, so the looks at [1] still come.
    const lk = look(undefined, n < ALIGN_CHECK_LINES[0]);
    st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via; st.slant = lk.measured;
    return st.al !== null;
  }
  if (st.refined || n < ALIGN_CHECK_LINES[1]) return false;
  st.refined = true;
  const cur = st.al;
  if (!cur) {
    // ★★ A FIRST LOOK THAT FOUND NOTHING IS TAKEN AGAIN, ONCE (2026-10-07). It was final: one look at 300 lines,
    //    and a chart it missed stayed rolled to the end (Stuart's DDK on the HF+, its 25 %-wide white band left in
    //    the middle of the picture for 2300 lines). A chart whose border was found already centred (atEdge) is not
    //    looked at again — that is the phased chart the second look must never cut.
    if (st.atEdge || st.retried) return false;
    st.retried = true;
    const lk = look();
    if (lk.measured !== undefined) st.slant = lk.measured;
    if (!lk.a) return false;
    st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via;
    return true;
  }
  // ★★ THE SECOND LOOK REFINES THE FIRST, IT NEVER CHANGES ITS MIND ABOUT WHAT KIND OF CHART THIS IS (2026-10-06):
  //    by line 600 a Northwood chart's light patches had grown into a 59–64 px "border" on 11 of 60 charts and the
  //    chart was re-cut ~50 px off its margin; a DDK border, gone noisy, would fall to the margin search. A margin
  //    chart is looked at by margin only; a border chart's look is used only if it found the border again.
  const lk = look(st.via === 'border' ? undefined : st.via), r = lk.a;
  // ★★ A chart left where it was at the first look (its border already centred) is never MOVED by the second:
  //    only its slant may be refined, and only by a look that again finds the border centred — null from that look
  //    is "centred, and at the station's slant".
  const again = lk.via === st.via && (st.atEdge ? lk.atEdge && (!r || r.shift === 0) : !!r);
  if (!again) return false;
  // ★★ …and the longer look's slant REPLACES the first's (2026-10-07), measured or not: twice the lines, and a slant
  //    the first look took from a line that does not hold up (the ice chart's thick frame edge, +0.053 at 300
  //    lines) went on being drawn when the second look found nothing to measure.
  const slantMoved = st.slant !== lk.measured;
  st.slant = lk.measured;
  const next = r ?? { shift: 0, slant: stationSlant };
  const d = (((next.shift - cur.shift) % width) + width) % width;
  if (Math.min(d, width - d) <= 2 && Math.abs(next.slant - cur.slant) < 0.002) return slantMoved;
  st.al = r ? next : (st.atEdge ? null : next);
  return true;
}
