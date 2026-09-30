/**
 * PopupShell — popups, menus and chat take the chassis (faceplates brief §10.3).
 *
 * > "Popup boxes can they be styled to the controls please same with menus too … be weird having a
 * >  light control scheme with black popups … and chat menu too." — Stuart
 *
 * Step one (this file's first cut): the TOKENS. `usePopupTheme()` is the popups' palette, resolved
 * from the live faceplate (src/constants/popupTokens.ts); `usePopupStyles(make)` builds a popup's
 * style sheet from it once per setting. The default chassis returns today's literals, so every
 * popup renders exactly as it did.
 */

import { useFaceplate } from '../contexts/FaceplateContext';
import { popupTokensFor, type PopupTokens } from '../constants/popupTokens';

export type { PopupTokens } from '../constants/popupTokens';

/** The popups' palette — live: follows the chassis, the controls colour and the resolved text
 *  colour (neon under Nixie). Components never hold their own gold. */
export function usePopupTheme(): PopupTokens {
  const fp = useFaceplate();
  return popupTokensFor(fp.settings.chassis, fp.controls.rgb, fp.text.rgb, fp.settings.display);
}

const styleCache = new WeakMap<(pt: PopupTokens) => unknown, WeakMap<PopupTokens, unknown>>();

/**
 * ★ A popup's style sheet, built from the LIVE tokens — once per setting, then shared (the same
 * pattern as DecoderShell's useDecoderStyles). `make` must be a module-level function: it is the
 * cache key. Token objects are memoised per setting, so this is a lookup on every render and a build
 * only when a setting changes.
 */
export function usePopupStyles<S>(make: (pt: PopupTokens) => S): S {
  const pt = usePopupTheme();
  let per = styleCache.get(make) as WeakMap<PopupTokens, S> | undefined;
  if (!per) { per = new WeakMap(); styleCache.set(make, per as WeakMap<PopupTokens, unknown>); }
  let s = per.get(pt);
  if (!s) { s = make(pt); per.set(pt, s); }
  return s;
}
