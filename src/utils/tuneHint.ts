/**
 * tuneHint — RTTY AUTO's tuning guide as the decoder header shows it (Stuart, 2026-10-04: "something like < 100Hz or
 * 500Hz >"). The server says how far the tones should move in AUDIO pitch; which way the DIAL goes depends on the
 * sideband: in USB (and CW-U, and anything else upper) the audio is RF − dial, so raising the pitch means lowering the
 * dial; in LSB / CW-L the reverse. The arrow points the way to turn the dial on its own scale (◀ lower, ▶ higher).
 * Shared by the app and the web client.
 */
export function tuneHintLabel(audioHz: number, mode: string): string {
  if (!audioHz) return '';
  const lower = /^(lsb|cwl|cw-l)$/i.test(mode.trim());
  const dial = lower ? audioHz : -audioHz;
  const hz = Math.abs(dial);
  const amt = hz >= 1000 ? `${(hz / 1000).toFixed(1)} kHz` : `${hz} Hz`;
  return dial < 0 ? `tune ◀ ${amt}` : `tune ${amt} ▶`;
}
