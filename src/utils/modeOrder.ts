/**
 * ★★★ ONE ORDER FOR THE DEMODULATOR KEYS, EVERY CLIENT (Stuart, 2026-11-20: "the digimodes need to drop down in order here.
 * wfm nfm am lsb usb cwl cwu sam ecss then digital modes").
 *
 * The lists used to be built by INSERTING each extra beside its parent (M17 and DV ID after NFM, the SAM family after AM,
 * FreeDV after LSB), which scattered the digital modes through the analogue ones. Now the analogue demodulators come first
 * in the listening order above, the synchronous AM family after them, then the digital voice / broadcast modes. Anything
 * not named here (an OpenWebRX's own extras) keeps its place after them, in the order it arrived. Shared by the web
 * client (mobile.ts menu via main.ts offeredModes) and the app (ModeSelector's main keys).
 */
export const MODE_ORDER: readonly string[] = [
  'wfm', 'fm', 'nfm', 'am', 'lsb', 'usb', 'cwl', 'cwu', 'cw',
  'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst',
  // ── digital ──
  'drm', 'freedv', 'm17', 'dvsig', 'data',
];

/** The list in MODE_ORDER; unknown ids after the known ones, in their own order (a stable sort). */
export function sortModes<T>(list: readonly T[], id: (m: T) => string = (m) => String(m)): T[] {
  const rank = (m: T) => { const i = MODE_ORDER.indexOf(id(m).toLowerCase()); return i < 0 ? MODE_ORDER.length : i; };
  return list.map((m, i) => ({ m, i })).sort((a, b) => rank(a.m) - rank(b.m) || a.i - b.i).map((x) => x.m);
}
