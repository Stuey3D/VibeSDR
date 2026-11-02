// navtexWind.ts — V12 2026-11-02 THE SHIPPING MAP's WEATHER ARROWS: what the server read from each sea area's forecast
// (vibe_navtex_met.h windOf — "wind" in navtex-met.json) drawn as an arrow at the area's label point. ONE RULE, TWO READERS:
// the web client (web/client/src/main.ts, through navtexWarn.ts navMetIcon) and the app (MapOverlay, the same function).
//
// ★★ Stuart (2026-11-02): "is it possible to have arrows in the direction of the weather/sea state so E 4-6 have the arrow
//    pointing and being bigger or smaller and red for severe that way it looks even more like a full shipping weather and
//    hazard breakdown."
//  · The arrow points DOWNWIND — the way the wind blows ("SW 6-8" blows towards the north-east), as on a chart.
//  · Its length and weight follow the Beaufort force (the upper bound of the forecast's range); colour: the map's ink
//    below 7, AMBER at 7, RED at gale 8 and above (and for a gale word the arrows do not carry).
//  · VARIABLE and CYCLONIC have no direction: a distinct ring (variable) or a curling arrow (cyclonic) — never a guess.
//  · A change ("VEERING W LATER", "BACKING SE 5 TO 7", "BECOMING …") gives a FAINTER, dashed second arrow for the later
//    wind, drawn behind the first.
//  · A compact label beside it: the force and the sea state ("6–8 · ROUGH"), the area's name above.

export interface NavWind {
  k: 'a' | 'v' | 'c' | 'n';   // arrow · variable · cyclonic · no direction known (force only)
  bf: number; sev: 0 | 1 | 2; from: number; to: number;
  chg: '' | 'veer' | 'back' | 'becoming';
  k2: '' | 'a' | 'v' | 'c'; bf2: number; sev2: 0 | 1 | 2; from2: number; to2: number;
}
const num = (v: unknown): number | null => { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; };
const deg = (v: unknown): number => { const n = num(v); return n != null && n >= 0 && n < 360 ? n : -1; };
const sevOf = (v: unknown): 0 | 1 | 2 => (v === 2 ? 2 : v === 1 ? 1 : 0);
/** ★ What the server sent, cut to its fields (a broken server must not put an object where a number goes). */
export function navWindSanitise(w: any): NavWind | null {
  if (!w || typeof w !== 'object') return null;
  const k = w.k === 'a' || w.k === 'v' || w.k === 'c' || w.k === 'n' ? w.k : null;
  if (!k) return null;
  const bf = Math.max(-1, Math.min(12, Math.round(num(w.bf) ?? -1)));
  const k2 = w.k2 === 'a' || w.k2 === 'v' || w.k2 === 'c' ? w.k2 : '';
  const out: NavWind = {
    k, bf, sev: sevOf(w.sev), from: deg(w.from), to: deg(w.to),
    chg: w.chg === 'veer' || w.chg === 'back' || w.chg === 'becoming' ? w.chg : '',
    k2, bf2: Math.max(-1, Math.min(12, Math.round(num(w.bf2) ?? -1))), sev2: sevOf(w.sev2), from2: deg(w.from2), to2: deg(w.to2),
  };
  if (out.k === 'a' && out.to < 0) out.k = 'n';
  if (out.k2 === 'a' && out.to2 < 0) out.k2 = '';
  return out;
}
/** ★ The colour of an arrow: the map's ink · AMBER at force 7 · RED at gale 8+. */
export const WIND_COLOURS = ['#cfe6ff', '#ffb74d', '#ff5a4a'] as const;
export function windColour(sev: 0 | 1 | 2): string { return WIND_COLOURS[sev] ?? WIND_COLOURS[0]; }
/** ★ Length (px) and stroke weight by force: 16 px at calm, 52 px at hurricane force — a near gale is plainly bigger than a breeze. */
export function windLength(bf: number): number { return Math.round(16 + 3 * Math.max(0, Math.min(12, bf < 0 ? 3 : bf))); }
export function windWeight(bf: number): number { return Math.round((1.6 + 0.28 * Math.max(0, Math.min(12, bf < 0 ? 3 : bf))) * 10) / 10; }
/** The box the arrows are drawn in: square, the longest arrow fits whichever way it points. */
export function windBox(w: NavWind): number { return Math.max(windLength(w.bf), w.k2 ? windLength(w.bf2) : 0) + 8; }

/** One arrow in an S×S box, centred, pointing `to` (degrees, 0 = north, clockwise — the screen's up is north). */
function arrowPath(S: number, to: number, bf: number, colour: string, faint: boolean): string {
  const L = windLength(bf), sw = windWeight(bf), c = S / 2;
  const head = Math.max(6, sw * 3.2);
  // drawn pointing UP (towards y = c − L/2), then rotated about the centre
  const y0 = c + L / 2, y1 = c - L / 2;
  const shaft = `<line x1="${c}" y1="${y0}" x2="${c}" y2="${y1 + head * 0.6}" stroke="${colour}" stroke-width="${sw}" stroke-linecap="round"${faint ? ' stroke-dasharray="3 3"' : ''}/>`;
  const tip = `<path d="M${c} ${y1} L${c - head * 0.62} ${y1 + head} L${c + head * 0.62} ${y1 + head} Z" fill="${colour}"/>`;
  // ★ a dark under-stroke so the arrow reads over a light basemap and the sea alike
  const halo = `<line x1="${c}" y1="${y0}" x2="${c}" y2="${y1 + head * 0.6}" stroke="#0b1420" stroke-opacity=".55" stroke-width="${sw + 2}" stroke-linecap="round"/>`;
  return `<g transform="rotate(${Math.round(to * 10) / 10} ${c} ${c})" opacity="${faint ? 0.5 : 1}">${faint ? '' : halo}${shaft}${tip}</g>`;
}
/** VARIABLE: a ring of short dashes, its weight by force. */
function variableGlyph(S: number, bf: number, colour: string, faint: boolean): string {
  const c = S / 2, r = Math.max(6, windLength(bf) / 2 - 4);
  return `<circle cx="${c}" cy="${c}" r="${r}" fill="none" stroke="${colour}" stroke-width="${windWeight(bf)}" stroke-dasharray="3 3" opacity="${faint ? 0.5 : 1}"/>`
    + `<text x="${c}" y="${c + 3}" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="8" font-weight="700" fill="${colour}" opacity="${faint ? 0.5 : 1}">VRB</text>`;
}
/** CYCLONIC: a curling arrow (three quarters of a circle and its head) — the wind goes round, it has no one direction. */
function cyclonicGlyph(S: number, bf: number, colour: string, faint: boolean): string {
  const c = S / 2, r = Math.max(6, windLength(bf) / 2 - 4), sw = windWeight(bf);
  // anticlockwise (as a northern-hemisphere depression turns), from the east round to the south
  const x0 = c + r, y0 = c, x1 = c, y1 = c + r;
  const head = Math.max(5, sw * 2.6);
  return `<g opacity="${faint ? 0.5 : 1}"><path d="M${x0} ${y0} A${r} ${r} 0 1 0 ${x1} ${y1}" fill="none" stroke="${colour}" stroke-width="${sw}" stroke-linecap="round"/>`
    + `<path d="M${x1} ${y1 - head * 0.7} L${x1 + head} ${y1} L${x1} ${y1 + head * 0.7} Z" fill="${colour}"/></g>`;
}
function glyph(S: number, k: string, to: number, bf: number, sev: 0 | 1 | 2, faint: boolean): string {
  const col = windColour(sev);
  if (k === 'a') return arrowPath(S, to, bf, col, faint);
  if (k === 'v') return variableGlyph(S, bf, col, faint);
  if (k === 'c') return cyclonicGlyph(S, bf, col, faint);
  return '';
}
/** ★ The arrows as one SVG (the later wind faint, behind). '' when there is nothing to draw (no direction, no change). */
export function windSvg(w: NavWind): { svg: string; size: number } {
  const S = windBox(w);
  const later = w.k2 ? glyph(S, w.k2, w.to2, w.bf2, w.sev2, true) : '';
  const first = glyph(S, w.k, w.to, w.bf, w.sev, false);
  if (!first && !later) return { svg: '', size: 0 };
  return { svg: `<svg width="${S}" height="${S}" viewBox="0 0 ${S} ${S}" style="display:block;overflow:visible">${later}${first}</svg>`, size: S };
}
const SEA = ['calm', 'calm', 'smooth', 'slight', 'moderate', 'rough', 'very rough', 'high', 'very high', 'phenomenal'];
/** ★ The compact label: the force range and the sea state — "6–8 · ROUGH" (· FOG when there is fog). */
export function windLabel(f: { fmin: number; fmax: number; sea: number; vis: number }, w: NavWind | null): string {
  const lo = f.fmin, hi = f.fmax;
  const force = lo < 0 ? (w && w.bf >= 0 ? `F${w.bf}` : '') : hi > lo ? `${lo}–${hi}` : String(lo);
  const sea = f.sea >= 0 ? SEA[f.sea].toUpperCase() : '';
  return [force, sea, f.vis === 0 ? 'FOG' : ''].filter(Boolean).join(' · ');
}
/** The words a screen reader says for the arrows: "wind from the south-west, force 8, veering west force 8 later". */
const C16 = ['north', 'north-north-east', 'north-east', 'east-north-east', 'east', 'east-south-east', 'south-east', 'south-south-east',
  'south', 'south-south-west', 'south-west', 'west-south-west', 'west', 'west-north-west', 'north-west', 'north-north-west'];
const point = (d: number) => C16[Math.round(((d % 360) + 360) % 360 / 22.5) % 16];
export function windWords(w: NavWind): string {
  const one = (k: string, from: number, bf: number) => (k === 'a' ? `from the ${point(from)}` : k === 'v' ? 'variable' : k === 'c' ? 'cyclonic' : '')
    + (bf >= 0 ? `, force ${bf}` : '');
  let s = `wind ${one(w.k, w.from, w.bf)}`.replace('wind , ', 'wind ');
  if (w.k2) s += `, ${w.chg === 'veer' ? 'veering' : w.chg === 'back' ? 'backing' : 'becoming'} ${one(w.k2, w.from2, w.bf2)} later`;
  if (w.sev === 2 || w.sev2 === 2) s += ' — gale';
  return s;
}
