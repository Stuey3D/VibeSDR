/**
 * MPX power (ITU-R BS.412) as both clients draw it — one formatter, so the app and the web panel say
 * the same words (one rule, two readers).
 *
 * ★★ "SETTLING", COUNTING DOWN (Stuart, 2026-10-02: "clarify the seconds — have it say settling 59 58
 *    57…"). The figure is a 60 s mean, and until the minute is full it is a younger mean that still
 *    moves with whatever passage is playing. "(16 s)" read as a duration nobody asked about; a countdown
 *    to the figure being a full minute says what the number is waiting for and when it will be steady.
 *
 * Pure: no React Native, no DOM.
 */

/** The full window, seconds — the server's ring (mpxmeasure.cpp) is 60 s of 50 ms windows. */
export const MPX_POWER_WINDOW_S = 60;

/** The figure and the countdown, SEPARATELY: both panels put the value in their fixed figures column and
 *  the countdown on its own line under it — the web's block lives in a 180 px plot column, where one long
 *  string would be clipped at the panel edge.
 *  `secs` = seconds the server's mean covers (0 = none yet, or an older server).
 *    value    "+6.6 dB", or "—" with nothing yet
 *    settling "settling 44 s" while the minute fills, "" once it is full (or with nothing yet) */
export function mpxPowerParts(db: number, secs: number): { value: string; settling: string } {
  if (!(secs > 0) || !Number.isFinite(db)) return { value: '—', settling: '' };
  const value = `${db >= 0 ? '+' : '−'}${Math.abs(db).toFixed(1)} dB`;
  const left = Math.ceil(MPX_POWER_WINDOW_S - secs - 0.5);
  return { value, settling: left > 0 ? `settling ${left} s` : '' };
}
