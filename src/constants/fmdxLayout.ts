/**
 * ★★★ THE FM-DX SCREEN GROWS INTO A BIG WINDOW (Stuart, 2026-10-04, the iPad build full screen on the Mac:
 * "loads of dead space on FM-DX … make the dial and the album art much bigger, as long as it reflows when
 * the app is shrunk"). ~550 pt sat empty between the transmitter card and the control deck.
 *
 * Sized from the WINDOW — the scroll area's own measured width and height — never the device
 * (layout follows the window: iPhone ↔ iPad ↔ Mac, split view, foldables). Pure, so the maths is tested
 * (scripts/test_fmdx_layout.ts) without a renderer.
 *
 * The rule:
 *   • A PHONE-SIZED WINDOW GETS TODAY'S LAYOUT EXACTLY: dial 158, logo box 68, every scale factor 1.
 *     Growth is gated on width — nothing below WIDE_FROM pt of content width, the full share by WIDE_FULL —
 *     so no phone in portrait grows, however tall; and a landscape phone has no spare height, so it doesn't
 *     either.
 *   • The spare height is what is left of the scroll area after today's dial + station card + an allowance
 *     for the cards that follow (band chip, transmitter, AF). Only that SPARE is handed out, so the cards
 *     below are never pushed off the bottom by the growth itself.
 *   • CONTINUOUS — every size is a smooth function of the window, so a resize drags it gently; there is no
 *     threshold to flap across, and nothing to hysterese.
 *   • Capped: a 2000 pt Mac window does not get a 600 pt dial. The logo is also capped by width so the PI
 *     and the TP · TA · AF columns either side of it keep room.
 */

export const DIAL_H_COMPACT = 158;
export const DIAL_H_MAX = 360;
export const LOGO_BOX_COMPACT = 68;
export const LOGO_BOX_MAX = 240;
/** The station card at compact size: the 68 pt logo box + 14 pt padding top and bottom. */
export const ID_CARD_COMPACT = LOGO_BOX_COMPACT + 28;
/** What the cards after the station card can take: band chip (~28 + gap), transmitter (~85 + gap),
 *  AF chips (~90 + gap), the 12 pt gaps between them. Generous on purpose — growth only uses what is
 *  left after these, so the full screen still fits without scrolling when they are all showing. */
export const BELOW_RESERVE = 260;
/** Content width (inside the scroll padding) where growth starts and where it reaches its full share.
 *  A 440 pt Pro Max in portrait is ~412 of content; an SE in Display Zoom is 292. */
export const WIDE_FROM = 500;
export const WIDE_FULL = 800;
/** The text scale the dial's labels reach at most (9 pt names → ~16, 10 pt MHz → ~18). */
export const DIAL_FONT_MAX = 1.8;
/** The logo may take at most this share of the card's width. */
const LOGO_W_SHARE = 0.34;

export interface FmdxLayout {
  /** FmdxDial height. */
  dialH: number;
  /** Dial typography/geometry scale (1 = today). */
  dialScale: number;
  /** The station card's logo box (square); the image inside is box − 4. */
  logoBox: number;
  /** Station-card text scale (PI, RDS mark, TP/TA/AF legends) — 1 = today. */
  cardScale: number;
}

export const COMPACT: FmdxLayout = { dialH: DIAL_H_COMPACT, dialScale: 1, logoBox: LOGO_BOX_COMPACT, cardScale: 1 };

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

/**
 * @param contentW the scroll content's width (window less insets and the 14 pt side padding)
 * @param availH   the scroll area's height less its top padding and the bottom padding that clears the
 *                 VTS strip and the control island — i.e. what the cards can occupy without scrolling
 */
export function fmdxLayout(contentW: number, availH: number): FmdxLayout {
  if (!(contentW > 0) || !(availH > 0)) return COMPACT;
  const wide = clamp((contentW - WIDE_FROM) / (WIDE_FULL - WIDE_FROM), 0, 1);
  const spare = Math.max(0, availH - (DIAL_H_COMPACT + 12 + ID_CARD_COMPACT + 12 + BELOW_RESERVE));
  const budget = spare * wide;
  if (budget < 1) return COMPACT;

  // The logo's own ceiling at this width — the PI and TP · TA · AF columns keep their share.
  const logoCap = clamp(Math.floor(contentW * LOGO_W_SHARE), LOGO_BOX_COMPACT, LOGO_BOX_MAX);
  // Split the budget: the dial takes a little over half (its labels are the thing you read); whatever one
  // side cannot use because it hit its cap goes to the other.
  let dialGrow = Math.min(DIAL_H_MAX - DIAL_H_COMPACT, budget * 0.55);
  let logoGrow = Math.min(logoCap - LOGO_BOX_COMPACT, budget - dialGrow);
  dialGrow = Math.min(DIAL_H_MAX - DIAL_H_COMPACT, budget - logoGrow);

  const dialH = Math.round(DIAL_H_COMPACT + dialGrow);
  const logoBox = Math.round(LOGO_BOX_COMPACT + logoGrow);
  const dialScale = Math.round(clamp(dialH / DIAL_H_COMPACT, 1, DIAL_FONT_MAX) * 100) / 100;
  // The card's text grows more gently than the logo it sits beside (68 → 240 = 3.5×; text to 1.6×).
  const cardScale = Math.round(clamp(1 + (logoBox - LOGO_BOX_COMPACT) / (LOGO_BOX_MAX - LOGO_BOX_COMPACT) * 0.6, 1, 1.6) * 100) / 100;
  return { dialH, dialScale, logoBox, cardScale };
}
