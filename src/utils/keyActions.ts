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
