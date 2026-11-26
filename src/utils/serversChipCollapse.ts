/**
 * ★★ THE SERVERS BUTTON TUCKS AWAY WHEN SOMETHING WOULD COVER IT (Stuart, 2026-11-25: "I think the servers button has
 * always clipped, especially on phones. Wonder if it's worth making it collapsible too" — "in waterfall mode it's there
 * fully"; "it will help the SE massively"). When a decoder box, the DAB box or the Advanced RDS box would overlap the
 * full "‹ Servers" chip, the chip becomes its icon-only edge tab (the health / time tabs' language, mirrored on the
 * left edge); over a plain waterfall it is the full button, as it always was.
 * ★ Judged against the FULL chip's rect, never the tab's — or tucking it would clear the overlap and bring it straight
 *   back. SDRScreen adds the time hysteresis (tuck at once, come back after a quiet moment), so a box that resizes
 *   (BIG / SMALL, a list growing) never makes it flicker. Pure, for scripts/test_servers_chip.ts.
 */
export interface ChipRect { x: number; y: number; w: number; h: number }

/** True when any overlay rect comes within `gap` of the chip's rect (window coordinates). 12 pt: the chip's soft scrim
 *  reaches ~14 pt past it, so a box closer than that is crowding it already (the SE's SMALL box, which starts at the
 *  server-name row 8 pt under the chip — Stuart: "it will help the SE massively"). */
export function chipCollapsed(chip: ChipRect, overlays: ChipRect[], gap = 12): boolean {
  if (chip.w <= 0 || chip.h <= 0) return false;
  for (const o of overlays) {
    if (o.w <= 0 || o.h <= 0) continue;
    if (o.x < chip.x + chip.w + gap && chip.x - gap < o.x + o.w && o.y < chip.y + chip.h + gap && chip.y - gap < o.y + o.h) return true;
  }
  return false;
}

/** How long nothing must overlap before the full button comes back (ms). */
export const CHIP_EXPAND_DELAY_MS = 400;

/** ★★ LAYERS (Stuart, iPhone 17 Pro Max, DAB BIG: "the BIG DAB box COMPLETELY covers the Servers button" — the chip
 *  showed faded under the station list and could not be reached). A decoder box's wrap sits at DECODER_BOX_Z; the
 *  tucked tab — and the chip's whole layer while tucked — at SERVERS_TAB_Z, above it, with the edge tabs, so the tab is
 *  drawn over the box and takes its own taps; the box's rows still scroll everywhere else. */
export const DECODER_BOX_Z = 200;
export const SERVERS_TAB_Z = 251;
/** ★★ THE SERVERS MENU IS THE TOP LAYER (Stuart: "when collapsed, when pressed it opens the menu as the top layer above
 *  all, as that is how we manage multiple servers too"). Open — from the full button or the tab, the same menu — the
 *  chip's whole layer sits above every box, the edge cards and tabs (EdgeChip 250 / 251), the notices (≤ 240), the
 *  deck (no layer), the chat layer (300) and the crash card (999); below the tour (9999).
 *  scripts/check-servers-menu-layer.mjs holds it against every layer in the app. */
export const SERVERS_MENU_Z = 9000;
/** Every other layer on the SDR screen the menu must clear (boxes, EdgeChip card / tab, banners, notices, chat, crash card). */
export const SDR_SCREEN_LAYERS_BELOW_MENU = [DECODER_BOX_Z, SERVERS_TAB_Z, 250, 240, 215, 210, 300, 999];
