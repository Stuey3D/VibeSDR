/**
 * capSheen.ts — the layout of a popup / decoder dome key's SHEEN (the light over the top 45 % of the
 * cap and the shade over its foot 35 %). Pure: plain style objects, no React Native import, so
 * scripts/test_chat_pad.ts can check them and both shells spread the same thing.
 *
 * ★★★ THE GHOST SLABS (Stuart, 2026-10-01: "the canned chat box is a complete broken mess in the new
 *   skins … overlapping squares behind them which are the touch targets"). The two layers were
 *   `position: 'absolute', height: '45%'` / `bottom: 0, height: '35%'`. Yoga resolves an absolute
 *   child's PERCENTAGE height against the wrong box when its key sits in a MULTI-LINE flex-wrap row —
 *   it used the wrapping container's height, not the cap's. A one-line row (the cog menu's MIN / ZOOM
 *   / MAX) is as tall as its key, so nothing showed; the chat drawer's phrase pad is eight lines tall
 *   (~290 pt), so every chip drew a cap-wide slab ~130 pt down from its top and ~100 pt UP from its
 *   foot, over the header and over the neighbouring chips. Measured on the bladeL8 emulator: the
 *   accessibility bounds (clipped to the cap) were right while the paint spilled; inset-pinned layers
 *   in the same cap (the 1 pt top line, the pressed layer) were right all along.
 *   The slabs are pointerEvents="none", so a tap through one landed on whatever chip was under it —
 *   which is exactly what "the touch targets are all over the place" looked like.
 * ★★ THE CURE IS NO PERCENTAGES: one layer pinned to the cap by its four INSETS (which Yoga resolves
 *   against the cap, always), holding a column of three flex SHARES — 45 light, 20 nothing, 35 shade.
 *   Same proportions on every key, in any row, at any height.
 */

export const SHEEN_HI = 45;
export const SHEEN_MID = 20;
export const SHEEN_LO = 35;

/** The sheen's styles for a cap of corner radius `r` + 1 (the layers sit 1 pt inside the border). */
export function CAP_SHEEN(r: number) {
  return {
    capSheen: { position: 'absolute' as const, left: 0, right: 0, top: 0, bottom: 0, flexDirection: 'column' as const },
    capHi:    { flex: SHEEN_HI, borderTopLeftRadius: r, borderTopRightRadius: r },
    capMid:   { flex: SHEEN_MID },
    capLo:    { flex: SHEEN_LO, borderBottomLeftRadius: r, borderBottomRightRadius: r },
  };
}
