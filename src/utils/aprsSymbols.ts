/**
 * aprsSymbols.ts — ★★ beta92 THE APRS MAP SYMBOLS, ONE SET for the app and the web page (src/services/tracks.ts and
 * web/client/src/tracks.ts both draw from here — they were two copies of the same 22 px drawings).
 *
 * ★★★ CLEARER ICONS (Stuart, 2026-11-23, the digi modes map: "when it works we need clearer icons too"). The old badge
 *     was a 22 px circle with a 16-unit glyph in it: at a normal zoom a digipeater (purple star-in-circle) and a house
 *     (green house-in-circle) were two coloured dots. Now:
 *       · 28 px, the glyph 18 px — a car, a house, a mast, a cloud read as themselves at a glance on a phone;
 *       · the BADGE SHAPE says what kind of thing it is before the glyph does: a ROUNDED SQUARE for the network's own
 *         furniture (digipeaters, gateways / iGates, repeaters), a CIRCLE for everything else;
 *       · a dark rim inside a thin light halo, so it stands off the dark map AND the satellite / terrain imagery;
 *       · glyphs are solid silhouettes with their details CUT in the badge's colour (windows, doors, wheel hubs) — no
 *         hairlines to vanish at 28 px;
 *       · a code we have not drawn is its own symbol CHARACTER, big, on the badge — never blank, never a guess.
 * ★ OUR OWN ARTWORK. The APRS 1.0.1 symbol table's MEANINGS are the published standard (code → "car", "house",
 *   "digipeater"…); every drawing below is ours — no third-party symbol set is copied.
 * ★ THE ALTERNATE TABLE ('\' or an overlay character) means something else for most codes, so only the codes whose
 *   alternate meaning is the same KIND of thing (a digi with an overlay, a gateway, an HF house, a car, a wx station…)
 *   take the glyph there; the rest show the character. The alternate table keeps its dashed rim, an overlay its letter
 *   or digit in a tag.
 */

/** A glyph: drawn in order on a 16-unit square. 'f' = solid (dark), 'c' = cut (the badge's colour, on top),
 *  'l' = a dark line, 'k' = a cut line (the badge's colour). */
type Stroke = ['f' | 'c' | 'l' | 'k', string];

const circ = (x: number, y: number, r: number) => `M${x - r} ${y} a${r} ${r} 0 1 0 ${2 * r} 0 a${r} ${r} 0 1 0 ${-2 * r} 0 Z`;

export const APRS_GLYPHS: Record<string, Stroke[]> = {
  car: [
    ['f', 'M1.2 11.4 V9.1 Q1.2 8.3 2 8.1 L3.7 5.1 Q4.1 4.4 5 4.4 H11 Q11.9 4.4 12.3 5.1 L14 8.1 Q14.8 8.3 14.8 9.1 V11.4 Z'],
    ['c', 'M4.3 7.9 L5.3 5.8 H7.5 V7.9 Z M8.5 7.9 V5.8 H10.7 L11.7 7.9 Z'],
    ['f', circ(4.6, 11.5, 2) + circ(11.4, 11.5, 2)],
    ['c', circ(4.6, 11.5, 0.75) + circ(11.4, 11.5, 0.75)],
  ],
  truck: [
    ['f', 'M0.8 3.6 H9.9 V11.6 H0.8 Z M10.7 5.8 H13.1 L15.2 8.7 V11.6 H10.7 Z'],
    ['c', 'M11.5 6.7 H12.7 L14 8.7 H11.5 Z'],
    ['f', circ(4, 11.9, 1.9) + circ(12.6, 11.9, 1.9)],
    ['c', circ(4, 11.9, 0.7) + circ(12.6, 11.9, 0.7)],
  ],
  house: [
    ['f', 'M8 1.4 L15.2 8 H13.1 V14.6 H2.9 V8 H0.8 Z'],
    ['c', 'M6.8 14.6 V10.1 H9.2 V14.6 Z'],
  ],
  // ★ the DIGIPEATER: a mast sending both ways — never a star again (a star in a circle read as a house in a circle)
  digi: [
    ['f', 'M8 5.6 L11.3 15 H9.5 L8 10.1 L6.5 15 H4.7 Z' + circ(8, 4.8, 1.5)],
    ['l', 'M5.2 2.4 A3.4 3.4 0 0 0 5.2 7.2 M10.8 2.4 A3.4 3.4 0 0 1 10.8 7.2 M3 0.9 A5.6 5.6 0 0 0 3 8.7 M13 0.9 A5.6 5.6 0 0 1 13 8.7'],
  ],
  // ★ a GATEWAY / iGate: RF to the internet — a globe
  gateway: [
    ['f', circ(8, 8, 6.8)],
    ['k', 'M1.4 8 H14.6 M8 1.2 V14.8 M2.6 4.6 H13.4 M2.6 11.4 H13.4 M8 1.2 A4.2 6.8 0 0 0 8 14.8 M8 1.2 A4.2 6.8 0 0 1 8 14.8'],
  ],
  // ★ a REPEATER / yagi: a beam on a mast (a boom with its elements)
  antenna: [
    ['l', 'M8 5 V14.8 M4.8 14.8 H11.2 M1.6 5 H14.4 M3.4 2 V8 M6.4 2.6 V7.4 M9.6 3.1 V6.9 M12.6 3.6 V6.4'],
  ],
  weather: [
    ['f', 'M4.2 12.6 A3.1 3.1 0 0 1 4 6.4 A4.1 4.1 0 0 1 11.8 5.6 A3.5 3.5 0 0 1 12.2 12.6 Z'],
    ['l', 'M5 14.9 L5.7 13.6 M8.2 14.9 L8.9 13.6 M11.4 14.9 L12.1 13.6'],
  ],
  balloon: [
    ['f', circ(8, 5.9, 4.9)],
    ['l', 'M5.4 9.9 L7.1 13 M10.6 9.9 L8.9 13'],
    ['f', 'M6.6 12.8 H9.4 V15.2 H6.6 Z'],
  ],
  plane: [
    ['f', 'M8 0.8 Q9 0.8 9.1 2.6 L9.2 5.9 L15 9.1 V10.7 L9.2 9 L9 12.6 L10.9 14.1 V15.2 L8 14.5 L5.1 15.2 V14.1 L7 12.6 L6.8 9 L1 10.7 V9.1 L6.8 5.9 L6.9 2.6 Q7 0.8 8 0.8 Z'],
  ],
  heli: [
    ['f', 'M3.6 7 H9.6 A2.7 2.7 0 0 1 9.6 12.4 H5.6 Q3.6 12.4 3.6 10.4 Z M11.4 8.9 H15 V10.3 H11.4 Z'],
    ['l', 'M1.2 4.4 H13.6 M7.4 4.4 V7 M15 7.6 V11.6'],
  ],
  boat: [
    ['f', 'M0.8 9.4 H15.2 L12.9 13.8 H3.1 Z M4.6 5.6 H9.8 L11 9.4 H3.6 Z'],
    ['c', 'M5.4 6.6 H6.9 V8.4 H5.1 Z M7.9 6.6 H9.3 L9.8 8.4 H7.9 Z'],
  ],
  yacht: [
    ['f', 'M1.2 11.2 H14.8 L12.7 14.4 H3.3 Z M7.4 1 V10.2 H2.2 Z M8.6 3.2 L13.2 10.2 H8.6 Z'],
  ],
  person: [
    ['f', circ(8, 2.8, 1.9) + 'M5.9 5.6 H10.1 L11 10.3 H9.6 L9.4 15 H8.4 L8 11 L7.6 15 H6.6 L6.4 10.3 H5 Z'],
  ],
  bike: [
    ['l', circ(3.8, 11, 2.7) + circ(12.2, 11, 2.7) + 'M3.8 11 L6.8 5.8 H10.4 L12.2 11 M6.8 5.8 L8.4 11 L10.4 5.8 M5.8 4.4 H8'],
  ],
  train: [
    ['f', 'M3.6 1.4 H12.4 Q13.6 1.4 13.6 2.6 V12.2 H2.4 V2.6 Q2.4 1.4 3.6 1.4 Z'],
    ['c', 'M3.8 3 H12.2 V7.2 H3.8 Z' + circ(5, 10, 1) + circ(11, 10, 1)],
    ['l', 'M4.8 12.6 L3 15 M11.2 12.6 L13 15'],
  ],
  cross: [
    ['f', 'M6 1.6 H10 V6 H14.4 V10 H10 V14.4 H6 V10 H1.6 V6 H6 Z'],
  ],
  tent: [
    ['f', 'M8 1.8 L15.2 14 H0.8 Z'],
    ['c', 'M8 7.2 L10.4 14 H5.6 Z'],
  ],
  node: [
    ['f', circ(8, 8, 6.6)],
    ['c', circ(8, 8, 4.4)],
    ['f', circ(8, 8, 2.3)],
  ],
};

/** The primary table ('/'): code → glyph. The meanings are APRS 1.0.1's. */
export const APRS_CODE_GLYPH: Record<string, string> = {
  '>': 'car', 'j': 'car',
  'k': 'truck', 'u': 'truck', 'v': 'truck', 'R': 'truck', 'U': 'truck', 'a': 'truck', 'f': 'truck',
  '-': 'house', 'K': 'house', 'H': 'house',
  '#': 'digi', '&': 'gateway', 'r': 'antenna', 'y': 'antenna',
  '_': 'weather', 'W': 'weather',
  'O': 'balloon', '^': 'plane', "'": 'plane', 'X': 'heli',
  's': 'boat', 'C': 'boat', 'Y': 'yacht',
  '[': 'person', 'b': 'bike', '<': 'bike',
  '=': 'train', '+': 'cross', 'h': 'cross', ';': 'tent', 'n': 'node',
};
/** ★ The alternate table ('\' or an overlay): only the codes that mean the same kind of thing there (a digi or gateway
 *  with an overlay, an HF house, a car / SUV / van / truck with an overlay, a wx station with a digi, a ship). */
export const APRS_ALT_SAME = '#&->_kuvjs';

/** The badge's colour: weather, an object / item, the network's furniture, something moving, a station. */
export function aprsColour(s: { kind: string; sym: string; speed?: number }): string {
  if (s.kind === 'weather' || s.sym[1] === '_') return '#39d0d8';
  if (s.kind === 'object' || s.kind === 'item') return '#ffad42';
  if (s.sym[1] === '#' || s.sym[1] === '&' || s.sym[1] === 'r') return '#c58cff';
  if (s.speed != null && s.speed >= 2) return '#4fa3ff';
  return '#59c26b';
}
/** The glyph's name for a two-character symbol ('/>' → 'car'), '' when we draw the character instead. */
export function aprsGlyphOf(sym: string): string {
  const table = sym[0] || '/', code = sym[1] || '';
  const g = APRS_CODE_GLYPH[code] || '';
  if (!g) return '';
  return table === '/' || APRS_ALT_SAME.includes(code) ? g : '';
}
/** ★ The network's own furniture (digipeater, gateway, repeater, yagi): a rounded-square badge, not a circle. */
export function aprsIsInfra(sym: string): boolean {
  const g = aprsGlyphOf(sym);
  return g === 'digi' || g === 'gateway' || g === 'antenna';
}

const escX = (v: string) => v.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' } as Record<string, string>)[c]);
const DARK = '#0b1014';
/** The icon's size in px (the map's anchor is its centre). */
export const APRS_ICON_PX = 28;

/** ★ THE ICON: an <svg> of our own (no script, link or foreign content — digiSvgOk / the map pages check again). */
export function aprsIcon(s: { kind: string; sym: string; speed?: number }, px: number = APRS_ICON_PX): { html: string; w: number; h: number } {
  const col = aprsColour(s);
  const sym = (s.sym || '').padEnd(2, ' ');
  const table = sym[0] || '/', code = sym[1] || '/';
  const gName = aprsGlyphOf(sym);
  const g = APRS_GLYPHS[gName];
  const overlay = table !== '/' && table !== '\\' && /[0-9A-Z]/.test(table) ? table : '';
  const dash = table !== '/' ? ' stroke-dasharray="3.4 1.8"' : '';
  const infra = aprsIsInfra(sym);
  // the badge: a light halo, then the coloured face with its dark rim (dashed on the alternate table)
  const badge = infra
    ? `<rect x="1" y="1" width="26" height="26" rx="7" fill="none" stroke="rgba(255,255,255,0.85)" stroke-width="1.4"/>`
      + `<rect x="2.6" y="2.6" width="22.8" height="22.8" rx="5.4" fill="${col}" stroke="${DARK}" stroke-width="1.7"${dash}/>`
    : `<circle cx="14" cy="14" r="13" fill="none" stroke="rgba(255,255,255,0.85)" stroke-width="1.4"/>`
      + `<circle cx="14" cy="14" r="11.5" fill="${col}" stroke="${DARK}" stroke-width="1.7"${dash}/>`;
  let inner = '';
  if (g) {
    inner = `<g transform="translate(5 5) scale(1.125)" stroke-linejoin="round" stroke-linecap="round">`
      + g.map(([k, d]) => k === 'f' ? `<path d="${d}" fill="${DARK}"/>`
        : k === 'c' ? `<path d="${d}" fill="${col}"/>`
        : `<path d="${d}" fill="none" stroke="${k === 'l' ? DARK : col}" stroke-width="${k === 'l' ? 1.6 : 1.1}"/>`).join('')
      + '</g>';
  } else {
    const ch = code.trim() || '?';
    inner = `<text x="14" y="19" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="15" font-weight="800" fill="${DARK}">${escX(ch)}</text>`;
  }
  const ov = overlay
    ? `<circle cx="22.2" cy="22.2" r="5.4" fill="${DARK}" stroke="#ffffff" stroke-width="1"/>`
      + `<text x="22.2" y="25.2" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="8.5" font-weight="800" fill="#ffffff">${escX(overlay)}</text>`
    : '';
  return { w: px, h: px, html: `<svg width="${px}" height="${px}" viewBox="0 0 28 28">${badge}${inner}${ov}</svg>` };
}

/** ★ The digi modes map's legend: the symbols most heard, in our drawings, with their standard meanings. */
export const APRS_LEGEND: { sym: string; kind: string; speed?: number; words: string }[] = [
  { sym: '/-', kind: 'position', words: 'Home station' },
  { sym: '/>', kind: 'position', speed: 40, words: 'Car · moving = blue' },
  { sym: '/k', kind: 'position', words: 'Truck / van / bus' },
  { sym: '/#', kind: 'position', words: 'Digipeater (square)' },
  { sym: 'I&', kind: 'position', words: 'Gateway / iGate (square)' },
  { sym: '/_', kind: 'weather', words: 'Weather station' },
  { sym: '/[', kind: 'position', words: 'On foot' },
  { sym: '/Y', kind: 'position', words: 'Yacht / boat' },
  { sym: '/^', kind: 'position', words: 'Aircraft' },
  { sym: '/O', kind: 'position', words: 'Balloon' },
  { sym: '/!', kind: 'object', words: 'Object · other symbols: their character' },
];
