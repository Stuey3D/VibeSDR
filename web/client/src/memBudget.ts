/**
 * memBudget.ts — how many BYTES this browser may spend on history it keeps beyond the screen.
 *
 * ★★★ BYTES, NOT ROWS (2026-11-01). M7EIW's Pi 3B+ (1 GB) ran out of memory in Chromium on its own desktop when he
 *     opened Setup, and handheld consoles (Anbernic and the like: 512 MB–2 GB, 640×480 to 1280×720) will run this page
 *     too. Measured in headless Edge, 11.0.16 is no bigger than 11.0.15 here — but the waterfall's GPU ring was a fixed
 *     2560 rows whatever the machine (five screens of history on a 480-line handheld), and the 2D path's full-capture
 *     layer is a 2048-wide RGBA canvas as tall as the waterfall. Each now asks for a byte budget from here.
 *
 * WHERE THE BUDGET COMES FROM:
 *   · navigator.deviceMemory (Chromium; GB, rounded down to a power of two, so a 1 GB Pi says 1 and a 512 MB board 0.5).
 *   · Not there (Firefox, Safari): the SCREEN. A small screen is the handheld/SBC case, so it gets the small budget; a
 *     desktop-sized screen keeps today's history. Unknown is never a reason to cut a big machine's history.
 * ★ The display itself is never cut: whatever the budget, the waterfall still fills the screen (callers clamp to it).
 */

export type MemClass = 'tiny' | 'small' | 'medium' | 'full';

export interface MemSignals { deviceMemory?: number; screenW?: number; screenH?: number }

/** Pure, for the tests: the class from what the browser says. */
export function memClassOf(s: MemSignals): MemClass {
  const dm = s.deviceMemory;
  if (typeof dm === 'number' && dm > 0) {
    if (dm <= 0.5) return 'tiny';
    if (dm <= 1) return 'small';
    if (dm <= 2) return 'medium';
    return 'full';
  }
  const w = s.screenW || 0, h = s.screenH || 0;
  // ★ 1280×800 and below (either orientation) = a handheld, a small SBC panel or a phone: assume little memory.
  if (w > 0 && h > 0 && Math.max(w, h) <= 1280 && Math.min(w, h) <= 800) return 'small';
  return 'full';
}

/** Bytes of off-screen waterfall history per class. 'full' = the old fixed ring (2560 rows ≈ 5.4 MB) and some room. */
export const HISTORY_BUDGET: Record<MemClass, number> = {
  tiny: 1 << 20,          // 1 MB  (~480 GPU rows)
  small: 2 << 20,         // 2 MB  (~970 GPU rows)
  medium: 4 << 20,        // 4 MB  (~1950 GPU rows)
  full: 8 << 20,          // 8 MB  (the 2560-row ring fits)
};

let cached: MemClass | null = null;
/** This browser's class, read once. `?mem=tiny|small|medium|full` overrides it (measurement and support only). */
export function memClass(): MemClass {
  if (cached) return cached;
  try {
    const q = typeof location !== 'undefined' ? new URLSearchParams(location.search).get('mem') : null;
    if (q === 'tiny' || q === 'small' || q === 'medium' || q === 'full') return (cached = q);
    const nav = navigator as Navigator & { deviceMemory?: number };
    cached = memClassOf({
      deviceMemory: typeof nav.deviceMemory === 'number' ? nav.deviceMemory : undefined,
      screenW: typeof screen !== 'undefined' ? screen.width : 0,
      screenH: typeof screen !== 'undefined' ? screen.height : 0,
    });
  } catch { cached = 'full'; }
  return cached;
}

export function historyBudgetBytes(): number { return HISTORY_BUDGET[memClass()]; }

/** Rows of history a ring of `bytesPerRow` may hold: the budget's worth, never fewer than the display (`minRows`), never
 *  more than `maxRows` (the old fixed ring). */
export function ringRowsFor(bytesPerRow: number, minRows: number, maxRows: number, budget = historyBudgetBytes()): number {
  const want = Math.floor(budget / Math.max(1, bytesPerRow));
  return Math.max(minRows, Math.min(maxRows, want));
}
