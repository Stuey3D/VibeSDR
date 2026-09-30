/**
 * domeClick.ts — WHEN a dome key clicks (faceplates brief §5). Pure: no React, no native modules,
 * so scripts/test_faceplate.ts can drive it with a fake clock.
 *
 * "Surface mount clicky buttons with no full push-in state … the metal dome underneath that pushes
 * down and clicks." A snap dome resists, then collapses; the click is the COLLAPSE, not the touch.
 *
 * ★★ THE PRESS CLICK IS 45 ms AFTER TOUCH-DOWN, DELIBERATELY — at the END of the press curve
 *   (`cubic-bezier(0.9, 0, 1, 0.6)`, which barely moves and then drops). Do not "fix" it to fire
 *   on touch-down (§5 TRAP): a click before the cap has moved reads as a software tick.
 * ★ A tap shorter than the snap still clicks twice. The press click fires the moment the finger
 *   lifts, and the release click one release-curve (35 ms) later, so the two stay distinct — a real
 *   dome cannot be released without first having collapsed.
 */

export const DOME_PRESS_MS   = 45;
export const DOME_RELEASE_MS = 35;
/** §5 curves, as cubic-bezier control points (x1, y1, x2, y2). */
export const DOME_PRESS_BEZIER:   [number, number, number, number] = [0.9, 0, 1, 0.6];
export const DOME_RELEASE_BEZIER: [number, number, number, number] = [0.2, 0.9, 0.3, 1.4];

type Timer = unknown;

export interface DomeClickDeps {
  press:    () => void;
  release:  () => void;
  setTimer: (ms: number, f: () => void) => Timer;
  clearTimer: (t: Timer) => void;
  /** Optional: how late the press click fired, in ms (the §5 TRAP asks us to check it on the Xcover). */
  onLate?:  (lateMs: number) => void;
  now?:     () => number;
}

export function createDomeClick(d: DomeClickDeps) {
  let pressT: Timer | null = null;
  let releaseT: Timer | null = null;
  let armedAt = 0;
  // ★ One release per press: TunerKeys ends a press on onPressOut AND onTouchCancel, and a second
  //   release click for the same press would be a click with no key under it.
  let held = false;

  const flush = () => {
    if (releaseT != null) { d.clearTimer(releaseT); releaseT = null; d.release(); }
  };

  const down = () => {
    // A new press while the last release click is still queued: let it land first, in order.
    flush();
    if (pressT != null) { d.clearTimer(pressT); pressT = null; }
    held = true;
    armedAt = d.now ? d.now() : 0;
    pressT = d.setTimer(DOME_PRESS_MS, () => {
      pressT = null;
      if (d.onLate && d.now) d.onLate(d.now() - armedAt - DOME_PRESS_MS);
      d.press();
    });
  };

  const up = () => {
    if (!held) return;
    held = false;
    if (pressT != null) {
      // Lifted before the dome collapsed: collapse now, spring back one release-curve later.
      d.clearTimer(pressT); pressT = null;
      d.press();
      releaseT = d.setTimer(DOME_RELEASE_MS, () => { releaseT = null; d.release(); });
      return;
    }
    d.release();
  };

  /** Unmount: drop anything queued without clicking. */
  const dispose = () => {
    held = false;
    if (pressT != null)   { d.clearTimer(pressT); pressT = null; }
    if (releaseT != null) { d.clearTimer(releaseT); releaseT = null; }
  };

  return { down, up, dispose };
}
