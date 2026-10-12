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
 *    glass tokens in (screens/ServerModeThemed.tsx).
 * ★★★ LITE LOOKS EXACTLY AS BEFORE (Stuart, 2026-10-13: VibeServer Lite has no colour UI and must not move). With no
 *    tokens the palette is LITE — every role the literal the screen and its editors drew before 034143e6, role for role.
 *    Where the rule merged two old colours into one role (green selector cards vs amber pills, the three amber fills,
 *    gold vs dim placeholders …) the role is SPLIT, so Lite carries both and the main app gives both the same token.
 *    scripts/test_server_colours.ts holds every Lite value to its old literal, with the old line numbers.
 */

import { popupTokensFor, type PopupTokens } from './popupTokens.ts';
import { meaningsFor, AMBER_TEXT_RGB } from './decoderTokens.ts';

/** The amber tokens the test sweeps as "Lite-like" contrast; ★ NOT what Lite draws — Lite draws LITE below. */
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
    textMuted:   pt.ink.textDim,   // a quiet value: an idle telemetry row, the share's end time (old gold-dim)
    // ★ A hint INSIDE a selected card sits on the key's fill: .80 measured 3.9:1 there in blue / red text, so a step up.
    textOnSel:   pt.ink.textSoft,
    // ★ textDim, not textFaint: a faint placeholder in blue or red text measured under 4.5:1 (test_server_colours).
    placeholder:    pt.ink.textDim,
    placeholderDim: pt.ink.textDim,   // (Lite: the dimmer of its two placeholder colours)
    // ── KEYS (the controls colour) ──
    key:          pt.ink.key,       // a key's legend at rest
    keyQuiet:     pt.ink.key,       // a quieter key at rest: unselected chips, SHOW / HIDE, the log and update links
    keyDim:       pt.gold.close,    // a key's secondary legend (the protocol card's tag)
    keyRemove:    pt.gold.close,    // the × on a set band / limit chip
    keyAct:       pt.gold.sel,      // the selected / on key's legend, ✓ (pills, option rows, toggles)
    keySel:       pt.gold.sel,      // the selected CARD's legend (Simple / Advanced, radio use, limits, editor chips)
    keyBorder:    pt.ink.keyBorder,
    keyBorderAct: pt.gold.selBorder,
    keySelBorder: pt.gold.selBorder,
    keyBgAct:     pt.ink.keyBgAct,  // a selected pill's fill
    keyBgCard:    pt.ink.keyBgAct,  // the selected protocol card's fill
    keyBgStart:   pt.ink.keyBgAct,  // the RTL-TCP start key's fill
    keySelBg:     pt.ink.keyBgAct,  // a selected card's fill
    border:       pt.ink.border,    // card and input edges
    borderBright: pt.gold.selBorder,
    trackOff:     pt.ink.border,
    trackOn:      pt.gold.selBorder,
    thumb:        pt.gold.thumb,
    fill:         pt.gold.fill,     // the measuring bar
    sliderFill:   pt.gold.fill,     // a slider's filled track (the band editor)
    sliderTrack:  pt.ink.border,    // a slider's empty track
    checkFill:    pt.gold.fill,     // the band editor's lock tick-box, ticked
    // ── MEANINGS (fixed; clash-ruled where the text colour could match) ──
    good: m.good, gradeGood: m.good,
    warn: m.warn, warnBorder: m.warn, gradeWarn: m.warn,
    bad: m.bad, badPublish: m.bad, badSoft: m.bad, m,
    selected: pt.ink.selected, selectedBg: pt.ink.selectedBg,
    ok: SERVING_GREEN, okBg: 'rgba(40,167,69,0.094)',
    danger: STOP_RED, dangerBg: 'rgba(224,80,80,0.094)',
  };
}

export type ServerPalette = ReturnType<typeof build>;

/* ★★★ VibeServer Lite — the literals of theme.ts `Colors` as the screen drew them before 034143e6 (amber / gold #ffb833,
 *   goldDim rgba(255,184,51,.55), textDim rgba(255,160,0,.45), border rgba(255,160,0,.22), borderBright .55, green
 *   #28A745, red #E05050) plus the file's own literals. The `C.x + '18'` fills are written as the 8-digit hex they made. */
const AMBER = '#ffb833', GOLD_DIM = 'rgba(255,184,51,0.55)', DIM = 'rgba(255,160,0,0.45)', GREEN = SERVING_GREEN, RED = STOP_RED;
export const LITE: ServerPalette = {
  bg: SERVER_BG,
  text: AMBER, heading: AMBER, section: DIM, textDim: DIM, textMuted: GOLD_DIM, textOnSel: DIM,
  placeholder: GOLD_DIM, placeholderDim: DIM,
  key: AMBER, keyQuiet: DIM, keyDim: GOLD_DIM, keyRemove: DIM,
  keyAct: AMBER, keySel: GREEN,
  keyBorder: DIM, keyBorderAct: AMBER, keySelBorder: GREEN,
  keyBgAct: AMBER + '22', keyBgCard: AMBER + '14', keyBgStart: AMBER + '18', keySelBg: GREEN + '18',
  border: 'rgba(255,160,0,0.22)', borderBright: 'rgba(255,160,0,0.55)',
  trackOff: 'rgba(255,160,0,0.22)', trackOn: GREEN, thumb: AMBER,
  fill: GREEN, sliderFill: AMBER, sliderTrack: 'rgba(255,255,255,0.25)', checkFill: GREEN + '55',
  good: GREEN, gradeGood: '#7bd88f',
  warn: AMBER, warnBorder: GOLD_DIM, gradeWarn: '#ffcc66',
  bad: RED, badPublish: '#ff5c5c', badSoft: '#ff8a7d',
  // ★ Amber text's meanings: badBold false, so meaningText never bolds anything in Lite (it never did).
  m: meaningsFor(AMBER_TEXT_RGB),
  selected: GREEN, selectedBg: GREEN + '18',
  ok: GREEN, okBg: GREEN + '18', danger: RED, dangerBg: RED + '18',
};

const cache = new WeakMap<PopupTokens, ServerPalette>();
/** The palette for a token set (the main app — the user's colours), or LITE with none (VibeServer Lite). */
export function serverPalette(pt?: PopupTokens): ServerPalette {
  if (!pt) return LITE;
  let p = cache.get(pt);
  if (!p) { p = build(pt); cache.set(pt, p); }
  return p;
}
