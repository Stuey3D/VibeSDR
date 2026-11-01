// navtexIcons.ts — V12 2026-11-01 THE NAVTEX MAP's HAZARD ICONS (Stuart: "so a picture of a shipwreck or a wind turbine
// with no nav light"; "unexploded ordinance would look good too"). Our own drawings, one per hazard class the server's
// classifier names (android/app/src/main/cpp/vibe_navtex_warn.h): no third-party icon set.
//
// ★★ THE HEALTH GLYPHS' LANGUAGE (src/components/HealthPill.tsx): a 16-unit viewBox, single colour, stroke 1.4, round
//    caps and joins, a small filled detail only where a stroke would close up at 16 px. The comparison sheet is
//    scripts/navtex-icon-sheet.mjs (16 / 24 / 32 px, dark and light, to ~/Desktop/navtex-icons/).
// ★ `unlit` is a MODIFIER: a light, buoy or structure whose light has failed carries the small UNLIT mark (a lamp
//    struck through) in its top-right corner. The unlit-light icon is the lighthouse with that mark.
// ★ PURE strings — no React Native, no DOM: the web client and the app's map page both draw from here.

export type NavHazard = 'wreck' | 'navaid' | 'light' | 'buoy' | 'structure' | 'firing' | 'uxo' | 'cable' | 'survey'
  | 'obstruction' | 'adrift' | 'works' | 'towing' | 'sar' | 'ice' | 'general';

/** The inner SVG of each glyph (16-unit box). Strokes inherit; `F` marks a filled detail (currentColor). */
const F = 'fill="currentColor" stroke="none"';
const WAVE = 'M1.5 13.3c1.1-.9 2.2-.9 3.3 0s2.2.9 3.3 0 2.2-.9 3.3 0 2.2.9 3.3 0';
export const NAV_GLYPHS: Record<NavHazard, string> = {
  // a hull heeled over and going down, its mast, the sea over it
  wreck: '<path d="M2.6 6.2c1.1-.9 2.2-.9 3.3 0s2.2.9 3.3 0 2.2-.9 3.3 0"/><path d="M3.2 10.4l9.4 1.6-1 2.4-6.8-1.1z"/><path d="M7.6 11.1l1.5-4.3"/>',
  // the lighthouse: a tapered tower, its lantern, its beams (the unlit version has no beams and the mark)
  light: '<path d="M5.8 14.4l1-8.2h2.4l1 8.2z"/><path d="M6.4 6.2h3.2M7 6.2V4.3h2v1.9"/><path d="M4.9 14.4h6.2"/><path d="M10.9 4.8l2.6-1.1M10.9 6.2h2.8M5.1 4.8L2.5 3.7M5.1 6.2H2.3"/>',
  // a buoy on the water: its body, a pole and a ball topmark
  buoy: '<path d="M5 11.4l1-4h4l1 4z"/><path d="M8 7.4V4"/><circle cx="8" cy="2.9" r="1.1"/><path d="' + WAVE + '"/>',
  // navaid (a beacon or a light not otherwise named) = the lighthouse
  navaid: '',
  // a wind turbine on its tower — hub, three blades
  structure: '<path d="M7.5 14.4l.25-7M8.5 14.4l-.25-7M6.2 14.4h3.6"/><circle cx="8" cy="6.3" r="1"/><path d="M7.6 5.4L6.6 2.4M8.95 6.5l3 .5M7.5 7.1L5.5 9.4"/>',
  // a gun-sight: the range is in use
  firing: '<circle cx="8" cy="8" r="4.6"/><circle cx="8" cy="8" r=".9" ' + F + '/><path d="M8 1.6v2.6M8 11.8v2.6M1.6 8h2.6M11.8 8h2.6"/>',
  // ★ a sea mine (Stuart, 2026-11-01, option C): a solid round case split by a band, four dots, short tapered horns
  uxo: '<path d="M3.43 7.90A4.6 4.6 0 0 1 12.57 7.90zM3.43 8.90A4.6 4.6 0 0 0 12.57 8.90zM5.45 6.30a0.75 0.75 0 1 0 1.5 0a0.75 0.75 0 1 0 -1.5 0zM9.05 6.30a0.75 0.75 0 1 0 1.5 0a0.75 0.75 0 1 0 -1.5 0zM5.45 10.50a0.75 0.75 0 1 0 1.5 0a0.75 0.75 0 1 0 -1.5 0zM9.05 10.50a0.75 0.75 0 1 0 1.5 0a0.75 0.75 0 1 0 -1.5 0z" fill="currentColor" fill-rule="evenodd" stroke="none"/><path d="M8.75 4.20L8.35 2.30L7.65 2.30L7.25 4.20zM11.87 6.61L13.20 5.19L12.80 4.61L11.01 5.38zM4.99 5.38L3.20 4.61L2.80 5.19L4.13 6.61zM11.01 11.42L12.80 12.19L13.20 11.61L11.87 10.19zM4.13 10.19L2.80 11.61L3.20 12.19L4.99 11.42zM7.25 12.60L7.65 14.50L8.35 14.50L8.75 12.60z" fill="currentColor" stroke="none"/>',
  // a cable on the seabed between two joints
  cable: '<path d="M2.6 8.6c2.2 0 2.4-3.6 5.4-3.6s3.2 3.6 5.4 3.6"/><circle cx="2.4" cy="8.6" r="1.2"/><circle cx="13.6" cy="8.6" r="1.2"/><path d="M1.6 12.6h2M5.2 12.6h2M8.8 12.6h2M12.4 12.6h2"/>',
  // a survey vessel and the sounding fanning out beneath it
  survey: '<path d="M1.8 6.6h12.4l-1.8 2.8H3.6z"/><path d="M6.4 6.6V4.4h3v2.2"/><path d="M5.6 11.4a3.4 3.4 0 0 0 4.8 0M4.1 13.4a5.6 5.6 0 0 0 7.8 0"/>',
  // an anchor: ring, stock, shank, flukes (anchoring, obstructions, shoals)
  obstruction: '<circle cx="8" cy="2.9" r="1.2"/><path d="M8 4.1v10.2M5.4 6.2h5.2"/><path d="M2.8 9.6c.4 2.8 2.6 4.7 5.2 4.7s4.8-1.9 5.2-4.7"/><path d="M2.8 9.6l-.9 1.2M13.2 9.6l.9 1.2"/>',
  // a container overboard, tilted on the water, its ribs
  adrift: '<path d="M3.2 6.4l8.2-2 1.4 5.6-8.2 2z"/><path d="M5.6 5.8l1.2 5.4M8 5.2l1.3 5.5M10.2 4.7l1.3 5.4"/><path d="' + WAVE + '"/>',
  // a crane barge: the pontoon, the jib, the grab on its wire
  works: '<path d="M1.8 11.4h12.4l-1.4 2.4H3.2z"/><path d="M4.6 11.4V3.6l7.6 2.6"/><path d="M12.2 6.2v2.4"/><path d="M11 8.6h2.4l-.5 1.6h-1.4z"/>',
  // a tug and its tow on a line
  towing: '<path d="M1.6 9.6h4.6l-1 2H2.6z"/><path d="M3.4 9.6V7.8h1.6v1.8"/><path d="M6.2 9.4l2.6-.8" stroke-dasharray="1.2 1.2"/><path d="M9.2 7.4h5.2v4.2H9.2z"/><path d="' + 'M1.5 14.2c1.1-.8 2.2-.8 3.3 0s2.2.8 3.3 0 2.2-.8 3.3 0 2.2.8 3.3 0' + '"/>',
  // a lifebuoy
  sar: '<circle cx="8" cy="8" r="5.6"/><circle cx="8" cy="8" r="2.4"/><path d="M4 4l2.3 2.3M12 4L9.7 6.3M4 12l2.3-2.3M12 12L9.7 9.7"/>',
  // an iceberg: the peaks above the water, the mass below it
  ice: '<path d="M2.4 9.2l2.4-4 1.8 1.8 2-3.6 3.6 5.8"/><path d="M1.5 9.2h13"/><path d="M3.4 11.2l1.6 2.6h6.2l1.6-2.6" stroke-dasharray="1.4 1.2"/>',
  // the general warning: a triangle and its mark
  general: '<path d="M8 1.9l6.4 11.4H1.6z"/><path d="M8 6.2v3.4"/><circle cx="8" cy="11.4" r=".75" ' + F + '/>',
};
NAV_GLYPHS.navaid = NAV_GLYPHS.light;
/** ★ THE UNLIT MARK (a modifier): a lamp, struck through, top right — over a light, a buoy or a structure. Drawn on a
 *  knocked-out disc so it reads over whatever is under it. `bg` is the ground colour behind the icon. */
export function navUnlitMark(bg: string): string {
  return `<circle cx="12.6" cy="3.4" r="3.2" fill="${bg}" stroke="none"/><circle cx="12.6" cy="3.4" r="2.2" stroke-width="1.1"/><path d="M11 5l3.2-3.2" stroke-width="1.1"/>`;
}
/** The unlit mark's own glyph (the lighthouse with the beams gone). */
const LIGHT_DARK = '<path d="M5.8 14.4l1-8.2h2.4l1 8.2z"/><path d="M6.4 6.2h3.2M7 6.2V4.3h2v1.9"/><path d="M4.9 14.4h6.2"/>';

/** The hazards the sheet and the legend list, in their order, with the words. */
export const NAV_HAZARDS: Array<{ id: NavHazard; words: string }> = [
  { id: 'wreck', words: 'Wreck' },
  { id: 'light', words: 'Light' },
  { id: 'buoy', words: 'Buoy or beacon' },
  { id: 'structure', words: 'Wind turbine or platform' },
  { id: 'firing', words: 'Firing or military exercise' },
  { id: 'uxo', words: 'Mine or unexploded ordnance' },
  { id: 'cable', words: 'Cable or pipeline' },
  { id: 'survey', words: 'Survey vessel' },
  { id: 'obstruction', words: 'Obstruction, shoal or anchoring' },
  { id: 'adrift', words: 'Adrift — container, debris, a derelict' },
  { id: 'works', words: 'Works, dredging or diving' },
  { id: 'towing', words: 'Tow' },
  { id: 'sar', words: 'Search and rescue' },
  { id: 'ice', words: 'Ice' },
  { id: 'general', words: 'Other warning' },
];

/** ★ The family colours (one per icon — the icon itself stays single-colour): danger red-orange, lights amber, work at
 *  sea yellow, rescue pink, ice pale blue, the rest grey-white. Chosen to read on the dark basemap and the light one. */
export const NAV_HAZARD_COLOURS: Record<NavHazard, string> = {
  wreck: '#ff7a59', uxo: '#ff5a5a', firing: '#ff5a5a', obstruction: '#ff9d5c', adrift: '#ff9d5c',
  light: '#ffc940', navaid: '#ffc940', buoy: '#ffc940',
  structure: '#e8d36a', cable: '#e8d36a', survey: '#e8d36a', works: '#e8d36a', towing: '#e8d36a',
  sar: '#ff7ac8', ice: '#9fdcff', general: '#d7dee6',
};

/** The server's class (and its unlit flag) → the icon drawn. A navaid that is a buoy says BUOY in its text — the server
 *  sends cls 'navaid'; `buoy` is chosen by the card's hit words when they say so (navHazardOf). */
export function navHazardOf(cls: string, hint = ''): NavHazard {
  if (cls === 'navaid') return /\b(BUOYS?|BOUEES?|BOYAS?|BOA|BOE|TONNEN?|BOEIEN?|BOJA|PLUTAC\w*|LANBY|BEACONS?|BALISES?|BALIZAS?|BAKENS?|MEDA)\b/.test(hint.toUpperCase()) ? 'buoy' : 'light';
  return (cls in NAV_GLYPHS ? cls : 'general') as NavHazard;
}

/** One glyph as a standalone SVG — the sheet, the legend, a card. */
export function navGlyphSvg(h: NavHazard, px: number, colour: string, unlit = false, bg = '#0b1420'): string {
  const body = unlit && (h === 'light' || h === 'navaid') ? LIGHT_DARK : NAV_GLYPHS[h];
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${px}" height="${px}" viewBox="0 0 16 16" fill="none" color="${colour}" stroke="${colour}"`
    + ` stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">${body}${unlit ? navUnlitMark(bg) : ''}</svg>`;
}

/** ★ THE MAP MARKER: the glyph on a dark disc rimmed in its colour (so it reads on any basemap), 24 px. Faded (expired,
 *  cancelled) = 40 % and a dashed grey rim. */
export function navHazardMarker(h: NavHazard, unlit: boolean, faded: boolean): { html: string; w: number; h: number } {
  const c = faded ? '#9aa6b2' : NAV_HAZARD_COLOURS[h];
  const rim = faded ? ' stroke-dasharray="2.4 1.8"' : '';
  const body = unlit && (h === 'light' || h === 'navaid') ? LIGHT_DARK : NAV_GLYPHS[h];
  const glyph = `<g transform="translate(4.4 4.4) scale(0.95)" fill="none" color="${c}" stroke="${c}" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">${body}${unlit ? navUnlitMark('#0b1420') : ''}</g>`;
  return {
    html: `<svg width="24" height="24" viewBox="0 0 24 24" aria-hidden="true" style="opacity:${faded ? 0.42 : 1}"><circle cx="12" cy="12" r="11" fill="#0b1420" stroke="${c}" stroke-width="1.3"${rim}/>${glyph}</svg>`,
    w: 24, h: 24,
  };
}
