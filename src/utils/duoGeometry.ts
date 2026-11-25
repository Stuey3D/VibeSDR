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

/** The deck's portrait scale for a phone window: width / 390, and no more than height / SHORT_REF_H (useUiScale). */
export function phonePortraitScale(W: number, H: number): number {
  return Math.max(0.75, Math.min(1.45, W / 390, H / SHORT_REF_H));
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
