/**
 * The server list's focus rules — the pure part (controller audit group H, 2026-10-20). Tested by
 * scripts/test_picker_nav.ts.
 *
 * The list has ONE index space in three zones, in the order drawn: the header controls (custom URL, its keys, the
 * chooser rows…), the list rows, then the footer. Two bugs lived in the arithmetic between the two:
 *   1. Back walked `listData` from the GLOBAL index, so it collapsed the wrong group — off by the header count
 *      (InstancePickerScreen's onBack). `pickerZone` turns a global index into a zone and a position.
 *   2. Enter on a directory row ran a different path from a tap: it left SpyServer rows dead, ignored a FULL receiver,
 *      and dropped `blocksApps` (a restricted receiver opened outside compatibility mode). `directoryRowAction` is the
 *      one decision both now call.
 */
export type PickerZone = { zone: 'header' | 'list' | 'footer'; at: number } | null;

export function pickerZone(i: number, hdr: number, list: number, ftr: number): PickerZone {
  if (i < 0) return null;
  if (i < hdr) return { zone: 'header', at: i };
  if (i < hdr + list) return { zone: 'list', at: i - hdr };
  if (i < hdr + list + ftr) return { zone: 'footer', at: i - hdr - list };
  return null;
}

/** The nearest group header at or above list row `at` (for Back: collapse the group you are standing in), or -1. */
export function headerAbove(kinds: readonly string[], at: number): number {
  for (let j = Math.min(at, kinds.length - 1); j >= 0; j--) if (kinds[j] === 'header') return j;
  return -1;
}

export interface DirRowLike {
  url: string;
  serverType?: string;
  users: number;
  maxUsers: number;
}

/** Full: the two directories count opposite ways — UberSDR's `users` is FREE slots, KiwiSDR's is IN USE. */
export function directoryRowFull(r: DirRowLike): boolean {
  const st = r.serverType ?? 'ubersdr';
  return r.maxUsers > 0 && (st === 'ubersdr' ? r.users <= 0 : r.users >= r.maxUsers);
}

export type DirRowAction =
  | { do: 'none' }
  | { do: 'spy'; host: string; port: number }
  | { do: 'vibeserver' }
  | { do: 'connect'; compatOnly: boolean };

/** What pressing a directory row does — the tap and Enter both. `restricted` = blocksApps(inst). */
export function directoryRowAction(r: DirRowLike, restricted: boolean): DirRowAction {
  if (directoryRowFull(r)) return { do: 'none' };   // the row is greyed and disabled for a hand, so for a pad too
  if (r.serverType === 'spyserver') {
    const m = /^spyserver:\/\/([^:]+):(\d+)$/.exec(r.url);
    return m ? { do: 'spy', host: m[1], port: parseInt(m[2], 10) } : { do: 'none' };
  }
  if (r.serverType === 'vibeserver') return { do: 'vibeserver' };
  return { do: 'connect', compatOnly: restricted };
}

/**
 * ★ Scroll a row of the list's header or footer into view (V12 489): the offset to scroll the list to, or null when the
 *   row is already wholly inside the list's own box. Positions are window coordinates from measureInWindow — the list's
 *   box (`box`) and the row (`row`) — and `scrollY` is the list's offset now. Centred: from across a room a row nudged
 *   just inside the edge is easy to lose.
 * ★★ The header rows used to scroll the list to the TOP whatever they were. With DISCOVERED holding seven servers and
 *    SEARCH AGAIN under them, the header is taller than a phone's list, so the bottom servers sat below the fold while
 *    the highlight was on them — "I cannot get the green box to land on the bottom few discovered servers".
 */
export function revealOffset(scrollY: number, box: { y: number; h: number }, row: { y: number; h: number }): number | null {
  if (!(box.h > 0) || !(row.h >= 0)) return null;
  const rel = row.y - box.y;                       // the row's top, from the top of the list's box
  if (rel >= 0 && rel + row.h <= box.h) return null;
  return Math.max(0, scrollY + rel - Math.max(0, (box.h - row.h) / 2));
}
