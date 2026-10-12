/**
 * serverPalette.ts — "Use as server" (ServerModeScreen) and the editors only it draws, BY ROLE.
 *
 * ★★★ STUART'S COLOUR RULE (2026-10-12), as in the decoder boxes, menus, pop-ups and the directory:
 *   "Follow the text colour except for when the colours have a meaning. So blue buttons and white text …
 *    Green for the selected station, amber and red for other status updates." — "yes colours apply everywhere".
 *   • TEXT — titles, values, typed text, switch labels, hints, row names, section headings, placeholders: the
 *     user's TEXT colour (`text` / `heading` / `section` / `textDim` / `placeholder`);
 *   • KEYS — every tappable legend (pills, option rows, selector cards, − / +, ↻, Default, DOWNLOAD NOW, Cancel),
 *     their borders and the selected one's fill, the switches' tracks and thumbs, the measuring bar: the
 *     CONTROLS colour (`key*`, `border`, `track*`, `thumb`, `fill`);
 *   • MEANINGS stay fixed, through the ONE clash helper (decoderTokens `meaningsFor`, via PopupTokens `ink.m`):
 *       `good` / `warn` / `bad` — the measurement grades, the PIN / open notice, "listen at …", the admin-password
 *       advice, the DAB landing warnings, every error (bad is WHITE BOLD under red text — use meaningText);
 *       `selected` / `selectedBg` — the chosen DAB LANDING STATION ("green for the selected station"; white under
 *       green / teal text, selectedFor);
 *       `ok` / `danger` — the server's own state on its keys: START and "server remains active" are the
 *       directory's SERVING green (theme.ts Colors.green), the STOP keys red (Colors.red). Kept as those literals
 *       so the green a host sees here is the green the directory shows for the same server.
 *   • KEPT: the near-black background (Colors.bg).
 *
 * ★ Pure (type imports + popupTokens.ts), so Node runs scripts/test_server_colours.ts straight from this file.
 * ★★ NO FACEPLATE IMPORT HERE OR IN ServerModeScreen: VibeServer Lite bundles ServerModeScreen on RN 0.73 with no
 *    Reanimated / Skia, so PopupShell / FaceplateContext cannot be in its import graph. The main app passes the
 *    glass tokens in (screens/ServerModeThemed.tsx); Lite, which has no colour settings, gets LITE_TOKENS.
 */

import { popupTokensFor, type PopupTokens } from './popupTokens.ts';
import { AMBER_TEXT_RGB } from './decoderTokens.ts';

/** VibeServer Lite (no faceplate, no colour settings): amber keys and amber text — the screen's old look, by the rule. */
export const LITE_TOKENS: PopupTokens = popupTokensFor('default', AMBER_TEXT_RGB, AMBER_TEXT_RGB, 'hyper');

/** theme.ts Colors.bg / green / red — not imported so this stays a two-file pure module (the test checks they agree). */
export const SERVER_BG    = '#080601';
export const SERVING_GREEN = '#28A745';
export const STOP_RED      = '#E05050';

function build(pt: PopupTokens) {
  const m = pt.ink.m;
  return {
    bg: SERVER_BG,
    // ── TEXT (the text colour) ──
    text:        pt.ink.text,      // titles, values, typed text, switch labels
    heading:     pt.ink.text,      // SERVER SETTINGS / RADIO SETTINGS (were the brighter gold headings)
    section:     pt.ink.section,   // section headings
    textDim:     pt.ink.textDim,   // hints, row names, sub-lines
    // ★ A hint INSIDE a selected card sits on the key's fill: .80 measured 3.9:1 there in blue / red text, so a step up.
    textOnSel:   pt.ink.textSoft,
    // ★ textDim, not textFaint: a faint placeholder in blue or red text measured under 4.5:1 (test_server_colours).
    placeholder: pt.ink.textDim,
    // ── KEYS (the controls colour) ──
    key:          pt.ink.key,       // a key's legend at rest
    keyDim:       pt.gold.close,    // a key's secondary legend (the protocol card's tag)
    keyAct:       pt.gold.sel,      // the selected / on key's legend, ✓
    keyBorder:    pt.ink.keyBorder,
    keyBorderAct: pt.gold.selBorder,
    keyBgAct:     pt.ink.keyBgAct,
    border:       pt.ink.border,    // card and input edges
    borderBright: pt.gold.selBorder,
    trackOff:     pt.ink.border,
    trackOn:      pt.gold.selBorder,
    thumb:        pt.gold.thumb,
    fill:         pt.gold.fill,
    // ── MEANINGS (fixed; clash-ruled where the text colour could match) ──
    good: m.good, warn: m.warn, bad: m.bad, m,
    selected: pt.ink.selected, selectedBg: pt.ink.selectedBg,
    ok: SERVING_GREEN, okBg: 'rgba(40,167,69,0.094)',
    danger: STOP_RED, dangerBg: 'rgba(224,80,80,0.094)',
  };
}

export type ServerPalette = ReturnType<typeof build>;

const cache = new WeakMap<PopupTokens, ServerPalette>();
/** The palette for a token set — memoised on the (already memoised) tokens object. */
export function serverPalette(pt: PopupTokens = LITE_TOKENS): ServerPalette {
  let p = cache.get(pt);
  if (!p) { p = build(pt); cache.set(pt, p); }
  return p;
}
