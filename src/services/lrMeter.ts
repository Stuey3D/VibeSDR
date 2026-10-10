/**
 * lrMeter — the Advanced RDS panel's LEFT / RIGHT deviation meters, as both clients draw them (2026-10-10, Stuart:
 * "is it worth including especially useful on distorted stations"). The server measures what each channel ALONE puts
 * on the carrier (MpxMeasure::lrMeter_, `lDev`/`rDev` in kHz, a robust 1 s peak); this turns them into bars and words.
 *
 * ★ 0–100 kHz like the deviation bar beside it (1 kHz = 1 %), and the SAME verdict lines: green to 75, amber 75–82 (the
 *   bar's hedge band — two radios disagree by ~7 %), red past 82; the line on each bar at 75. ✗ It first went amber at
 *   67.5 kHz (full modulation for one channel alone) and Heart lit both channels amber directly under a deviation bar
 *   reading "nominal · 74 kHz" (Stuart's screenshot, 2026-10-10) — one panel contradicting itself. A processed station
 *   legitimately runs each channel up there; the meters say which channel is driving the peak, the bar judges it.
 * ★ ONE COPY, BOTH CLIENTS (AdvRdsPanel.tsx, web main.ts). Absent from an older server, or with no stereo to split —
 *   then `null`, and neither client draws the meters at all (never a false zero).
 */
export type LrTone = 'ok' | 'warn' | 'bad' | 'none';
export interface LrSide { pct: number; text: string; tone: LrTone }
/** Where the limit line sits on each bar — the deviation bar's own 75 kHz. */
export const LR_LIMIT_KHZ = 75;

/** The meters with nothing to show: an empty track and a dash — never a false zero. */
export const LR_EMPTY: { l: LrSide; r: LrSide } = { l: { pct: 0, text: '—', tone: 'none' }, r: { pct: 0, text: '—', tone: 'none' } };

/**
 * ★★ STEADY ON A WEAK STATION (Stuart, 2026-10-10: "on weak stations they flash on and off screen"). The server sends
 *  lDev/rDev only while its pilot PLL is tracking, and on a weak signal that drops in and out — so the rows appeared and
 *  vanished, and the panel jumped. Once a reading has been seen (`seen`, kept by the client for the panel's life) the rows
 *  STAY and show LR_EMPTY through a gap. Never seen (an older server, a mono station) — null: not drawn at all.
 */
export function lrDisplay(lDev: number | undefined, rDev: number | undefined, seen: boolean): { l: LrSide; r: LrSide } | null {
  return lrParts(lDev, rDev) ?? (seen ? LR_EMPTY : null);
}

export function lrParts(lDev: number | undefined, rDev: number | undefined): { l: LrSide; r: LrSide } | null {
  if (typeof lDev !== 'number' || typeof rDev !== 'number' || !(lDev >= 0) || !(rDev >= 0)) return null;
  const side = (k: number): LrSide => ({
    pct: Math.max(0, Math.min(100, k)),
    text: `${k.toFixed(0)} kHz`,
    tone: k > 82 ? 'bad' : k > LR_LIMIT_KHZ ? 'warn' : 'ok',
  });
  return { l: side(lDev), r: side(rDev) };
}
