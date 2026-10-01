/**
 * useScreenCovered — "an OPAQUE full-screen overlay is on top of everything, so nothing under it can
 * be seen". The receiver's own clocks (the waterfall shader, the meters' frame callbacks) read it and
 * stop drawing while it is true.
 *
 * ★★★ WHY. A full-screen `<Modal transparent={false}>` (the map, recordings, About) hides the radio
 *   screen completely, but nothing under it knew: the waterfall kept re-running its full-screen
 *   shader for every row, the trace tween and the LED meters kept their 120 Hz frame callbacks — on
 *   top of the map, which is itself a GPU map (MapLibre in a WebView). Two GPU workloads, one of
 *   them invisible. The power audit of 2026-10-01 (build 357/358 "the phone gets warm").
 * ★★ THE OVERLAY DECLARES IT, not the screen that opens it. The screen does not know which of its
 *   overlays are opaque (MenuSheet, AudioSheet and the decoders are see-through and the waterfall
 *   MUST keep moving behind them — owner rule: it always draws while the radio runs and is visible).
 *   The overlay does — so the overlay calls `useCoversScreen(visible)` and nobody else has to keep a
 *   list. ONE RULE, ONE READER.
 * ★★ A COUNT, not a flag: two overlays can be up at once (About opened over the map), and closing one
 *   must not uncover the other.
 * ★★ COVERING IS DELAYED, UNCOVERING IS NOT (useScreenCovered below). A modal SLIDES in: for its first
 *   ~half second the waterfall is still on screen above it, and freezing it then would be a visible
 *   stall. Closing must resume at once, because the slide-out reveals it immediately.
 */
import { useEffect, useState } from 'react';

let covers = 0;
const subs = new Set<(covered: boolean) => void>();

function set(next: number) {
  const was = covers > 0;
  covers = Math.max(0, next);
  const now = covers > 0;
  if (was !== now) subs.forEach(f => f(now));
}

/** Call from an OPAQUE full-screen overlay with its own `visible`. Never from a see-through one. */
export function useCoversScreen(visible: boolean): void {
  useEffect(() => {
    if (!visible) return;
    set(covers + 1);
    return () => set(covers - 1);
  }, [visible]);
}

/** The slide-in of a full-screen modal (animationType "slide") is ~0.5 s on both platforms; this
 *  waits it out, with margin, before anything under it stops. */
export const COVER_SETTLE_MS = 700;

/** True while an opaque overlay hides the screen — reported COVER_SETTLE_MS after it opens (the
 *  slide-in is still showing what is underneath), and at once when it closes. */
export function useScreenCovered(): boolean {
  const [covered, setCovered] = useState(false);
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | null = null;
    const apply = (now: boolean) => {
      if (timer) { clearTimeout(timer); timer = null; }
      if (now) timer = setTimeout(() => { timer = null; setCovered(true); }, COVER_SETTLE_MS);
      else setCovered(false);
    };
    apply(covers > 0);
    subs.add(apply);
    return () => { subs.delete(apply); if (timer) clearTimeout(timer); };
  }, []);
  return covered;
}
