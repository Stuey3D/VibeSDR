/**
 * useUiScale — port of computeUiScale() + applyUiScale() from Scalable_Mobile_UI_v6_3_1.html
 *
 * Original JS (lines 4263–4384):
 *   function computeUiScale() {
 *     var W = window.innerWidth, H = window.innerHeight;
 *     if (W > H) return Math.max(0.58, Math.min(1.45, W / 926));   // landscape
 *     return Math.max(0.75, Math.min(1.45, W / 390));               // portrait
 *   }
 *
 * Portrait scale examples:
 *   320dp (SE Display Zoom) → 0.82
 *   375dp (SE3 normal)      → 0.96
 *   390dp (iPhone 14)       → 1.00  ← reference
 *   430dp (14 Plus)         → 1.10
 *
 * Landscape scale examples:
 *   568dp (SE Display Zoom) → 0.61
 *   667dp (SE3)             → 0.72
 *   844dp (iPhone 14)       → 0.91
 *   926dp (reference)       → 1.00
 *
 * isSmall: W <= 415 — triggers portrait-sm layout (skin line 4297)
 * isTiny:  W <= 330 — Display Zoom SE absolute minimum
 *
 * r(n): Math.round(n * scale) — use for all layout dp values
 * f(n): n * scale             — use for fontSize (React Native accepts floats)
 */

import { useWindowDimensions } from 'react-native';
import { useMemo } from 'react';

export interface UiScale {
  scale:       number;
  r:           (n: number) => number;
  f:           (n: number) => number;
  isPortrait:  boolean;
  isLandscape: boolean;
  isSmall:     boolean;   // W <= 415
  isTiny:      boolean;   // W <= 330
  isTablet:    boolean;   // shortest side >= TABLET_MIN_SIDE (600: Android sw600dp, every iPad)
  W:           number;
  H:           number;
  drumW:       number;    // scaled BASE_LSV_DW (140)
  drumH:       number;    // scaled drum height (60 portrait / 44 landscape)
  pxStep:      number;    // scaled BASE_LSV_PX_STEP (22)
}

export function useUiScale(): UiScale {
  const { width: W, height: H } = useWindowDimensions();

  return useMemo(() => {
    const isPortrait  = H >= W;
    const isLandscape = !isPortrait;

    // Direct port of computeUiScale()
    /* ★★ A TABLET HELD UPRIGHT KEEPS ITS LANDSCAPE SIZE (Stuart, 2026-10-11, the Galaxy Tab A9: "look at portrait they
     *  are massive and feels like half of the screen is wasted"). The phone rule — width over 390 — hit its 1.45 cap on
     *  a 601 pt tablet and drew a phone's column half again as big. A tablet's controls are thumb-sized, the same size
     *  whichever way it is held: upright it takes the scale its LONG side gives in landscape. Phones are unchanged. */
    const tablet = Math.min(W, H) >= TABLET_MIN_SIDE;
    const scale = isLandscape || tablet
      ? Math.max(0.58, Math.min(1.45, Math.max(W, H) / 926))
      : Math.max(0.75, Math.min(1.45, W / 390));

    const r = (n: number) => Math.round(n * scale);
    const f = (n: number) => n * scale;

    const isSmall = isPortrait && W <= 415;
    const isTiny  = W <= 330;
    const isTablet = Math.min(W, H) >= TABLET_MIN_SIDE;

    const drumW  = r(140);
    const drumH  = isPortrait ? r(60) : r(44);
    const pxStep = r(22);

    return { scale, r, f, isPortrait, isLandscape, isSmall, isTiny, isTablet, W, H, drumW, drumH, pxStep };
  }, [W, H]);
}

/** ★★ THE TABLET LINE: a window whose SHORTEST side is at least this (pt) is a tablet (Android's sw600dp). It was 768
 *  (the iPad's), so a 601 pt Galaxy Tab A9, an open foldable and an iPad mini (744) were treated as PHONES and got the
 *  phone column scaled up 1.45× — "as if someone had taken a phone and made it 8 inches" (Stuart, 2026-10-11).
 *  ONE number for every reader (useUiScale, SDRScreen, MenuSheet). */
export const TABLET_MIN_SIDE = 600;
