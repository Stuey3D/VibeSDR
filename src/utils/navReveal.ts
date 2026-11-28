/**
 * navReveal.ts — ★★★ WHERE A SCROLL VIEW GOES TO SHOW THE HIGHLIGHTED ITEM (PanelNav revealIn; beta97, Stuart on build 586:
 * "moving the arrow down on the list goes to the lower lines but doesn't scroll"). Pure, so scripts/test_map_chrome.ts proves
 * it: `rel` = the item's distance from the viewport's top, `h` its height, `viewH` the viewport, `scrollY` the offset now.
 * Already fully visible → null (no jump); else CENTRED (from across a room, a row nudged just inside the edge is easy to
 * lose) — which leaves at least a row's margin either side of it whenever the viewport has room for three.
 */
export function revealScrollTarget(rel: number, h: number, viewH: number, scrollY: number): number | null {
  if (!(viewH > 0)) return null;
  if (rel >= 0 && rel + h <= viewH) return null;
  return Math.max(0, scrollY + rel - (viewH - h) / 2);
}
