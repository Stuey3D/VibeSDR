/* ★★★ beta96 THE AIR & SEA MAP'S TUNE LIST (Stuart 11-26) — ONE collapsible chip in the map's top bar ("TUNE · VDL2
 *  136.700–136.975 MHz ▾") that opens a list of the air / sea whole-radio modes THIS radio can run, grouped AVIATION /
 *  MARITIME / WEATHER BALLOONS, each row an icon, the mode and its frequencies. ONE reader for the browser and the app.
 *
 *  ★★★ EVERYTHING COMES FROM THE RADIO'S OWN vibeserver.json (per radio — the radio the listener is on, never the machine):
 *    - a mode with no capability field is NOT a row (it cannot reach the band — AGENTS.md: no dead controls); a capability
 *      false is a row GREYED with the server's reason (the owner switched it off, the rate is too low);
 *    - the bands and their frequencies are the server's tables: `avdataBands` (vibeavi::bands — chLo/chHi), `sondesBands`
 *      (vibers41::bands — lo/hi), `ndbLo`/`ndbHi`, `vormapLo`/`vormapHi`; ADS-B's 1090 MHz and AIS's two channels are fixed by
 *      their standards;
 *    - THE COVER RULE (Stuart 11-26): where the radio's capture holds a whole family, the family is ONE row — the server only
 *      offers the wide option (sondes "all" at ≥ 6.7 MS/s, "ACARS + VDL2" at ≥ 6.1 MS/s, MARITIME where the capture holds
 *      AIS + DSC ch 70) when its gate passes, so the rule is derived from what is offered: a band that covers every other
 *      band offered replaces them. Never a per-driver table. A row that is LIVE is always kept (so the chip can name it).
 *  ★ DSC alone is not a whole-radio mode (it is a decoder, and inside MARITIME); HFDL stays a decoder.
 *  ★★ The ask / lock rules are not decided here: the caller greys every row with the lock's reason (TrackView.setDialLock). */
import type { AvBand } from './aviation';
import type { SondeBand } from './sondes';

export type TuneCap = { ok: boolean; why: string } | null | undefined;
export type TuneGroup = 'aviation' | 'maritime' | 'balloons';
export type TuneIcon = 'aircraft' | 'ship' | 'balloon' | 'beacon' | 'navaid';
export interface AirSeaTuneSource {
  adsb?: TuneCap; avdata?: TuneCap; avBands?: AvBand[]; vormap?: TuneCap; vormapLo?: number; vormapHi?: number;
  ndb?: TuneCap; ndbLo?: number; ndbHi?: number; ais?: TuneCap; aisDsc?: boolean; sondes?: TuneCap; sondeBands?: SondeBand[];
  /** The only mode this server runs ('' = none): the other modes are not rows. */
  only?: string;
}
export interface AirSeaLive { mode: '' | 'adsb' | 'avdata' | 'vormap' | 'ndb' | 'ais' | 'sondes'; avBand?: string; sdBand?: string; aisDsc?: boolean }
export interface AirSeaTuneRow {
  /** 'adsb' · 'av:<band>' · 'vormap' · 'ndb' · 'ais' · 'maritime' · 'sd:<band>' */
  id: string; group: TuneGroup; icon: TuneIcon; name: string; freq: string; live: boolean;
  /** The server's reason the row cannot be picked ('' = it can). */
  why: string;
}
export const TUNE_GROUP_LABEL: Record<TuneGroup, string> = { aviation: 'AVIATION', maritime: 'MARITIME', balloons: 'WEATHER BALLOONS' };

/** 131525000 → "131.525"; 400000000 → "400"; 117950000 → "117.95". */
export function mhz(hz: number): string { return (hz / 1e6).toFixed(4).replace(/\.?0+$/, ''); }
function range(lo: number, hi: number, unit: 'MHz' | 'kHz' = 'MHz'): string {
  if (unit === 'kHz') return `${Math.round(lo / 1e3)}–${Math.round(hi / 1e3)} kHz`;
  return lo === hi ? `${mhz(lo)} MHz` : `${mhz(lo)}–${mhz(hi)} MHz`;
}

/** The datalink bands with the cover rule applied: a band whose id names every other offered band ("acars+vdl2") replaces them. */
export function avBandsShown(bands: AvBand[], liveId?: string): AvBand[] {
  const cover = bands.find((b) => { const parts = b.id.split('+'); return parts.length > 1 && bands.every((o) => o === b || parts.includes(o.id)); });
  return cover ? bands.filter((b) => b === cover || b.id === liveId) : bands;
}
/** The sonde bands with the cover rule applied: a band spanning every other offered band replaces them. */
export function sondeBandsShown(bands: SondeBand[], liveId?: string): SondeBand[] {
  if (bands.length < 2) return bands;
  const lo = Math.min(...bands.map((b) => b.lo)), hi = Math.max(...bands.map((b) => b.hi));
  const cover = bands.find((b) => b.lo > 0 && b.lo <= lo && b.hi >= hi);
  return cover ? bands.filter((b) => b === cover || b.id === liveId) : bands;
}

/** The TUNE list's rows, in its order (AVIATION, MARITIME, WEATHER BALLOONS). */
export function airSeaTuneRows(s: AirSeaTuneSource, live: AirSeaLive): AirSeaTuneRow[] {
  const rows: AirSeaTuneRow[] = [];
  const allowed = (m: string) => !s.only || s.only === m;
  const why = (c: TuneCap) => (c && !c.ok ? c.why || 'Not available on this radio' : '');
  // ── AVIATION ──
  if (s.adsb && allowed('adsb')) rows.push({ id: 'adsb', group: 'aviation', icon: 'aircraft', name: 'ADS-B', freq: '1090 MHz', live: live.mode === 'adsb', why: why(s.adsb) });
  if (s.avdata && allowed('avdata')) {
    const bands = s.avBands ?? [];
    if (!bands.length) rows.push({ id: 'av:', group: 'aviation', icon: 'aircraft', name: 'DATALINK', freq: 'ACARS / VDL2', live: live.mode === 'avdata', why: why(s.avdata) });
    for (const b of avBandsShown(bands, live.mode === 'avdata' ? live.avBand : undefined)) {
      const both = b.id.includes('+');
      rows.push({ id: `av:${b.id}`, group: 'aviation', icon: 'aircraft', name: b.name,
                  freq: b.chLo && b.chHi ? (both ? `both · ${range(b.chLo, b.chHi)}` : range(b.chLo, b.chHi)) : (b.hz ? `${mhz(b.hz)} MHz` : ''),
                  live: live.mode === 'avdata' && live.avBand === b.id, why: why(s.avdata) });
    }
  }
  if (s.vormap && allowed('vormap'))
    rows.push({ id: 'vormap', group: 'aviation', icon: 'navaid', name: 'VOR / ILS scan', freq: range(s.vormapLo || 108e6, s.vormapHi || 117.95e6), live: live.mode === 'vormap', why: why(s.vormap) });
  if (s.ndb && allowed('ndb'))
    rows.push({ id: 'ndb', group: 'aviation', icon: 'beacon', name: 'NDB', freq: s.ndbLo && s.ndbHi ? range(s.ndbLo, s.ndbHi, 'kHz') : '190–535 kHz', live: live.mode === 'ndb', why: why(s.ndb) });
  // ── MARITIME ── (MARITIME = AIS + DSC ch 70 replaces AIS where the capture holds both — the cover rule)
  if (s.ais && allowed('ais')) {
    const aisLive = live.mode === 'ais' && !live.aisDsc;
    if (!s.aisDsc || aisLive) rows.push({ id: 'ais', group: 'maritime', icon: 'ship', name: 'AIS', freq: '161.975 + 162.025 MHz', live: aisLive, why: why(s.ais) });
    if (s.aisDsc) rows.push({ id: 'maritime', group: 'maritime', icon: 'ship', name: 'MARITIME', freq: 'AIS + DSC ch 70', live: live.mode === 'ais' && !!live.aisDsc, why: why(s.ais) });
  }
  // ── WEATHER BALLOONS ──
  if (s.sondes && allowed('sondes')) {
    const bands = s.sondeBands ?? [];
    if (!bands.length) rows.push({ id: 'sd:', group: 'balloons', icon: 'balloon', name: 'Sondes', freq: '400–406 MHz', live: live.mode === 'sondes', why: why(s.sondes) });
    for (const b of sondeBandsShown(bands, live.mode === 'sondes' ? live.sdBand : undefined))
      rows.push({ id: `sd:${b.id}`, group: 'balloons', icon: 'balloon', name: 'Sondes', freq: b.lo && b.hi ? range(b.lo, b.hi) : b.name,
                  live: live.mode === 'sondes' && live.sdBand === b.id, why: why(s.sondes) });
  }
  return rows;
}

/** The chip's words: "TUNE · VDL2 136.700–136.975 MHz" (the live row), or "TUNE" when nothing here is live. */
export function airSeaTuneChip(rows: AirSeaTuneRow[]): string {
  const l = rows.find((r) => r.live);
  return l ? `TUNE · ${l.name} ${l.freq}` : 'TUNE';
}

/** A row's mode in the only-mode / capability terms (for the callers' gates). */
export function tuneRowMode(id: string): AirSeaLive['mode'] {
  return id === 'adsb' ? 'adsb' : id.startsWith('av:') ? 'avdata' : id === 'vormap' ? 'vormap' : id === 'ndb' ? 'ndb'
    : id === 'ais' || id === 'maritime' ? 'ais' : id.startsWith('sd:') ? 'sondes' : '';
}

/** The row icons (16 px line glyphs, currentColor) — the web's TrackView and the app's list draw the same shapes. */
export const TUNE_ICON_PATH: Record<TuneIcon, string> = {
  aircraft: 'M8 1.5c.6 0 1 .5 1 1.2V6l5 3v1.4l-5-1.5v3l1.5 1.2V14L8 13.3 5.5 14v-.9L7 11.9v-3L2 10.4V9l5-3V2.7c0-.7.4-1.2 1-1.2z',
  ship: 'M3 9h10l-1.6 3.6c-.2.5-.7.9-1.3.9H5.9c-.6 0-1.1-.4-1.3-.9L3 9zm2-1V5h2V3h2v2h2v3H5z',
  balloon: 'M8 1.5a4.5 4.5 0 0 1 4.5 4.5c0 2.6-2.3 4.6-3.6 5.4L9.5 13h-3l.6-1.6C5.8 10.6 3.5 8.6 3.5 6A4.5 4.5 0 0 1 8 1.5zM6.5 14h3v1h-3z',
  beacon: 'M7.3 6h1.4L11 14.5H9.6L8 8.6 6.4 14.5H5L7.3 6zM8 2.2a2 2 0 1 1 0 3.6 2 2 0 0 1 0-3.6zM3.2 1.8l1 .9a4.6 4.6 0 0 0 0 4.6l-1 .9a6 6 0 0 1 0-6.4zm9.6 0a6 6 0 0 1 0 6.4l-1-.9a4.6 4.6 0 0 0 0-4.6z',
  navaid: 'M8 1.5 14.5 8 8 14.5 1.5 8 8 1.5zm0 2.1L3.6 8 8 12.4 12.4 8 8 3.6zM8 6.3a1.7 1.7 0 1 1 0 3.4 1.7 1.7 0 0 1 0-3.4z',
};
