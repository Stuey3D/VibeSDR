/**
 * The frequency card's TUNE pane as the highlight sees it — in the order it is DRAWN (FreqModal), tested by
 * scripts/test_nav_move.ts.
 *
 * ★★ Stuart, V12 build 485: "in the app no way of selecting the frequency box when the frequency and bookmarks entry is
 *   on screen with the keyboard". The pane was one flat list in the order the code happened to build it — tabs, the
 *   three units, THEN the field — while the screen draws the field ABOVE the units. So Down from the tabs went to Hz,
 *   Up from the units went back to the tabs, and the frequency box, sitting between the two, was never where an arrow
 *   went: it hid at the far end, after MHz, reachable only by pressing Down past the last unit — which moved the
 *   highlight UP the screen. Now each line on screen is a row:
 *
 *     row 0   TUNE  BOOKMARKS              (when the card has bookmarks)
 *     row 1   ◄  station  ►                (the nearby-station skip, where the screen shows it)
 *     row 2   [ frequency box ]
 *     row 3   Hz  kHz  MHz                 (only MHz on a receiver that locks the unit)
 *     row 4   profile                      (when the receiver has profiles)
 *
 *   Up / Down step a line, Left / Right walk along one (src/utils/navMove.ts). Opened from the keyboard, the
 *   highlight starts ON the frequency box (`start`), so Enter puts the cursor straight in it.
 */
export type TuneCardKey =
  | 'tab:tune' | 'tab:bookmarks' | 'vts:prev' | 'vts:next' | 'field'
  | 'unit:hz' | 'unit:khz' | 'unit:mhz' | 'profile';

export interface TuneCardItem { k: TuneCardKey; row: number }

export function tuneCardItems(o: { tabs: boolean; vts: boolean; lockUnit: boolean; profiles: boolean }):
    { items: TuneCardItem[]; start: number } {
  const items: TuneCardItem[] = [];
  if (o.tabs) items.push({ k: 'tab:tune', row: 0 }, { k: 'tab:bookmarks', row: 0 });
  if (o.vts) items.push({ k: 'vts:prev', row: 1 }, { k: 'vts:next', row: 1 });
  items.push({ k: 'field', row: 2 });
  for (const u of ['hz', 'khz', 'mhz'] as const) {
    if (!o.lockUnit || u === 'mhz') items.push({ k: `unit:${u}` as TuneCardKey, row: 3 });
  }
  if (o.profiles) items.push({ k: 'profile', row: 4 });
  return { items, start: items.findIndex((x) => x.k === 'field') };
}
