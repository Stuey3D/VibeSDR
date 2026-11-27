// liveMaps.ts — ★★★ V12 beta96 THE TWO LIVE MAPS (docs/v12/ONE-MAP.md): AIR & SEA (aviation, maritime, radiosondes)
// and AMATEUR (spots, APRS, mesh, beacons). ONE RULE, TWO READERS: the web client (web/client/src/main.ts, TrackView)
// and the app (SDRScreen → MapOverlay). PURE — no DOM, no React Native, nothing fetched.
//
// ★★★ THE CURRENT MODE DECIDES WHAT IS LIVE (Stuart, 2026-11-26): its layer updates in real time and is emphasised; every
//     other layer is what this server has STORED, faded by age. A mode change while the map is open only changes which
//     layer is live — the title, the exit key and the list follow it on the next render, because they are COMPUTED here
//     from (map, current mode) every time and never kept in the view. (Mac Ha 11-26: the AIS map kept "SHIPPING MAP"
//     after an exit to ADS-B — a view made per mode and titled once. One view per map, chrome recomputed, cannot.)
// ★★ NO DEAD CONTROLS (AGENTS.md): a layer or a tab with nothing on this server is ABSENT; the live one is always there.
import type { AvLayerId } from './aviationMap';
import type { ShipLayerId } from './shippingMap';

export type LiveMapId = 'airsea' | 'amateur';
/** The receiver's current mode, as far as the maps care ('' = none of theirs: every layer is stored). */
export type LiveMode = '' | 'adsb' | 'datalink' | 'hfdl' | 'ndb' | 'vormap' | 'ais' | 'dsc' | 'navtex' | 'sondes'
  | 'spots' | 'aprs' | 'ism' | 'ncdxf';

// ── the layers ─────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ One id space for both maps. The aviation map's 'stations' (HFDL ground stations) is 'hfdlst' here and the
 *  shipping map's 'stations' (NAVTEX) is 'navst' — the two old maps each had a 'stations'. */
export type AirSeaLayerId = 'adsb' | 'acars' | 'vdl2' | 'hfdl' | 'adsc' | 'hfdlst' | 'ndb' | 'vor' | 'airports'
  | 'sondes' | 'ships' | 'dsc' | 'navst' | 'warnings' | 'weather' | 'areas';
export type AmateurLayerId = 'spots' | 'aprs' | 'mesh' | 'beacons';
export type LiveLayerId = AirSeaLayerId | AmateurLayerId;
export type LiveGroupId = 'aviation' | 'aloft' | 'maritime' | 'amateur';
export interface LiveLayerDef { id: LiveLayerId; group: LiveGroupId; label: string; words: string }

export const LIVE_GROUPS: Record<LiveGroupId, string> = { aviation: 'AVIATION', aloft: 'WEATHER ALOFT', maritime: 'MARITIME', amateur: 'AMATEUR' };
export const LIVE_LAYERS: LiveLayerDef[] = [
  { id: 'adsb', group: 'aviation', label: 'ADS-B', words: 'Show the aircraft heard on ADS-B (1090 MHz)' },
  { id: 'vdl2', group: 'aviation', label: 'VDL2', words: 'Show the aircraft heard on VDL Mode 2 (136 MHz)' },
  { id: 'acars', group: 'aviation', label: 'ACARS', words: 'Show the aircraft heard on VHF ACARS (131 MHz)' },
  { id: 'hfdl', group: 'aviation', label: 'HFDL', words: 'Show the aircraft heard on HFDL (HF)' },
  { id: 'adsc', group: 'aviation', label: 'ADS-C', words: 'Show the aircraft whose position came in an ADS-C report' },
  { id: 'hfdlst', group: 'aviation', label: 'HFDL STATIONS', words: 'Show the HFDL ground stations heard' },
  { id: 'ndb', group: 'aviation', label: 'NDB', words: 'Show the NDB beacons heard' },
  { id: 'vor', group: 'aviation', label: 'VOR', words: 'Show the VOR and ILS stations heard' },
  { id: 'airports', group: 'aviation', label: 'AIRPORTS', words: 'Show the airports and runways' },
  { id: 'sondes', group: 'aloft', label: 'SONDES', words: 'Show the radiosondes heard' },
  { id: 'ships', group: 'maritime', label: 'SHIPS', words: 'Show the ships (AIS)' },
  { id: 'dsc', group: 'maritime', label: 'DSC', words: 'Show the DSC calls that sent a position' },
  { id: 'navst', group: 'maritime', label: 'NAVTEX', words: 'Show the NAVTEX stations received' },
  { id: 'warnings', group: 'maritime', label: 'WARNINGS', words: 'Show the NAVTEX navigational warnings' },
  { id: 'weather', group: 'maritime', label: 'WEATHER', words: 'Show the NAVTEX weather: wind arrows per sea area' },
  { id: 'areas', group: 'maritime', label: 'SEA AREAS', words: 'Show the sea areas the forecasts name' },
  { id: 'spots', group: 'amateur', label: 'SPOTS', words: 'Show the stations spotted on the digital modes, CW and RTTY' },
  { id: 'aprs', group: 'amateur', label: 'APRS', words: 'Show the APRS stations heard' },
  { id: 'mesh', group: 'amateur', label: 'MESH', words: 'Show the MeshCore and Meshtastic nodes heard' },
  { id: 'beacons', group: 'amateur', label: 'BEACONS', words: 'Show the NCDXF / IARU beacons heard' },
];
const DEF = new Map(LIVE_LAYERS.map((l) => [l.id, l]));
export const MAP_LAYERS: Record<LiveMapId, LiveLayerId[]> = {
  airsea: ['adsb', 'vdl2', 'acars', 'hfdl', 'adsc', 'hfdlst', 'ndb', 'vor', 'airports', 'sondes', 'ships', 'dsc', 'navst', 'warnings', 'weather', 'areas'],
  amateur: ['spots', 'aprs', 'mesh', 'beacons'],
};
/** ★ The aviation and shipping maps' own ids → ours (their composers are reused unchanged). */
export const AV_ID: Record<AvLayerId, AirSeaLayerId> = { adsb: 'adsb', acars: 'acars', vdl2: 'vdl2', hfdl: 'hfdl', adsc: 'adsc', stations: 'hfdlst', airports: 'airports' };
export const SHIP_ID: Record<ShipLayerId, AirSeaLayerId> = { ships: 'ships', dsc: 'dsc', stations: 'navst', warnings: 'warnings', weather: 'weather', areas: 'areas' };
export function avLayerOf(id: LiveLayerId): AvLayerId | null {
  for (const [k, v] of Object.entries(AV_ID)) if (v === id) return k as AvLayerId;
  return null;
}
export function shipLayerOf(id: LiveLayerId): ShipLayerId | null {
  for (const [k, v] of Object.entries(SHIP_ID)) if (v === id) return k as ShipLayerId;
  return null;
}

// ── the live layer ─────────────────────────────────────────────────────────────────────────────────────────────────
export type AirSeaTab = 'aircraft' | 'messages' | 'ships' | 'navtex' | 'navaids' | 'sondes';
export type AmateurTab = 'spots' | 'aprs' | 'mesh' | 'beacons';
export type LiveTab = AirSeaTab | AmateurTab;
export interface LiveOf {
  map: LiveMapId | null;
  /** the layers this mode feeds in real time (first = the one named in the title) */
  layers: LiveLayerId[];
  tab: LiveTab | null;
  /** the aircraft list's sort when this mode opens the map */
  sort: AircraftSort;
  /** ★ the map IS this mode's screen (EXIT <MODE> leaves the mode); false = a box's map (CLOSE MAP, the mode carries on) */
  screen: boolean;
  /** the words for the title and EXIT key ("ADS-B", "AIS", "DATALINK") */
  name: string;
  /** what the title adds ("ADS-B 1090 MHz") */
  detail: string;
}
const LIVE: Record<Exclude<LiveMode, ''>, LiveOf> = {
  adsb:     { map: 'airsea', layers: ['adsb'], tab: 'aircraft', sort: 'nearest', screen: true, name: 'ADS-B', detail: 'ADS-B 1090 MHz' },
  datalink: { map: 'airsea', layers: ['vdl2', 'acars', 'adsc'], tab: 'messages', sort: 'latest', screen: true, name: 'DATALINK', detail: 'DATALINK' },
  hfdl:     { map: 'airsea', layers: ['hfdl', 'hfdlst'], tab: 'messages', sort: 'latest', screen: false, name: 'HFDL', detail: 'HFDL' },
  ndb:      { map: 'airsea', layers: ['ndb'], tab: 'navaids', sort: 'nearest', screen: true, name: 'NDB', detail: 'NDB' },
  vormap:   { map: 'airsea', layers: ['vor'], tab: 'navaids', sort: 'nearest', screen: true, name: 'VOR MAP', detail: 'VOR / ILS' },
  ais:      { map: 'airsea', layers: ['ships'], tab: 'ships', sort: 'nearest', screen: true, name: 'AIS', detail: 'AIS 161.975 + 162.025 MHz' },
  dsc:      { map: 'airsea', layers: ['dsc'], tab: 'ships', sort: 'nearest', screen: false, name: 'DSC', detail: 'DSC' },
  navtex:   { map: 'airsea', layers: ['navst', 'warnings', 'weather'], tab: 'navtex', sort: 'nearest', screen: false, name: 'NAVTEX', detail: 'NAVTEX' },
  sondes:   { map: 'airsea', layers: ['sondes'], tab: 'sondes', sort: 'nearest', screen: true, name: 'SONDES', detail: 'RADIOSONDES' },
  spots:    { map: 'amateur', layers: ['spots'], tab: 'spots', sort: 'latest', screen: false, name: 'SPOTS', detail: 'SPOTS' },
  aprs:     { map: 'amateur', layers: ['aprs'], tab: 'aprs', sort: 'latest', screen: false, name: 'APRS', detail: 'APRS' },
  ism:      { map: 'amateur', layers: ['mesh'], tab: 'mesh', sort: 'latest', screen: false, name: 'ISM', detail: 'MESH' },
  ncdxf:    { map: 'amateur', layers: ['beacons'], tab: 'beacons', sort: 'latest', screen: false, name: 'NCDXF', detail: 'BEACONS' },
};
const NONE: LiveOf = { map: null, layers: [], tab: null, sort: 'nearest', screen: false, name: '', detail: '' };
/** ★★ What the current mode makes live — on ITS map; on the other map nothing is live. */
export function liveOf(mode: LiveMode | string | null | undefined, map?: LiveMapId): LiveOf {
  const l = mode ? LIVE[mode as Exclude<LiveMode, ''>] : undefined;
  if (!l) return NONE;
  if (map && l.map !== map) return NONE;
  return l;
}

// ── the chrome: title, exit, list title ────────────────────────────────────────────────────────────────────────────
export const MAP_TITLE: Record<LiveMapId, string> = { airsea: 'AVIATION & MARITIME', amateur: 'AMATEUR MAP' };
export interface LiveChrome { title: string; exitLabel: string; exitLeavesMode: boolean; live: LiveOf }
/** ★★★ The title and the exit — COMPUTED, every render (never stored on the view). `only` = THE ONLY MODE: the map's
 *  mode cannot be left, so CLOSE MAP (the note says why). */
export function liveChrome(map: LiveMapId, mode: LiveMode | string | null | undefined, only = false): LiveChrome {
  const live = liveOf(mode, map);
  const title = live.layers.length ? `${MAP_TITLE[map]} · ${live.detail}` : MAP_TITLE[map];
  const leaves = live.screen && !only;
  return { title, exitLabel: leaves ? `EXIT ${live.name}` : 'CLOSE MAP', exitLeavesMode: leaves, live };
}

// ── presence: the layers with something, the live ones first ───────────────────────────────────────────────────────
/** ★★ The layers drawn as chips, in order: the live layer(s) first, then `first` (the layer the map was opened on), then the map's order — a layer with nothing is
 *  absent. `always`: present whatever the count (AIRPORTS — the basemap always has them). One layer alone, no chips. */
export function livePresent(map: LiveMapId, have: Partial<Record<LiveLayerId, number>>, live: LiveOf, always: LiveLayerId[] = [], first: LiveLayerId[] = []): LiveLayerId[] {
  const out: LiveLayerId[] = [];
  const add = (id: LiveLayerId) => { if (MAP_LAYERS[map].includes(id) && !out.includes(id)) out.push(id); };
  for (const id of live.layers) if (id !== 'adsc' || (have.adsc ?? 0) > 0) add(id);   // ★ ADS-C rides the datalink: only when it has some
  // ★ the layer a key or a gallery row opened the map on (not live — its decoder is not running): present and next
  for (const id of first) add(id);
  for (const id of MAP_LAYERS[map]) if ((have[id] ?? 0) > 0 || always.includes(id)) add(id);
  return out;
}
export interface LiveChip { id: LiveLayerId; label: string; words: string; group: string; live: boolean }
/** The chips: a group's name on its first chip (only when two or more groups are drawn), LIVE on the live layer's. */
export function liveChips(present: LiveLayerId[], live: LiveOf): LiveChip[] {
  if (present.length < 2) return [];
  const groups = new Set(present.map((id) => DEF.get(id)!.group));
  const many = groups.size > 1;
  let last = '';
  return present.map((id) => {
    const d = DEF.get(id)!;
    const g = many && d.group !== last ? LIVE_GROUPS[d.group] : '';
    last = d.group;
    const isLive = live.layers.includes(id);
    return { id, label: d.label, words: isLive ? `${d.words} — live now` : d.words, group: g, live: isLive };
  });
}
/** On: present, and (one layer alone, or switched on). ★ A live layer can be switched off like any other (NAVTEX's three
 *  layers are live together, and WARNINGS off to read the weather is the point of a toggle) — its chip stays, marked live. */
export function liveLayerOn(id: LiveLayerId, present: LiveLayerId[], toggled: Partial<Record<LiveLayerId, boolean>>, _live?: LiveOf): boolean {
  if (!present.includes(id)) return false;
  if (present.length < 2) return true;
  return toggled[id] !== false;
}
/** Saved toggles. Old maps' keys (aviation 'stations', shipping 'stations') carried over once as starting values. */
export function liveLayersOf(saved: string | null | undefined, ...old: Array<{ json: string | null | undefined; map: Record<string, LiveLayerId> }>): Partial<Record<LiveLayerId, boolean>> {
  const out: Partial<Record<LiveLayerId, boolean>> = {};
  for (const o of old) {
    try { const j = o.json ? JSON.parse(o.json) : null; if (j && typeof j === 'object') for (const [k, v] of Object.entries(j)) if (typeof v === 'boolean' && o.map[k]) out[o.map[k]] = v; } catch { /* ignored */ }
  }
  try {
    const j = saved ? JSON.parse(saved) : null;
    if (j && typeof j === 'object') for (const [k, v] of Object.entries(j)) if (typeof v === 'boolean' && DEF.has(k as LiveLayerId)) out[k as LiveLayerId] = v;
  } catch { /* default */ }
  return out;
}
/** ★ The toggles a reused composer (avCompose / shipCompose) understands, from ours. */
export function avLayersFrom(t: Partial<Record<LiveLayerId, boolean>>, present: LiveLayerId[], live: LiveOf): Record<AvLayerId, boolean> {
  const out = {} as Record<AvLayerId, boolean>;
  for (const [k, v] of Object.entries(AV_ID)) out[k as AvLayerId] = present.includes(v) ? liveLayerOn(v, present, t, live) : t[v] !== false;
  return out;
}
export function shipLayersFrom(t: Partial<Record<LiveLayerId, boolean>>, present: LiveLayerId[], live: LiveOf): Record<ShipLayerId, boolean> {
  const out = {} as Record<ShipLayerId, boolean>;
  for (const [k, v] of Object.entries(SHIP_ID)) out[k as ShipLayerId] = present.includes(v) ? liveLayerOn(v, present, t, live) : t[v] !== false;
  return out;
}

// ── the basemap ────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ AIR & SEA: airports (unless AIRPORTS is off) AND ports while any maritime layer is drawn. A pack that predates the
 *  'airsea' profiles: the host falls back to aero / aero-plain with 'ports' switched on by name (TrackView.setProfile). */
export function liveProfile(map: LiveMapId, present: LiveLayerId[], on: (id: LiveLayerId) => boolean): string {
  if (map === 'amateur') return 'spots';
  const sea = present.some((id) => DEF.get(id)!.group === 'maritime' && on(id));
  const air = present.includes('airports') ? on('airports') : true;
  if (sea) return air ? 'airsea' : 'airsea-plain';
  return air ? 'aero' : 'aero-plain';
}

// ── the tabs ───────────────────────────────────────────────────────────────────────────────────────────────────────
export const TAB_ORDER: Record<LiveMapId, LiveTab[]> = {
  airsea: ['aircraft', 'messages', 'ships', 'navtex', 'navaids', 'sondes'],
  amateur: ['spots', 'aprs', 'mesh', 'beacons'],
};
export const TAB_LABEL: Record<LiveTab, string> = {
  aircraft: 'AIRCRAFT', messages: 'MESSAGES', ships: 'SHIPS', navtex: 'NAVTEX', navaids: 'NAVAIDS', sondes: 'SONDES',
  spots: 'SPOTS', aprs: 'APRS', mesh: 'MESH', beacons: 'BEACONS',
};
/** ★ The tabs drawn: those with rows, the live layer's tab, and the tab the map was opened on — in the map's order. */
export function liveTabs(map: LiveMapId, have: Partial<Record<LiveTab, number>>, live: LiveOf, opened?: LiveTab | null): LiveTab[] {
  return TAB_ORDER[map].filter((t) => (have[t] ?? 0) > 0 || t === live.tab || t === opened);
}
/** ★★ The tab shown. The viewer's own pick wins while it is drawn; else the live layer's tab (a mode change moves to it);
 *  else the tab it was opened on; else the first drawn. */
export function liveTabPick(tabs: LiveTab[], picked: LiveTab | null | undefined, live: LiveOf, opened?: LiveTab | null): LiveTab | null {
  if (picked && tabs.includes(picked)) return picked;
  if (live.tab && tabs.includes(live.tab)) return live.tab;
  if (opened && tabs.includes(opened)) return opened;
  return tabs[0] ?? null;
}
/** ★ Where a menu entry opens (the map, the layer first, the tab) — the old entries' words kept so nobody loses their way. */
export type LiveOpener = 'adsb' | 'datalink' | 'hfdl' | 'aviation' | 'ais' | 'shipping' | 'navtex' | 'dsc' | 'ndb' | 'vormap' | 'sondes'
  | 'digi' | 'aprs' | 'mesh' | 'beacons';
export function openerOf(o: LiveOpener): { map: LiveMapId; tab: LiveTab; sort: AircraftSort } {
  switch (o) {
    case 'adsb': case 'aviation': return { map: 'airsea', tab: 'aircraft', sort: 'nearest' };
    case 'datalink': case 'hfdl': return { map: 'airsea', tab: 'messages', sort: 'latest' };
    case 'ais': case 'shipping': case 'dsc': return { map: 'airsea', tab: 'ships', sort: 'nearest' };
    case 'navtex': return { map: 'airsea', tab: 'navtex', sort: 'nearest' };
    case 'ndb': case 'vormap': return { map: 'airsea', tab: 'navaids', sort: 'nearest' };
    case 'sondes': return { map: 'airsea', tab: 'sondes', sort: 'nearest' };
    case 'aprs': return { map: 'amateur', tab: 'aprs', sort: 'latest' };
    case 'mesh': return { map: 'amateur', tab: 'mesh', sort: 'latest' };
    case 'beacons': return { map: 'amateur', tab: 'beacons', sort: 'latest' };
    default: return { map: 'amateur', tab: 'spots', sort: 'latest' };
  }
}

// ── the AIRCRAFT list: one row per aircraft, emergency + military pinned, NEAREST | LATEST ──────────────────────────
export type AircraftSort = 'nearest' | 'latest';
export interface SortableRow { key: string; em?: boolean; mil?: boolean; nm?: number | null; age?: number }
/** ★★ Emergency first, then military, then the sort: NEAREST (no position last, then the newest) or LATEST (the most
 *  recently heard). Stable: equal rows keep their order. */
export function aircraftOrder<T extends SortableRow>(rows: T[], sort: AircraftSort): T[] {
  const rank = (r: T) => (r.em ? 0 : r.mil ? 1 : 2);
  const age = (r: T) => (Number.isFinite(r.age as number) ? (r.age as number) : 1e9);
  return rows.map((r, i) => ({ r, i })).sort((a, b) => {
    const d = rank(a.r) - rank(b.r);
    if (d) return d;
    if (sort === 'nearest') {
      const an = a.r.nm ?? null, bn = b.r.nm ?? null;
      if (an != null && bn != null && an !== bn) return an - bn;
      if (an != null && bn == null) return -1;
      if (an == null && bn != null) return 1;
    }
    return age(a.r) - age(b.r) || a.i - b.i;
  }).map((x) => x.r);
}
/** ★ The list's cap (the 1 GB phones): only the tab shown is drawn, and at most this many rows of it. */
export const LIVE_ROW_CAP = 300;

// ── the MESSAGES feed ──────────────────────────────────────────────────────────────────────────────────────────────
/** One datalink message as the feed shows it. ⚖ `text` only where the server sent it (the owner's opt-in, own session). */
export interface LiveMsg { id: string; key: string; t: number; who: string; src: string; label: string; labelName: string; text?: string;
  /** ★ beta96 that message's SNR (dB) and its scale key (src/utils/snrColour.ts) — this server's own decode only */
  snr?: number; snrMode?: string }
/** What the feed is built from: an aircraft (the merged table's row), its message count and its last label. */
export interface MsgSource { key: string; who: string; src: string; n: number; age: number; label?: string; labelName?: string; text?: string;
  snr?: number; snrMode?: string }
/** ★ beta96 the datalink report's last-message SNRs by every key an aircraft goes by (icao / reg / flight — as the bodies
 *  map) — the merged table's rows look theirs up here. Only this server's own decodes have one. */
export function avSnrByKey(aircraft: { icao?: string; reg?: string; flight?: string; snr?: number; sm?: string }[] | null | undefined):
    Map<string, { snr: number; snrMode: string }> {
  const m = new Map<string, { snr: number; snrMode: string }>();
  for (const a of aircraft ?? []) if (a.snr != null && a.sm) for (const k of [a.icao, a.reg, a.flight]) if (k) m.set(k, { snr: a.snr, snrMode: a.sm });
  return m;
}
export const MSG_CAP = 200;
/** ★★ The feed, newest first: on each report an aircraft whose count grew (or a first sight with a label) adds its last
 *  message, timed `age` seconds before `nowMs`. Bounded (MSG_CAP). `seen` keeps each key's last count. Pure: returns the
 *  new feed and changes `seen` in place. */
export function msgFeedUpdate(feed: LiveMsg[], seen: Map<string, number>, from: MsgSource[], nowMs: number): LiveMsg[] {
  const add: LiveMsg[] = [];
  for (const a of from) {
    if (!a.key || !(a.label || a.labelName || a.text)) { if (a.key) seen.set(a.key, a.n); continue; }
    const was = seen.get(a.key);
    seen.set(a.key, a.n);
    if (was != null && a.n <= was) continue;
    const t = Math.round(nowMs / 1000 - Math.max(0, a.age || 0));
    add.push({ id: `${a.key}#${a.n}`, key: a.key, t, who: a.who, src: a.src, label: a.label || '', labelName: a.labelName || '', ...(a.text ? { text: a.text } : {}),
               ...(a.snr != null && a.snrMode ? { snr: a.snr, snrMode: a.snrMode } : {}) });
  }
  if (seen.size > 4096) { const drop = seen.size - 4096; let i = 0; for (const k of seen.keys()) { if (i++ >= drop) break; seen.delete(k); } }
  if (!add.length) return feed;
  const ids = new Set(add.map((m) => m.id));
  return [...add, ...feed.filter((m) => !ids.has(m.id))].sort((a, b) => b.t - a.t).slice(0, MSG_CAP);
}
/** ★ Tap an aircraft → the messages are filtered to it ('' = every message). */
export function msgFilter(feed: LiveMsg[], key: string | null | undefined): LiveMsg[] {
  return key ? feed.filter((m) => m.key === key) : feed;
}

// ── the fade ───────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ A stored item's strength by age (s): full while fresh, down to 0.4 at `full` seconds and beyond. Live = 1. */
export function fadeOf(ageSec: number, live: boolean, fresh = 600, full = 6 * 3600): number {
  if (live || !(ageSec > fresh)) return 1;
  if (ageSec >= full) return 0.4;
  return Math.round((1 - 0.6 * (ageSec - fresh) / (full - fresh)) * 100) / 100;
}

/** ★★★ beta96 SHOW (Stuart 11-26): each layer chip's colour dot — the web's and the app's, the community map's colours. */
export const LIVE_CHIP_COLOUR: Partial<Record<LiveLayerId, string>> = {
  adsb: '#ffd34d', vdl2: '#7ee787', acars: '#79c0ff', hfdl: '#ffa657', adsc: '#d2a8ff', hfdlst: '#ffa657', airports: '#9fb0c2',
  ships: '#4fc3f7', dsc: '#ff6b6b', navst: '#f0c674', warnings: '#ff8a65', weather: '#81c784', areas: '#e0a040',
  vor: '#c792ea', ndb: '#ffb000', sondes: '#80deea',
};
