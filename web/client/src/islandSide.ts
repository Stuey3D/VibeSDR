/**
 * islandSide.ts — ★★★ V12 WHICH SIDE IS THE ISLAND ON, in a browser (Stuart 11-27, iPhone landscape). iOS Safari reports
 * env(safe-area-inset-left) and -right EQUAL in landscape, whichever side the island / notch is on — the problem the app's
 * useIslandSide solves (src/hooks/useIslandSide.ts, edgeChipGeometry islandSideFor). Here the orientation ANGLE says it:
 *
 *   90        rotated counter-clockwise = Apple's landscapeRight (Home button on the right) ⇒ the island on the LEFT
 *   270 / −90 rotated clockwise         = landscapeLeft                                    ⇒ the island on the RIGHT
 *
 * The island side gets the full inset; the other only a rounded-corner allowance. No angle (desktop, Android — whose
 * cutout insets are already asymmetric) or not iOS: the env() values as given. Writes --isl-l / --isl-r on <html>, which
 * the map's --sai-l / --sai-r read (tracks.ts); the radio page can read the same pair when it has its safe-area pass.
 */
import { islandSideFor, type IslandSide } from '../../../src/components/edgeChipGeometry';

/** What a rounded display corner takes from a control on the side without the island (CSS px). The app measures it per
 *  chip (cornerIntrusion); the web has no display radius to measure from, so one allowance for every control. */
export const CORNER_ALLOWANCE_PX = 20;

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
  const st = document.documentElement.style;
  /* ★★ Stuart 17:0x (a real iPhone, island left): the 20 px corner allowance on the other side let the right column run off
   *  the glass. Both sides now take iOS's own env() inset (it reports both) — the side is still known (data-island) for
   *  anything that wants to lean on it; the allowance stays the floor where env() says less. */
  const floor = (k: string) => `max(env(safe-area-inset-${k},0px),${side === 'left' || side === 'right' ? CORNER_ALLOWANCE_PX : 0}px)`;
  if (side === 'left' || side === 'right') { st.setProperty('--isl-l', floor('left')); st.setProperty('--isl-r', floor('right')); }
  else { st.removeProperty('--isl-l'); st.removeProperty('--isl-r'); }
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
