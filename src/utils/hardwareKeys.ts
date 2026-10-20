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
 *
 * ★★★ GAME CONTROLLERS COME IN THROUGH THIS SAME DOOR (V12). The native side (VibeGamepad.swift on iPhone / iPad / Mac,
 *   VibeGamepad.kt on Android) reports the whole pad as a 'VibePad' event whenever it changes; src/utils/padActions.ts
 *   turns that into the keyboard's own keys by Stuart's scheme, and they go through the same bus with `pad: true`. So
 *   every listener above works with a pad, with no code of its own. Nothing runs without a pad connected.
 *   ★ SELECT is not a key: it opens and closes the CONTROLLER GUIDE (PadGuide, 2026-10-19) through addPadGuideListener,
 *     and the menu's CONTROLLER GUIDE key asks for it with openPadGuide — both on the layout in use (padProfileNow).
 *   ★ While a field has the cursor the pad obeys the keyboard's rule (toHwKey): only enter and back reach the app, and
 *     back lets go of the cursor — natively, as Escape does (leaveTextField).
 */
import { NativeEventEmitter, NativeModules } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { KeyBus, type HwKey, type HwKeyEvent } from './keyActions';
import { PadMapper, type PadButton, type PadOut, type PadProfile, type PadReport } from './padActions';
import { inputSource } from './inputSource';

export type { HwKey, HwKeyEvent };

const bus = new KeyBus();
let native: { remove(): void }[] | null = null;

function ensureNative() {
  if (native) return;
  const mod = NativeModules.VibePowerModule;
  if (!mod) { native = []; return; }   // no native module (a platform without one): no keys
  const em = new NativeEventEmitter(mod);
  native = [
    // ★ The last input source (src/utils/inputSource.ts) is set HERE, before any listener hears the key: a physical
    //   key is 'keyboard', a pad report 'pad' (runPad). Only keys that are ours count — but a key typed into a field
    //   still says a keyboard is in use.
    em.addListener('VibeKeyDown', (r: any) => { if (r?.key) inputSource.set('keyboard'); bus.dispatch('down', r); }),
    em.addListener('VibeKeyUp', (r: any) => bus.dispatch('up', r)),
    em.addListener('VibePad', (r: any) => onPad(r)),
  ];
}

// ── Game controllers ─────────────────────────────────────────────────────────
// The choice START last made is remembered on this device (the web page keeps its own, in the browser).
const PAD_PROFILE_KEY = 'vibe.padProfile';
const pad = new PadMapper(null, (p) => { AsyncStorage.setItem(PAD_PROFILE_KEY, p).catch(() => {}); });
let padLoaded = false;
let padTyping = false;
let padHoldTimer: ReturnType<typeof setTimeout> | null = null;
const padNoticeListeners = new Set<(text: string, profile: PadProfile) => void>();
const padGuideListeners = new Set<(cmd: 'toggle' | 'open', profile: PadProfile) => void>();

/** What START last chose on this device — read once, on the first pad report or the first guide. */
function loadPadChoice(): Promise<void> {
  if (padLoaded) return Promise.resolve();
  padLoaded = true;
  return AsyncStorage.getItem(PAD_PROFILE_KEY).then((v) => { if (v === 'sticks' || v === 'buttons') pad.setChosen(v); }).catch(() => {});
}

function onPad(r: (PadReport & { typing?: boolean }) | null | undefined) {
  if (!r || typeof r.id !== 'string') return;
  void loadPadChoice();
  padTyping = !!r.typing;
  runPad(pad.feed(r, Date.now()));
  armPadHold();
}

const padKbListeners = new Set<(o: { kbButton: PadButton; profile: PadProfile } | { kbSubmit: true }) => void>();

/** ★ The controller keyboard is open: the mapper turns START taps into submit and the spare buttons into edits. */
export function setPadKeyboardOpen(on: boolean) { pad.setKeyboardOpen(on); }
/** The controller keyboard's pad buttons (kbPadAction) and START taps. */
export function addPadKbListener(f: (o: { kbButton: PadButton; profile: PadProfile } | { kbSubmit: true }) => void): { remove(): void } {
  ensureNative();
  padKbListeners.add(f);
  return { remove: () => { padKbListeners.delete(f); } };
}

function runPad(outs: PadOut[]) {
  for (const o of outs) {
    if ('kbButton' in o || 'kbSubmit' in o) {
      inputSource.set('pad');
      padKbListeners.forEach((f) => { try { f(o); } catch { /* a listener's fault */ } });
      continue;
    }
    // ★ Any pad press says a pad is in use — even with a field's cursor up, when the bus below drops the key: that is
    //   how a pad takes over from the system keyboard (the controller keyboard's hand-over).
    if ('kind' in o && o.kind === 'down') inputSource.set('pad');
    if ('guide' in o) { emitGuide('toggle', o.profile); continue; }
    if ('notice' in o) { padNoticeListeners.forEach((f) => { try { f(o.notice, o.profile); } catch { /* a listener's fault */ } }); continue; }
    bus.dispatch(o.kind, { key: o.key, plain: false, typing: padTyping, pad: true });
    // ★ Back in a field leaves it — one layer, exactly as Escape on the keyboard (the native window does that one).
    if (o.kind === 'down' && o.key === 'Escape' && padTyping) NativeModules.VibePowerModule?.leaveTextField?.();
  }
}

/** A held START reports nothing more, so the switch is timed here. */
function armPadHold() {
  if (padHoldTimer) { clearTimeout(padHoldTimer); padHoldTimer = null; }
  const d = pad.nextDeadline();
  if (d === null) return;
  padHoldTimer = setTimeout(() => { padHoldTimer = null; runPad(pad.tick(Date.now())); armPadHold(); }, Math.max(0, d - Date.now()));
}

/** The notice to show when HOLD START switches a pad's controls (PadNotice draws it). */
export function addPadNoticeListener(f: (text: string, profile: PadProfile) => void): { remove(): void } {
  ensureNative();
  padNoticeListeners.add(f);
  return { remove: () => { padNoticeListeners.delete(f); } };
}

function emitGuide(cmd: 'toggle' | 'open', p: PadProfile) {
  padGuideListeners.forEach((f) => { try { f(cmd, p); } catch { /* a listener's fault */ } });
}

/** SELECT on a pad ('toggle') or the menu's CONTROLLER GUIDE key ('open'), with the layout to open on (PadGuide). */
export function addPadGuideListener(f: (cmd: 'toggle' | 'open', profile: PadProfile) => void): { remove(): void } {
  ensureNative();
  padGuideListeners.add(f);
  return { remove: () => { padGuideListeners.delete(f); } };
}

/** The layout in use now: the connected pad's, else what START last chose here, else two sticks. */
export function padProfileNow(): PadProfile { return pad.current(); }

/** The menu's CONTROLLER GUIDE key — opens it on the layout in use (once the remembered choice has been read). */
export function openPadGuide() {
  loadPadChoice().then(() => emitGuide('open', pad.current()));
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
