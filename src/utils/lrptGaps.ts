/**
 * lrptGaps.ts — METEOR LRPT's LIVE PREVIEW with its gaps SQUASHED (beta96, 2026-11-26). Shared by the app
 * (DecoderImageCanvas) and the web page (main.ts) — the rule and the pixels are here, each client only shows them.
 *
 * ★★★ WHY (Stuart, a Meteor-M2-4 pass, 11-26): the beta95 hatch worked, but a gap was drawn at its TRUE height — 616 rows
 * of no signal took as much room as the decoded picture; and near LOS a fade of thin strips between short gaps carried a
 * label each ("NO SIGNAL 14:42" ×3) and scrolled the good cloud picture out of view.
 *
 * WHAT: the server still sends the preview in TRUE geometry (an older client shows that; the live map outline is in those
 * coordinates) and lists its gaps in lrpt_status ("gaps": [[r0, r1, msA, msB, lines, patchy], …] in preview rows, merged
 * on the server — vibe_lrpt_run.h gapart::mergeGaps: a short run of picture between two gaps makes one PATCHY gap). Here
 * each listed gap becomes ONE short band (GAP_BAND_ROWS, never taller than the gap itself), hatched as the server hatches
 * and labelled "NO SIGNAL 14:40-14:41" over "616 LINES" (or "PATCHY, 88 LINES"); a label never repeats the one before it
 * (then the times carry seconds). The outline is mapped through the same plan, so it stays on the picture.
 *
 * NOT CHANGED: the gallery's pictures (full size, true geometry, made by the server), SAVE of the live picture (the true
 * preview), the server's map overlay. Only what the live preview SHOWS.
 */
import type { Outline } from './lrptOutline';

export interface LrptGap {
  r0: number; r1: number;     // preview rows [r0, r1) in true geometry
  msA: number; msB: number;   // satellite time (ms of the day, UTC) of the decoded strip before / after; −1 none
  lines: number;              // lines of no signal (a patchy gap's decoded slivers not counted)
  patchy: boolean;            // several gaps with only thin strips of picture between them
}

/** A squashed gap's height in preview rows (the preview is 392 wide: ≈ 8 % of its width). */
export const GAP_BAND_ROWS = 32;
const MAX_GAPS = 128;

/** lrpt_status's "gaps" → sorted, non-overlapping gaps; [] for none or an older server. */
export function parseLrptGaps(v: unknown): LrptGap[] {
  if (!Array.isArray(v)) return [];
  const out: LrptGap[] = [];
  for (const a of v.slice(0, MAX_GAPS)) {
    if (!Array.isArray(a) || a.length < 6 || !a.slice(0, 6).every((x) => typeof x === 'number' && isFinite(x))) continue;
    const r0 = Math.round(a[0]), r1 = Math.round(a[1]);
    if (r0 < 0 || r1 <= r0 || r1 > 100000) continue;
    out.push({ r0, r1, msA: a[2] >= 0 ? a[2] : -1, msB: a[3] >= 0 ? a[3] : -1, lines: Math.max(0, Math.round(a[4])), patchy: a[5] === 1 });
  }
  out.sort((x, y) => x.r0 - y.r0);
  return out.filter((g, i) => i === 0 || g.r0 >= out[i - 1].r1);
}
export const sameGaps = (a: LrptGap[], b: LrptGap[]) =>
  a.length === b.length && a.every((g, i) => g.r0 === b[i].r0 && g.r1 === b[i].r1 && g.lines === b[i].lines && g.patchy === b[i].patchy && g.msA === b[i].msA && g.msB === b[i].msB);

export interface SquashSeg {
  src0: number; src1: number;   // true rows [src0, src1)
  dst0: number; dstH: number;   // where they are shown
  gap: LrptGap | null;          // a band (null = picture, shown row for row)
  label: string[];              // a band's label lines ([] = too short for one)
}
export interface SquashPlan { segs: SquashSeg[]; srcH: number; h: number; bands: number }

function hhmm(ms: number, secs: boolean): string {
  if (ms < 0) return '';
  const t = Math.floor(ms / 1000) % 86400, p = (n: number) => String(n).padStart(2, '0');
  return `${p(Math.floor(t / 3600))}:${p(Math.floor(t / 60) % 60)}${secs ? ':' + p(t % 60) : ''}`;
}
/** "NO SIGNAL 14:40-14:41" (seconds when `secs`). */
export function gapTitle(g: LrptGap, secs = false): string {
  const a = hhmm(g.msA, secs), b = hhmm(g.msB, secs);
  return 'NO SIGNAL' + (a ? ' ' + a + (b && b !== a ? '-' + b : '') : b ? ' -' + b : '');
}
/** "616 LINES" / "PATCHY, 88 LINES" — what the second line says. */
export function gapCount(g: LrptGap): string {
  return `${g.patchy ? 'PATCHY, ' : ''}${g.lines} LINE${g.lines === 1 ? '' : 'S'}`;
}

/** The display plan for the first `srcH` true rows: picture row for row, each gap one band. */
export function squashPlan(gaps: LrptGap[], srcH: number): SquashPlan {
  const segs: SquashSeg[] = [];
  let at = 0, dst = 0, bands = 0, lastTitle = '';
  for (const g0 of gaps) {
    if (g0.r0 >= srcH) break;
    if (g0.r0 < at) continue;
    const g = { ...g0, r1: Math.min(g0.r1, srcH) };
    if (g.r0 > at) { segs.push({ src0: at, src1: g.r0, dst0: dst, dstH: g.r0 - at, gap: null, label: [] }); dst += g.r0 - at; }
    const h = Math.min(g.r1 - g.r0, GAP_BAND_ROWS);
    // ★ a label never repeats the one before it (two gaps in the same minute): then the times carry seconds
    let title = gapTitle(g);
    if (title === lastTitle) title = gapTitle(g, true);
    lastTitle = title;
    const label = h >= 24 ? [title, gapCount(g)] : h >= 9 ? [`${title} (${gapCount(g)})`] : [];
    segs.push({ src0: g.r0, src1: g.r1, dst0: dst, dstH: h, gap: g, label });
    dst += h; at = g.r1; bands++;
  }
  if (srcH > at) { segs.push({ src0: at, src1: srcH, dst0: dst, dstH: srcH - at, gap: null, label: [] }); dst += srcH - at; }
  return { segs, srcH, h: dst, bands };
}

/** A true row (fractions allowed) → its shown row. Inside a band, spread over the band; past the end, carried on. */
export function mapRow(plan: SquashPlan, y: number): number {
  for (const s of plan.segs) {
    if (y < s.src1) {
      if (y < s.src0) return s.dst0;
      return s.gap ? s.dst0 + (y - s.src0) * s.dstH / Math.max(1, s.src1 - s.src0) : s.dst0 + (y - s.src0);
    }
  }
  return plan.h + (y - plan.srcH);
}

// ── the band's pixels: our own 5 × 7 glyphs (the server's gapart, plus what "PATCHY, 88 LINES" needs) ──
const G: Record<string, number[]> = {
  '0': [14, 17, 19, 21, 25, 17, 14], '1': [4, 12, 4, 4, 4, 4, 14], '2': [14, 17, 1, 2, 4, 8, 31], '3': [30, 1, 1, 14, 1, 1, 30],
  '4': [2, 6, 10, 18, 31, 2, 2], '5': [31, 16, 30, 1, 1, 17, 14], '6': [6, 8, 16, 30, 17, 17, 14], '7': [31, 1, 2, 4, 8, 8, 8],
  '8': [14, 17, 17, 14, 17, 17, 14], '9': [14, 17, 17, 15, 1, 2, 12],
  N: [17, 25, 21, 19, 17, 17, 17], O: [14, 17, 17, 17, 17, 17, 14], S: [15, 16, 16, 14, 1, 1, 30], I: [14, 4, 4, 4, 4, 4, 14],
  G: [14, 17, 16, 23, 17, 17, 15], A: [14, 17, 17, 31, 17, 17, 17], L: [16, 16, 16, 16, 16, 16, 31], P: [30, 17, 17, 30, 16, 16, 16],
  T: [31, 4, 4, 4, 4, 4, 4], C: [14, 17, 16, 16, 16, 17, 14], H: [17, 17, 17, 31, 17, 17, 17], Y: [17, 17, 10, 4, 4, 4, 4],
  E: [31, 16, 16, 30, 16, 16, 31], ':': [0, 12, 12, 0, 12, 12, 0], '-': [0, 0, 0, 31, 0, 0, 0], '(': [2, 4, 8, 8, 8, 4, 2],
  ')': [8, 4, 2, 2, 2, 4, 8], ',': [0, 0, 0, 0, 12, 4, 8],
};
export const INK = [214, 214, 224] as const;
/** The hatch (the server's gapart::hatch): dim diagonal stripes. */
function hatch(o: Uint8Array | Uint8ClampedArray, i: number, x: number, y: number) {
  const v = (x + y) % 6 < 2 ? 54 : 26;
  o[i] = v; o[i + 1] = v; o[i + 2] = v + 10; o[i + 3] = 255;
}
function text(o: Uint8Array | Uint8ClampedArray, w: number, h: number, t: string, scale: number, y0: number) {
  const tw = (t.length * 6 - 1) * scale, x0 = Math.max(0, Math.floor((w - tw) / 2));
  for (let c = 0; c < t.length; c++) {
    const g = G[t[c]]; if (!g) continue;
    for (let gy = 0; gy < 7; gy++) for (let gx = 0; gx < 5; gx++) {
      if (!((g[gy] >> (4 - gx)) & 1)) continue;
      for (let dy = 0; dy < scale; dy++) for (let dx = 0; dx < scale; dx++) {
        const x = x0 + (c * 6 + gx) * scale + dx, y = y0 + gy * scale + dy;
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        const i = (y * w + x) * 4; o[i] = INK[0]; o[i + 1] = INK[1]; o[i + 2] = INK[2]; o[i + 3] = 255;
      }
    }
  }
}
/** A band's RGBA pixels (w × h) into `o` from row `oy` (o is w wide). */
export function drawBand(o: Uint8Array | Uint8ClampedArray, w: number, oy: number, h: number, label: string[]) {
  const band = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) hatch(band, (y * w + x) * 4, x, y);
  if (label.length === 2) {
    const s1 = (label[0].length * 6 - 1) * 2 <= w - 8 ? 2 : 1;
    const top = Math.max(1, Math.floor((h - (7 * s1 + 3 + 7)) / 2));
    text(band, w, h, label[0], s1, top);
    text(band, w, h, label[1], 1, top + 7 * s1 + 3);
  } else if (label.length === 1) {
    text(band, w, h, label[0], 1, Math.floor((h - 7) / 2));
  }
  o.set(band, oy * w * 4);
}
/** A band on its own (the web page puts it on the canvas). */
export function bandPixels(w: number, h: number, label: string[]): Uint8ClampedArray<ArrayBuffer> {
  const o = new Uint8ClampedArray(w * h * 4); drawBand(o, w, 0, h, label); return o;
}
/** The whole shown picture (the app): `src` RGBA, w wide, true rows → the squashed RGBA (plan.h rows). */
export function renderSquashed(src: Uint8Array, w: number, plan: SquashPlan): Uint8Array {
  const o = new Uint8Array(w * Math.max(1, plan.h) * 4);
  for (const s of plan.segs) {
    if (s.gap) drawBand(o, w, s.dst0, s.dstH, s.label);
    else o.set(src.subarray(s.src0 * w * 4, s.src1 * w * 4), s.dst0 * w * 4);
  }
  return o;
}

/** The live MAP OUTLINE moved with the picture (its rows through the plan; a gap's lines run through its band). */
export function mapOutline(o: Outline, plan: SquashPlan): Outline {
  if (!plan.bands) return o;
  const mapLines = (ls: number[][]) => ls.map((l) => { const m = l.slice(); for (let i = 1; i < m.length; i += 2) m[i] = Math.round(mapRow(plan, m[i]) * 10) / 10; return m; });
  const lines = { coast: mapLines(o.lines.coast), lake: mapLines(o.lines.lake), border: mapLines(o.lines.border), grid: mapLines(o.lines.grid) };
  const path = (ls: number[][]) => ls.map((l) => { let d = `M${l[0]} ${l[1]}`; for (let i = 2; i + 1 < l.length; i += 2) d += `L${l[i]} ${l[i + 1]}`; return d; }).join('');
  return {
    ...o, h: Math.max(1, Math.round(mapRow(plan, o.h))), lines,
    coast: path(lines.coast), lake: path(lines.lake), border: path(lines.border), grid: path(lines.grid),
    labels: o.labels.map((l) => ({ ...l, y: mapRow(plan, l.y) })),
    centre: o.centre ? [o.centre[0], mapRow(plan, o.centre[1])] : null,
  };
}

/* ★★ FIT OR FOLLOW (the SATELLITE box, which draws the picture at its full width and scrolls). With the gaps squashed a
 * whole pass is usually short enough to show WHOLE — so the early clean picture never scrolls away. Fit while the fitted
 * picture is still at least half the box's width; past that it would be a sliver, so follow the newest DECODED row as
 * before. A tap on the picture flips between the two for this pass. */
export type LrptView = 'auto' | 'fit' | 'follow';
export function lrptFits(picW: number, picH: number, boxW: number, boxH: number): boolean {
  if (picW <= 0 || picH <= 0 || boxW <= 0 || boxH <= 0) return false;
  return picW * Math.min(boxW / picW, boxH / picH) >= boxW * 0.5;
}
export function lrptFitNow(view: LrptView, picW: number, picH: number, boxW: number, boxH: number): boolean {
  return view === 'fit' || (view === 'auto' && lrptFits(picW, picH, boxW, boxH));
}
/** A tap: the other one of what is shown now. */
export const lrptViewToggle = (fitNow: boolean): LrptView => (fitNow ? 'follow' : 'fit');
export const lrptViewHint = (fitNow: boolean) => (fitNow ? 'Whole pass shown — tap to follow the newest lines' : 'Following the newest lines — tap to show the whole pass');
