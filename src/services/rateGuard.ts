/**
 * ★★★ A RADIO THAT FELL OVER AT A RATE A MODE BOOSTED TO (Stuart, 2026-10-24) — the app's reader of the server's
 * record (vibe_rate_guard.h RateFault: "stalled" or "crashed", the rate, the mode). beta95: one stop is only noted; a
 * second within 24 h holds the boosts one rung down (`latched`, `ceiling`) until a retry after 24 h or the owner's Clear.
 * The OWNER'S switch is never changed by it — the screen shows their choice and draws this line beside it.
 * ★★ ONE RULE, TWO READERS: vibe_setup_page.h rateGuardLine is the same — scripts/test_capture_and_rate_guard.ts runs both.
 * Pure: no React, no native module.
 */
export type RateGuardState = {
  fault?: '' | 'stalled' | 'crashed'; at?: number; rate?: number; mode?: string; blocked?: boolean; say?: string;
  latched?: boolean; ceiling?: number; noted?: number; probation?: boolean; retried?: boolean; retryAt?: number;
};

export function rateGuardLine(st: RateGuardState | null | undefined, fmt: (d: Date) => string = (d) => d.toLocaleString()): string {
  if (!st || !st.fault) return '';
  const t = st.at && st.at > 0 ? new Date(st.at * 1000) : null;
  return (st.say || '') + (t && Number.isFinite(t.getTime()) ? ` (${fmt(t)})` : '');
}
