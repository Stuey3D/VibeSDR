/* wfRingSize.ts — HOW MANY ROWS THE APP'S WATERFALL HISTORY RING HOLDS (the iOS/iPad/Mac JS path and the Android engine).
 *
 * ★★★ Stuart, 2026-10-30: "the history only goes when it genuinely goes off screen" — tune away and back, at 5 fps, on a
 *     tall portrait monitor: the history must still be there until it scrolls off the bottom. The ring was a FIXED 1024
 *     rows (one row per point), so a waterfall taller than 1024 pt — an iPad Pro, a Mac window on a pivoted 4K screen
 *     ("a 3840x2160p screen that is on a pivot could do 3840 rows") — could not be filled: the rows grew thicker instead.
 *
 * ★★★ GROW-ONLY. The ring starts at RING_MIN_ROWS (1024, as before) and grows whenever the waterfall becomes taller than
 *     it holds (a resize, a move to a bigger screen, a rotation) — COPYING every row it has into the new ring
 *     (utils/wfRingGrow.ts), so nothing is lost and the picture neither jumps nor blanks. It NEVER shrinks during a
 *     session: a smaller window keeps the history it had (the hero feature — the waterfall persists through a resize).
 *
 * ★★★ CAPPED BY THE DEVICE'S RAM ("hardware info tells us how much ram is available"), read once by the existing
 *     deviceClass() getter (iOS ProcessInfo.physicalMemory, Android ActivityManager.MemoryInfo.totalMem + isLowRamDevice):
 *       • under ~2 GB, or Android's isLowRamDevice → 1024 rows — EXACTLY today's ring: the 1 GB 32-bit phones pay nothing;
 *       • 2–4 GB → 2048;   • 4 GB and up → 4096.
 *     ★★ UNKNOWN RAM IS THE LOW TIER (Stuart, 2026-10-30: "if a ram figure cannot be detected for whatever reason, assume
 *        the worst and default to the low ram mode") — an old binary without the getter, a failed read, 0, NaN → 1024.
 *     ★ The OS reports a little under the marketed size (a "4 GB" phone ≈ 3.6–3.8 GiB, a "2 GB" one ≈ 1.8 GiB), so the
 *       lines sit at 1.5 GiB and 3.5 GiB — the marketed class, as transparency.ts does for its 3.5 GiB line.
 *
 * ★ THE TRADE-OFF (RAM ↔ nothing else): a ring row is RING_W = 1024 bytes, so the ring is 1 MiB per 1024 rows — 4 MiB at
 *   4096. While a zoom eases the displayed ring doubles it (8 MiB at 4096), the per-row maps/motion are ~40 B a row
 *   (160 KiB at 4096) and the full-capture layer ~0.8 KiB a row once a V12 server sends it (3.2 MiB at 4096). The image
 *   made each row is the ring's size too (4 MiB at 4096 — retired 300 ms later). CPU is unchanged per row; a zoom's fan
 *   regenerates only the rows ON SCREEN, which on a 3840-row view is ~3.75× a phone's. The GPU samples the same number of
 *   texels per pixel. Only a view taller than 1024 rows ever grows the ring — a phone never does.
 *
 * Pure: no React, no imports — scripts/test_wf_ring_grow.ts drives every rule.
 */

/** Today's ring, and the floor of every ring: no device ever holds fewer rows. */
export const RING_MIN_ROWS = 1024;
/** Growth is in whole steps of this many rows (fewer, larger copies while a window is dragged taller). */
export const RING_GROW_STEP = 256;
/** Rows kept beyond what the view shows: the shader's blend row, the pacer's few rows in hand, and the bottom edge. */
export const RING_VIEW_MARGIN = 16;
const GIB = 1024 * 1024 * 1024;
/** The marketed-class lines (see the header): under LOW → the low tier; under MID → 2048; else 4096. */
export const RING_TIER_LOW_BYTES = 1.5 * GIB;
export const RING_TIER_MID_BYTES = 3.5 * GIB;

/** The most rows this device's ring may ever hold. Unknown / unreadable RAM → the LOW tier (assume the worst). */
export function ringTierRows(totalMemoryBytes: number | null | undefined, isLowRamDevice = false): number {
  if (isLowRamDevice) return RING_MIN_ROWS;
  if (typeof totalMemoryBytes !== 'number' || !Number.isFinite(totalMemoryBytes) || totalMemoryBytes <= 0) return RING_MIN_ROWS;
  if (totalMemoryBytes < RING_TIER_LOW_BYTES) return RING_MIN_ROWS;
  if (totalMemoryBytes < RING_TIER_MID_BYTES) return 2048;
  return 4096;
}

/** The rows a view of `viewRows` ring rows needs: the view + the margin, rounded up to a whole growth step. */
export function ringRowsNeeded(viewRows: number): number {
  if (!(viewRows > 0) || !Number.isFinite(viewRows)) return RING_MIN_ROWS;
  return Math.max(RING_MIN_ROWS, Math.ceil((Math.ceil(viewRows) + RING_VIEW_MARGIN) / RING_GROW_STEP) * RING_GROW_STEP);
}

/** The ring's NEXT size: grow-only (never below `current`), never above `cap` (unless `current` already is), and never
 *  above what the view needs + the margin. `viewRows` = rows the view would show at its natural row height. */
export function ringNextRows(current: number, viewRows: number, cap: number): number {
  const cur = Math.max(RING_MIN_ROWS, current | 0);
  const want = Math.min(Math.max(RING_MIN_ROWS, cap | 0), ringRowsNeeded(viewRows));
  return want > cur ? want : cur;
}

/** The cap from what the native deviceClass() getter handed back (services/deviceClass.ts) — { totalMemoryBytes,
 *  isLowRamDevice? }. Anything missing or malformed (an old binary, a refused sync call, web, tests) → the low tier. */
export function ringCapFromDeviceClass(raw: unknown): number {
  const o = (raw && typeof raw === 'object' ? raw : {}) as Record<string, unknown>;
  const mem = typeof o.totalMemoryBytes === 'number' ? o.totalMemoryBytes : null;
  return ringTierRows(mem, o.isLowRamDevice === true);
}
