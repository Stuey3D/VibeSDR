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
/** Copy `src` (one line) into `dst` moved left by `off` pixels, wrapping. */
export function rotateLine(src: ArrayLike<number>, dst: Uint8Array | number[], width: number, off: number): void {
  for (let x = 0; x < width; x++) dst[x] = src[(x + off) % width] ?? 0;
}
/** Steps for the adjust keys. */
export const SHIFT_STEP = 5;
export const SLANT_STEP = 0.005;

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
                    fromRow: number, count: number): { col: number; strength: number; ok: boolean } | null {
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
  if (n < count / 2) return null;
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
                                fromRow = 0, count = MARGIN_AFTER_LINES): { col: number; slant: number } | null {
  let best: { col: number; strength: number; slant: number } | null = null;
  const tryAt = (k: number) => {
    const m = marginPeak(rows, width, k, fromRow, count);
    if (m && m.ok && (!best || m.strength > best.strength)) best = { col: m.col, strength: m.strength, slant: k };
  };
  for (let i = -10; i <= 10; i++) tryAt(centre + i * SLANT_SEARCH / 10);
  if (!best) return null;
  const c = (best as { slant: number }).slant;
  for (let i = -4; i <= 4; i++) if (i) tryAt(Math.round((c + i * 0.001) * 1000) / 1000);
  const b = best as { col: number; slant: number };
  return { col: b.col, slant: Math.round(b.slant * 1000) / 1000 };
}

/** ★ The narrowest blank border findGutter accepts, px. DDK's is ~115 (left border + right border, joined). */
const GUTTER_MIN = 24;

/**
 * ★★★ NO MARGIN? FIND THE BLANK BORDER (Stuart, 2026-10-05, joined a DDK chart half way: "chart is wrapped around on
 *     itself"). DWD/DDK send no black margin — the map sits in a frame on WHITE paper. Joined without phasing, the
 *     line starts anywhere and that white border lands mid-picture. It is as findable as a margin: a band of
 *     columns with NO ink on ANY line (map whites are crossed by grid lines and text within a few lines; a border
 *     never is). The widest such band, ≥ GUTTER_MIN, is where the lines' ends meet: its centre goes to the edge.
 *     Returns the band's centre column and width, or null when there is none.
 */
export function findGutter(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                           fromRow = 0, count = MARGIN_AFTER_LINES): { col: number; len: number } | null {
  const W = width;
  if (!W) return null;
  const inked = new Uint8Array(W);
  let n = 0;
  for (let y = fromRow; y < fromRow + count && y < rows.length; y++) {
    const r = rows[y];
    if (!r) continue;
    const off = ((Math.round(slant * y) % W) + W) % W;
    for (let x = 0; x < W; x++) if ((r[(x + off) % W] ?? 255) < 160) inked[x] = 1;
    n++;
  }
  if (n < count / 2) return null;
  // Longest circular run of un-inked columns.
  let start = -1;
  for (let x = 0; x < W; x++) if (inked[x]) { start = x; break; }
  if (start < 0) return null;                      // nothing drawn at all
  let bestLen = 0, bestAt = 0, len = 0;
  for (let i = 1; i <= W; i++) {
    const x = (start + i) % W;
    if (!inked[x]) { len++; if (len > bestLen) { bestLen = len; bestAt = (x - len + 1 + W) % W; } }
    else len = 0;
  }
  if (bestLen < GUTTER_MIN) return null;
  return { col: (bestAt + Math.floor(bestLen / 2)) % W, len: bestLen };
}

/** ★ When a chart's shift is decided, and when it is checked once more (redrawn only if the answer changed).
 *  Measured on 120 RX888 charts: 150 lines agreed with the whole chart on 42/60 Northwood, 300 on 58/60. */
export const ALIGN_CHECK_LINES = [300, 600];

/**
 * ★★★ ONE CALL FOR EVERY STATION: the margin (Northwood-style), else the blank border (DDK-style), else nothing.
 * `shift` moves the found column to the left edge (wefaxOffset), `slant` is this chart's — measured from the margin,
 * else the station's own.
 */
export function findChartAlign(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, stationSlant: number,
                               count: number): WefaxAlign | null {
  // ★★ A BORDERED CHART NEVER HAS ITS FRAME TAKEN FOR A MARGIN: a blank band means a DDK-style chart, and its
  //    frame and meridians are as straight as a margin (5 of 60 DDK charts were mis-moved by the margin search
  //    when it ran first). Measured: no Northwood chart of 60 has a blank band.
  const g = findGutter(rows, width, stationSlant, 0, count);
  if (g) {
    // Already at the edge (a phased chart) → leave it exactly as received; a phased DDK chart's border is split
    // between the two edges, so its band's centre sits within half a band of column 0.
    const fromEdge = Math.min(g.col, width - g.col);
    return fromEdge <= g.len / 2 + GUTTER_MIN ? null : { shift: g.col, slant: stationSlant };
  }
  const m = findMarginSlant(rows, width, stationSlant, 0, count);
  return m ? { shift: m.col - 2, slant: m.slant } : null;
}

/** Per-station slant (px per line); the shift comes from findMargin per chart. Northwood −0.06, everything else 0. */
export function stationSlant(dialHz: number): number { return wefaxPreset(dialHz).slant; }

/** ★ One chart's automatic alignment: `al` undefined = not looked yet, null = looked and nothing to move. */
export interface ChartAlignState { al?: WefaxAlign | null; refined?: boolean }

/**
 * ★ The per-chart alignment as line `line` arrives, for both clients: decided once the chart reaches
 * ALIGN_CHECK_LINES[0], checked once more at [1] (by ≥, so a dropped row cannot skip either). Updates `st` and
 * returns true when the chart must be redrawn. `getRows` is called only on those two lines.
 * The second look only REFINES a chart the first one moved: on two phased DDK charts (RX888 set) the white border
 * had collected enough noise specks by line 600 to hide it, and the frame then passed for a margin — so a chart
 * left alone at 300 stays alone.
 */
export function chartAlignStep(st: ChartAlignState, getRows: () => ReadonlyArray<ArrayLike<number> | undefined>,
                               width: number, stationSlant: number, line: number): boolean {
  if (st.al === undefined) {
    if (line < ALIGN_CHECK_LINES[0]) return false;
    st.al = findChartAlign(getRows(), width, stationSlant, line);
    return st.al !== null;
  }
  if (st.refined || line < ALIGN_CHECK_LINES[1]) return false;
  st.refined = true;
  const cur = st.al;
  if (!cur) return false;
  const r = findChartAlign(getRows(), width, stationSlant, line);
  if (!r) return false;
  const d = (((r.shift - cur.shift) % width) + width) % width;
  if (Math.min(d, width - d) <= 2 && Math.abs(r.slant - cur.slant) < 0.002) return false;
  st.al = r;
  return true;
}
