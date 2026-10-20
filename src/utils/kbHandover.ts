/**
 * ★★★ FOLLOW THE LIVE INPUT SOURCE WHILE EDITING (Stuart, decided 2026-10-20 — brief "Decided 10-20" §5). The pure
 * decision; ControllerKeyboard and usePadField act on it. Tested by scripts/test_kb_handover.ts.
 *
 *   editing with            then the user…            becomes
 *   controller (ours)       touches                   system — ours closes, the field opens the system keyboard
 *   controller              presses a physical key    inplace — ours closes, they type in the field
 *   controller              presses a pad button      (stays)
 *   system (touch opened)   types on a keyboard       (stays — the OS drops its keyboard itself; we do not fight it)
 *   system                  presses a pad button      controller — the field lets go; ours opens
 *   inplace (key opened)    touches                   (stays — the OS shows its keyboard itself)
 *   inplace                 presses a pad button      controller
 *
 * ★ THE VALUE NEVER RESETS IN A HAND-OVER, and nor does the caret or password mode: `carry` is the only way the edit
 *   moves between the two editors, and it copies all three.
 */
import type { InputSource } from './inputSource';

export type Editor = 'controller' | 'system' | 'inplace';

/** Which editor a field opens in, by the source at the moment Enter or a tap opens it. */
export function editorFor(src: InputSource): Editor {
  return src === 'pad' ? 'controller' : src === 'keyboard' ? 'inplace' : 'system';
}

/** The editor to switch to when `src` acts during an edit in `cur`, or null to stay. */
export function handover(cur: Editor, src: InputSource): Editor | null {
  if (cur === 'controller') return src === 'touch' ? 'system' : src === 'keyboard' ? 'inplace' : null;
  return src === 'pad' ? 'controller' : null;
}

export interface EditCarry { value: string; caret: number; secure: boolean }

/** What moves across a hand-over — everything, unchanged. */
export function carry(from: EditCarry): EditCarry {
  return { value: from.value, caret: Math.max(0, Math.min(from.value.length, from.caret)), secure: from.secure };
}
