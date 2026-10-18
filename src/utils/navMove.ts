/**
 * navMove — where a panel's highlight goes for one movement key. The pure core of PanelNav's useNavKeys, taken out so
 * it can be tested (scripts/test_nav_move.ts) without React.
 *
 *  • Entries arrive SORTED by row, then id (the JSX reading order).
 *  • Down goes to the FIRST entry of the next row, Up to the LAST entry of the row above (both clamp at the ends).
 *  • Left / Right stay inside the row — except in a FLAT list, where every entry is its own row and left / right
 *    would otherwise be dead, so they act as up / down.
 *
 * Returns the index to move to, or -1 to stay put.
 */
export type NavMoveKey = 'ArrowUp' | 'ArrowDown' | 'ArrowLeft' | 'ArrowRight';

export function navMove(rows: readonly number[], cur: number, key: NavMoveKey, flat: boolean): number {
  const n = rows.length;
  if (!n) return -1;
  if (cur < 0 || cur >= n) return 0;                       // nothing highlighted yet: the first movement takes focus
  const row = rows[cur];
  const clamp = (i: number) => Math.max(0, Math.min(n - 1, i));
  const nextRow = () => { const i = rows.findIndex((r) => r > row); return clamp(i < 0 ? n - 1 : i); };
  const prevRow = () => { for (let i = cur - 1; i >= 0; i--) if (rows[i] < row) return i; return 0; };
  switch (key) {
    case 'ArrowDown':  return nextRow();
    case 'ArrowUp':    return prevRow();
    case 'ArrowRight': return flat ? nextRow() : (cur + 1 < n && rows[cur + 1] === row ? cur + 1 : -1);
    case 'ArrowLeft':  return flat ? prevRow() : (cur > 0 && rows[cur - 1] === row ? cur - 1 : -1);
    default: return -1;
  }
}
