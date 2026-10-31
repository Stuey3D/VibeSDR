/**
 * ★★★ AUTO FIRST, MANUAL FOR THE SESSION ONLY (Stuart, 2026-10-31: "decoder default to auto when activated, unless a user
 * has set manual settings in the same session"). ONE rule for every decoder that has an AUTO, in the app and the web:
 *
 *   • turning a decoder on starts it in AUTO;
 *   • a manual choice made earlier in THIS session is kept when the decoder is turned on again;
 *   • nothing is written to storage — the next session (an app launch, a page load) starts in AUTO again.
 *
 * The memory is this module's own: it lives as long as the JavaScript does — the app's process (across servers and
 * screens, which remount) or the web page (until it is reloaded). Decoders that use it: RTTY, TIME (station), the text
 * modes (MFSK / OLIVIA / CONTESTIA / THOR) and PAGE. A decoder's own non-AUTO options (CW's view, WEFAX's LPM …) are not
 * AUTO choices and are not affected.
 */
const mem = new Map<string, unknown>();

/** This session's manual choice for `key`, or `auto` (the decoder's AUTO value) when none was made. */
export function sessionChoice<T>(key: string, auto: T): T {
  return mem.has(key) ? (mem.get(key) as T) : auto;
}
/** Remember a choice for the rest of the session. Choosing the AUTO value itself forgets the manual one. */
export function setSessionChoice<T>(key: string, v: T, auto?: T): void {
  if (auto !== undefined && JSON.stringify(v) === JSON.stringify(auto)) mem.delete(key);
  else mem.set(key, v);
}
/** For tests: a new session. */
export function resetDecoderSession(): void { mem.clear(); }
