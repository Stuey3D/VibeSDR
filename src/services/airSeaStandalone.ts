/* ★★★ V12 beta96 THE STANDALONE AIR & SEA MAP's COMPOSITION, for the app (docs/v12/ADSB-FEED.md §The standalone AIR & SEA
 * map) — the AIR & SEA map with NO RADIO: no live mode, no ADS-B report, no AIS report, no NDB — only what this server
 * remembers and shares over HTTP: the shared aircraft table (every radio's and every feed's aircraft), the ship store, DSC
 * from the gallery, the NAVTEX stations / warnings / weather, the sea areas, and the gallery's remembered sondes and VOR
 * stations. The SHOW chips and the normal time windows only.
 *
 * ★★ THE SAME RULES AS A RADIO'S AIR & SEA MAP (SDRScreen airSeaTrk, the web's asRender) with the live mode '' — the same
 *    liveMaps.ts functions (chips, tabs, the row order, the messages feed), the same composers (avCompose, the ship store's
 *    rows, the NAVTEX layers, storedLayers). Pure: the screen (AirSeaScreen.tsx) owns the state and the polls; this turns
 *    them into MapOverlay's `trk`. Tested in node (scripts/test_airsea_standalone.ts).
 */
import { AV_EMPTY, AV_SRC, avCompose, avHave, avNowRule, avMerge, type AvLayers, type SharedAircraft } from './aviationMap';
import { liveChrome, livePresent, liveChips, liveProfile, liveTabs, liveTabPick, aircraftOrder, msgFeedUpdate, msgFilter, avLayerOf, shipLayerOf,
         AV_ID, SHIP_ID, TAB_LABEL, LIVE_ROW_CAP, type LiveLayerId, type LiveTab, type AircraftSort, type LiveMsg } from './liveMaps';
import { shipLayersFor, shipToggle, shipLayersPresent, shipOn, shipShips, shipItems, shipRows, shipStatus,
         dscInRange, dscPinItems, dscPinRows, dscStatus, type ShipLayers, type ShipLayerId, type StoredShips, type DscPin } from './shippingMap';
import { navItems, navListRows, navStatus, navName, NAV_RANGES, NAV_RANGE_HINT, type NavReport } from './navtexMap';
import { navWarnVisible, navWarnItems, navWarnStatus, navWarnSummary, navWarnTag, navMetVisible, navMetListed, navMetRowWords, navMetItems,
         navMetStatus, navFcGale, navBuilding, type NavWarnReport, type NavMetReport } from './navtexWarn';
import { NAV_HAZARD_COLOURS } from './navtexIcons';
import { seaCompose, seaAreasFor, seaPlaceMet } from './seaAreas';
import { storedSondeItems, storedVorItems, storedSondeRows, storedVorRows, storedStatus, type GalPos } from './storedLayers';
import { AIS_EMPTY, type LatLon, type TrackItem, type TrackRow } from './tracks';

export interface AirSeaState {
  shared: SharedAircraft | null;
  shipStore: StoredShips | null;
  dscGal: DscPin[];
  navReport: NavReport | null;
  navWarnRep: NavWarnReport | null;
  navMetRep: NavMetReport | null;
  navRange: string;
  sdStored: GalPos[];
  vmStored: GalPos[];
  avLayers: AvLayers;
  shipLayers: ShipLayers;
  rx: LatLon | null;
  view: [number, number, number, number] | null;
  picked: LiveTab | null;
  opened: LiveTab | null;
  sort: AircraftSort;
  msgKey: string;
  /** the status line's first part: the time limit's clock ('' = none) */
  clock: string;
  nowMs: number;
}
export interface AirSeaHandlers {
  setPicked: (t: LiveTab) => void;
  setSort: (s: AircraftSort) => void;
  setMsgKey: (k: string) => void;
  setAvLayers: (f: (l: AvLayers) => AvLayers) => void;
  setShipLayers: (f: (l: ShipLayers) => ShipLayers) => void;
  setNavRange: (r: string) => void;
  onView: (b: [number, number, number, number]) => void;
  cardText: (file: string) => Promise<{ ok: boolean; text: string }>;
}
/** The MESSAGES feed is kept across renders (its newest-first list grows as aircraft speak) — the caller holds one. */
export interface MsgFeed { feed: LiveMsg[]; seen: Map<string, number> }
export function newMsgFeed(): MsgFeed { return { feed: [], seen: new Map() }; }

const ago = (sec: number) => (sec < 3600 ? `${Math.round(sec / 60)} min` : sec < 86400 ? `${Math.round(sec / 3600)} h` : `${Math.round(sec / 86400)} d`);

export function composeAirSeaStandalone(st: AirSeaState, h: AirSeaHandlers, msgs: MsgFeed) {
  const chrome = liveChrome('airsea', '', false);
  const live = chrome.live;
  const rx = st.rx ?? st.navReport?.rx ?? null;
  // ── aviation: the shared table only (avCompose with no live report) ──
  const av = avCompose(null, st.shared, 'adsb', st.avLayers, rx, 'app');
  // ── maritime: the ship store, DSC from the gallery, NAVTEX ──
  const now = st.navWarnRep?.now ?? st.navMetRep?.now ?? st.navReport?.now ?? Math.floor(st.nowMs / 1000);
  const vessels = shipShips(null, st.shipStore);
  const pins = dscInRange(st.dscGal, now);
  const nearAreas = seaAreasFor(rx, st.navMetRep).length;
  const shipHave = { ships: vessels.length, dsc: pins.length, stations: st.navReport?.stations.length ?? 0,
    warnings: st.navWarnRep ? st.navWarnRep.list.length + st.navWarnRep.pending : 0, weather: st.navMetRep?.areas.length ?? 0, aisMode: false,
    areas: nearAreas };
  // ★ no opened layer here (no radio, no mode asked for one): a layer with nothing in it is absent — SEA AREAS excepted (drawn worldwide)
  const shipPresent = shipLayersPresent(shipHave, 'ships').filter((id) => id === 'areas' || (shipHave as Record<string, number | boolean>)[id] as number > 0);
  const effShipBase = shipLayersFor(st.shipLayers, nearAreas > 0);
  // ── the chips: each composer's own presence, the remembered sondes / VOR ──
  const have: Partial<Record<LiveLayerId, number>> = {};
  const avCounts = avHave(avNowRule(avMerge(null, st.shared), st.shared).planes, st.shared?.stations ?? []) as unknown as Record<string, number>;
  for (const x of av.present ?? []) if (x !== 'airports' && (avCounts[x] ?? 0) > 0) have[AV_ID[x]] = avCounts[x];   // ★ no forced ADS-B chip
  for (const id of shipPresent.map((x) => SHIP_ID[x])) have[id] = 1;
  const vmMem = st.vmStored.length > 0, sdMem = st.sdStored.length > 0;
  if (vmMem) have.vor = st.vmStored.length;
  if (sdMem) have.sondes = st.sdStored.length;
  const nowStored = Math.floor(st.nowMs / 1000);
  const aviationAny = (av.planes?.length ?? 0) > 0 || vmMem;
  const present = livePresent('airsea', have, live, aviationAny ? ['airports'] : []).filter((id) => have[id] || id === 'airports');
  const seaHere = present.some((id) => id !== 'areas' && shipLayerOf(id) != null);
  const effShip: ShipLayers = st.shipLayers.areasSet ? effShipBase : { ...effShipBase, areas: seaHere && nearAreas > 0 };
  const toggled: Record<string, boolean> = {};
  for (const id of present) {
    const a = avLayerOf(id), sh = shipLayerOf(id);
    toggled[id] = a ? st.avLayers[a] !== false : sh ? effShip[sh] !== false : true;
  }
  const on = (id: LiveLayerId) => present.includes(id) && toggled[id] !== false;
  const shOn = (id: ShipLayerId) => shipOn(id, shipPresent, effShip) && on(SHIP_ID[id]);
  // ── the ship / NAVTEX items and rows (SDRScreen shipCompose + navTrk, no live AIS) ──
  const sea = shOn('areas') ? seaCompose(rx, st.navMetRep, shOn('weather'), 'app', st.view).items : [];
  const shipItemsA = [...sea, ...(shOn('ships') ? shipItems(vessels, rx) : []), ...(shOn('dsc') ? dscPinItems(pins, rx, now, 'app') : [])];
  const shipRowsA = [...(shOn('ships') ? shipRows(vessels, rx) : []), ...(shOn('dsc') ? dscPinRows(pins, rx, now) : [])];
  const warns = shOn('warnings') ? navWarnVisible(st.navWarnRep, st.navRange) : [];
  const areas = shOn('weather') ? seaPlaceMet(navMetVisible(st.navMetRep, st.navRange)) : [];
  const listed = shOn('weather') ? navMetListed(st.navMetRep) : [];
  const navRows: TrackRow[] = [
    ...(shOn('stations') ? navListRows(st.navReport) : []),
    ...listed.slice().sort((a, b) => Number(a.stale) - Number(b.stale) || Number(navFcGale(b.fc[0])) - Number(navFcGale(a.fc[0])) || b.fc[0].t - a.fc[0].t).map((a) => ({
      key: a.key, colour: a.stale ? '#9aa6b2' : navFcGale(a.fc[0]) ? '#ff6b5a' : '#5d8fb8', flag: '', name: a.name, second: navMetRowWords(a), sp: '', trend: 0 as const,
      range: '', brg: '', noPos: a.stale, age: ago(Math.max(0, now - a.fc[0].t)), em: false })),
    ...warns.slice().sort((a, b) => Number(!!navWarnTag(a)) - Number(!!navWarnTag(b)) || b.last - a.last).slice(0, 300).map((w) => ({
      key: w.key, colour: navWarnTag(w) ? '#9aa6b2' : NAV_HAZARD_COLOURS[w.cls === 'navaid' ? 'light' : (w.cls as keyof typeof NAV_HAZARD_COLOURS)] ?? '#d7dee6', flag: '',
      name: `${w.id} · ${navWarnSummary({ ...w, hit: '', lang: '' })}`, second: [w.st ? navName(w.st, w.sc) : '', navWarnTag(w)].filter(Boolean).join(' · '),
      sp: '', trend: 0 as const, range: '', brg: '', noPos: false, age: ago(Math.max(0, now - w.last)), em: false })),
  ];
  const navItemsA = [...(shOn('stations') ? navItems(st.navReport) : []), ...navMetItems(areas, now, 'app'), ...navWarnItems(warns, now, 'app')];
  const building = navBuilding(st.navWarnRep);
  const shipStatusLine = [shOn('ships') ? shipStatus(vessels, st.shipStore, null) : '', shOn('dsc') ? dscStatus(pins) : '',
    shOn('stations') ? navStatus(st.navReport) : '', shOn('warnings') ? navWarnStatus(st.navWarnRep, st.navRange) : '', shOn('weather') ? navMetStatus(st.navMetRep, st.navRange) : '', building].filter(Boolean).join(' · ');
  // ── MESSAGES: every aircraft's last message as it changes ──
  const planesAll = avNowRule(avMerge(null, st.shared), st.shared).planes;
  msgs.feed = msgFeedUpdate(msgs.feed, msgs.seen, planesAll.map((p) => ({ key: `a:${p.key}`, who: p.flight || p.reg || p.icao || 'aircraft',
    src: p.src.join(' + ') || p.posSrc || '', n: p.n, age: p.age, label: p.label, labelName: p.labelName })), st.nowMs);
  const msgList = msgFilter(msgs.feed, st.msgKey).slice(0, LIVE_ROW_CAP);
  const hm = (t: number) => { const d = new Date(t * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
  const msgRows: TrackRow[] = msgList.map((m) => ({ key: `m:${m.id}`, colour: AV_SRC.find((x) => x.name === m.src.split(' + ')[0])?.colour ?? '#9fb0c2', flag: '', name: m.who,
    second: [m.label, m.labelName, m.src, m.text ?? ''].filter(Boolean).join(' · '), sp: '', trend: 0 as const, range: '', brg: '', noPos: false, age: hm(m.t), em: false }));
  // ── AIRCRAFT: one row each, emergency + military pinned, NEAREST | LATEST ──
  const byKey = new Map(av.rows.map((r) => [r.key, r]));
  const order = aircraftOrder((av.planes ?? []).map((p) => {
    let nm: number | null = null;
    if (rx && p.lat != null && p.lon != null) { const d2r = Math.PI / 180; const hv = Math.sin((p.lat - rx.lat) * d2r / 2) ** 2 + Math.cos(rx.lat * d2r) * Math.cos(p.lat * d2r) * Math.sin((p.lon - rx.lon) * d2r / 2) ** 2;
      nm = 2 * 6371.0088 * Math.asin(Math.min(1, Math.sqrt(hv))) / 1.852; }
    return { key: `a:${p.key}`, mil: !!p.info?.mil, em: false, nm, age: Math.min(p.age, ...Object.values(p.srcAge)) };
  }), st.sort).slice(0, LIVE_ROW_CAP);
  const acRows = order.map((x) => byKey.get(x.key)).filter((r): r is TrackRow => !!r);
  const shipTabRows = (on('ships') || on('dsc')) ? shipRowsA : [];
  const vorRows = vmMem && on('vor') ? storedVorRows(st.vmStored, nowStored, rx) : [];
  const sondeRowsA = sdMem && on('sondes') ? storedSondeRows(st.sdStored, nowStored, rx) : [];
  const tabHave: Partial<Record<LiveTab, number>> = { navaids: vorRows.length, sondes: sondeRowsA.length, aircraft: acRows.length,
    messages: msgs.feed.length, ships: shipTabRows.length, navtex: navRows.length };
  const tabs = liveTabs('airsea', tabHave, live, st.opened);
  const tab = liveTabPick(tabs, st.picked, live, st.opened) ?? 'aircraft';
  const rows = tab === 'aircraft' ? acRows : tab === 'messages' ? msgRows : tab === 'ships' ? shipTabRows.slice(0, LIVE_ROW_CAP)
    : tab === 'navaids' ? vorRows.slice(0, LIVE_ROW_CAP) : tab === 'sondes' ? sondeRowsA.slice(0, LIVE_ROW_CAP) : navRows.slice(0, LIVE_ROW_CAP);
  const msgWho = st.msgKey ? (msgs.feed.find((m) => m.key === st.msgKey)?.who ?? st.msgKey.replace(/^a:/, '')) : '';
  const maritime = present.some((id) => shipLayerOf(id) != null && id !== 'areas');
  return {
    title: chrome.title, exitLabel: 'CLOSE MAP',
    camera: (present.some((l) => shipLayerOf(l) != null && l !== 'areas') && !(av.planes?.length) ? 'track' : 'aircraft') as 'aircraft' | 'track',
    items: [...(vmMem && on('vor') ? storedVorItems(st.vmStored, nowStored) : []), ...(sdMem && on('sondes') ? storedSondeItems(st.sdStored, nowStored) : []),
            ...(aviationAny ? (av.items as TrackItem[]) : []), ...shipItemsA, ...navItemsA],
    rows,
    status: [st.clock, vmMem && on('vor') ? storedStatus('vormap', st.vmStored.length) : '', sdMem && on('sondes') ? storedStatus('sondes', st.sdStored.length) : '',
             aviationAny ? av.status : '', shipStatusLine].filter(Boolean).join(' \u00b7 '),
    empty: tab === 'aircraft' ? AV_EMPTY : tab === 'messages' ? 'No datalink messages yet. ACARS, VDL2 and HFDL each add theirs \u2014 on any radio of this server.' : tab === 'ships' ? AIS_EMPTY : 'Nothing here in this time.',
    note: building,
    credit: tab === 'aircraft' && !!av.credit,
    toggles: liveChips(present, live).map((c) => ({ id: c.id, label: c.label, words: c.words })),
    toggled,
    onToggle: (id: string, v: boolean) => {
      const a = avLayerOf(id as LiveLayerId), sh = shipLayerOf(id as LiveLayerId);
      if (a) h.setAvLayers((l) => ({ ...l, [a]: v }));
      else if (sh) h.setShipLayers((cur) => shipToggle(shipLayersFor(cur, nearAreas > 0), sh, v));
    },
    profile: liveProfile('airsea', present, on),
    // ★ the normal time windows: the maritime filter where a maritime layer is drawn (aircraft keep the table's hour)
    ranges: maritime ? NAV_RANGES : undefined, range: st.navRange, rangeHint: NAV_RANGE_HINT,
    onRange: (id: string) => h.setNavRange(id),
    onView: h.onView,
    cardText: h.cardText,
    tabs: tabs.map((t) => ({ id: t, label: TAB_LABEL[t], n: t === 'messages' ? msgFilter(msgs.feed, st.msgKey).length : tabHave[t] ?? 0 })),
    tab, onTab: (t: string) => h.setPicked(t as LiveTab),
    sort: tab === 'aircraft' ? st.sort : null,
    onSort: (v: string) => h.setSort(v === 'latest' ? 'latest' : 'nearest'),
    filter: tab === 'messages' && st.msgKey ? `Messages from ${msgWho}` : '', onFilterClear: () => h.setMsgKey(''),
    onPick: (k: string) => {
      if (k.startsWith('a:')) { h.setMsgKey(k); return k; }
      if (k.startsWith('m:')) { const m = msgs.feed.find((x) => `m:${x.id}` === k); return m ? m.key : k; }
      return k;
    },
    trail: (key: string) => Promise.resolve(key.startsWith('s:') ? (st.shipStore?.pts.get(Number(key.slice(2))) ?? []) : [] as [number, number][]),
    /** for the screen and tests: what is present, and the counts per tab */
    present, tabHave,
  };
}
