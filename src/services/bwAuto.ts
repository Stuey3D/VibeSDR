/**
 * ★★ V12 THE AUTO KEY BY THE PASSBAND SLIDERS (docs/v12/AM-SYNC.md §11) — shared by the app (ModeSelector) and the
 * web client's bandwidth row. Pure functions; no React, no DOM.
 *
 * ★★★ NOTHING HERE DECIDES WHERE AUTO EXISTS. The server lists the modes it can act on (hwinfo `autoBwModes`) and
 *     says what it has done (`{"type":"bwauto"}`); a client draws the key only where that list names the current
 *     mode, and draws AUTO's edges only while the server says it is cutting (feedback_app_no_hardcoded_settings).
 *     An older server lists nothing, so the row is exactly the one it always was: two sliders and SYNC.
 */

/** What the server says AUTO has done to this listener's passband. lo/hi in the sliders' own coordinates (Hz from the
 *  dial); null = that edge is the listener's own. */
export interface BwAutoState { on: boolean; cut: boolean; lo: number | null; hi: number | null }

/** hwinfo `autoBwModes` → a clean list of lower-case mode ids, or [] (absent, malformed, an older server). */
export function parseAutoBwModes(v: unknown): string[] {
  if (!Array.isArray(v)) return [];
  return v.filter((m): m is string => typeof m === 'string' && /^[a-z0-9]{1,8}$/i.test(m)).map((m) => m.toLowerCase()).slice(0, 32);
}

const edge = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) && Math.abs(v) <= 500_000 ? Math.round(v) : null);

/** The `bwauto` message → state, or null if it is not one. */
export function parseBwAuto(o: unknown): BwAutoState | null {
  const m = o as Record<string, unknown> | null;
  if (!m || m.type !== 'bwauto' || typeof m.on !== 'boolean') return null;
  const cut = m.cut === true;
  return { on: m.on, cut, lo: cut ? edge(m.lo) : null, hi: cut ? edge(m.hi) : null };
}

/** Is the AUTO key drawn for this mode? Only where the server listed it. */
export function autoBwOffered(modes: readonly string[] | null | undefined, mode: unknown): boolean {
  return !!modes && modes.length > 0 && modes.includes(String(mode ?? '').toLowerCase());
}

/** The edges the sliders SHOW: AUTO's while it is on and cutting, otherwise the listener's own. Display only — these
 *  are never sent back as a bandwidth (the server set them; the client has no vote). */
export function shownEdges(own: { low: number; high: number }, autoOn: boolean, st: BwAutoState | null): { low: number; high: number } {
  if (!autoOn || !st || !st.on || !st.cut) return own;
  const low = st.lo ?? own.low, high = st.hi ?? own.high;
  return high > low ? { low, high } : own;
}
