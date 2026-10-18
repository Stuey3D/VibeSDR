/**
 * hardwareKeys.ts — the app's ONE door for hardware-keyboard keys (iPhone, iPad, Mac).
 *
 * ★★★ ONE SCHEME (Stuart, 2026-10-18 — src/utils/keyActions.ts): arrows and W A S D move the highlight, Enter selects,
 *   Backspace and Escape go back, < > tune, - + zoom. Exactly what a game controller or the Apple TV remote does, and
 *   exactly what the web page does. Every older shortcut is gone.
 *
 * VibeKeyWindow (ios/VibeSDR/AppDelegate.swift) reports the raw key; THIS maps it, once, through the same table the
 * web page uses, and hands every listener a canonical name:
 *
 *   'ArrowUp' 'ArrowDown' 'ArrowLeft' 'ArrowRight'   move (W A S D arrive as these)
 *   'Enter'                                          select
 *   'Back'                                           back (Backspace, Escape, forward Delete)
 *   'TuneDown' 'TuneUp' 'ZoomOut' 'ZoomIn'           tune and zoom (< > , .  - + = _)
 *
 * ★ `plain` is false for anything that is not the physical arrow key it stands for (W A S D) — the Full Keyboard
 *   Access detector needs that, see PanelNav noteKeyForFka.
 * ★ `typing` is true while a text field has the cursor. The native side then sends only Enter (the field's own
 *   commit — a typed frequency, a search) and Escape (leave the field and close its panel). Nothing else: a letter,
 *   comma, full stop, minus or Backspace is a character somebody is typing.
 * ★★ NEWEST LISTENER FIRST, and a listener that returns `true` CONSUMES the key. That is how "back" peels one layer at
 *   a time: a panel's sub-page (or a slider in adjust mode) takes Back before the screen underneath would close the
 *   whole panel. A listener that returns nothing passes it on, as every listener always did.
 */
import { NativeEventEmitter, NativeModules } from 'react-native';
import { KeyBus, type HwKey, type HwKeyEvent } from './keyActions';

export type { HwKey, HwKeyEvent };

const bus = new KeyBus();
let native: { remove(): void }[] | null = null;

function ensureNative() {
  if (native) return;
  const mod = NativeModules.VibePowerModule;
  if (!mod) { native = []; return; }   // no native module (a platform without one): no keys
  const em = new NativeEventEmitter(mod);
  native = [
    em.addListener('VibeKeyDown', (r: any) => bus.dispatch('down', r)),
    em.addListener('VibeKeyUp', (r: any) => bus.dispatch('up', r)),
  ];
}

/** Listen for key presses ('down') or releases ('up'). Returns a subscription like NativeEventEmitter's. A 'down'
 *  listener that returns true consumes the key (newest listener first — see the header). */
export function addKeyListener(kind: 'down' | 'up', f: (e: HwKeyEvent) => boolean | void,
                               opts?: { priority?: number }): { remove(): void } {
  ensureNative();
  return bus.add(kind, f, opts?.priority ?? 0);
}

/** The priority a SCREEN's own listener takes, so every panel and box over it hears a key first. */
export const SCREEN_PRIORITY = -1;

/** A movement key's direction, for the highlight code. */
export function keyDirOf(k: HwKey | string): 'up' | 'down' | 'left' | 'right' | null {
  return k === 'ArrowUp' ? 'up' : k === 'ArrowDown' ? 'down' : k === 'ArrowLeft' ? 'left' : k === 'ArrowRight' ? 'right' : null;
}
