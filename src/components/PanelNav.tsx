// VibeSDR — shared keyboard/D-pad navigation for panels.
//
// Extracted from MenuSheet, which grew this first. It is shared because four panels
// need it (MenuSheet, StepPicker, AudioSheet, ModeSelector) and because the SAME
// machinery is what a game controller's D-pad will drive — see
// briefs/BRIEF-controls-keyboard-and-gamepad.md. Copying it four times would have meant
// fixing every bug four times, and the controller work would have made it eight.
//
// Two shapes, one core:
//   • usePanelNav() — a 2-D GRID, rows declared by <NavRow>, buttons registering
//     themselves from JSX. Reading order falls out of the tree; nobody writes a map.
//   • useListNav()  — a flat COLUMN (StepPicker, ModeSelector), where the caller
//     already has an array and just wants a focused index.
//
// ★ OWNER ARBITRATION. Panels are not structurally prevented from being visible at
// once, and two panels both listening for keys would both move their focus on
// one key press. So visible panels push themselves onto a stack and only the TOP one
// handles keys. Cheap, and it means a panel opening over another cannot steal or
// double-handle input.
//
// ★★★ ONE SCHEME (Stuart, 2026-10-18 — src/utils/keyActions.ts): arrows / W A S D move, Enter selects (on a slider:
// ADJUST it — left/right change the value, Enter or Back finishes — exactly as a game controller's enter does on the
// web page), Back (Backspace / Escape) steps out one layer. Keys come through src/utils/hardwareKeys.ts with canonical
// names; nothing here knows which physical key was pressed.
//
// ★ BACK, ONE LAYER AT A TIME: the top panel takes Back first (leave adjust mode; step out of a sub-page). When it has
// nothing left to step out of it passes Back on, and the SCREEN closes the panel — one owner for that rule.
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, ScrollView, View } from 'react-native';
import { addKeyListener, type HwKeyEvent } from '../utils/hardwareKeys';
import { navMove } from '../utils/navMove';
import { useFaceplate } from '../contexts/FaceplateContext';
import { focusFor, focusTint, FOCUS_GREEN } from '../constants/decoderTokens';

export type NavEntry = {
  id: number;
  row: number;
  /** Activate this entry (Enter / A). */
  press?: () => void;
  /** Measured scroll-into-view, if the panel scrolls. */
  reveal?: () => void;
  /**
   * ★ Present on VALUE controls (sliders, and the live-bar controls). Enter puts it in ADJUST mode: left/right then
   * change its value, Enter or Back finishes — the game controller's rule on the web page (2026-10-18). Without
   * this a slider is reachable but dead, which is exactly what once shipped.
   */
  adjust?: (dir: -1 | 1) => void;
};

/** Keyboard / D-pad focus green — the colour on every controls colour but green (see useNavFocus). */
export const NAV_FOCUS = FOCUS_GREEN;
/** A value control in ADJUST mode (Enter on a slider) — the amber the web page's game-controller highlight uses. */
export const NAV_ADJUST = '#ffb833';

/**
 * ★★ THE FOCUS COLOUR, LIVE (Stuart, 2026-10-12: "yes make it white"): WHITE when the controls colour is green —
 * green focus vanished among green keys once keys took the controls colour — and NAV_FOCUS green otherwise.
 * One hook so every panel agrees. `tint(a)` is the focused row's fill.
 */
export function useNavFocus(): { ring: string; tint: (a: number) => string } {
  const rgb = useFaceplate().controls.rgb;
  return useMemo(() => ({ ring: focusFor(rgb), tint: (a: number) => focusTint(rgb, a) }), [rgb]);
}

/**
 * ★ The highlight drawn OVER a control that has no focus style of its own (controller audit, 2026-10-20): an absolute
 *   ring inside the control's own box, so nothing moves when it appears and a touch user — who never sees it — gets
 *   exactly today's layout. Put it as the LAST child of the focused control (or of a wrapper beside a dimmed one: a
 *   ring inside an `opacity: 0.45` row is a ring nobody can see from the sofa).
 */
export function NavRing({ on, color, radius = 10, inset = 0 }: { on: boolean; color: string; radius?: number; inset?: number }) {
  if (!on) return null;
  return (
    <View pointerEvents="none" style={{
      position: 'absolute', left: inset, top: inset, right: inset, bottom: inset,
      borderWidth: 3, borderColor: color, borderRadius: radius,
    }} />
  );
}

// Ids are unique across ALL panels, so a panel closing and another opening can never
// collide on a stale focus id.
let nextNavId = 1;
export const nextNavButtonId = () => nextNavId++;

// ── Keyboard-mode idle timeout ───────────────────────────────────────────────
//
// ★ A menu opened by a stray key press must not sit there forever. On touch you always
// know a panel is open, because you opened it with your hand and the screen is in front of
// you — but with the phone face-down driving a TV, an accidental key leaves a sheet over
// everything with nothing to dismiss it. (Stuart: "if no input has been taken after a few
// seconds then the menu times out… incase a button was pressed by accident".)
//
// ★★ ONLY IN KEYBOARD MODE. A touch means a person is present and looking, so it cancels
// the timer outright and the menu behaves exactly as it always has. The flag flips back on
// the next key press, so alternating between hand and keyboard needs no mode switch.
export const PANEL_IDLE_MS = 10_000;

let keyboardMode = false;
const kbListeners = new Set<(on: boolean) => void>();
const setKeyboardMode = (on: boolean) => {
  if (keyboardMode === on) return;
  keyboardMode = on;
  kbListeners.forEach(f => f(on));
};
/** Any pointer/touch interaction — a present user, so idle-close is wrong. */
export function noteTouchInteraction() { setKeyboardMode(false); }

/**
 * Is the user driving by keyboard RIGHT NOW? Global, deliberately.
 *
 * ★ A panel cannot answer this from its own state: you press Enter to OPEN it, so by the
 * time its listener mounts the keypress has already happened and it looks like a touch
 * user until you press something else. Stuart: "if I have hit the enter button to go into
 * the frequency entry box then the boxes around the keys should be there already."
 */
export function useKeyboardMode(): boolean {
  const [on, setOn] = useState(keyboardMode);
  useEffect(() => {
    setOn(keyboardMode);
    kbListeners.add(setOn);
    const sub = addKeyListener('down', () => { setKeyboardMode(true); });
    return () => { kbListeners.delete(setOn); sub.remove(); };
  }, []);
  return on;
}

function useIdleClose(active: boolean, onTimeout?: () => void) {
  const cb = useRef(onTimeout); cb.current = onTimeout;
  useEffect(() => {
    if (!active || !onTimeout) return;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const arm = () => {
      if (timer) clearTimeout(timer);
      // Re-checked when it FIRES, not when armed: a touch part-way through must call it off.
      timer = setTimeout(() => { if (keyboardMode) cb.current?.(); }, PANEL_IDLE_MS);
    };
    const sub = addKeyListener('down', () => {
      if (shortcutsSuppressed()) return;
      setKeyboardMode(true); arm();
    });
    if (keyboardMode) arm();   // opened BY the keyboard — start counting immediately
    return () => { if (timer) clearTimeout(timer); sub.remove(); };
  }, [active, !!onTimeout]);   // eslint-disable-line react-hooks/exhaustive-deps
}

// ── External content: shortcuts OFF ──────────────────────────────────────────
//
// ★★ While a page WE DO NOT CONTROL is on screen — a receiver's own web UI in compatibility
// mode — every VibeSDR shortcut is suppressed. This is a safety rule, not tidiness: our keys
// firing underneath someone else's page is worse than them not working, because the user is
// looking at that page and attributing what happens to it. We cannot know what a key means
// there, so we must not guess. (Stuart, 2026-07-25.)
//
// ★ OUR OWN maps are deliberately NOT suppressed — Stuart: "our maps are super simple so they
// can be accessed". The rule is about AUTHORSHIP, not about the fact that it is a WebView.
//
// A counter, not a boolean: overlays can stack, and the last one to close must not switch
// shortcuts back on while another is still up.
let externalContent = 0;
export const shortcutsSuppressed = () => externalContent > 0;

/** Call from any component that displays third-party content for as long as it is shown. */
export function useSuppressShortcuts(active = true) {
  useEffect(() => {
    if (!active) return;
    externalContent++;
    return () => { externalContent = Math.max(0, externalContent - 1); };
  }, [active]);
}

// ── Arrival and departure announcements ──────────────────────────────────────
//
// ★ Stuart's design: the highlight FLASHES ONCE on arrival, sits steady while in use, and on
// an idle timeout flashes once more and fades as it closes. You get an announcement of
// arrival AND departure.
//
// ★★ The departure half is the valuable one. A menu that simply vanishes after a timeout
// reads as a fault — you did not ask it to close and nothing said why. A flash on the way out
// makes the close deliberate rather than mysterious, and costs nothing to read.
export const ANNOUNCE_MS = 450;

export function useAnnounce() {
  const value = useRef(new Animated.Value(0)).current;
  const flash = useCallback(() => {
    value.setValue(1);
    Animated.timing(value, { toValue: 0, duration: ANNOUNCE_MS, useNativeDriver: true }).start();
  }, [value]);
  /** Flash, then run `after` once it has been seen — for closing on a timeout. */
  const flashThen = useCallback((after: () => void) => {
    value.setValue(1);
    Animated.timing(value, { toValue: 0, duration: ANNOUNCE_MS, useNativeDriver: true })
      .start(() => after());
  }, [value]);
  return { value, flash, flashThen };
}

// ── Key repeat ───────────────────────────────────────────────────────────────
//
// ★ iOS does NOT auto-repeat `pressesBegan` for a held key, so every step needed its own
// press. Stuart is driving this from one of those handheld rubber keyboards, where repeated
// presses are genuinely hard work — and a long list is a lot of presses.
//
// ★★ A DELAY BEFORE IT STARTS is what keeps a single tap a single step. Without it, holding
// for a fraction too long would overshoot, which on a list is worse than pressing again.
// It then accelerates once, because the case that needs speed is a long list rather than a
// short one, and a constant fast rate would make short moves twitchy.
const REPEAT_DELAY_MS = 380;   // hold this long before it starts repeating at all
const REPEAT_MS       = 110;   // steady rate
const REPEAT_FAST_MS  = 55;    // after REPEAT_RAMP_MS of holding
const REPEAT_RAMP_MS  = 1200;

/**
 * Subscribe to keys with auto-repeat for `repeatKeys`. Everything else fires once.
 *
 * ★ The handler may return true to CONSUME the key (see hardwareKeys: newest listener first) — how a panel takes Back
 *   before the screen beneath would close it. Only the press can be consumed, never a repeat.
 * ★ Repeat stops on key UP, on any OTHER key, and on unmount. A repeat that outlives its key
 * would walk a list on its own, which is the kind of fault that looks like a possessed app.
 * ★ A key reported while TYPING (Enter, Escape in a text field) is passed only to handlers that ask (`typing`).
 */
export function useRepeatingKeys(
  active: boolean,
  handler: (k: string, e: HwKeyEvent) => boolean | void,
  repeatKeys: readonly string[],
  opts?: { typing?: boolean; priority?: number },
) {
  const hRef = useRef(handler); hRef.current = handler;
  const keysRef = useRef(repeatKeys); keysRef.current = repeatKeys;
  const typingOk = !!opts?.typing;
  const priority = opts?.priority ?? 0;
  useEffect(() => {
    if (!active) return;
    let held: string | null = null;
    let heldEv: HwKeyEvent | null = null;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let startedAt = 0;
    const stop = () => { if (timer) clearTimeout(timer); timer = null; held = null; heldEv = null; };
    const tick = () => {
      if (!held || !heldEv) return;
      hRef.current(held, heldEv);
      const elapsed = Date.now() - startedAt;
      timer = setTimeout(tick, elapsed > REPEAT_RAMP_MS ? REPEAT_FAST_MS : REPEAT_MS);
    };
    const down = addKeyListener('down', (e) => {
      const k = e.key;
      if (!e.pad) noteKeyForFka(k, e.plain);   // a game controller says nothing about Full Keyboard Access
      if (e.typing && !typingOk) return;
      if (k === held) return;      // a re-fired down for the key already held: ignore
      stop();
      const used = hRef.current(k, e);
      if (keysRef.current.includes(k)) {
        held = k; heldEv = e;
        startedAt = Date.now();
        timer = setTimeout(tick, REPEAT_DELAY_MS);
      }
      return used === true;
    }, { priority });
    const up = addKeyListener('up', (e) => {
      if (e.key === held) stop();
    });
    return () => { stop(); down.remove(); up.remove(); };
  }, [active, typingOk, priority]);   // eslint-disable-line react-hooks/exhaustive-deps
}

export const NAV_REPEAT_KEYS = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'] as const;
/** Movement plus tune and zoom — everything a pad repeats while held. */
export const ALL_REPEAT_KEYS = [...NAV_REPEAT_KEYS, 'TuneDown', 'TuneUp', 'ZoomOut', 'ZoomIn'] as const;

// ── iOS Full Keyboard Access detection ───────────────────────────────────────
//
// ★★ TESTED ON DEVICE 2026-07-26. With Settings → Accessibility → Keyboards → **Full Keyboard
// Access** switched on, iOS takes the ARROW KEYS for its own focus navigation before they reach
// the app at all. Letters and Enter still arrive — M opens the menu, Enter opens the frequency
// box — but nothing can be tuned or navigated, and there is no error to explain it.
//
// ★ Note this is the OPPOSITE of the worry that prompted the test: we are not stealing keys from
// an accessibility feature, the system is outranking us. Apple's feature works exactly as
// intended; ours quietly does not, which is the failure a user cannot diagnose.
//
// ★★ There is NO public iOS API for this (`isFullKeyboardAccessEnabled` is macOS/AppKit), so it
// is inferred from the SHAPE OF WHAT ARRIVES: several keys received and not one of them ever an
// arrow. That cannot be certain — someone might genuinely never press an arrow — so it is only
// ever used to offer an EXPLANATION, never to change behaviour.
let keysSeen = 0;
let arrowsSeen = 0;
const fkaListeners = new Set<(on: boolean) => void>();
export const fullKeyboardAccessSuspected = () => keysSeen >= 4 && arrowsSeen === 0;

/**
 * ★★ `plain` matters more than it looks. W A S D arrive as REAL arrow names — that is the whole
 * point of them — and Shift+arrow does too. Both are the WORKAROUNDS for Full Keyboard Access, so
 * counting either as "an arrow arrived" made the feature suppress its own diagnosis: the moment a
 * user pressed the one key that still worked, the app concluded FKA was off.
 * (Found 2026-07-26 with the old `< > - +` arrow aliases, after the splash failed to appear.)
 */
export function noteKeyForFka(k: string, plain = true) {
  const was = fullKeyboardAccessSuspected();
  if (k.startsWith('Arrow') && plain) arrowsSeen++; else keysSeen++;
  const now = fullKeyboardAccessSuspected();
  if (was !== now) fkaListeners.forEach(f => f(now));
}

/** True when several keys have arrived and never an arrow — see the note above. */
export function useFullKeyboardAccessSuspected(): boolean {
  const [on, setOn] = useState(fullKeyboardAccessSuspected());
  useEffect(() => {
    setOn(fullKeyboardAccessSuspected());
    fkaListeners.add(setOn);
    return () => { fkaListeners.delete(setOn); };
  }, []);
  return on;
}

// ── Region capture ───────────────────────────────────────────────────────────
//
// ★ The decoder box floats ABOVE the live screen rather than covering it, so it is not a
// "panel" in the sense SDRScreen means — the waterfall is still there and the arrows still
// tune. When the keyboard is handed to the box (Tab), the main screen has to stop acting on
// keys entirely, or up and down would retune the radio underneath the list being read.
let region: string | null = null;
const regionListeners = new Set<(r: string | null) => void>();
export const regionCaptured = () => region !== null;
export function captureRegion(name: string | null) {
  if (region === name) return;
  region = name;
  regionListeners.forEach(f => f(region));
}

/**
 * A token that increments whenever a captured region HANDS THE KEYBOARD BACK.
 *
 * ★ Stuart: when focus moves from the decoder box back to tune/zoom, the drums or keys should
 * flash too. The departure flash says "leaving"; without this nothing says "arriving here",
 * and on a TV across the room the controls that just became live are the thing you need to
 * find. An announcement of arrival is only half an answer if only one end announces.
 */
export function useRegionHandback(): number {
  const [tok, setTok] = useState(0);
  useEffect(() => {
    const f = (r: string | null) => { if (r === null) setTok(t => t + 1); };
    regionListeners.add(f);
    return () => { regionListeners.delete(f); };
  }, []);
  return tok;
}

// ── Owner stack ──────────────────────────────────────────────────────────────
const owners: object[] = [];
const isTopOwner = (tok: object) => owners.length > 0 && owners[owners.length - 1] === tok;

function useNavOwner(visible: boolean): React.MutableRefObject<object> {
  const tok = useRef<object>({});
  useEffect(() => {
    if (!visible) return;
    const t = tok.current;
    owners.push(t);
    return () => {
      const i = owners.lastIndexOf(t);
      if (i >= 0) owners.splice(i, 1);
    };
  }, [visible]);
  return tok;
}

// ── The key handling core, shared by both shapes ─────────────────────────────
//
// `getEntries` is called on every key press rather than captured, so a panel whose
// buttons mount, unmount or reorder (a ScrollView switching sections, a conditional
// row) is always navigated as it currently IS, not as it was when the listener was
// installed.
function useNavKeys(opts: {
  visible: boolean;
  getEntries: () => NavEntry[];
  focusedRef: React.MutableRefObject<number>;
  setFocused: (id: number) => void;
  /** The value control in ADJUST mode, or -1. */
  setAdjusting?: (id: number) => void;
  /** Flat lists (and wrapped grids) move on left/right as well as up/down. */
  flat?: boolean;
  /** Back — step OUT of a sub-panel. Return false when there was nothing to step out of, so the screen closes the
   *  panel instead (anything else counts as handled). */
  onBack?: () => boolean | void;
}) {
  const { visible, getEntries, focusedRef, setFocused, setAdjusting, flat, onBack } = opts;
  const tok = useNavOwner(visible);
  const cbRef = useRef({ getEntries, setFocused, setAdjusting, onBack });
  cbRef.current = { getEntries, setFocused, setAdjusting, onBack };
  const adjustingRef = useRef(-1);
  const setAdj = (id: number) => { adjustingRef.current = id; cbRef.current.setAdjusting?.(id); };

  // ★ `visible`, NOT `open`. An earlier version of this said `open` and type-checked
  // perfectly, because TypeScript's DOM lib declares a global `open` (window.open) — tsc saw
  // a valid symbol while at runtime it was undefined, and the screen crash-looped. A clean
  // tsc is NOT proof a name is in scope when the DOM lib is loaded.
  useEffect(() => { if (!visible) { setFocused(-1); setAdj(-1); } }, [visible, setFocused]);   // eslint-disable-line react-hooks/exhaustive-deps

  useRepeatingKeys(visible, (k: string) => {
    if (!k || shortcutsSuppressed()) return;      // third-party page on screen — see above
    if (!isTopOwner(tok.current)) return;        // a panel above us owns the keys
    const { getEntries: get, setFocused: set, onBack: back } = cbRef.current;
    // Sorted by row then id = the reading order of the JSX, without anyone having to declare it.
    const all = [...get()].sort((a, b) => a.row - b.row || a.id - b.id);
    const cur = all.findIndex(x => x.id === focusedRef.current);
    const here = cur >= 0 ? all[cur] : undefined;
    // ★ ADJUST MODE ends if its control has gone (a section collapsed under it).
    if (adjustingRef.current >= 0 && (!here || here.id !== adjustingRef.current)) setAdj(-1);
    // ★★ BACK, ONE LAYER: leave adjust mode; else step out of a sub-panel. Only when there is nothing to step out of
    //    does it pass on (return false) — the screen then closes the panel itself.
    if (k === 'Back') {
      if (adjustingRef.current >= 0) { setAdj(-1); return true; }
      if (back) return back() !== false;
      return false;
    }
    if (!all.length) return;
    const move = (next: number) => {
      const t = all[Math.max(0, Math.min(all.length - 1, next))];
      if (!t) return;
      setAdj(-1);
      set(t.id);
      t.reveal?.();
    };
    if (k === 'Enter') {
      if (!here) { move(0); return true; }         // nothing highlighted yet: Enter just takes focus
      // ★ Enter on a VALUE control toggles ADJUST mode, as a pad's enter does on the web page: left/right then change
      //   it, Enter or Back finishes. Without the mode, left/right on a slider could never leave it sideways.
      if (here.adjust) { setAdj(adjustingRef.current === here.id ? -1 : here.id); return true; }
      here.press?.();
      return true;
    }
    if (cur < 0) { if (k.startsWith('Arrow')) move(0); return; }   // first movement just takes focus
    if (adjustingRef.current === here!.id && (k === 'ArrowLeft' || k === 'ArrowRight')) {
      here!.adjust?.(k === 'ArrowRight' ? 1 : -1);
      return;
    }
    // The movement rule itself is pure (src/utils/navMove.ts, tested by scripts/test_nav_move.ts).
    if (k === 'ArrowUp' || k === 'ArrowDown' || k === 'ArrowLeft' || k === 'ArrowRight') {
      const to = navMove(all.map(x => x.row), cur, k, !!flat);
      if (to >= 0) move(to);
    }
  }, NAV_REPEAT_KEYS);
}

// ── Measured scroll-into-view ────────────────────────────────────────────────
//
// ★ This replaces an estimate. The original said `row * 46 - 120`, assuming a
// uniform row height — fine for MenuSheet's flat rows, wrong the moment rows are
// nested in sections or vary in height, and unverifiable by inspection.
// measureLayout against the ScrollView's own content node gives the TRUE offset
// whatever the nesting, so no panel has to be laid out in any particular way.
// ★ `fallbackRow` keeps the OLD estimate (`row * 46 - margin`) as a floor. It was
// imprecise but it demonstrably scrolled, and `getInnerViewNode()` is the one part of
// this that cannot be verified without running on device — if it is ever absent under
// the New Architecture, reveal degrades to the previous behaviour instead of silently
// doing nothing at all.
const EST_ROW_H = 46;

export function revealIn(
  scrollRef: React.MutableRefObject<ScrollView | null>,
  viewRef: React.MutableRefObject<View | null>,
  fallbackRow = -1,
  margin = 120,
  scrollY: React.MutableRefObject<number> = { current: 0 },
) {
  const sv = scrollRef.current, v = viewRef.current;
  if (!sv) return;
  const estimate = () => {
    if (fallbackRow < 0) return;
    sv.scrollTo({ y: Math.max(0, fallbackRow * EST_ROW_H - margin), animated: true });
  };
  // ★★★ measureLayout PROVED UNRELIABLE HERE. First with a node handle (a no-op under the
  // New Architecture), then with getInnerViewRef() — and the menu still did not follow the
  // focus. Rather than keep guessing at which handle Fabric accepts, this now uses
  // measureInWindow, which is the same on both renderers and needs no inner-view handle at
  // all: measure the button and the ScrollView in WINDOW space, and the difference is how
  // far the button sits from the top of the viewport. Add the current scroll offset and you
  // have the content position, with no framework-version guesswork in it.
  if (!v) { estimate(); return; }
  // ★★★ A ScrollView has NO measureInWindow of its own — its public surface is scrollTo,
  // getScrollableNode, getInnerViewRef and friends. So the previous version's callback
  // simply never ran and reveal did nothing at all. `getNativeScrollRef()` hands back the
  // underlying host view, which does measure. THIRD attempt at this: node handle, inner
  // view ref, and now the native scroll ref — the lesson being to check that a method
  // exists before building on it rather than assuming the component forwards it.
  const svAny = sv as any;
  const host = svAny.getNativeScrollRef?.() ?? svAny;
  if (typeof host?.measureInWindow !== 'function') { estimate(); return; }
  host.measureInWindow((_sx: number, svY: number, _sw: number, svH: number) => {
    (v as any).measureInWindow?.((_bx: number, bY: number, _bw: number, bH: number) => {
      if (!(svH > 0)) { estimate(); return; }
      const rel = bY - svY;                       // distance from the top of the viewport
      if (rel >= 0 && rel + bH <= svH) return;    // already fully visible — do not jump
      // Centre it: from across a room, a row nudged just inside the edge is easy to lose.
      const target = scrollY.current + rel - (svH - bH) / 2;
      sv.scrollTo({ y: Math.max(0, target), animated: true });
    });
  });
}

// ── Shape 1: the 2-D grid (MenuSheet, AudioSheet) ────────────────────────────
type NavCtxValue = {
  register: (e: NavEntry) => () => void;
  focused: number;
  /** The value control in ADJUST mode (Enter on a slider), or -1. */
  adjusting: number;
  nextRow: () => number;
  /** The panel's ScrollView, so buttons can reveal themselves without being
   *  handed it individually — module-scoped button components cannot see it. */
  scrollRef: React.MutableRefObject<ScrollView | null>;
  /** Live scroll offset, kept by onScroll — measureInWindow gives viewport-relative
   *  positions, so the offset is what turns one into a content position. */
  scrollY: React.MutableRefObject<number>;
};
export const NavCtx = React.createContext<NavCtxValue | null>(null);
export const RowCtx = React.createContext<number>(-1);

/**
 * Grid navigation for a panel whose buttons register themselves via NavCtx.
 * Wrap the content in `<NavCtx.Provider value={navCtx}>` and rows in `<NavRow>`.
 */
export function usePanelNav(
  visible: boolean,
  opts?: { onBack?: () => boolean | void; onTimeout?: () => void },
) {
  const entries   = useRef<NavEntry[]>([]);
  const rowSeq    = useRef(0);
  const scrollRef = useRef<ScrollView | null>(null);
  const scrollY   = useRef(0);
  const [focused, setFocused] = useState(-1);
  const [adjusting, setAdjusting] = useState(-1);
  const focusedRef = useRef(focused);
  useEffect(() => { focusedRef.current = focused; }, [focused]);

  // ★ Row numbers are assigned in TREE order, not mount order, by resetting the
  // sequence at the start of every render pass. React renders children in order, so
  // each <NavRow> (and each own-row value control) takes its number as it is reached
  // — which means a CONDITIONALLY rendered row or slider appearing mid-session lands
  // in its VISUAL position rather than at the end of the focus order.
  //
  // Caching the number per component instance, as this first did, got that wrong: the
  // AUTO/MANUAL waterfall sliders mount after the panel is already open, so they
  // sorted last and arrow-down reached them out of order until the panel was reopened.
  rowSeq.current = 0;

  const register = useCallback((e: NavEntry) => {
    entries.current.push(e);
    return () => { entries.current = entries.current.filter(x => x.id !== e.id); };
  }, []);
  const nextRow = useCallback(() => rowSeq.current++, []);

  useNavKeys({
    visible,
    getEntries: useCallback(() => entries.current, []),
    focusedRef,
    setFocused,
    setAdjusting,
    onBack: opts?.onBack,
  });
  useIdleClose(visible, opts?.onTimeout);

  const navCtx = useMemo<NavCtxValue>(() => ({ register, focused, adjusting, nextRow, scrollRef, scrollY }),
                                     [register, focused, adjusting, nextRow]);
  // ★ `scrollProps` MUST be spread onto the panel's ScrollView. It carries the ref and the
  // onScroll that keeps `scrollY` current — without the offset, reveal cannot convert a
  // viewport position into a content position and the panel will not follow the focus.
  const scrollProps = useMemo(() => ({
    ref: scrollRef,
    scrollEventThrottle: 16,
    onScroll: (e: any) => { scrollY.current = e?.nativeEvent?.contentOffset?.y ?? 0; },
  }), []);
  return { navCtx, focused, scrollRef, scrollProps, scrollY };
}

/** One row of the grid. Up/down moves between rows, left/right within one. */
export function NavRow({ children }: { children: React.ReactNode }) {
  const nav = React.useContext(NavCtx);
  // Taken fresh each render, NOT cached — that is what makes the ordering follow the
  // tree rather than mount history. See the note in usePanelNav.
  const row = nav ? nav.nextRow() : -1;
  return <RowCtx.Provider value={row}>{children}</RowCtx.Provider>;
}

/**
 * Register a button from inside the tree. Returns its focus state and the ref to
 * attach — scrolling it into view is handled here, so callers never wire it up.
 *
 * `onPress` is read through a ref, so a button whose handler changes every render
 * does not need to re-register (which would reorder it and scramble reading order).
 */
export function useNavButton(onPress?: () => void, skip?: boolean) {
  const nav = React.useContext(NavCtx);
  const row = React.useContext(RowCtx);
  const idRef = useRef<number>(-1);
  if (idRef.current < 0) idRef.current = nextNavButtonId();
  const id = idRef.current;
  const viewRef = useRef<View | null>(null);

  const pressRef = useRef(onPress); pressRef.current = onPress;

  useEffect(() => {
    // ★ `skip` means DO NOT REGISTER — the caret passes over as though the control were not
    // there. Registering and disabling it would still stop focus on a dead row, which is worse
    // than absence: it reads as the keyboard having failed rather than as a deliberate choice.
    if (!nav || row < 0 || skip) return;
    return nav.register({
      id, row,
      press:  () => pressRef.current?.(),
      reveal: () => revealIn(nav.scrollRef, viewRef, row, 120, nav.scrollY),
    });
  }, [nav, row, id, skip]);

  return { focused: !skip && !!nav && nav.focused === id, viewRef, id };
}

/**
 * Register a VALUE control (a slider, or a live-bar control) in the focus order.
 *
 * ★ Fixes a real gap: only buttons registered, so keyboard focus SKIPPED sliders
 * entirely — they were unreachable rather than merely awkward. A focused range
 * consumes left/right to change its value (see NavEntry.adjust).
 *
 * `onAdjust` receives -1 or +1 and owns the increment, because only the caller knows
 * what one nudge means for its units (a dB, a percent, a squelch step).
 */
export function useNavRange(onAdjust: (dir: -1 | 1) => void) {
  const nav = React.useContext(NavCtx);
  const ctxRow = React.useContext(RowCtx);
  // ★ Sliders live in ordinary layout Views, not <NavRow>s, so there is usually no
  // row in context. A value control is always full width, so it takes a row of its
  // own — which also means up/down naturally steps onto and off it.
  const row = ctxRow >= 0 ? ctxRow : (nav ? nav.nextRow() : -1);
  const idRef = useRef<number>(-1);
  if (idRef.current < 0) idRef.current = nextNavButtonId();
  const id = idRef.current;
  const viewRef = useRef<View | null>(null);

  const adjRef = useRef(onAdjust); adjRef.current = onAdjust;

  useEffect(() => {
    if (!nav || row < 0) return;
    return nav.register({
      id, row,
      adjust: (dir: -1 | 1) => adjRef.current?.(dir),
      reveal: () => revealIn(nav.scrollRef, viewRef, row, 120, nav.scrollY),
    });
  }, [nav, row, id]);

  return { focused: !!nav && nav.focused === id, adjusting: !!nav && nav.adjusting === id, viewRef, id };
}

// ── Shape 2: the flat column (StepPicker, ModeSelector) ──────────────────────
/**
 * Navigation for a panel that already has its items in an array. Each item is its
 * own row, so up/down walks the list and Enter activates. Returns the focused
 * INDEX (-1 until the first key press), which the caller uses to style.
 *
 * `reveal` is optional and receives the index, for lists inside a ScrollView.
 */
export interface ListNavOpts {
  /** Rows for a GRID of items (the frequency card: tabs / field / units). Without it every item is its own row and
   *  left / right act as up / down. Up / Down then move between rows, left / right inside one (src/utils/navMove.ts). */
  rowOf?: (index: number) => number;
  /** The item the highlight starts on when the list becomes visible while the keyboard (or a pad) is in use. */
  startAt?: number;
  /** Filled with a setter the owner can call to move the highlight itself. */
  handle?: React.MutableRefObject<{ setFocus(index: number): void } | null>;
}

export function useListNav(
  visible: boolean,
  length: number,
  onActivate: (index: number) => void,
  reveal?: (index: number) => void,
  onTimeout?: () => void,
  /** ★ Back — leave WITHOUT choosing. A dropdown is the top owner while it is open, so
   *  without this its Back hit a handler that had none: there was no keyboard way out of a list
   *  except by picking something. (Stuart.) Return false to pass Back on to the screen. */
  onBack?: () => boolean | void,
  opts?: ListNavOpts,
) {
  // ★ Focus is held as an ENTRY ID, because that is what the shared core compares
  // against; the INDEX is derived for the caller. Holding the index here instead
  // silently breaks navigation, since `focusedRef` would never match an entry.
  const [focusedId, setFocusedId] = useState(-1);
  const focusedRef = useRef(-1);
  useEffect(() => { focusedRef.current = focusedId; }, [focusedId]);

  const cb = useRef({ onActivate, reveal, rowOf: opts?.rowOf });
  cb.current = { onActivate, reveal, rowOf: opts?.rowOf };

  // Ids are 1:1 with indices here, offset so they never collide with grid ids.
  const base = useRef(0);
  if (base.current === 0) base.current = nextNavButtonId() * 100000;

  const getEntries = useCallback(() => {
    const out: NavEntry[] = [];
    for (let i = 0; i < length; i++) {
      out.push({
        id: base.current + i,
        row: cb.current.rowOf ? cb.current.rowOf(i) : i,
        press:  () => cb.current.onActivate(i),
        reveal: () => cb.current.reveal?.(i),
      });
    }
    return out;
  }, [length]);

  useNavKeys({ visible, getEntries, focusedRef, setFocused: setFocusedId, flat: !opts?.rowOf, onBack });
  useIdleClose(visible, onTimeout);

  // ★ startAt: opened BY THE KEYBOARD (or a pad), the highlight is already on the thing you came for — Stuart: "if I
  //   have hit the enter button to go into the frequency entry box then the boxes around the keys should be there
  //   already." A touch user sees nothing (keyboard mode is off), so the card still opens inert for a hand.
  const startAt = opts?.startAt ?? -1;
  useEffect(() => {
    if (visible && keyboardMode && startAt >= 0 && startAt < length) setFocusedId(base.current + startAt);
  }, [visible]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ★ handle: lets the owner put the highlight somewhere itself (the frequency card: a field tapped into keeps the
  //   highlight when Escape lets go of the cursor, so the arrows carry on from the field rather than from the top).
  if (opts?.handle) opts.handle.current = { setFocus: (i: number) => setFocusedId(i < 0 ? -1 : base.current + i) };

  const focused = focusedId < 0 ? -1 : focusedId - base.current;

  // A list that shortens while focus sits past its new end (a filtered mode list,
  // a shorter step ladder on a different backend) must not keep focus off the end.
  useEffect(() => {
    if (length > 0 && focused >= length) setFocusedId(base.current + length - 1);
  }, [length, focused]);

  return focused;
}
