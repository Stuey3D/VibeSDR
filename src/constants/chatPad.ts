/**
 * chatPad.ts — how much of the chat drawer the canned PHRASE PAD may take (ChatDrawer, shared dial).
 * Pure: no React, no React Native, so scripts/test_chat_pad.ts checks it on every phone size.
 *
 * ★★★ FOURTEEN PHRASES DO NOT FIT A PHONE'S DRAWER, and nothing said so. The pad was a flowing wrap
 *   laid straight into a fixed-height drawer: on a 320 pt phone (the SE in Display Zoom, and the
 *   bladeL8 emulator) the drawer is 313 pt, the wrap is ~300 pt, so the transcript was squeezed to
 *   NOTHING and phrases 7–14 ("I'm running a decoder…" down to "Sorry, didn't realise!") were laid
 *   out below the drawer's bottom edge — drawn off-screen, under the nav bar, and impossible to tap.
 *   Same on every chassis; the faceplates only made it visible (2026-10-01).
 * ★★ So the pad SCROLLS, inside a cap, and keeps its look: the same chips in the same flowing wrap,
 *   only now inside a ScrollView no taller than this. The transcript keeps the rest — it is half of
 *   the conversation, and a phrase you cannot see the answer to is a phrase shouted into a void.
 * ★ The cap is on the drawer's MEASURED body (below the header), so it is right in landscape, on a
 *   tablet and in a resized Mac window without a table of devices.
 */

/** One row of chips: a dome key is 32 pt tall (a default pill ~35), plus the 6 pt gap. */
export const PAD_ROW = 38;
/** Never smaller than one row and a half — the half row is what says "there is more, scroll". */
export const PAD_MIN = Math.round(PAD_ROW * 1.5);
/** The transcript keeps at least three lines of chat (18 pt each) and the window's padding. */
export const THREAD_MIN = 64;
/** The pad never takes more than this share of the body, however tall the drawer. */
export const PAD_SHARE = 0.6;

/**
 * The phrase pad's scroller max height, in points.
 * @param bodyH   the drawer's height below its header (transcript + room line + pad), measured
 * @param lineH   the room line's height above the chips (0 when there is none)
 * @param chrome  the pad's own vertical padding (inputRow paddingVertical × 2)
 */
export function phrasePadMaxHeight(bodyH: number, lineH = 0, chrome = 16): number {
  const room = Math.max(0, bodyH - lineH - chrome);
  const leaveThread = room - THREAD_MIN;
  const share = Math.round(room * PAD_SHARE);
  return Math.max(PAD_MIN, Math.min(share, leaveThread));
}
