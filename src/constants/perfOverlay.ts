/**
 * ★★★ THE B19 PERF OVERLAY — THIS BUILD ONLY (Stuart, 2026-10-02: "add a cpu/gpu/ram overlay for this build only
 * I will toggle the setting and check the figures"). It lets the tilt light be judged on a real iPhone by
 * toggling MOTION EFFECTS and watching the numbers (src/components/PerfOverlay.tsx).
 *
 * ▶▶ SET THIS TO false FOR THE NEXT BUILD. Nothing else needs touching: with it false the overlay never mounts,
 *    the native frame clock never starts, and nothing is polled.
 */
export const PERF_OVERLAY_THIS_BUILD = false;

/** One line of the overlay from the native stats + the tilt counters. Pure, so it is testable. */
export function perfLines(s: { cpuPct: number; footprintMB: number; uiFps: number; uiP50Ms: number; uiP90Ms: number },
                          tilt: { running: string; writesPerSec: number; rendersPerSec: number }): string[] {
  const f = (n: number, d = 0) => (Number.isFinite(n) ? n.toFixed(d) : '—');
  return [
    `CPU ${f(s.cpuPct)}%  RAM ${f(s.footprintMB)} MB`,
    `UI ${f(s.uiFps)}fps p50 ${f(s.uiP50Ms, 1)} p90 ${f(s.uiP90Ms, 1)} ms`,
    `TILT ${tilt.running || 'off'}  ${f(tilt.writesPerSec)} w/s  ${f(tilt.rendersPerSec)} renders/s`,
  ];
}
