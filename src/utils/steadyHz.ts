/* steadyHz.ts — the display refresh to pin while the waterfall's rows flow (services/frameRate.ts setSteadyRefresh).
 *  PURE, so scripts/test_frame_rate.ts can run it. */

/** The refresh to pin while rows flow (Stuart, 2026-10-28: "only ramp up the FPS on interaction and then when the
 *  waterfall/spectrum is running slow the Pro Motion down to 30fps or whatever the spectrum etc runs at").
 *  ★★★ THE SAME RATE ON A GESTURE (Stuart, 2026-10-28, on build 508's 40 → 60 Hz switch while tuning: "dont change the
 *    waterfall or spectrum frame rate on interaction it is resulting in an awkward judder as it settles and looks less
 *    smooth"). One rate while rows flow: the lowest panel rate from 30 (the trace tween's own rate)
 *  that is a WHOLE MULTIPLE of the feed — every row the same number of refreshes, or rows land on an uneven beat (the
 *  judder) — 10/15/30 fps → 30, 20 → 40, 40 → 40; anything else (25, a wandering feed) → 60. PURE. */
export const STEADY_RATES = [30, 40, 60];
/** The server's frame rates: a measured feed (19.6, 20.4…) is snapped to the nearest within 8 % first, so the pick does
 *  not flip between rates on arrival jitter. */
export const FEED_RATES = [5, 10, 15, 20, 25, 30, 40, 50, 60];
export function steadyHzFor(feedFps: number): number {
  if (!(feedFps > 0)) return 60;
  const near = FEED_RATES.reduce((b, f) => (Math.abs(f - feedFps) < Math.abs(b - feedFps) ? f : b), FEED_RATES[0]);
  if (Math.abs(near - feedFps) > near * 0.08) return 60;
  for (const f of STEADY_RATES) if (f >= near && f % near === 0) return f;
  return 60;
}
