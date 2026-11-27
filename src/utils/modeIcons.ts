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
 *   balloon     SONDES (RS41 radiosondes — a weather balloon with its instrument hanging below)
 *   satellite   SATELLITE (V12 — a body with two solar panels and its downlink waves)
 */

export type ModeIconName =
  | 'aircraft' | 'ship' | 'beacon' | 'sensor' | 'aprs' | 'tv' | 'fax' | 'teleprinter'
  | 'morseKey' | 'keyboard' | 'spots' | 'broadcast' | 'clock' | 'radio' | 'map' | 'picture' | 'pager' | 'balloon' | 'satellite'
  | 'servers' | 'list' | 'navtex' | 'zoomAuto' | 'zoomPause' | 'zoomLens' | 'noEntry';

/** One primitive of a glyph. `fill` = solid in the label colour; otherwise stroked (`w` = stroke width, default 1.8). */
export type ModeIconPart =
  | { d: string; fill?: boolean; w?: number }
  | { c: [number, number, number]; fill?: boolean; w?: number }
  | { r: [number, number, number, number, number]; fill?: boolean; w?: number };

export const MODE_ICONS: Record<ModeIconName, ModeIconPart[]> = {
  /* ★★★ beta96 the SERVERS glyph — network nodes — ONE source: the app's ServersChip (SectionIcon 'instance' draws these
   *  parts) and the web map's SERVERS chip (Stuart 11-27: "servers glyph is not the one we use everywhere else") */
  servers: [
    { c: [6, 7, 2], w: 1.7 }, { c: [18, 7, 2], w: 1.7 }, { c: [12, 18, 2], w: 1.7 },
    { d: 'M8 7h8M7.2 8.7 10.8 16.3M16.8 8.7 13.2 16.3', w: 1.7 },
  ],
  /* ★★★ beta96 THE NAVTEX GLYPH (Stuart 11-27 picked "C": "mast and sheet probably fits best as the icons for the
   *  transmission stations are the same masts") — a coast-station mast (the station markers' own A-frame and crossbar,
   *  navtexMap.ts navIcon) sending, its message coming out as a printed sheet with a torn edge: navigational telex.
   *  Our own drawing. ONE source: the map's station markers, LAYERS, the LIST's NAVTEX tab — web and app. */
  navtex: [
    { d: 'M7 13 9.5 3l2.5 10M8 9.5h3' },
    { d: 'M5.2 3.6a4.5 4.5 0 0 0 0 5.3M13.8 3.6a4.5 4.5 0 0 1 0 5.3', w: 1.6 },
    { d: 'M11 13.5h9v6.6l-1.5-1-1.5 1-1.5-1-1.5 1-1.5-1-1.5 1Z' },
    { d: 'M13 16h5', w: 1.5 },
  ],
  /* ★★★ beta96 THE AUTO-ZOOM STATE (Stuart 11-27) on the map's MAP chip and its AUTO key: a magnifier with an "A" (auto zoom
   *  on); the magnifier PAUSED (‖ — a card is open, amber); the bare magnifier under a NO-ENTRY sign (off: you moved the
   *  map — the sign drawn in red by its holder). One source for the web and the app. */
  zoomAuto: [ { c: [10, 10, 6.5] }, { d: 'M14.8 14.8 20.5 20.5', w: 2.2 }, { d: 'M7.4 13 10 6.8l2.6 6.2M8.3 11h3.4', w: 1.6 } ],
  zoomPause: [ { c: [10, 10, 6.5] }, { d: 'M14.8 14.8 20.5 20.5', w: 2.2 }, { d: 'M8.4 7.4v5.2M11.6 7.4v5.2', w: 2 } ],
  zoomLens: [ { c: [10, 10, 6.5] }, { d: 'M14.8 14.8 20.5 20.5', w: 2.2 } ],
  noEntry: [ { c: [12, 12, 10.2], w: 2.2 }, { d: 'M4.8 19.2 19.2 4.8', w: 2.4 } ],
  /* ★ beta96 a list: three rows, each with its bullet — the web map's LIST chip (there was no house list glyph) */
  list: [
    { d: 'M9 6h11M9 12h11M9 18h11' },
    { c: [4.5, 6, 1.2], fill: true }, { c: [4.5, 12, 1.2], fill: true }, { c: [4.5, 18, 1.2], fill: true },
  ],
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
  balloon: [   // ★ V12 a weather balloon, its line and the sonde hanging below
    { d: 'M12 2.2c3.6 0 6.2 2.7 6.2 6.1 0 3.6-3.2 6.6-6.2 7.6-3-1-6.2-4-6.2-7.6 0-3.4 2.6-6.1 6.2-6.1Z' },
    { d: 'M12 15.9v3.2', w: 1.4 },
    { r: [9.6, 19.1, 4.8, 3.2, 0.6], fill: true },
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
  satellite: [   // ★ V12 SATELLITE mode: a body tipped 45° with a solar panel either side, its downlink waves below-left
    { d: 'M10.6 8.2l3.6-3.6 3.6 3.6-3.6 3.6z' },
    { d: 'M12.4 6.4 9.6 3.6 6.8 6.4l2.8 2.8M16 10l2.8 2.8-2.8 2.8-2.8-2.8', w: 1.5 },
    { d: 'M6.6 14.2a3.6 3.6 0 0 0 3.2 3.2M3.6 14a6.6 6.6 0 0 0 6.4 6.4', w: 1.5 },
    { c: [11.2, 12.8, 1.2], fill: true },
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
    case 'ale': return 'radio';   // ★ V12 2G HF ALE: a net's link-setting bursts
    case 'adsb': case 'hfdl': case 'selcal': case 'acars': case 'vdl2': case 'vdl': case 'hfdlmap': case 'avdata': case 'airsea': return 'aircraft';   // ★★★ beta96 AIR & SEA LIVE wears ADS-B's aircraft (ModeSelector's iconId="adsb")
    case 'ais': case 'dsc': case 'navtex': case 'sitora': case 'sitor': case 'amtor': return 'ship';   // ★ SITOR-A beside NAVTEX (SITOR-B)
    case 'ndb': case 'ncdxf': case 'vor': case 'ils': case 'vormap': return 'beacon';   // ★ V12 NCDXF / IARU beacons · VOR / ILS (a ground navaid, as an NDB)
    case 'sarsat': case 'epirb': case 'elt': case 'plb': return 'ship';  // ★ V12 406 MHz beacons (EPIRBs, mostly)
    case 'ism': return 'sensor';
    case 'sondes': case 'sonde': case 'rs41': case 'radiosonde': return 'balloon';   // ★ V12 radiosondes
    case 'balloon': return 'balloon';   // ★ V12 amateur balloons (UKHAS / Horus / SSDV) — the same glyph
    case 'paging': case 'pocsag': case 'flex': case 'pager': return 'pager';   // ★ V12 PAGING (and OpenWebRX's POCSAG)
    case 'aprs': case 'packet': return 'aprs';
    case 'sstv': return 'tv';
    case 'wefax': case 'fax': return 'fax';
    case 'rtty': case 'rtty45': case 'rtty50': case 'rtty85': case 'rtty170': case 'rtty450': case 'mfrtty': return 'teleprinter';
    case 'cw': case 'morse': case 'cwdecoder': case 'cwskimmer': return 'morseKey';
    case 'psk': case 'bpsk31': case 'bpsk63': case 'psk31': case 'psk63': case 'jtty': case 'mfsk': case 'hell': return 'keyboard';
    case 'tones': case 'dtmf': case 'zvei': case 'ccir': return 'keyboard';   // ★ V12 DTMF + 5-tone: a keypad's tones
    case 'lrpt': case 'meteor': return 'picture';   // ★ V12 METEOR LRPT: a weather-satellite picture (the gallery's METEOR kind)
    case 'sat': case 'satellite': case 'orbcomm': case 'skyplot': return 'satellite';   // ★★ 2026-11-24 ORBCOMM + the sky plot (the gallery's kind)   // ★ V12 SATELLITE mode (one key for every satellite)
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
