/**
 * ★★ "YOU CAN REJOIN IN …" — ONE WORDING, TWO READERS (the app and the web page).
 *
 * The server's `kicked` message carries `secs`: how long until this session may connect again. It
 * comes both with the owner's DISCONNECT and with every refused reconnect during the cool-off, so
 * the number is always the time LEFT, never the length the owner picked.
 * ★ Rounded UP: "rejoin in 0 minutes" with a minute to go would send them straight into a refusal.
 */
export function rejoinIn(secs: number): string {
  if (!(secs > 0)) return 'a few minutes';
  const mins = Math.ceil(secs / 60);
  if (mins < 90) return `${mins} minute${mins === 1 ? '' : 's'}`;
  const h = Math.floor(mins / 60), m = mins % 60;
  // ★ Hours and minutes past 90: "300 minutes" makes the reader do the sum.
  return `${h} hour${h === 1 ? '' : 's'}${m ? ` ${m} minute${m === 1 ? '' : 's'}` : ''}`;
}

/** The sentence Stuart asked for (2026-11-01). */
export function kickedSentence(secs: number): string {
  return `Disconnected by the server owner. You can rejoin in ${rejoinIn(secs)}.`;
}
