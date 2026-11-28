/**
 * islandSide.ts — ★★★ V12 WHICH SIDE IS THE ISLAND ON, in a browser (Stuart 11-27, iPhone landscape). iOS Safari reports
 * env(safe-area-inset-left) and -right EQUAL in landscape, whichever side the island / notch is on — the problem the app's
 * useIslandSide solves (src/hooks/useIslandSide.ts, edgeChipGeometry islandSideFor). Here the orientation ANGLE says it:
 *
 *   90        rotated counter-clockwise = Apple's landscapeRight (Home button on the right) ⇒ the island on the LEFT
 *   270 / −90 rotated clockwise         = landscapeLeft                                    ⇒ the island on the RIGHT
 *
 * ★ Since Stuart's SAFE RECTANGLE (11-27 evening) no layout value depends on the side: the map's chrome stands inside the
 * insets as iOS reports them. The side is kept as <html data-island> for ?perf=1 (window.__mapGeom) and the app port.
 */
import { islandSideFor, type IslandSide } from '../../../src/components/edgeChipGeometry';

function isIos(): boolean {
  const n = navigator as any;
  return /iPhone|iPad|iPod/.test(n.userAgent || '') || (n.platform === 'MacIntel' && n.maxTouchPoints > 1);
}
export function islandSideWeb(): IslandSide | null {
  if (typeof window === 'undefined' || !isIos()) return null;
  let a: number | null = null;
  const so = (screen as any).orientation;
  if (so && typeof so.angle === 'number') a = so.angle;
  else if (typeof (window as any).orientation === 'number') a = (window as any).orientation;
  if (a === null) return null;
  a = ((a % 360) + 360) % 360;
  return islandSideFor(a === 90 ? 'landscapeRight' : a === 270 ? 'landscapeLeft' : 'portrait');
}
let wired = false;
export function applyIslandSide() {
  const side = islandSideWeb();
  document.documentElement.dataset.island = side ?? '';
}
/** Once: now, and again on every rotation (the 180° flip moves neither the window nor the insets — orientationchange sees it). */
export function wireIslandSide() {
  if (wired || typeof window === 'undefined') return;
  wired = true;
  applyIslandSide();
  const again = () => { applyIslandSide(); setTimeout(applyIslandSide, 300); };
  window.addEventListener('orientationchange', again);
  window.addEventListener('resize', again);
  try { (screen as any).orientation?.addEventListener?.('change', again); } catch { /* old Safari */ }
}
