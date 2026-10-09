/**
 * dabCapacity — the multiplex's capacity units as a little bar (Stuart, 2026-10-09: "can we show a little coloured bar
 * for the capacity units another DAB app shows it this way too, keep the readout but add a little bar too please").
 *
 * A DAB multiplex carries 864 capacity units (CU) per CIF (EN 300 401 §6.1). Every sub-channel occupies a run of them
 * (FIG 0/1: start address + size). The bar is the whole 864: each sub-channel faint, the playing one lit, the unused
 * capacity left as the empty track — so a listener sees at a glance how big a share their station gets.
 *
 * ★ ONE COPY, BOTH CLIENTS: DabPanel (app) and web/client/src/main.ts draw from these segments.
 */
export const DAB_CU_TOTAL = 864;

export interface CuSegment { start: number; size: number; current: boolean }

/** Every sub-channel once (services sharing one sub-channel are one segment), clipped to the multiplex, in CU order.
 *  `curSubch` is the playing service's sub-channel (−1/undefined: none lit). */
export function dabCuSegments(
  services: ReadonlyArray<{ subch?: number; cuStart?: number; cuSize?: number }>,
  curSubch: number | undefined,
): CuSegment[] {
  const bySub = new Map<number, CuSegment>();
  for (const s of services) {
    if (s.subch === undefined || s.cuStart === undefined || s.cuSize === undefined) continue;
    if (!(s.cuSize > 0) || s.cuStart < 0 || s.cuStart >= DAB_CU_TOTAL || bySub.has(s.subch)) continue;
    bySub.set(s.subch, { start: s.cuStart, size: Math.min(s.cuSize, DAB_CU_TOTAL - s.cuStart),
                         current: curSubch !== undefined && s.subch === curSubch });
  }
  return [...bySub.values()].sort((a, b) => a.start - b.start);
}

/** Left edge and width of a segment as percentages of the bar. */
export const cuPct = (seg: CuSegment) => ({ left: (seg.start / DAB_CU_TOTAL) * 100, width: (seg.size / DAB_CU_TOTAL) * 100 });
