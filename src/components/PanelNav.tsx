// VibeSDR — shared keyboard/D-pad navigation for panels.
//
// Extracted from MenuSheet, which grew this first. It is shared because four panels
// need it (MenuSheet, StepPicker, AudioSheet, ModeSelector) and because the SAME
// machinery is what a game controller's D-pad will drive — see
// BRIEF-controls-keyboard-and-gamepad.md. Copying it four times would have meant
// fixing every bug four times, and the controller work would have made it eight.
//
// Two shapes, one core:
//   • usePanelNav() — a 2-D GRID, rows declared by <NavRow>, buttons registering
//     themselves from JSX. Reading order falls out of the tree; nobody writes a map.
//   • useListNav()  — a flat COLUMN (StepPicker, ModeSelector), where the caller
//     already has an array and just wants a focused index.
//
// ★ OWNER ARBITRATION. Panels are not structurally prevented from being visible at
// once, and two panels both listening to VibeKeyDown would both move their focus on
// one key press. So visible panels push themselves onto a stack and only the TOP one
// handles keys. Cheap, and it means a panel opening over another cannot steal or
// double-handle input.
//
// ★ Esc is deliberately NOT handled here. SDRScreen owns it (close what is open, or
// open the servers menu when nothing is), and one owner for that rule is what keeps
// the precedence honest.
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { NativeEventEmitter, NativeModules, ScrollView, View } from 'react-native';

export type NavEntry = {
  id: number;
  row: number;
  /** Activate this entry (Enter / A). */
  press?: () => void;
  /** Measured scroll-into-view, if the panel scrolls. */
  reveal?: () => void;
  /**
   * ★ Present on VALUE controls (sliders, and the live-bar controls that will
   * replace them). A focused range CONSUMES left/right to change its value instead
   * of moving focus; up/down still navigates away from it. Without this a slider is
   * reachable but dead, which is exactly what shipped.
   *
   * The same rule serves a game controller's D-pad, where left/right on a focused
   * slider adjusting it is the established console idiom.
   */
  adjust?: (dir: -1 | 1) => void;
};

/** Keyboard / D-pad focus colour. One value so every panel agrees. */
export const NAV_FOCUS = '#7CFF9B';

// Ids are unique across ALL panels, so a panel closing and another opening can never
// collide on a stale focus id.
let nextNavId = 1;
export const nextNavButtonId = () => nextNavId++;

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
  /** Flat lists (and wrapped grids) move on left/right as well as up/down. */
  flat?: boolean;
  /** Backspace — step OUT of a sub-panel. See the note on the key handler. */
  onBack?: () => void;
}) {
  const { visible, getEntries, focusedRef, setFocused, flat, onBack } = opts;
  const tok = useNavOwner(visible);
  const cbRef = useRef({ getEntries, setFocused, onBack });
  cbRef.current = { getEntries, setFocused, onBack };

  useEffect(() => {
    // ★ `visible`, NOT `open`. An earlier version of this said `open` and
    // type-checked perfectly, because TypeScript's DOM lib declares a global
    // `open` (window.open) — tsc saw a valid symbol while at runtime it was
    // undefined, and the screen crash-looped. A clean tsc is NOT proof a name is
    // in scope when the DOM lib is loaded.
    if (!visible) { setFocused(-1); return; }
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      const k = e?.key;
      if (!k || !isTopOwner(tok.current)) return;   // a panel above us owns the keys
      const { getEntries: get, setFocused: set, onBack: back } = cbRef.current;
      // ★ BACKSPACE STEPS OUT of a sub-panel. Multi-level menus (Display Settings,
      // Bookmarks) had a ‹ BACK row but no keyboard way to reach it, so a keyboard-only
      // user could enter a sub-panel and not get out. Safe to claim: the native side only
      // sends Backspace when NO text field has focus, so it still deletes while typing.
      if (k === 'Backspace') { back?.(); return; }
      // Sorted by row then id = the reading order of the JSX, without anyone
      // having to declare it.
      const all = [...get()].sort((a, b) => a.row - b.row || a.id - b.id);
      if (!all.length) return;
      const cur = all.findIndex(x => x.id === focusedRef.current);
      const move = (next: number) => {
        const t = all[Math.max(0, Math.min(all.length - 1, next))];
        if (!t) return;
        set(t.id);
        t.reveal?.();
      };
      if (k === 'Enter') { all[cur]?.press?.(); return; }
      if (cur < 0) { move(0); return; }             // first key press just takes focus
      const here = all[cur];
      const row = here.row;
      // ★ A focused VALUE control owns left/right — see NavEntry.adjust. Checked
      // before navigation, so the slider changes instead of focus moving off it.
      if (here.adjust && (k === 'ArrowLeft' || k === 'ArrowRight')) {
        here.adjust(k === 'ArrowRight' ? 1 : -1);
        return;
      }
      const nextRowFrom = () => {
        const i = all.findIndex(x => x.row > row);
        move(i < 0 ? all.length - 1 : i);
      };
      const prevRowFrom = () => {
        const prev = [...all].reverse().find(x => x.row < row);
        move(prev ? all.indexOf(prev) : 0);
      };
      switch (k) {
        case 'ArrowDown':  nextRowFrom(); break;
        case 'ArrowUp':    prevRowFrom(); break;
        // In a flat list every item is its own row, so left/right would otherwise be
        // dead. Wrapped grids (StepPicker, ModeSelector) are the common case and
        // users reach for left/right there first.
        case 'ArrowRight': if (flat) nextRowFrom(); else if (all[cur + 1]?.row === row) move(cur + 1); break;
        case 'ArrowLeft':  if (flat) prevRowFrom(); else if (cur > 0 && all[cur - 1].row === row) move(cur - 1); break;
        default: break;
      }
    });
    return () => sub.remove();
  }, [visible, focusedRef, tok]);
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
) {
  const sv = scrollRef.current, v = viewRef.current;
  if (!sv) return;
  const estimate = () => {
    if (fallbackRow < 0) return;
    sv.scrollTo({ y: Math.max(0, fallbackRow * EST_ROW_H - margin), animated: true });
  };
  const node = (sv as any).getInnerViewNode?.();
  if (!v || node == null) { estimate(); return; }
  (v as any).measureLayout?.(
    node,
    (_x: number, y: number) => sv.scrollTo({ y: Math.max(0, y - margin), animated: true }),
    estimate,   // measurement can fail mid-unmount — fall back rather than lose it
  );
}

// ── Shape 1: the 2-D grid (MenuSheet, AudioSheet) ────────────────────────────
type NavCtxValue = {
  register: (e: NavEntry) => () => void;
  focused: number;
  nextRow: () => number;
  /** The panel's ScrollView, so buttons can reveal themselves without being
   *  handed it individually — module-scoped button components cannot see it. */
  scrollRef: React.MutableRefObject<ScrollView | null>;
};
export const NavCtx = React.createContext<NavCtxValue | null>(null);
export const RowCtx = React.createContext<number>(-1);

/**
 * Grid navigation for a panel whose buttons register themselves via NavCtx.
 * Wrap the content in `<NavCtx.Provider value={navCtx}>` and rows in `<NavRow>`.
 */
export function usePanelNav(visible: boolean, opts?: { onBack?: () => void }) {
  const entries   = useRef<NavEntry[]>([]);
  const rowSeq    = useRef(0);
  const scrollRef = useRef<ScrollView | null>(null);
  const [focused, setFocused] = useState(-1);
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
    onBack: opts?.onBack,
  });

  const navCtx = useMemo<NavCtxValue>(() => ({ register, focused, nextRow, scrollRef }),
                                     [register, focused, nextRow]);
  // `scrollRef` is attached to the panel's ScrollView by the caller; it is optional,
  // and a panel that does not scroll simply never sets it.
  return { navCtx, focused, scrollRef };
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
export function useNavButton(onPress?: () => void) {
  const nav = React.useContext(NavCtx);
  const row = React.useContext(RowCtx);
  const idRef = useRef<number>(-1);
  if (idRef.current < 0) idRef.current = nextNavButtonId();
  const id = idRef.current;
  const viewRef = useRef<View | null>(null);

  const pressRef = useRef(onPress); pressRef.current = onPress;

  useEffect(() => {
    if (!nav || row < 0) return;
    return nav.register({
      id, row,
      press:  () => pressRef.current?.(),
      reveal: () => revealIn(nav.scrollRef, viewRef, row),
    });
  }, [nav, row, id]);

  return { focused: !!nav && nav.focused === id, viewRef, id };
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
      reveal: () => revealIn(nav.scrollRef, viewRef, row),
    });
  }, [nav, row, id]);

  return { focused: !!nav && nav.focused === id, viewRef, id };
}

// ── Shape 2: the flat column (StepPicker, ModeSelector) ──────────────────────
/**
 * Navigation for a panel that already has its items in an array. Each item is its
 * own row, so up/down walks the list and Enter activates. Returns the focused
 * INDEX (-1 until the first key press), which the caller uses to style.
 *
 * `reveal` is optional and receives the index, for lists inside a ScrollView.
 */
export function useListNav(
  visible: boolean,
  length: number,
  onActivate: (index: number) => void,
  reveal?: (index: number) => void,
) {
  // ★ Focus is held as an ENTRY ID, because that is what the shared core compares
  // against; the INDEX is derived for the caller. Holding the index here instead
  // silently breaks navigation, since `focusedRef` would never match an entry.
  const [focusedId, setFocusedId] = useState(-1);
  const focusedRef = useRef(-1);
  useEffect(() => { focusedRef.current = focusedId; }, [focusedId]);

  const cb = useRef({ onActivate, reveal });
  cb.current = { onActivate, reveal };

  // Ids are 1:1 with indices here, offset so they never collide with grid ids.
  const base = useRef(0);
  if (base.current === 0) base.current = nextNavButtonId() * 100000;

  const getEntries = useCallback(() => {
    const out: NavEntry[] = [];
    for (let i = 0; i < length; i++) {
      out.push({
        id: base.current + i,
        row: i,
        press:  () => cb.current.onActivate(i),
        reveal: () => cb.current.reveal?.(i),
      });
    }
    return out;
  }, [length]);

  useNavKeys({ visible, getEntries, focusedRef, setFocused: setFocusedId, flat: true });

  const focused = focusedId < 0 ? -1 : focusedId - base.current;

  // A list that shortens while focus sits past its new end (a filtered mode list,
  // a shorter step ladder on a different backend) must not keep focus off the end.
  useEffect(() => {
    if (length > 0 && focused >= length) setFocusedId(base.current + length - 1);
  }, [length, focused]);

  return focused;
}
