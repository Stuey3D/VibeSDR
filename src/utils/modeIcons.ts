/**
 * ★★ MODE ICONS — a small CATEGORY glyph before a decoder / whole-radio mode's label, so a newcomer can tell at a
 * glance what each one is for (Stuart, 2026-10-28: "I thought AIS was aircraft because of the A but its shipping
 * related" — "makes the app a bit more visual and easy for a new person to the hobby to pick up").
 *
 * ONE SOURCE FOR BOTH CLIENTS: the app draws these with react-native-svg (components/ModeIcon.tsx) and the web client
 * as an inline <svg> string (modeIconSvg below, used by web/client/src). Monochrome, 24 × 24, stroked in the label's
 * own colour (currentColor on the web) — never a colour of their own, so every faceplate and lit state just works.
 *
 *   aircraft    ADS-B · HFDL · ACARS · VDL2         (top-down airliner — never confusable with the ship)
 *   ship        AIS · DSC · NAVTEX                  (side-on ship on the water)
 *   beacon      NDB                                 (lattice mast radiating)
 *   sensor      ISM                                 (thermometer — weather stations, sensors)
 *   aprs        APRS                                (map pin — stations and positions)
 *   tv          SSTV                                (a TV with rabbit ears — Stuart's pick)
 *   fax         WEFAX                               (fax: handset beside the body, a page on top — Stuart's pick)
 *   teleprinter RTTY                                (printer, paper out top AND bottom, no handset — Stuart's pick)
 *   morseKey    CW                                  (a straight key — Stuart's pick)
 *   keyboard    PSK · JTTY                          (keyboard-to-keyboard text modes)
 *   spots       FT8 / DIGITAL SPOTS (FT4, WSPR, JS8…)  (a globe with spots on it)
 *   broadcast   DAB
 *   clock       TIME (MSF, DCF77, WWV…)
 *   radio       the AMATEUR group's heading in the decoder panel (utils/decoderBands)
 *   map         MAP — the spots map, an OpenWebRX's map (VIEWS; was the colour 🗺 emoji, 2026-10-29)
 *   picture     FILES — an OpenWebRX's decoded pictures (VIEWS; was the colour 🖼 emoji)
 */

export type ModeIconName =
  | 'aircraft' | 'ship' | 'beacon' | 'sensor' | 'aprs' | 'tv' | 'fax' | 'teleprinter'
  | 'morseKey' | 'keyboard' | 'spots' | 'broadcast' | 'clock' | 'radio' | 'map' | 'picture' | 'pager';

/** One primitive of a glyph. `fill` = solid in the label colour; otherwise stroked (`w` = stroke width, default 1.8). */
export type ModeIconPart =
  | { d: string; fill?: boolean; w?: number }
  | { c: [number, number, number]; fill?: boolean; w?: number }
  | { r: [number, number, number, number, number]; fill?: boolean; w?: number };

export const MODE_ICONS: Record<ModeIconName, ModeIconPart[]> = {
  aircraft: [
    { d: 'M12 1.8c.95 0 1.45.85 1.45 1.9v5.1l8 4.8v2.4l-8-2.45v4.95l2.3 1.75v1.85L12 21.1l-3.75 1v-1.85l2.3-1.75V13.55l-8 2.45v-2.4l8-4.8V3.7c0-1.05.5-1.9 1.45-1.9Z', fill: true },
  ],
  ship: [
    { d: 'M2.5 13.5h19l-3 5.5h-13Z', fill: true },
    { d: 'M6.5 13.5V10h9v3.5' },
    { d: 'M9 10V6.5h4V10' },
    { d: 'M11 6.5V3' },
    { d: 'M2 22c1.7-1.1 3.3-1.1 5 0s3.3 1.1 5 0 3.3-1.1 5 0 3.3 1.1 5 0', w: 1.5 },
  ],
  beacon: [
    { d: 'M12 9.5 8 21.5M12 9.5l4 12M9.3 17.5h5.4M10.4 14h3.2' },
    { c: [12, 7.5, 1.6], fill: true },
    { d: 'M8.2 3.9a5.4 5.4 0 0 0 0 7.2M15.8 3.9a5.4 5.4 0 0 1 0 7.2' },
    { d: 'M5.3 1.6a9 9 0 0 0 0 11.8M18.7 1.6a9 9 0 0 1 0 11.8', w: 1.5 },
  ],
  sensor: [
    { d: 'M9.8 14.6V4.4a2.2 2.2 0 0 1 4.4 0v10.2a4 4 0 1 1-4.4 0Z' },
    { c: [12, 18, 1.9], fill: true },
    { d: 'M12 16V8.5', w: 2 },
    { d: 'M16.8 6h2.5M16.8 9.5h2.5M16.8 13h2.5', w: 1.5 },
  ],
  aprs: [
    { d: 'M12 22s-6.5-5.8-6.5-11a6.5 6.5 0 0 1 13 0c0 5.2-6.5 11-6.5 11Z' },
    { c: [12, 11, 2.3], fill: true },
  ],
  tv: [
    { r: [2.5, 7.5, 19, 13, 2] },
    { d: 'M8 2.5l4 5 4-5' },
    { r: [5.5, 10.5, 10, 7, 1], w: 1.4 },
    { c: [18.5, 12, 0.9], fill: true },
    { c: [18.5, 15.5, 0.9], fill: true },
  ],
  fax: [
    { r: [2, 7.5, 4.6, 14, 2.3] },
    { r: [8.5, 11, 13.5, 10.5, 1.5] },
    { d: 'M10.8 11V3h7.5l1.7 1.7V11' },
    { d: 'M12.8 6h4.6M12.8 8.5h4.6', w: 1.4 },
    { d: 'M11.5 14.5h.01M14 14.5h.01M16.5 14.5h.01M11.5 17.5h.01M14 17.5h.01M16.5 17.5h.01', w: 2.2 },
  ],
  teleprinter: [
    { r: [2.5, 8.5, 19, 9, 1.8] },
    { d: 'M7 8.5V2.5h10v6' },
    { d: 'M7 14v7.5h10V14' },
    { d: 'M9.2 17.2h5.6M9.2 19.6h3.6', w: 1.4 },
    { c: [18.3, 11.5, 0.9], fill: true },
  ],
  morseKey: [
    { r: [2.5, 18, 19, 3.5, 1] },
    { d: 'M4.5 14.6 17.5 11.4' },
    { d: 'M7.5 13.9V18M15 12V18', w: 1.6 },
    { r: [14.6, 7.3, 6.4, 2.6, 1.3], fill: true },
    { d: 'M17.8 9.9v1.6' },
  ],
  keyboard: [
    { r: [2, 6.5, 20, 12, 2] },
    { d: 'M6 10.2h.01M9.3 10.2h.01M12.6 10.2h.01M15.9 10.2h.01M18 10.2h.01M7.6 13h.01M10.9 13h.01M14.2 13h.01M17.4 13h.01', w: 2.1 },
    { d: 'M8 15.9h8' },
  ],
  spots: [   // a globe with stations spotted on it — weak-signal modes heard worldwide
    { c: [12, 12, 9], w: 1.6 },
    { d: 'M12 3c-2.6 2.6-3.6 5.6-3.6 9s1 6.4 3.6 9c2.6-2.6 3.6-5.6 3.6-9S14.6 5.6 12 3ZM3.3 9.5h17.4M3.3 14.5h17.4', w: 1.2 },
    { c: [7, 7.5, 1.9], fill: true },
    { c: [16.8, 11.8, 1.9], fill: true },
    { c: [10, 17.5, 1.9], fill: true },
  ],
  broadcast: [
    { c: [6, 18, 2], fill: true },
    { d: 'M6 12.5a5.5 5.5 0 0 1 5.5 5.5' },
    { d: 'M6 7a11 11 0 0 1 11 11' },
    { d: 'M6 2a16 16 0 0 1 16 16', w: 1.5 },
  ],
  radio: [   // a transceiver with its whip — the AMATEUR group's heading
    { r: [2.5, 9, 19, 12, 2] },
    { d: 'M6.5 9 17 3' },
    { c: [8.5, 15, 3], w: 1.6 },
    { d: 'M14.5 12.5h4M14.5 15h4M14.5 17.5h4', w: 1.5 },
  ],
  pager: [   // a pager: the case, its screen, one key
    { r: [3, 6, 18, 12, 2.5] },
    { r: [6, 9, 9, 4, 1], w: 1.5 },
    { c: [17.5, 15, 1.3], fill: true },
  ],
  clock: [
    { c: [12, 12, 9] },
    { d: 'M12 6.8V12l3.6 2.2' },
  ],
  map: [   // a folded paper map, three panels — the house line style, not the colour emoji it replaced
    { d: 'M2.5 6.2 8.8 3.5l6.4 2.7 6.3-2.7v14.3l-6.3 2.7-6.4-2.7-6.3 2.7Z' },
    { d: 'M8.8 3.5v14.3M15.2 6.2v14.3', w: 1.5 },
  ],
  picture: [   // a framed picture: the sun over hills
    { r: [2.5, 4, 19, 16, 2] },
    { c: [8.3, 9.3, 1.7], fill: true },
    { d: 'M3 17.5l5.2-5 4 3.8 2.8-2.7 5.5 5.2' },
  ],
};

/**
 * The glyph for a mode / decoder id, or null for one with none (a demodulator, SCANNER, GALLERY…).
 * ★ Ids are every client's: ours (adsb, ism, ndb, ais, dsc, aprs, cw, psk, jtty, rtty, navtex, wefax, sstv, time, ft8,
 *   dab) and OpenWebRX's (hfdl, vdl2, packet, fax, bpsk31, rtty170, cwdecoder, wspr…), so the app's OWRX list gets them too.
 * ★ NOT for the DEMODULATOR 'cw' (the mode grid's CW key is a way to listen, not a decoder) — callers ask only from
 *   decoder / whole-radio keys.
 */
export function modeIconFor(id: string): ModeIconName | null {
  const k = id.toLowerCase().replace(/[^a-z0-9]/g, '');
  switch (k) {
    case 'adsb': case 'hfdl': case 'selcal': case 'acars': case 'vdl2': case 'vdl': case 'hfdlmap': return 'aircraft';
    case 'ais': case 'dsc': case 'navtex': case 'sitora': case 'sitor': case 'amtor': return 'ship';   // ★ SITOR-A beside NAVTEX (SITOR-B)
    case 'ndb': return 'beacon';
    case 'ism': return 'sensor';
    case 'paging': case 'pocsag': case 'flex': case 'pager': return 'pager';   // ★ V12 PAGING (and OpenWebRX's POCSAG)
    case 'aprs': case 'packet': return 'aprs';
    case 'sstv': return 'tv';
    case 'wefax': case 'fax': return 'fax';
    case 'rtty': case 'rtty45': case 'rtty50': case 'rtty85': case 'rtty170': case 'rtty450': case 'mfrtty': return 'teleprinter';
    case 'cw': case 'morse': case 'cwdecoder': case 'cwskimmer': return 'morseKey';
    case 'psk': case 'bpsk31': case 'bpsk63': case 'psk31': case 'psk63': case 'jtty': case 'mfsk': case 'olivia': case 'thor': return 'keyboard';
    case 'ft8': case 'ft4': case 'wspr': case 'js8': case 'jt65': case 'jt9': case 'q65': case 'fst4': case 'fst4w':
    case 'msk144': case 'digi': case 'spots': case 'spotsband': return 'spots';
    case 'dab': case 'rds': case 'advrds': case 'same': case 'eas': return 'broadcast';
    case 'amateur': return 'radio';
    case 'time': return 'clock';
    case 'map': case 'spotsmap': case 'owrxmap': return 'map';
    case 'files': case 'owrxfiles': return 'picture';
    default: return null;
  }
}

/** The glyph as an inline <svg> string (the web client) — stroked/filled in currentColor, so it follows the button. */
export function modeIconSvg(name: ModeIconName, cls = 'modeIco'): string {
  const parts = MODE_ICONS[name].map(p => {
    const paint = p.fill ? 'fill="currentColor" stroke="none"' : `fill="none" stroke="currentColor" stroke-width="${p.w ?? 1.8}"`;
    if ('d' in p) return `<path d="${p.d}" ${paint}/>`;
    if ('c' in p) return `<circle cx="${p.c[0]}" cy="${p.c[1]}" r="${p.c[2]}" ${paint}/>`;
    return `<rect x="${p.r[0]}" y="${p.r[1]}" width="${p.r[2]}" height="${p.r[3]}" rx="${p.r[4]}" ${paint}/>`;
  }).join('');
  return `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true" focusable="false" stroke-linecap="round" stroke-linejoin="round">${parts}</svg>`;
}
