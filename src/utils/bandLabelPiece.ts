/**
 * bandLabelPiece.ts — ★★ V12 beta95 THE BAND-PLAN STRIP'S LABEL ROOM (Stuart, 2026-11-25, 225.648 MHz: "DAB / DAB+ (Band
 * III)" and "UHF Military Airband" printed on top of each other). Segments are drawn widest first (BAND_PLAN_DRAW_ORDER),
 * so a wide band's bar lies under every narrower band on it; its label belongs only in the part still showing — its span
 * less every LATER segment, the widest such piece ([x0, x0] = no room). The app's WaterfallView has the same rule (main
 * 35459577a); the web strip (main.ts drawBands) uses this.
 */
export function bandLabelPieces(segs: ReadonlyArray<{ x0: number; x1: number }>): Array<[number, number]> {
  return segs.map((sg, i) => {
    let pieces: Array<[number, number]> = [[sg.x0, sg.x1]];
    for (let j = i + 1; j < segs.length; j++) {
      const o = segs[j];
      pieces = pieces.flatMap(([a, z]): Array<[number, number]> => (o.x1 <= a || o.x0 >= z ? [[a, z]]
        : ([[a, Math.min(z, o.x0)], [Math.max(a, o.x1), z]] as Array<[number, number]>).filter(([p, q]) => q - p > 0)));
    }
    return pieces.reduce<[number, number] | null>((m, pc) => (!m || pc[1] - pc[0] > m[1] - m[0] ? pc : m), null) ?? [sg.x0, sg.x0];
  });
}
