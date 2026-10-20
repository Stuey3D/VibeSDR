/**
 * ★★★ THE CONTROLLER KEYBOARD'S LAYOUTS, AS DATA (brief: "Layouts are defined as data … shared by all platforms").
 * Pure — no React Native — so the web page can import it as it does padGuide.ts. Tested by scripts/test_kb_layouts.ts.
 *
 * Each key has a BOX in key units; the highlight moves between boxes with pickNext (src/utils/gamepadNav.ts — the
 * picker the receiver screen and the web page use), so wide keys need no special case: "down from space" lands on
 * the key whose centre is nearest below. When nothing lies that way, the highlight WRAPS to the far end of the same
 * row (left/right) or column (up/down).
 *
 * ★ Decided 10-20 (Stuart): BOTH layouts carry a visible, focusable ↵ (submit) and Back (cancel) key, as well as ⌫,
 *   so every action can be done by moving to a key and pressing it — on any pad, a no-stick pad, or a remote.
 *
 *   numeric                         qwerty (lower)
 *   [7][8][9]   [⌫]                 [1][2][3][4][5][6][7][8][9][0]
 *   [4][5][6]   [kHz/MHz]           [q][w][e][r][t][y][u][i][o][p]
 *   [1][2][3]   [Clear]             [a][s][d][f][g][h][j][k][l][⌫]
 *   [.][0][↵]   [Back]              [⇧][z][x][c][v][b][n][m][-][.]
 *                                   [Back][#+=][ÀÉ][  space  ][ ↵ ]
 *
 * ★ Modes: `frequency` (numeric; the unit key toggles kHz ↔ MHz, absent when the receiver locks the unit), `pin`
 *   (numeric, digits only: no `.`, no unit), `search` / `text` / `password` (QWERTY; ÀÉ = the accents layer — the
 *   brief's long-press is a layer key here, so no timing is needed), `url` (QWERTY with no capitals: the ⇧ place is
 *   `/`, and `:` sits beside `.`). `password` adds a SHOW / HIDE key.
 */
import { pickNext, type Box, type Dir } from './gamepadNav';

export type KbLayoutName = 'numeric' | 'qwerty';
export type KbMode = 'frequency' | 'pin' | 'search' | 'text' | 'password' | 'url';
export type KbLayer = 'lower' | 'upper' | 'caps' | 'symbols' | 'accents' | 'accentsUp';

export type KeyAct =
  | { t: 'ch'; ch: string }
  | { t: 'bs' } | { t: 'enter' } | { t: 'back' }
  | { t: 'shift' } | { t: 'symbols' } | { t: 'accents' } | { t: 'abc' }
  | { t: 'space' } | { t: 'clear' } | { t: 'unit' } | { t: 'reveal' };

export interface KbKey extends Box { id: string; label: string; act: KeyAct }

export const layoutFor = (mode: KbMode): KbLayoutName => (mode === 'frequency' || mode === 'pin' ? 'numeric' : 'qwerty');

const ch = (c: string): KeyAct => ({ t: 'ch', ch: c });

export interface KbKeysOpts {
  /** frequency: the receiver locks the unit — no unit key. */
  unitLocked?: boolean;
  /** frequency: the unit key's label (the unit it switches TO, e.g. "MHz"). */
  unitLabel?: string;
  /** password: SHOW / HIDE's current label. */
  revealLabel?: string;
}

function numericKeys(mode: KbMode, o: KbKeysOpts): KbKey[] {
  const out: KbKey[] = [];
  const at = (id: string, label: string, act: KeyAct, col: number, row: number) =>
    out.push({ id, label, act, x: col < 3 ? col : 3.25, y: row, w: col < 3 ? 1 : 1.5, h: 1 });
  [['7', '8', '9'], ['4', '5', '6'], ['1', '2', '3']].forEach((r, row) => r.forEach((d, col) => at(d, d, ch(d), col, row)));
  if (mode === 'frequency') at('.', '.', ch('.'), 0, 3);
  at('0', '0', ch('0'), 1, 3);
  at('enter', '↵', { t: 'enter' }, 2, 3);
  at('bs', '⌫', { t: 'bs' }, 3, 0);
  if (mode === 'frequency' && !o.unitLocked) at('unit', o.unitLabel ?? 'MHz', { t: 'unit' }, 3, 1);
  at('clear', 'Clear', { t: 'clear' }, 3, 2);
  at('back', 'Back', { t: 'back' }, 3, 3);
  return out;
}

const LETTERS = ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'];
const SYMBOLS = ['-/:;()$&@"', ".,?!'#%*+", '=_\\|~<>[]'];
const ACCENTS = ['àáâäãåæçèé', 'êëìíîïñòó', 'ôöõøùúûüß'];

function qwertyKeys(mode: KbMode, layer: KbLayer, o: KbKeysOpts): KbKey[] {
  const out: KbKey[] = [];
  const put = (id: string, label: string, act: KeyAct, x: number, y: number, w = 1) => out.push({ id, label, act, x, y, w, h: 1 });
  const upper = (layer === 'upper' || layer === 'caps' || layer === 'accentsUp') && mode !== 'url';
  const accents = layer === 'accents' || layer === 'accentsUp';
  const cased = (c: string) => (upper ? c.toUpperCase() : c);
  // Row 0: the digits, on every layer.
  [...'1234567890'].forEach((d, i) => put('d' + d, d, ch(d), i, 0));
  if (layer === 'symbols') {
    [...SYMBOLS[0]].forEach((c, i) => put('s0' + i, c, ch(c), i, 1));
    [...SYMBOLS[1]].forEach((c, i) => put('s1' + i, c, ch(c), i, 2));
    put('bs', '⌫', { t: 'bs' }, 9, 2);
    put('abc', 'ABC', { t: 'abc' }, 0, 3);
    [...SYMBOLS[2]].forEach((c, i) => put('s2' + i, c, ch(c), i + 1, 3));
  } else if (accents) {
    [...ACCENTS[0]].forEach((c, i) => put('a0' + i, cased(c), ch(cased(c)), i, 1));
    [...ACCENTS[1]].forEach((c, i) => put('a1' + i, cased(c), ch(cased(c)), i, 2));
    put('bs', '⌫', { t: 'bs' }, 9, 2);
    put('shift', upper ? '⇪' : '⇧', { t: 'shift' }, 0, 3);
    [...ACCENTS[2]].forEach((c, i) => {
      const k = c === 'ß' ? c : cased(c);   // ß has no single-letter capital here
      put('a2' + i, k, ch(k), i + 1, 3);
    });
  } else {
    [...LETTERS[0]].forEach((c, i) => put('l' + c, cased(c), ch(cased(c)), i, 1));
    [...LETTERS[1]].forEach((c, i) => put('l' + c, cased(c), ch(cased(c)), i, 2));
    put('bs', '⌫', { t: 'bs' }, 9, 2);
    if (mode === 'url') put('slash', '/', ch('/'), 0, 3);
    else put('shift', layer === 'caps' ? '⇪' : '⇧', { t: 'shift' }, 0, 3);
    [...LETTERS[2]].forEach((c, i) => put('l' + c, cased(c), ch(cased(c)), i + 1, 3));
    if (mode === 'url') { put('colon', ':', ch(':'), 8, 3); put('dot', '.', ch('.'), 9, 3); }
    else { put('dash', '-', ch('-'), 8, 3); put('dot', '.', ch('.'), 9, 3); }
  }
  // Row 4: Back, #+=, (ÀÉ), (SHOW), space, ↵ — ten units in all.
  let x = 0;
  put('back', 'Back', { t: 'back' }, x, 4, 1.5); x += 1.5;
  put('sym', layer === 'symbols' ? 'ABC' : '#+=', layer === 'symbols' ? { t: 'abc' } : { t: 'symbols' }, x, 4, 1.5); x += 1.5;
  const accentsOk = mode === 'text' || mode === 'password' || mode === 'search';
  if (accentsOk) { put('acc', accents ? 'abc' : 'ÀÉ', accents ? { t: 'abc' } : { t: 'accents' }, x, 4, 1); x += 1; }
  if (mode === 'password') { put('reveal', o.revealLabel ?? 'SHOW', { t: 'reveal' }, x, 4, 1); x += 1; }
  const spaceOk = mode !== 'url';
  const spaceW = 10 - 2 - x;
  if (spaceOk) put('space', 'space', { t: 'space' }, x, 4, spaceW);
  put('enter', '↵', { t: 'enter' }, spaceOk ? x + spaceW : 8, 4, spaceOk ? 2 : 10 - x);
  return out;
}

/** Every key on screen for this mode and layer. */
export function kbKeys(mode: KbMode, layer: KbLayer = 'lower', o: KbKeysOpts = {}): KbKey[] {
  return layoutFor(mode) === 'numeric' ? numericKeys(mode, o) : qwertyKeys(mode, layer, o);
}

const cxOf = (k: Box) => k.x + k.w / 2;
const cyOf = (k: Box) => k.y + k.h / 2;
const overlapsRow = (a: Box, b: Box) => a.y < b.y + b.h && b.y < a.y + a.h;
const overlapsCol = (a: Box, b: Box) => a.x < b.x + b.w && b.x < a.x + a.w;

/** Where the highlight goes from key `cur` in `dir` — pickNext, wrapping at the edges. -1 → the first key. */
export function kbMove(keys: readonly KbKey[], cur: number, dir: Dir): number {
  if (!keys.length) return -1;
  if (cur < 0 || cur >= keys.length) return 0;
  const here = keys[cur];
  // ★ pickNext works in POINTS (it ignores a move of a point or less), so key units are scaled up first.
  const S = 100;
  const px = (k: Box): Box => ({ x: k.x * S, y: k.y * S, w: k.w * S, h: k.h * S });
  const i = pickNext(px(here), keys.map(px), dir);
  if (i >= 0) return i;
  // Wrap: the far end of this row (left / right) or of this column (up / down).
  let best = -1;
  for (let j = 0; j < keys.length; j++) {
    if (j === cur) continue;
    const k = keys[j];
    if (dir === 'left' || dir === 'right') {
      if (!overlapsRow(k, here)) continue;
      if (best < 0 || (dir === 'left' ? cxOf(k) > cxOf(keys[best]) : cxOf(k) < cxOf(keys[best]))) best = j;
    } else {
      if (!overlapsCol(k, here)) continue;
      if (best < 0 || (dir === 'up' ? cyOf(k) > cyOf(keys[best]) : cyOf(k) < cyOf(keys[best]))) best = j;
    }
  }
  return best >= 0 ? best : cur;
}

/** The key id the highlight starts on: ↵ is never the start (a stray Enter must not submit). */
export function kbStartKey(mode: KbMode): string {
  return layoutFor(mode) === 'numeric' ? '5' : 'lg';
}
