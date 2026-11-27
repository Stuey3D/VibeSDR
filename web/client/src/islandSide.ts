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
  /* ★★★ Stuart 17:44, the APP's rule (EdgeChip.tsx RC16): the island side takes iOS's inset in full; the OTHER side only the
   *  rounded corner's allowance, and only for a piece IN a corner — a tab in the middle of that edge sits on the glass.
   *  --isl-le / --isl-re: an edge tab; --isl-lc / --isl-rc: a corner piece (tracks.ts --sai-le / -re / -lc / -rc). */
  for (const k of ['--isl-l', '--isl-r', '--isl-le', '--isl-re', '--isl-lc', '--isl-rc']) st.removeProperty(k);
  const corner = (k: string) => `min(env(safe-area-inset-${k},0px),${CORNER_ALLOWANCE_PX}px)`;
  if (side === 'left') { st.setProperty('--isl-re', '0px'); st.setProperty('--isl-rc', corner('right')); }
  else if (side === 'right') { st.setProperty('--isl-le', '0px'); st.setProperty('--isl-lc', corner('left')); }
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
