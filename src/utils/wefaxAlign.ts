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

/** The station preset for a dial frequency, or zero. The dial in USB sits ~1.9 kHz below the carrier, so match
 *  within 5 kHz of a carrier. */
export function wefaxPreset(dialHz: number): WefaxAlign {
  const khz = dialHz / 1000;
  return NORTHWOOD_KHZ.some((c) => Math.abs(khz - c) <= 5) ? { ...NORTHWOOD_ALIGN } : { ...WEFAX_ALIGN_ZERO };
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
  for (let i = -10; i <= 10; i++) tryAt(centre + i * SLANT_SEARCH / 10);
  if (!best) return null;
  const c = (best as { slant: number }).slant;
  for (let i = -4; i <= 4; i++) if (i) tryAt(Math.round((c + i * 0.001) * 1000) / 1000);
  const b = best as { col: number; slant: number };
  return { col: b.col, slant: Math.round(b.slant * 1000) / 1000 };
}

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
export interface BlankBand { col: number; len: number; edge: number; slant: number; sharp: number }

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
  let pl = 0, pr = 0;
  for (let k = 1; k <= 12; k++) {
    pl = Math.max(pl, ink[((bestAt - k) % W + W) % W]);
    pr = Math.max(pr, ink[(bestAt + bestLen - 1 + k) % W]);
  }
  return { col: (bestAt + Math.floor(bestLen / 2)) % W, len: bestLen, edge, slant, sharp: Math.max(pl, pr) / n };
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
  for (let k = SLANT_SEARCH + 0.01; k <= BORDER_SLANT_SEARCH + 1e-9; k += 0.01) { tryAt(centre + k, 2); tryAt(centre - k, 2); }
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
  if (b && b.sharp >= BORDER_LINE_INK) return b;
  // ★ …and then the band is the one AT the station's slant, or none: a band that only appears at another slant, with no
  //   straight line to say that slant is real, was a Northwood sea patch lined up into a 114 px "border" (1 of 60).
  return findGutter(rows, width, centre, fromRow, count, minRows);
}
/** ★★ How far from the station's slant a BORDERED chart's slant is searched, px per line (2026-10-07). Stuart's DDK
 *  7880 on the Pi 500's Airspy HF+ came out leaning +0.11 px/line (frame line measured on the finished chart,
 *  11:10 BST) against +0.010 for the same chart on the UberSDR's RX888 — and ±SLANT_SEARCH (±0.05) could not reach
 *  it: the "best" slant was the search's own end (0.056), which leaves the chart leaning half as much. A border's
 *  slant is only believed from a SOLID line right beside the band (BORDER_LINE_INK), and the frame line is the
 *  sharpest such line at its true slant by a wide margin (254 ink at 0.11–0.13 against ≤ 82 anywhere in ±0.05 on
 *  the rebuilt chart), so the wider search does not line up the meridians inside the map the way the margin search
 *  would. ±0.15 is ±83 ppm at 1809 px. */
export const BORDER_SLANT_SEARCH = 0.15;
/** ★ How dark (average ink of 255) the darkest column beside a border must be to measure a slant from it. DDK's
 *  frame line reads ~145 at its true slant; text and ragged map edges beside a border 7–40. */
const BORDER_LINE_INK = 80;

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
                               out?: AlignLookOut): WefaxAlign | null {
  // ★★ A BORDERED CHART NEVER HAS ITS FRAME TAKEN FOR A MARGIN: a blank band means a DDK-style chart, and its
  //    frame and meridians are as straight as a margin (5 of 60 DDK charts were mis-moved by the margin search
  //    when it ran first). The margin search runs only when there is no blank band at all.
  const g = out?.via === 'margin' ? null : findBorder(rows, width, stationSlant, 0, count, minRows);
  if (out) out.via = g ? 'border' : 'margin';
  // ★★ The slant MEASURED on this chart (2026-10-07), whatever is returned — see AlignLookOut.measured.
  if (g && out && g.sharp >= BORDER_LINE_INK) out.measured = g.slant;
  if (g) {
    // ★★★ A BORDERED CHART IS CENTRED ON ITS BORDER (Stuart, 2026-10-06: "if there is a clear white border either
    //     side, our auto align just needs to centre the image on it — 55 left 110 right then we just do 82 left 83
    //     right of white"). The band's centre goes to the line's ends: equal white either side, wherever the band
    //     was — split across the edge (phased, a little off) or mid-picture (joined late). A chart already within
    //     CENTRE_DEADBAND px of centred is left exactly as received ("it was already aligned and we broke the
    //     alignment"). ★ Content decides, never the phasing signal: Northwood is phased on every chart on Stuart's
    //     UberSDR and still arrives with its margin 35–46 px in, or ~475 px in.
    const fromEdge = Math.min(g.col, width - g.col);
    if (fromEdge < CENTRE_DEADBAND) {
      if (out) out.atEdge = true;
      return Math.abs(g.slant - stationSlant) < 0.002 ? null : { shift: 0, slant: g.slant };
    }
    return { shift: g.col, slant: g.slant };
  }
  const m = findMarginSlant(rows, width, stationSlant, 0, count, minRows);
  if (m && out) out.measured = m.slant;
  return m ? { shift: m.col - 2, slant: m.slant } : null;
}
/** What findChartAlign found besides its answer. */
export interface AlignLookOut {
  /** The border was already centred on the line's ends — the chart was left where it was. */
  atEdge?: boolean;
  /** Which way the answer was found; passed in as 'margin', the border search is skipped. */
  via?: 'border' | 'margin';
  /** ★★ The chart's slant as MEASURED from a solid line — the margin, or the frame beside the border — or undefined
   *  when nothing on the chart could measure it (2026-10-07). Set even when nothing is moved: a phased chart left
   *  where it was still has a slant of its own, and the clients draw THAT, not a slant saved on some other day on
   *  some other radio (see ChartAlignState.slant). */
  measured?: number;
}

/** Per-station slant (px per line); the shift comes from findMargin per chart. Northwood −0.06, everything else 0. */
export function stationSlant(dialHz: number): number { return wefaxPreset(dialHz).slant; }

/** ★ One chart's automatic alignment: `al` undefined = not looked yet, null = looked and nothing to move; `n` = lines
 *  with content seen so far (see FLAT_ROW). */
export interface ChartAlignState { al?: WefaxAlign | null; refined?: boolean; n?: number;
  /** ★ The first look found the blank border already centred on the line's ends: its shift stays 0 for good. */
  atEdge?: boolean;
  /** ★ How the first look decided — by the blank border or by the margin; the second look keeps to it. */
  via?: 'border' | 'margin';
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
                               width: number, stationSlant: number, row: ArrayLike<number>): boolean {
  if (!rowIsFlat(row, width)) { st.n = (st.n ?? 0) + 1; st.flat = 0; }
  else st.flat = (st.flat ?? 0) + 1;
  const n = st.n ?? 0;
  const look = (via?: 'border' | 'margin') => {
    const rows = getRows();
    const out: AlignLookOut = { via };
    // every row so far, featureless ones blanked (their y still sets the slant offset); half must have content
    const a = findChartAlign(rows.map((r) => (r && !rowIsFlat(r, width) ? r : undefined)), width, stationSlant,
                             rows.length, n / 2, out);
    return { a, atEdge: !!out.atEdge, via: out.via, measured: out.measured };
  };
  if (st.al === undefined) {
    // ★★ `>=`, not `===` (2026-10-07): an exact count is one chance — a line the client handled without calling here
    //    (ALIGN dragging, a re-render) and the chart was never looked at. Ended early: look with what there is.
    const ended = n >= MARGIN_AFTER_LINES && (st.flat ?? 0) === CHART_END_FLAT;
    if (n < ALIGN_CHECK_LINES[0] && !ended) return false;
    const lk = look();
    st.al = lk.a; st.atEdge = lk.atEdge; st.via = lk.via; st.slant = lk.measured;
    if (n < ALIGN_CHECK_LINES[1]) st.refined = ended || undefined;   // a chart that has ended gets no second look
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
  const lk = look(st.via === 'margin' ? 'margin' : undefined), r = lk.a;
  if (!r || lk.via !== st.via) return false;
  // ★★ A chart left where it was at the first look (its border already centred) is never MOVED by the second:
  //    only its slant may be refined, and only by a look that again finds the border centred.
  if (st.atEdge && (!lk.atEdge || r.shift !== 0)) return false;
  if (lk.measured !== undefined) st.slant = lk.measured;
  const d = (((r.shift - cur.shift) % width) + width) % width;
  if (Math.min(d, width - d) <= 2 && Math.abs(r.slant - cur.slant) < 0.002) return false;
  st.al = r;
  return true;
}
