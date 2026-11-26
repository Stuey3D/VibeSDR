/**
 * ★★★ THE MAP PAGE BELONGS TO ONE KIND OF MAP — AND TO ONE OPENING OF IT (beta96, Mac Ha's report, Discord 11-26).
 *
 * "If you switch between AIS and ADS-B, there is a mismatch between the map and the radio … at the top it still says AIS
 *  Shipping map" — his screenshots: EXIT AIS · SHIPPING MAP with the AIRCRAFT list under it, and EXIT ADS-B · ADS-B
 *  AIRCRAFT MAP with the RECEIVED ships under it.
 *
 * ★★ WHY. MapOverlay writes its page to ONE file (mapPack.ts writeMapPage → <documents>/vibemap.html) and hands the
 *    WebView that file's uri. The uri is the same string for every kind of map, so:
 *    1. A MODE CHANGE WHILE THE MAP IS UP (the shared dial: another listener — or the server's mode_resume — moves the
 *       radio from AIS to ADS-B; the server's ais_off and the first ADS-B report change `kind` under the open map):
 *       the new page was written, setPageUri(the same uri) changed nothing, and the WebView never reloaded. The lists
 *       (RN-drawn, they follow `kind`) changed; the page — its title, EXIT key, layers — stayed the old mode's.
 *    2. EXIT, THEN ANOTHER MODE (the path Mac Ha describes): the overlay stays MOUNTED while hidden and kept the old
 *       pageUri, so the next opening mounted its WebView AT ONCE on the file still holding the OLD page (the new one is
 *       written a beat later — ADS-B's even later, after its saved toggles are read), and the write that followed set
 *       the same uri: no reload. Exiting and choosing the mode again "fixed" it because by then the file held it.
 *
 * ★★ THE RULE (one place, tested — scripts/test_map_page_kind.ts): a written page is remembered WITH the kind it was
 *    built for and the opening it was built in. It is shown only while both still match; a different kind (or a new
 *    opening) gets a NEW page id, which is the WebView's key, so a fresh WebView loads the fresh file. A rewrite for the
 *    same kind in the same opening (the receiver's position arriving, a colour) keeps its id — the map is not torn down
 *    under the user's finger for that, exactly as before.
 */

export interface MapPage<K extends string = string> {
  /** the file the page was written to (the same file every time — see above) */
  uri: string;
  /** the kind of map this page was BUILT for */
  kind: K;
  /** the opening of the overlay it was built in */
  open: number;
  /** its id — the WebView's key: a new one means a fresh WebView on the fresh file */
  id: number;
}

/** ★ A page has just been written for `kind` in opening `open`: keep the id when it replaces the same kind's page in the
 *  same opening, else a new one (`nextId` — a counter the caller owns). */
export function mapPageWritten<K extends string>(prev: MapPage<K> | null, uri: string, kind: K, open: number,
                                                 nextId: () => number): MapPage<K> {
  if (prev && prev.kind === kind && prev.open === open) return prev.uri === uri ? prev : { ...prev, uri };
  return { uri, kind, open, id: nextId() };
}

/** ★ The page to show now, or null (nothing yet — never the last mode's page, never the last opening's). */
export function mapPageShown<K extends string>(page: MapPage<K> | null, visible: boolean, kind: K | null,
                                               open: number): MapPage<K> | null {
  if (!visible || !kind || !page) return null;
  return page.kind === kind && page.open === open ? page : null;
}

/* ★★ WHICH SERVER MAP THE RECEIVER SCREEN SHOWS — the precedence SDRScreen used inline, lifted out so the mode changes
 *    can be driven in a test (AIS → ADS-B by the shared dial; exit then another mode). The whole-radio modes' maps are
 *    their screens; the decoder boxes' MAPs follow; `picked` is a server map opened from the Mode sheet. */
export type ShownMapKind = 'hfdl' | 'digi' | 'cw' | 'ais' | 'aprs' | 'ndb' | 'dsc' | 'ism' | 'navtex' | 'sondes' | 'avdata' | 'vormap'
  | 'airsea';   // ★★★ beta96 THE AIR & SEA MAP (docs/v12/ONE-MAP.md): ADS-B and AIS modes' map, the shipping map and the aviation map
export interface ShownMapState {
  appleTv: boolean;
  adsbView: boolean; ndbView: boolean; aisView: boolean; aprsMap: boolean; dscMap: boolean; navMap: boolean;
  ismGallery: boolean; ismMap: boolean; ismOn: boolean;
  vmMap: boolean; vmOn: boolean; sdMap: boolean; sdOn: boolean;
  avMap: boolean; avOn: boolean; avFocusDatalink: boolean;
  picked: ShownMapKind | null;
}
export function shownMapOf(s: ShownMapState): ShownMapKind | null {
  if (s.appleTv) return s.picked;   // ★ no WebView on Apple TV — only the Mode sheet's server maps were ever offered there
  // ★★★ beta96 stage 6: ADS-B's screen is the AIR & SEA map too — AIS ↔ ADS-B is the SAME page, re-titled (__trkChrome)
  return s.adsbView ? 'airsea' : s.ndbView ? 'ndb'
    : s.aisView ? 'airsea' : s.aprsMap ? 'aprs'
    : s.dscMap ? 'dsc'
    : s.navMap ? 'airsea'
    : s.ismGallery ? 'ism'
    : s.ismMap && s.ismOn ? 'ism'
    : s.vmMap && s.vmOn ? 'vormap'
    : s.sdMap && s.sdOn ? 'sondes'
    : s.avMap && (s.avOn || !s.avFocusDatalink) ? 'airsea' : s.picked;
}
