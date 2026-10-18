/**
 * ★★★ THE KEYBOARD — ONE SCHEME, APP AND WEB PAGE (Stuart, 2026-10-18; memory: gamepad_mapping).
 *
 *   "the app gets D-Pad and WASD support for moving the selection, enter and backspace for select and back.
 *    < > for tune - + for zoom … the whole keyboard controls we already have can be stripped out as they are
 *    too complicated … escape as a 2nd back … full parity with the controls on both based on game controller
 *    and remote control use"
 *
 *   Arrow keys, W A S D ........ move the highlight (the game controller's D-pad, the Apple TV remote's touch surface)
 *   Enter ...................... select (on a slider or drop-down: adjust it, as a pad's enter does)
 *   Backspace, Escape .......... back (close the top panel or menu; leave adjust mode)
 *   < >  (, . unshifted) ....... tune down / up one step
 *   - +  (= unshifted, numpad) . zoom out / in
 *
 * Nothing else. Every older shortcut (letters that opened panels, mode letters, mute, step cycling, Shift×10, the
 * arrows tuning and zooming) is gone — a keyboard now does exactly what a pad or a remote does, so there is one
 * thing to learn and one thing to describe.
 *
 * ★ The PURE part only, shared by the web page (web/client/src/main.ts) and the app (src/utils/hardwareKeys.ts, fed by
 *   VibeKeyWindow in ios/VibeSDR/AppDelegate.swift). Tested by scripts/test_key_actions.ts.
 * ★ A key with Ctrl, Cmd or Alt held is never ours — system and browser shortcuts (Cmd-Q, Ctrl-+ page zoom, Cmd-W)
 *   stay theirs. Shift is allowed: it is how < > and + are typed.
 * ★ The CALLER decides about text fields: none of these keys act while somebody is typing (Escape may still close
 *   the panel the field sits in — see each caller).
 */
export type KeyAction =
  | 'up' | 'down' | 'left' | 'right'
  | 'enter' | 'back'
  | 'tuneDown' | 'tuneUp'
  | 'zoomOut' | 'zoomIn';

export interface KeyMods { ctrl?: boolean; meta?: boolean; alt?: boolean }

const MAP: Record<string, KeyAction> = {
  ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
  w: 'up', s: 'down', a: 'left', d: 'right',
  W: 'up', S: 'down', A: 'left', D: 'right',          // Caps Lock or Shift held — still the same key
  Enter: 'enter',
  Backspace: 'back', Escape: 'back',
  // ★ Forward delete too: on a PC keyboard it sits beside Backspace and is pressed for it; a Mac laptop has none.
  Delete: 'back',
  ',': 'tuneDown', '<': 'tuneDown',
  '.': 'tuneUp', '>': 'tuneUp',
  '-': 'zoomOut', '_': 'zoomOut',
  '+': 'zoomIn', '=': 'zoomIn',
};

/** The action a key stands for, or null if it is not one of ours. `key` is a DOM KeyboardEvent.key value (the app's
 *  native layer reports the same names). */
export function keyAction(key: string | null | undefined, mods?: KeyMods): KeyAction | null {
  if (!key) return null;
  if (mods && (mods.ctrl || mods.meta || mods.alt)) return null;
  return MAP[key] ?? null;
}

/** Movement keys, in the shape the highlight code wants. */
export function keyDir(a: KeyAction | null): 'up' | 'down' | 'left' | 'right' | null {
  return a === 'up' || a === 'down' || a === 'left' || a === 'right' ? a : null;
}

/** Actions that repeat while the key is held (movement, tune, zoom) — enter and back never do. */
export function keyRepeats(a: KeyAction | null): boolean {
  return !!a && a !== 'enter' && a !== 'back';
}

// ── The app's canonical key names and its listener bus (src/utils/hardwareKeys.ts wires the native side to it) ──

export type HwKey =
  | 'ArrowUp' | 'ArrowDown' | 'ArrowLeft' | 'ArrowRight'
  | 'Enter' | 'Back'
  | 'TuneDown' | 'TuneUp' | 'ZoomOut' | 'ZoomIn';

/** ★ `pad`: it came from a game controller (src/utils/padActions.ts), not a keyboard — the Full Keyboard Access
 *  detector must not count it (PanelNav noteKeyForFka). Everything else treats the two alike, on purpose. */
export interface HwKeyEvent { key: HwKey; plain: boolean; typing: boolean; pad?: boolean }

const HW_NAME: Record<KeyAction, HwKey> = {
  up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight',
  enter: 'Enter', back: 'Back',
  tuneDown: 'TuneDown', tuneUp: 'TuneUp', zoomOut: 'ZoomOut', zoomIn: 'ZoomIn',
};

/** A raw native report ({key, plain, typing}) → the event the app's listeners get, or null if it is not one of ours. */
export function toHwKey(raw: { key?: string; plain?: boolean; typing?: boolean; pad?: boolean } | null | undefined): HwKeyEvent | null {
  const a = keyAction(raw?.key);
  if (!a) return null;
  const typing = !!raw?.typing;
  // ★ While a field has the cursor only its Enter (commit) and Escape (leave, close the panel) are ever ours.
  if (typing && !(a === 'enter' || raw?.key === 'Escape')) return null;
  // ★ plain = the physical arrow key it stands for. W A S D are not (PanelNav's Full Keyboard Access detector).
  const e: HwKeyEvent = { key: HW_NAME[a], plain: raw?.plain !== false && /^Arrow/.test(raw?.key ?? ''), typing };
  if (raw?.pad) e.pad = true;
  return e;
}

type HwListener = (e: HwKeyEvent) => boolean | void;

/** Newest listener first; a 'down' listener returning true consumes the key, so back peels one layer at a time.
 *  ★ `priority`: a SCREEN's own listener registers low (-1) so the panels and boxes drawn over it always hear a key
 *    first, whatever order their effects happened to run in (a child's effect runs before its parent's). */
export class KeyBus {
  private lists: Record<'down' | 'up', { f: HwListener; pri: number; seq: number }[]> = { down: [], up: [] };
  private seq = 0;
  add(kind: 'down' | 'up', f: HwListener, priority = 0): { remove(): void } {
    const entry = { f, pri: priority, seq: ++this.seq };
    this.lists[kind].push(entry);
    return { remove: () => { const l = this.lists[kind]; const i = l.indexOf(entry); if (i >= 0) l.splice(i, 1); } };
  }
  dispatch(kind: 'down' | 'up', raw: { key?: string; plain?: boolean; typing?: boolean; pad?: boolean } | null | undefined): void {
    const e = toHwKey(raw);
    if (!e) return;
    // A copy: a listener may open or close a panel, adding or removing listeners mid-dispatch.
    const order = this.lists[kind].slice().sort((a, b) => b.pri - a.pri || b.seq - a.seq);
    for (const { f } of order) {
      let used: boolean | void = false;
      try { used = f(e); } catch (err) { console.warn('[keys]', err); }
      if (used === true && kind === 'down') return;
    }
  }
}
