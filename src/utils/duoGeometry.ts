/**
 * ★★ Pure geometry for the iPhone Duo work (2026-11-25) — the reserved-region types and helpers (useReservedRegions
 * reads UIKit's rects into these), and the window rule for the folded front screen. No React Native here, so
 * scripts/test_duo_layout.ts can prove it.
 */

export interface RegionRect { x: number; y: number; w: number; h: number }
export interface HingeState { status: 'closed' | 'partiallyOpen' | 'fullyOpen' | 'unknown'; angleDeg: number }
export interface ReservedRegions {
  /** Drawn over the app by the system (the outer camera, an active inner camera), margins included. */
  occlusion: RegionRect[];
  /** Where the window divides (the fold, while partially open). */
  division: RegionRect[];
  /** The status bar's frame (UIStatusBarManager) — on the Duo the clock + Wi-Fi pill; [] while hidden. */
  statusBar: RegionRect[];
  /** UIHingeInteraction (iOS 27.1): null = this window is not on a folding display (or the OS cannot say). */
  hinge: HingeState | null;
}

export const NO_REGIONS: ReservedRegions = { occlusion: [], division: [], statusBar: [], hinge: null };

/** ★★ THE TABLET LINE: a window whose SHORTEST side is at least this (pt) is a tablet (Android's sw600dp). It was 768
 *  (the iPad's), so a 601 pt Galaxy Tab A9, an open foldable and an iPad mini (744) were treated as PHONES and got the
 *  phone column scaled up 1.45× — "as if someone had taken a phone and made it 8 inches" (Stuart, 2026-10-11).
 *  ONE number for every reader (useUiScale, SDRScreen, MenuSheet). */
export const TABLET_MIN_SIDE = 600;

/** The height a 390 pt-wide phone window is measured against for the deck's scale (see useUiScale). */
export const SHORT_REF_H = 650;

/** ★★ The widest long : short side a window can have and still be "short" — the iPhone Duo's folded front screen is
 *  678 / 466 = 1.46; the squarest iPhone (SE, 8, 8 Plus) is 1.78. */
export const FRONT_SCREEN_MAX_ASPECT = 1.6;

/** ★★ A further, uniform shrink of the whole deck on a SHORT portrait window (Stuart, Duo folded front screen: "the
 *  main controls could have an all round shrink as they look huge"): keys, drums, tubes, text, paddings and radii
 *  together, because every one of them is drawn through the same scale. */
export const SHORT_WINDOW_DECK_K = 0.78;

/** The deck's portrait scale for a phone window: width / 390 — and on a SHORT window (height under
 *  FRONT_SCREEN_MAX_ASPECT × width: the Duo's front screen, a share of the same shape; no iPhone is) no more than
 *  height / SHORT_REF_H, then × SHORT_WINDOW_DECK_K. Every other window: exactly width / 390, as always. */
export function phonePortraitScale(W: number, H: number): number {
  const byWidth = Math.min(1.45, W / 390);
  if (!(W > 0) || H / W >= FRONT_SCREEN_MAX_ASPECT) return Math.max(0.75, byWidth);
  return Math.max(0.75, Math.min(byWidth, H / SHORT_REF_H) * SHORT_WINDOW_DECK_K);
}

/**
 * ★★★ THE FOLDED FRONT SCREEN, TOLD BY THE WINDOW (iPhone Duo, Stuart 2026-11-25). Decides the one thing that is
 *  ONLY for that screen: the deck's reachability grab bar. Everything else (full bleed, the deck's scale, keeping
 *  clear of the camera) follows general window rules and the system's own reserved regions.
 *
 *  A window that is
 *    • portrait,
 *    • phone-sized (shorter side under TABLET_MIN_SIDE),
 *    • short for its width (height ÷ width under FRONT_SCREEN_MAX_ASPECT — no iPhone is), and
 *    • the WHOLE screen (not a Split View share, whose shape says nothing about how it is held).
 *  No posture or hinge API is needed for this: UIKit's UIHinge (iOS 27.1) reports the hinge, but the question is
 *  "is this window a short, whole, one-hand screen", and the window answers it on every platform and every future
 *  device of that shape — an Android cover screen of the same shape included (its `screen` usually counts the system
 *  bars, so the whole-screen test is stricter there, never looser).
 *  ★ An iPad Stage Manager window of that shape is NOT the whole screen, so it never gets the grab bar.
 */
export function isFoldedFrontWindow(W: number, H: number, screenW: number, screenH: number): boolean {
  return H > W && isFrontScreenWindow(W, H, screenW, screenH);
}

/** The same screen held either way up (the spectrum / waterfall ratio keeps a value for each): a whole-screen phone
 *  window whose long side is under FRONT_SCREEN_MAX_ASPECT × its short side. */
export function isFrontScreenWindow(W: number, H: number, screenW: number, screenH: number): boolean {
  const short = Math.min(W, H), long = Math.max(W, H);
  if (!(short > 0) || short >= TABLET_MIN_SIDE || long / short >= FRONT_SCREEN_MAX_ASPECT) return false;
  const whole = Math.abs(screenW - W) <= 1 && Math.abs(screenH - H) <= 1;
  const wholeTurned = Math.abs(screenW - H) <= 1 && Math.abs(screenH - W) <= 1;
  return whole || wholeTurned;
}

/** How far down a band of the window [x0, x1] must start to clear every occluding region at the top of it (0 = no
 *  region there). Only regions that begin in the top quarter count: this is for header furniture. */
/** Everything our controls and readouts keep out of: the occluding regions and the status bar's frame. */
export function avoidRects(regions: ReservedRegions): RegionRect[] {
  return regions.statusBar.length ? [...regions.occlusion, ...regions.statusBar] : regions.occlusion;
}

export function clearBelow(regions: ReservedRegions, x0: number, x1: number, windowH: number): number {
  let y = 0;
  for (const r of avoidRects(regions)) {
    if (r.y > windowH * 0.25) continue;
    if (r.x + r.w <= x0 || r.x >= x1) continue;
    y = Math.max(y, r.y + r.h);
  }
  return Math.ceil(y);
}

/** The left edge of the first occluding region at the top-right that overlaps the rows [y0, y1] (Infinity = none). */
export function rightLimitAt(regions: ReservedRegions, y0: number, y1: number, windowW: number): number {
  let x = Infinity;
  for (const r of avoidRects(regions)) {
    if (r.y >= y1 || r.y + r.h <= y0) continue;
    if (r.x >= windowW || r.x + r.w <= 0) continue;  // another Split View share's, not this window's
    if (r.w >= windowW * 0.5) continue;           // a full-width bar (a status band) is not a corner
    if (r.x + r.w / 2 < windowW / 2) continue;   // a region on the left half is not this edge's
    x = Math.min(x, r.x);
  }
  return x;
}

/** ★★ How far a bottom-anchored deck LIFTS so no avoid-rect falls on it (Stuart, Duo folded landscape, camera at
 *  the bottom right on the deck's status line: "simply shift the controls upwards to avoid the camera here"). The deck
 *  moves as a block — never inset or reshaped round the camera — just enough to sit `gap` above the highest rect in its
 *  rows [y0, windowH]; the waterfall shows beneath it as it already does. left / right stay 0 (kept for callers). */
export function deckClearance(regions: ReservedRegions, y0: number, windowW: number, windowH: number, gap = 6):
  { left: number; right: number; bottom: number } {
  let bottom = 0;
  for (const r of avoidRects(regions)) {
    if (r.y + r.h <= y0 || r.y >= windowH || r.x >= windowW || r.x + r.w <= 0) continue;
    bottom = Math.max(bottom, windowH - r.y + gap);
  }
  return { left: 0, right: 0, bottom: Math.ceil(bottom) };
}

/**
 * ★★★ WHERE THE RECEIVER'S NAME GOES on a full-bleed window (Stuart, Duo Split View, right-hand share: "name still
 * displays under the system status pill"). Two shapes of system rect cross the name's rows [y0, y0 + rowsH]:
 *   • a CORNER rect (the camera / the folded screen's status column, narrower than half the window): the name's right
 *     edge ends `gap` left of it — `right` is the distance from the window's right edge;
 *   • a full-width STATUS BAND (a Split View share reports its status bar frame across the whole share, with the
 *     clock + Wi-Fi pill drawn somewhere along it — the frame does not say where): the name drops below the band,
 *     `minTop`, and keeps its usual right edge.
 * `crowded` = either applied, and the name then fades after showing (WaterfallView stationIdFadeMs).
 */
export function namePlacement(regions: ReservedRegions, y0: number, rowsH: number, windowW: number,
                              baseRight: number, gap = 8): { right: number; minTop: number; crowded: boolean } {
  let right = baseRight, minTop = 0, crowded = false;
  for (const r of avoidRects(regions)) {
    if (r.y >= y0 + rowsH || r.y + r.h <= y0) continue;
    if (r.x >= windowW || r.x + r.w <= 0) continue;
    if (r.w >= windowW * 0.5) { minTop = Math.max(minTop, Math.ceil(r.y + r.h) + 4); crowded = true; continue; }
    if (r.x + r.w / 2 < windowW / 2) continue;
    right = Math.max(right, Math.ceil(windowW - r.x) + gap);
    crowded = true;
  }
  return { right, minTop, crowded };
}

/**
 * ★★★ HALF-OPEN: THE DECODER BOX AS TWO PANELS (iPhone Duo, Stuart 2026-11-25: "dont cover over the crease. id suggest
 * splitting the data and treat the decoder box as 2 halves"). While the hinge is partially open the box lives in the
 * LEFT column (the deck has the right). BIG is then two separately framed panels in that column:
 *   • UPPER — from the top (below anything the system draws over the column: the status pill and camera are top-RIGHT
 *     on the Duo, so normally the top edge) down to `gap` above the fold;
 *   • LOWER — from `gap` below the fold down to the bottom (above the home indicator).
 * Nothing of ours is on the crease: the waterfall shows through the gap between them. Rects in the column above the
 * fold push the upper panel's top down; rects below it lift the lower panel's bottom (an inner camera, a share's pill).
 * Window coordinates (y down). null = no usable split (either panel under `minH`) — the box then stays below the fold
 * and BIG is not offered, as before.
 */
export interface HalfOpenSplit {
  /** The column's left and right edges (window x). */
  x0: number; x1: number;
  upper: { top: number; bottom: number };
  lower: { top: number; bottom: number };
}
export function halfOpenSplit(regions: ReservedRegions, fold: RegionRect, windowH: number,
                              col: { x0: number; x1: number }, opts: { top: number; bottomInset: number; gap?: number; minH?: number }):
  HalfOpenSplit | null {
  const gap = opts.gap ?? 8, minH = opts.minH ?? 96;
  const foldTop = fold.y, foldBottom = fold.y + fold.h;
  let top = opts.top + gap;
  let bottom = windowH - Math.max(opts.bottomInset, 0) - gap;
  for (const r of avoidRects(regions)) {
    if (r.x + r.w <= col.x0 || r.x >= col.x1 || r.y >= windowH || r.y + r.h <= 0) continue;
    const mid = r.y + r.h / 2;
    if (mid < foldTop) top = Math.max(top, Math.ceil(r.y + r.h) + gap);
    else if (mid > foldBottom) bottom = Math.min(bottom, Math.floor(r.y) - gap);
  }
  const upper = { top, bottom: Math.floor(foldTop) - gap };
  const lower = { top: Math.ceil(foldBottom) + gap, bottom };
  if (upper.bottom - upper.top < minH || lower.bottom - lower.top < minH) return null;
  return { x0: col.x0, x1: col.x1, upper, lower };
}

/**
 * ★★ WHERE THE SERVERS BUTTON GOES (Stuart, Duo folded landscape, camera top-left: "it's avoiding the camera by coming in
 * too far, so just do what we did with the health chips and drop it lower"). Its x is the ordinary left margin, the same
 * in every pose; if a system rect (the camera, the status bar) would meet it there, it DROPS below the rect, `gap`
 * clear — the way the health and time cards drop below the status column. Never shifted sideways.
 * `left` is the caller's margin: 12 on a full-bleed (hinged) window, where the camera comes as a reserved rect, not a
 * safe-area inset; the inset + 8 everywhere else (an iPhone's Dynamic Island side), exactly as before.
 */
export function serversChipPlacement(regions: ReservedRegions, base: { left: number; top: number }, chipW: number, chipH: number,
                                     windowW: number, windowH: number, gap = 8): { left: number; top: number } {
  let top = base.top;
  // Drop past every rect in the chip's column that it would meet (a rect may sit below another: repeat until clear).
  for (let i = 0; i < 4; i++) {
    let moved = false;
    for (const r of avoidRects(regions)) {
      if (r.x >= windowW || r.x + r.w <= 0 || r.y >= windowH) continue;
      if (r.x >= base.left + chipW + gap || r.x + r.w <= base.left - gap) continue;   // not in the chip's column
      if (r.y >= top + chipH + gap || r.y + r.h + gap <= top) continue;               // not in its rows
      top = Math.ceil(r.y + r.h) + gap; moved = true;
    }
    if (!moved) break;
  }
  return { left: base.left, top };
}
