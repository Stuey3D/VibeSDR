// airSeaScope.ts — beta96 the AIR & SEA map's sources through the scope (mapScope.ts): OTHER RADIOS ON THIS SERVER and
// THIS SESSION, applied to the DATA before anything is composed — so the chips' SHOW counts, the tabs, the items and the
// rows all follow from one filter. One rule for the web (main.ts asRender) and the app (SDRScreen airSeaTrk). Pure —
// scripts/test_map_scope.ts.
//
// ★ Times: a server's absolute times are moved onto this viewer's clock by the answer's own skew (its `now` minus ours
//   when it arrived), so a server whose clock is a minute out cannot put a fresh ship before the session's start.
// ★ What this radio's live report carries is this radio's: only THIS SESSION filters it (first heard from the shared
//   table where it knows the aircraft / the ship's own age / the VOR's first; else FirstSeen — when this viewer first saw it).
import { scopeRadioOk, scopeSessionOk, type MapScope, type FirstSeen } from './mapScope';
import type { SharedAircraft } from './aviationMap';
import type { StoredShips, DscPin } from './shippingMap';
import type { NavReport } from './navtexMap';
import type { NavWarnReport, NavMetReport } from './navtexWarn';
import type { GalPos } from './storedLayers';

export interface AirSeaSources {
  /** aircraft.json and its skew (server now − our now when it arrived, s) */
  shared: SharedAircraft | null; sharedSkew: number;
  /** the ship store, the NAVTEX stations / warnings / weather, the gallery's DSC alerts, and their skew */
  ships: StoredShips | null; nav: NavReport | null; warn: NavWarnReport | null; met: NavMetReport | null; dscG: DscPin[]; navSkew: number;
  /** this radio's live reports (only THIS SESSION filters them) */
  adsb: { aircraft: ReadonlyArray<{ hex?: unknown }> } | null;
  ais: { now?: number; vessels: ReadonlyArray<{ mmsi: number; age: number }> } | null;
  av: { aircraft: ReadonlyArray<{ icao?: string; reg?: string; flight?: string }> } | null;
  sd: { sondes: ReadonlyArray<{ serial: string }> } | null;
  vm: { now: number; beacons: ReadonlyArray<{ hz: number; first: number }> } | null;
  /** the gallery's remembered sondes / VOR stations */
  sdStored: GalPos[]; vmStored: GalPos[];
}
const regKey = (r: string) => r.replace(/[.\- ]/g, '').toUpperCase();
const hexOf = (v: unknown) => (typeof v === 'string' ? v.replace(/^~/, '').toUpperCase() : '');

/** ★★ The sources through the scope (the same object back when nothing filters). `now` = this viewer's clock (unix s). */
export function airSeaScope<T extends AirSeaSources>(raw: T, sc: MapScope, now: number, fs: FirstSeen): T {
  const radio = !(sc.others || !sc.mine), sess = !!sc.t0;
  const rOk = (r?: string[]) => scopeRadioOk(r, sc);
  const tOk = (f: number | null | undefined) => scopeSessionOk(f, sc);
  // ── ★ OBSERVE ALWAYS (session picked or not): an entry the server gives no first-heard time for is stamped when this viewer
  //     first sees it, from the moment the map is up — so picking THIS SESSION later still knows what came before ──
  const sNow = raw.shared ? raw.shared.now - raw.sharedSkew : now;   // the answer's moment on our clock
  const firstOf = (p: { key: string; first?: number }) => fs.first(`a:${p.key}`, now, p.first != null ? sNow - p.first : null);
  const byIcao = new Map<string, number>(), byReg = new Map<string, number>(), byFlt = new Map<string, number>();
  for (const p of raw.shared?.aircraft ?? []) {
    const f = firstOf(p);
    if (p.icao) byIcao.set(p.icao.toUpperCase(), f);
    if (p.reg) byReg.set(regKey(p.reg), f);
    if (p.flight) byFlt.set(p.flight.replace(/[. ]/g, '').toUpperCase(), f);
  }
  const adsbFirst = (a: { hex?: unknown }) => { const h = hexOf(a.hex); return fs.first(`adsb:${h}`, now, byIcao.get(h) ?? null); };
  const avFirst = (a: { icao?: string; reg?: string; flight?: string }) => {
    const f = (a.icao ? byIcao.get(a.icao.toUpperCase()) : undefined) ?? (a.reg ? byReg.get(regKey(a.reg)) : undefined)
      ?? (a.flight ? byFlt.get(a.flight.replace(/[. ]/g, '').toUpperCase()) : undefined) ?? null;
    return fs.first(`av:${a.icao || a.reg || a.flight || ''}`, now, f);
  };
  const vmSkew = raw.vm ? raw.vm.now - now : 0;
  const vmFirst = (b: { hz: number; first: number }) => (b.first ? b.first - vmSkew : fs.first(`vor:${b.hz}`, now));
  const sdFirst = (x: { serial: string }) => fs.first(`sd:${x.serial}`, now);
  for (const a of raw.adsb?.aircraft ?? []) adsbFirst(a);
  for (const a of raw.av?.aircraft ?? []) avFirst(a);
  for (const x of raw.sd?.sondes ?? []) sdFirst(x);
  for (const b of raw.vm?.beacons ?? []) vmFirst(b);
  if (!radio && !sess) return raw;
  // ── the shared aircraft table ──
  const shared = raw.shared ? { ...raw.shared, aircraft: raw.shared.aircraft.filter((p) => rOk(p.radios) && (!sess || tOk(firstOf(p)))) } : raw.shared;
  // ── the maritime stores (absolute server times → our clock) ──
  const ns = raw.navSkew;
  const ships = raw.ships ? { ...raw.ships, vessels: raw.ships.vessels.filter((v) => rOk(v.radios) && (!sess || tOk((raw.ships!.now || now + ns) - ns - v.age))) } : null;
  const nav = raw.nav ? { ...raw.nav, stations: raw.nav.stations.filter((s) => rOk(s.radios) && (!sess || tOk(s.first ? s.first - ns : null))) } : null;
  // ★ the warnings and the weather carry no radio yet (unknown: shown under every radio — the logs' rule)
  const warn = raw.warn && sess ? { ...raw.warn, list: raw.warn.list.filter((w) => tOk(w.first ? w.first - ns : null)) } : raw.warn;
  const met = raw.met && sess ? { ...raw.met, areas: raw.met.areas.filter((a) => tOk(a.fc[0] ? a.fc[0].t - ns : null)) } : raw.met;
  const dscG = raw.dscG.filter((p) => rOk(p.radios) && (!sess || tOk(p.t - ns)));
  const sdStored = raw.sdStored.filter((p) => rOk(p.radios) && (!sess || tOk(p.first - ns)));
  const vmStored = raw.vmStored.filter((p) => rOk(p.radios) && (!sess || tOk(p.first - ns)));
  if (!sess) return { ...raw, shared, ships, nav, warn, met, dscG, sdStored, vmStored } as T;
  // ── THIS SESSION on this radio's live reports ──
  const adsb = raw.adsb ? { ...raw.adsb, aircraft: raw.adsb.aircraft.filter((a) => tOk(adsbFirst(a))) } : null;
  const ais = raw.ais ? { ...raw.ais, vessels: raw.ais.vessels.filter((v) => tOk(now - v.age)) } : null;
  const av = raw.av ? { ...raw.av, aircraft: raw.av.aircraft.filter((a) => tOk(avFirst(a))) } : null;
  const sd = raw.sd ? { ...raw.sd, sondes: raw.sd.sondes.filter((x) => tOk(sdFirst(x))) } : null;
  const vm = raw.vm ? { ...raw.vm, beacons: raw.vm.beacons.filter((b) => tOk(vmFirst(b))) } : null;
  return { ...raw, shared, ships, nav, warn, met, dscG, sdStored, vmStored, adsb, ais, av, sd, vm } as T;
}
/** ★ This listener's own DSC box calls and the NDB beacons (this radio's): THIS SESSION only. Call it whether or not the
 *  session is picked — it OBSERVES the entry too (FirstSeen), so picking the session later knows what came before. */
export function airSeaLiveOk(key: string, firstClient: number | null, sc: MapScope, now: number, fs: FirstSeen): boolean {
  const f = fs.first(key, now, firstClient);
  return !sc.t0 || scopeSessionOk(f, sc);
}
