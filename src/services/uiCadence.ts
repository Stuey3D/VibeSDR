/**
 * uiCadence — the app's own frame pacing under a FRAME RATE cap (constants/faceplate.ts FrameRate).
 *
 * ★★★ WHY THE APP PACES ITSELF. On iOS the cap is applied to every CADisplayLink natively (VibeLocalSDR.mm), so
 *   every frame callback simply runs less often. Android has no equivalent: a window can ask for a DISPLAY MODE, and
 *   phones list 60 / 90 / 120 Hz modes but practically never 30 — so on Android a 30 cap (and a 60 cap on a panel
 *   with no 60 mode) has to be honoured by the app: the per-frame worklets that make the canvases redraw (the
 *   waterfall's engine poll and glide, the spectrum tween, the LED and needle meters) run on every OTHER vsync.
 * ★★ ON THE VSYNC, NEVER BETWEEN: a frame is skipped only if it comes sooner than (1000 / cap − 4) ms after the last
 *   one that ran, measured with the vsync timestamps the callbacks are given. So a 30 cap on a 60 Hz panel runs
 *   exactly every second vsync — evenly paced, which is the whole point (Stuart: "I'd take a better frame paced 30
 *   over an erratic 40 or 50"). On iOS under the same cap the display link already runs at 30, nothing is ever
 *   sooner than the threshold, and this never skips — the two mechanisms cannot fight.
 * ★ The gap returned is the time since this callback last RAN — what timeSincePreviousFrame used to say — so a
 *   time-based step (a meter's ballistics, the trace tween's 33 ms accumulator) covers the frames it skipped.
 */
import { makeMutable, type SharedValue } from 'react-native-reanimated';

/** The cap the frame callbacks honour, Hz; 0 = none. Set by FaceplateContext from the FRAME RATE setting. */
export const uiFrameCapHz = makeMutable(0);

/**
 * Call first in a frame callback with its own `last` (a SharedValue<number> starting at 0) and the frame's timestamp.
 * Returns -1 when this frame must be SKIPPED, else the ms since the callback last ran (16 on its first run).
 */
export function cadenceGap(last: SharedValue<number>, ts: number): number {
  'worklet';
  const prev = last.value;
  const cap = uiFrameCapHz.value;
  if (cap > 0 && prev > 0 && ts - prev < 1000 / cap - 4) return -1;
  last.value = ts;
  return prev > 0 ? ts - prev : 16;
}
