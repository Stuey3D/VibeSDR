/**
 * wefaxCrisp — the "gold standard" WEFAX rendering, app + web (no server cost).
 *
 * ★★★ WHY (Stuart, 2026-10-04, holding up a DWD chart received perfectly: "this is probably a gold standard WEFAX
 *     receive" — clean white paper, solid black lines, readable text). Our charts came out as mid-grey speckle:
 *     measured on Northwood 4610 the "white" paper sat at ~138 of 255. Tested on two real Northwood charts:
 *       1. a 5×5 [1 4 6 4 1] smoothing — noise is random pixel to pixel and averages away; lines and text are
 *          several pixels wide and survive;
 *       2. the chart's OWN paper and ink levels (70th / 2nd percentile of everything received so far);
 *       3. a firm curve: white above 90 % of the way from ink to paper, black below 35 %, a straight ramp between.
 *     ★ NOT pure black-and-white: a hard threshold erased Northwood's grey-shaded title box (dark text on light
 *       shading) and its lighter precipitation areas. The ramp keeps both, and the paper still comes out white.
 * ★ Works on the lines AFTER SHIFT / SLANT (wefaxAlign), so the smoothing runs down the page as it is drawn. A line
 *   is final once the two below it have arrived (~1 s at 120 lpm); until then it is drawn with what exists.
 */
export const CRISP_WHITE = 0.90;
export const CRISP_BLACK = 0.35;

/** A running histogram of the chart's raw pixels — 256 counters; add every line as it arrives. */
export function newHist(): Uint32Array { return new Uint32Array(256); }
export function addToHist(h: Uint32Array, line: ArrayLike<number>): void {
  for (let i = 0; i < line.length; i++) h[line[i] & 255]++;
}
/** [lo, hi] — below lo is ink (black), above hi is paper (white). Null until there is enough to judge. */
export function crispLevels(h: Uint32Array): [number, number] | null {
  let n = 0;
  for (let v = 0; v < 256; v++) n += h[v];
  if (n < 2000) return null;
  const at = (p: number) => { let acc = 0; for (let v = 0; v < 256; v++) { acc += h[v]; if (acc >= n * p) return v; } return 255; };
  const ink = at(0.02), paper = at(0.70);
  if (paper - ink < 8) return null;              // a blank or flat page: leave it as it is
  return [ink + CRISP_BLACK * (paper - ink), ink + CRISP_WHITE * (paper - ink)];
}

const K = [1, 4, 6, 4, 1];   // /16 each way

/**
 * The crisp version of line `y`: `row(j)` returns line j as DRAWN (after shift/slant) or undefined if it has not
 * arrived — a missing neighbour is replaced by the nearest line that has. `out` receives `width` greys.
 */
export function crispLine(row: (j: number) => ArrayLike<number> | undefined, y: number, width: number,
                          lv: [number, number], out: Uint8Array): void {
  const W = width;
  const rows: ArrayLike<number>[] = [];
  const self = row(y);
  if (!self) return;
  for (let d = -2; d <= 2; d++) {
    let r = row(y + d);
    if (!r) r = d < 0 ? (row(y + d + 1) ?? self) : (row(y + d - 1) ?? self);
    if (!r) r = self;
    rows.push(r);
  }
  const v = new Float32Array(W);
  for (let x = 0; x < W; x++) {
    v[x] = (rows[0][x] * 1 + rows[1][x] * 4 + rows[2][x] * 6 + rows[3][x] * 4 + rows[4][x] * 1) / 16;
  }
  const [lo, hi] = lv, span = Math.max(1, hi - lo);
  for (let x = 0; x < W; x++) {
    const a = v[x < 2 ? x : x - 2], b = v[x < 1 ? x : x - 1], c = v[x], d = v[x > W - 2 ? x : x + 1], e = v[x > W - 3 ? x : x + 2];
    const g = (a * K[0] + b * K[1] + c * K[2] + d * K[3] + e * K[4]) / 16;
    const t = (g - lo) / span;
    out[x] = t <= 0 ? 0 : t >= 1 ? 255 : Math.round(t * 255);
  }
}
