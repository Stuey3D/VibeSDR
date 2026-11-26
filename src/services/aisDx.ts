/**
 * aisDx.ts — V12 beta95 THE SHIP'S POSITION, BELIEVED OR NOT (the server decides — vibe_ais_service.h kDxNm). ONE RULE,
 * TWO READERS: the web client's tracks.ts and the app's src/services/tracks.ts draw the same words from these fields.
 *
 * ★★★ Stuart, 2026-11-25: a yacht moored in Schiedam plotted in the Gulf of Guinea ("no way that can be right"), then
 *     "i dont want to rule out distance" (Spanish FM in Newcastle on Sporadic-E). So:
 *   · dx      — believed, and further than 75 NM from the receiver: TWO reports agreed. The exciting ones: tagged DX, gold.
 *   · unconf  — one far report, waiting for a second to agree: LISTED (with where it claims, uLat/uLon), never a dot.
 *   · held    — a report that jumped far from the ship's known track: held, the ship stays where it was.
 *   · fakes   — reports with latitude AND longitude on whole minutes (a transponder with no fix): dropped. With no better
 *               position the ship reads "position implausible".
 */
export interface AisDxFields { dx?: number; unconf?: number; uLat?: number; uLon?: number; held?: number; fakes?: number }

const n = (x: unknown): number | undefined => (typeof x === 'number' && Number.isFinite(x) ? x : undefined);
/** The fields off the wire (absent on an older server: every ship reads as before). */
export function aisDxSanitise(v: any): AisDxFields {
  const o: AisDxFields = {};
  if (v?.dx === 1) o.dx = 1;
  if (v?.unconf === 1) {
    const la = n(v.uLat), lo = n(v.uLon);
    o.unconf = 1;
    if (la != null && lo != null && Math.abs(la) <= 90 && Math.abs(lo) <= 180) { o.uLat = la; o.uLon = lo; }
  }
  if (v?.held === 1) o.held = 1;
  const f = n(v?.fakes);
  if (f != null && f > 0) o.fakes = Math.min(1e6, Math.round(f));
  return o;
}

export type AisDxState = 'dx' | 'unconf' | 'implausible' | '';
export function aisDxState(v: AisDxFields & { lat?: number }): AisDxState {
  if (v.dx) return 'dx';
  if (v.lat == null && v.unconf) return 'unconf';
  if (v.lat == null && v.fakes) return 'implausible';
  return '';
}
/** ★ The DX colour (the map's selected-gold): the tag, the range, the icon's halo. */
export const AIS_DX_COLOUR = '#ffd34d';
/** The short word for the list's range column. */
export const AIS_DX_SHORT: Record<Exclude<AisDxState, ''>, string> = { dx: 'DX', unconf: 'DX?', implausible: 'implausible' };
/** The card's words, plain. */
export function aisDxWords(v: AisDxFields & { lat?: number }): string {
  switch (aisDxState(v)) {
    case 'dx': return 'DX — heard far beyond normal AIS range; two reports agree on where it is';
    case 'unconf': return 'Possible DX — one report from far away; it is plotted once a second report agrees';
    case 'implausible': return `Position implausible — ${v.fakes === 1 ? 'its report' : `${v.fakes} reports`} put it exactly on whole minutes, `
      + 'what a transponder with no GPS fix sends; not plotted';
    default: return v.held ? 'One report jumped far from its track — held, not believed, until another agrees' : '';
  }
}
