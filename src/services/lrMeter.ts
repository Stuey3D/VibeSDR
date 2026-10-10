/**
 * lrMeter — the Advanced RDS panel's LEFT / RIGHT deviation meters, as both clients draw them (2026-10-10, Stuart:
 * "is it worth including especially useful on distorted stations"). The server measures what each channel ALONE puts
 * on the carrier (MpxMeasure::lrMeter_, `lDev`/`rDev` in kHz, a robust 1 s peak); this turns them into bars and words.
 *
 * ★ 0–100 kHz like the deviation bar beside it (1 kHz = 1 %). One channel at 67.5 kHz is FULL modulation — the pilot
 *   and RDS take the rest of the 75 — so: green up to 67.5, amber to 75 (at the limit), red past 75 (that channel alone
 *   is over). The line on each bar is at 67.5.
 * ★ ONE COPY, BOTH CLIENTS (AdvRdsPanel.tsx, web main.ts). Absent from an older server, or with no stereo to split —
 *   then `null`, and neither client draws the meters at all (never a false zero).
 */
export type LrTone = 'ok' | 'warn' | 'bad';
export interface LrSide { pct: number; text: string; tone: LrTone }
export const LR_FULL_KHZ = 67.5;

export function lrParts(lDev: number | undefined, rDev: number | undefined): { l: LrSide; r: LrSide } | null {
  if (typeof lDev !== 'number' || typeof rDev !== 'number' || !(lDev >= 0) || !(rDev >= 0)) return null;
  const side = (k: number): LrSide => ({
    pct: Math.max(0, Math.min(100, k)),
    text: `${k.toFixed(0)} kHz`,
    tone: k > 75 ? 'bad' : k > LR_FULL_KHZ ? 'warn' : 'ok',
  });
  return { l: side(lDev), r: side(rDev) };
}
