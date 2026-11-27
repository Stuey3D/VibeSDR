/**
 * ndb.ts — the web client's NDB mode (V12, docs/v12/NDB-SKIMMER.md): the map view of every aeronautical beacon the
 * server's skimmer is decoding at once.
 *
 * ★★ THE PAGE'S HALF ONLY. The server decodes (vibe_ndb_service.h): it finds every carrier, reads each ident, confirms it
 *    only when several repeats agree, matches it against the published list (OurAirports + FAA, public domain;
 *    OpenStreetMap, ODbL — scripts/gen-ndb-navaids.mjs) and sends a report a second. This file draws it: the map (NdbMap + web/mapkit/ndblayer.js), the live list with its tiles
 *    (CARRIERS · IDENTIFIED · FURTHEST · COVERAGE), the beacon card, the heard log, Find NDB and the stats (NdbView).
 * ★★ THE IDENT COLUMN TELLS THE TRUTH ABOUT ITSELF: ✓ = the ident read by consensus matches the published beacon;
 *    ≈ = read by consensus, one letter off the published one; plain = read by consensus, not in the list; italic + "?"
 *    = NOT READ YET — the published beacon nearest the receiver on that frequency, a guess and labelled as one.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets HTML.
 * ★★ MAP-FIRST, ADS-B'S PATTERN (Stuart, 2026-10-27): the beacons at their published positions on our map, each with a
 *    3-bar signal meter; the live list streams the readings like the CW skimmer's; the waterfall is throttled behind the
 *    map (the server's 1 fps) and the whole band is skimmed at once. ★★★ beta96: on the AIR & SEA map (see THE VIEW).
 */

export interface NdbDb { k: 'match' | 'near' | 'cand'; ident: string; name: string; cc: string; hz: number; lat: number; lon: number; km: number; brg: number; own?: boolean }
/** ★★ V12 2026-11-04 — the server's "Suspected …" clue for a beacon heard but not in the published list (vibe_ndb_suspect.h):
 *  the best airfield, the evidence in plain words, up to two runners-up. Never the station — always labelled a suspicion. */
export interface NdbSus { icao: string; iata: string; name: string; cc: string; lat: number; lon: number; km: number; brg: number; why: string; alt?: { icao: string; name: string; km: number; why: string }[] }
export interface NdbBeacon {
  id: number; hz: number; snr: number; lvl: number; tone: number; tsnr: number; a1a: boolean; wpm: number;
  ident: string; votes: number; ballots: number; read: string; active: boolean; first: number; last: number; idAt: number; lastId: number;
  db?: NdbDb; alt?: { ident: string; name: string; cc: string; km: number }[]; also?: string[];
  harm?: { id: number; ident: string; hz: number }; copy: string;
  /** ★ not listed: the suspected airfield · listed but probably wrong (far, yet heard by day): why */
  sus?: NdbSus; unlikely?: string;
}
export interface NdbLog { ident: string; hz: number; name: string; cc: string; km: number; brg: number; lat: number; lon: number; best: number; first: number; last: number; sus?: NdbSus; unlikely?: string }
export type NdbQ = 'high' | 'medium' | 'low' | 'poor';
export interface NdbCopy { s: number; t: number; hz: number; id: number; text: string; q: NdbQ }
export interface NdbReport {
  type: 'ndb'; centre: number; lo: number; hi: number; rate?: number; now?: number; carriers: number; identified: number;
  published?: number; heardIn?: number; pos?: boolean; far?: { ident: string; km: number }; bin?: number; source?: string;
  starting?: boolean; beacons: NdbBeacon[]; log: NdbLog[]; copy: NdbCopy[];
}
export interface NdbFound { ident: string; hz: number; name: string; cc: string; lat: number; lon: number; km: number; brg: number; heard: boolean }

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
const ident = (v: unknown) => str(v, 8).replace(/[^A-Z0-9?]/g, '');

/** Trust nothing: numbers are numbers, strings are bounded, idents are A–Z / 0–9. */
export function ndbSanitise(m: any): NdbReport | null {
  if (!m || m.type !== 'ndb') return null;
  const db = (d: any): NdbDb | undefined => d && typeof d === 'object' && (d.k === 'match' || d.k === 'near' || d.k === 'cand')
    ? { k: d.k, ident: ident(d.ident), name: str(d.name, 60), cc: str(d.cc, 2).replace(/[^A-Z]/g, ''), hz: num(d.hz), lat: num(d.lat), lon: num(d.lon), km: num(d.km, -1), brg: num(d.brg), ...(d.own === true ? { own: true } : {}) }
    : undefined;
  const code = (v: unknown, n: number) => str(v, n).replace(/[^A-Z0-9]/g, '');
  const sus = (x: any): NdbSus | undefined => x && typeof x === 'object' && code(x.icao, 4).length === 4
    ? { icao: code(x.icao, 4), iata: code(x.iata, 3), name: str(x.name, 60), cc: str(x.cc, 2).replace(/[^A-Z]/g, ''), lat: num(x.lat), lon: num(x.lon), km: num(x.km, -1), brg: num(x.brg), why: str(x.why, 200),
        ...(Array.isArray(x.alt) ? { alt: x.alt.slice(0, 2).map((a: any) => ({ icao: code(a?.icao, 4), name: str(a?.name, 60), km: num(a?.km, -1), why: str(a?.why, 200) })) } : {}) }
    : undefined;
  const extra = (o: any) => ({ ...(sus(o.sus) ? { sus: sus(o.sus) } : {}), ...(typeof o.unlikely === 'string' && o.unlikely ? { unlikely: str(o.unlikely, 200) } : {}) });
  const beacons: NdbBeacon[] = (Array.isArray(m.beacons) ? m.beacons : []).slice(0, 200).map((b: any) => ({
    id: num(b.id), hz: num(b.hz), snr: num(b.snr), lvl: num(b.lvl), tone: num(b.tone), tsnr: num(b.tsnr), a1a: b.a1a === true, wpm: num(b.wpm),
    ident: ident(b.ident), votes: num(b.votes), ballots: num(b.ballots), read: ident(b.read), active: b.active === true,
    first: num(b.first), last: num(b.last), idAt: num(b.idAt), lastId: num(b.lastId), db: db(b.db),
    alt: Array.isArray(b.alt) ? b.alt.slice(0, 6).map((a: any) => ({ ident: ident(a.ident), name: str(a.name, 60), cc: str(a.cc, 2), km: num(a.km, -1) })) : undefined,
    also: Array.isArray(b.also) ? b.also.slice(0, 4).map(ident) : undefined,
    harm: b.harm && typeof b.harm === 'object' ? { id: num(b.harm.id), ident: ident(b.harm.ident), hz: num(b.harm.hz) } : undefined,
    copy: str(b.copy, 80).replace(/[^A-Z0-9? ]/g, ''), ...extra(b),
  }));
  const log: NdbLog[] = (Array.isArray(m.log) ? m.log : []).slice(0, 300).map((e: any) => ({
    ident: ident(e.ident), hz: num(e.hz), name: str(e.name, 60), cc: str(e.cc, 2), km: num(e.km, -1), brg: num(e.brg),
    lat: num(e.lat), lon: num(e.lon), best: num(e.best), first: num(e.first), last: num(e.last), ...extra(e),
  }));
  const copy: NdbCopy[] = (Array.isArray(m.copy) ? m.copy : []).slice(-200).map((c: any) => ({ s: num(c.s), t: num(c.t), hz: num(c.hz), id: num(c.id), text: str(c.text, 12).replace(/[^A-Z0-9? ]/g, ''),
    q: (c.q === 'high' || c.q === 'medium' || c.q === 'poor' ? c.q : 'low') as NdbQ }));
  return {
    type: 'ndb', centre: num(m.centre), lo: num(m.lo), hi: num(m.hi), rate: num(m.rate), now: num(m.now), carriers: num(m.carriers), identified: num(m.identified),
    published: num(m.published), heardIn: num(m.heardIn), pos: m.pos === true ? true : m.pos === false ? false : undefined, far: m.far && typeof m.far === 'object' ? { ident: ident(m.far.ident), km: num(m.far.km) } : undefined,
    bin: num(m.bin), source: str(m.source, 160), starting: m.starting === 1 || m.starting === true, beacons, log, copy,
  };
}

export function esc(s: string): string { return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }

const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
export function compass(deg: number): string { return COMPASS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16]; }
export function flag(cc: string): string {
  if (!/^[A-Z]{2}$/.test(cc)) return '';
  return String.fromCodePoint(0x1f1e6 + cc.charCodeAt(0) - 65, 0x1f1e6 + cc.charCodeAt(1) - 65);
}
export function khz(hz: number): string { return (hz / 1e3).toFixed(1); }
/** ★ Distances in NAUTICAL MILES, as on the ADS-B map (its rings are these rings): "94 NM S". */
export function nmText(km: number, brg?: number): string {
  if (!(km >= 0)) return '';
  const nm = km / 1.852;
  return `${nm < 10 ? nm.toFixed(1) : Math.round(nm)} NM${brg !== undefined ? ` ${compass(brg)}` : ''}`;
}
/** ★ The 3-bar meter from the live SNR (the carrier over the noise in the skimmer's ≈ 23 Hz bin): < 10 dB one, < 20 dB
 *  two, else three; 0 = not heard in the last minute. ONE RULE, TWO READERS: web/mapkit/ndblayer.js barsFor. */
export function ndbBars(snr: number, live: boolean): number { if (!live) return 0; return snr < 10 ? 1 : snr < 20 ? 2 : 3; }
export function barsHtml(n: number): string {
  return `<span class="sig l${n}" title="${n ? `Signal ${n} of 3` : 'Not heard in the last minute'}">${[1, 2, 3].map((k) => `<i class="${k <= n ? 'on' : ''}"></i>`).join('')}</span>`;
}

/** ★ The ident cell: what it says, how it is drawn, and why (the title). See the file's header. */
export function ndbIdentCell(b: NdbBeacon): { text: string; cls: string; title: string } {
  if (b.harm) return { text: b.ident || b.harm.ident || '—', cls: 'harm', title: `2nd harmonic of ${b.harm.ident || 'the carrier'} on ${khz(b.harm.hz)} kHz` };
  if (b.ident) {
    if (b.db?.k === 'match' && b.unlikely) return { text: b.ident, cls: 'unlikely', title: `${b.ident} matches ${b.db.name || b.db.ident} in the published list — but unlikely: ${b.unlikely}` };
    if (b.db?.k === 'match') return { text: `✓ ${b.ident}`, cls: 'conf', title: `${b.ident} read by ${b.votes || 'folded'} agreeing repeats — matches the published beacon` };
    if (b.db?.k === 'near') return { text: `≈ ${b.ident}`, cls: 'near', title: `${b.ident} read by consensus — the published beacon here is ${b.db.ident}` };
    if (b.sus) return { text: b.ident, cls: 'sus', title: `${b.ident} read by consensus — not in the published list; suspected ${b.sus.name}: ${b.sus.why}` };
    return { text: b.ident, cls: 'unl', title: `${b.ident} read by consensus — not in the published list` };
  }
  if (b.db?.k === 'cand') return { text: `${b.db.ident}?`, cls: 'cand', title: 'Not read yet — the published beacon nearest you on this frequency (a guess)' };
  return { text: '—', cls: 'none', title: b.read ? `Reading so far: ${b.read} (not yet confirmed)` : 'Carrier — no ident read yet' };
}
/** Station · distance + bearing, from the published match (or the candidate's). */
export function ndbStation(b: NdbBeacon): { name: string; dist: string } {
  const d = b.db;
  if (!d && b.sus && !b.harm) return { name: `Suspected ${b.sus.name}`, dist: nmText(b.sus.km, b.sus.brg) };
  if (!d) return { name: b.harm ? 'harmonic' : '', dist: '' };
  const fl = flag(d.cc);
  return { name: `${fl ? fl + ' ' : ''}${d.name}${d.k === 'cand' ? '?' : ''}${b.unlikely ? ' (unlikely)' : ''}`, dist: nmText(d.km, d.brg) };
}
/** ★★ Stuart's words for a suspected beacon (2026-11-04): "Suspected RAF Mildenhall (not on list — confirm?)". The
 *  "confirm?" is the OWNER's invitation (only they can confirm); a listener reads "unconfirmed". */
export function ndbSusLabel(s: NdbSus, owner: boolean): string { return `Suspected ${s.name} (not on list — ${owner ? 'confirm?' : 'unconfirmed'})`; }
/** The suspected beacons' map entries: a HOLLOW ring at the airfield, labelled "IDENT?" — never a solid dot. */
export function ndbSusFeed(r: NdbReport): { key: string; ident: string; label: string; lat: number; lon: number; bars: number; stale: boolean; kind: 'sus' }[] {
  const nowMs = (r.now || Date.now() / 1000) * 1000;
  return ndbRows(r).filter((b) => !b.db && !!b.sus && !b.harm && !(b.sus.lat === 0 && b.sus.lon === 0)).map((b) => {
    const live = b.active && nowMs - b.last < 60000;
    return { key: String(b.id), ident: b.ident, label: `${b.ident}?`, lat: b.sus!.lat, lon: b.sus!.lon, bars: ndbBars(b.snr, live), stale: !live, kind: 'sus' as const };
  });
}
/** The four tiles. */
export function ndbTiles(r: NdbReport): { carriers: string; identified: string; furthest: string; coverage: string } {
  return {
    carriers: String(r.carriers),
    identified: String(r.identified),
    furthest: r.far ? `${r.far.ident} ${nmText(r.far.km)}` : '—',
    coverage: r.published && r.published > 0 ? `${Math.round(100 * (r.heardIn ?? 0) / r.published)} %` : '—',
  };
}
export function ndbAgo(nowS: number, tS: number): string {
  const d = Math.max(0, Math.round(nowS - tS));
  return d < 60 ? `${d} s` : d < 3600 ? `${Math.round(d / 60)} min` : `${(d / 3600).toFixed(1)} h`;
}
export function utc(tS: number): string { const d = new Date(tS * 1000); return d.toISOString().slice(11, 19); }
/** CSV from rows (quoted where needed). */
export function csv(head: string[], rows: (string | number)[][]): string {
  const q = (v: string | number) => { const s = String(v); return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s; };
  return [head.map(q).join(','), ...rows.map((r) => r.map(q).join(','))].join('\n') + '\n';
}
/** ★ The list's rows, LOWEST FREQUENCY FIRST (the CW skimmer's order — a row keeps its place while it updates):
 *  every carrier being skimmed, and the identified ones that have gone quiet (dimmed). */
export function ndbRows(r: NdbReport): NdbBeacon[] {
  return r.beacons.filter((b) => b.active || b.ident).sort((a, b) => a.hz - b.hz);
}
/** Is this beacon on the map? Only where the published list puts it (✓, ≈, or the candidate). */
export function ndbOnMap(b: NdbBeacon): boolean { return !!b.db && !b.unlikely && Number.isFinite(b.db.lat) && Number.isFinite(b.db.lon) && !(b.db.lat === 0 && b.db.lon === 0); }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE VIEW — ★★★ beta96 (docs/v12/ONE-MAP.md stage 8): NDB is a LIVE STATE OF THE AIR & SEA MAP. Its pieces (the markup
//  under #ndbView in index.html, which is never shown itself now — it is where they wait) are moved into the map's slots
//  while NDB is live: RINGS · UNHEARD in the top bar, the tiles, HEARD LOG · FIND NDB · STATS and the beacon list under
//  the NAVAIDS tab's header, the card over the map. The beacon layer (web/mapkit/ndblayer.js) is made on the map's own
//  map (TrackView.whenMap) — no map of its own; the map's FIT key and auto fit frame the beacons (morePoints).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

const PREF = 'vibesdr.ndbMapPrefs';
function loadPrefs(): { rings: boolean; unheard: boolean; autofit: boolean } {
  const d = { rings: true, unheard: true, autofit: true };
  try { return { ...d, ...JSON.parse(localStorage.getItem(PREF) || '{}') }; } catch { return d; }
}
function runSource(src: string) { const el = document.createElement('script'); el.textContent = src; document.head.appendChild(el); }
const $ = (id: string) => document.getElementById(id) as HTMLElement;

export interface NdbHooks {
  onExit: () => void;
  find: (q: string) => void;
  stats: () => void;
  myPos: () => { lat: number; lon: number } | null;
  /** ★★ V12 2026-11-04: this page holds the server's admin (the OWNER) — only they may confirm a suspected beacon. */
  owner?: () => boolean;
  decide?: (act: 'confirm' | 'reject' | 'forget', ident: string, hz: number, airfield: string) => void;
}
/** ★ beta96 what NdbView needs of the AIR & SEA map's view (tracks.ts TrackView) — kept narrow. */
export interface NdbHost {
  slot(where: 'top' | 'head' | 'over' | 'ctl'): HTMLElement;
  whenMap(cb: (m: { gl: any; lm: any } | null) => void): () => void;
  morePoints: (() => [number, number][]) | null;
  pickAircraft(x: number, y: number, touch: boolean): boolean;
  pokeFit(): void;
  pauseFit(): void;
  note(text: string, ms?: number): void;
  /** ★ beta97 a beacon chosen on the map: its LIST row lit (TrackView.markRow) */
  markRow?(key: string): void;
  readonly root: HTMLElement;
}

/** ★ The beacon layer on the host's map (made when the map is, gone with it). */
export class NdbMap {
  layer: any = null;
  private pending: (() => void) | null = null;
  private off: (() => void) | null = null;
  private gen = 0;
  onSelect: ((key: string | null) => void) | null = null;
  constructor(private note: (t: string) => void) {}
  openOn(host: NdbHost, rx: { lat: number; lon: number } | null, prefs: Record<string, boolean>) {
    this.close();
    const my = ++this.gen;
    this.off = host.whenMap((m) => {
      if (!m) { this.drop(); return; }
      void (async () => {
        const w = window as any;
        const { ADSB_LAYER_JS } = await import('./generated/adsbLayerSource');
        const { NDB_LAYER_JS } = await import('./generated/ndbLayerSource');
        if (my !== this.gen) return;
        if (!w.VibeAdsbLayer) runSource(ADSB_LAYER_JS);   // ★ the rings are ADS-B's (VibeAdsbLayer.shared)
        if (!w.VibeNdbLayer) runSource(NDB_LAYER_JS);
        if (!w.VibeNdbLayer) { this.note('The beacon layer did not load — the list still updates.'); return; }
        this.drop();
        try {
          this.layer = w.VibeNdbLayer.create(m.gl || m.lm, { receiver: rx, theme: 'dark', prefs, autoFit: false, drawReceiver: false,
            yieldClick: (x: number, y: number, touch: boolean) => host.pickAircraft(x, y, touch),
            onSelect: (k: string | null) => { try { this.onSelect?.(k); } catch (e) { console.error('[ndb] onSelect', e); } } });
        } catch (e) { console.error('[ndb] the beacon layer could not start', e); this.layer = null; return; }
        (window as any).__vibeNdbLayer = this.layer;   // ★ a handle for drivers (scripts/test-web-ndb.mjs), as __vibeAcLayer
        const p = this.pending; this.pending = null; p?.();
      })().catch((e) => console.error('[ndb] layer', e));
    });
  }
  /** Run now if the layer is there, else as soon as it is. */
  with(f: (layer: any) => void) { if (this.layer) { try { f(this.layer); } catch (e) { console.error('[ndb] layer', e); } } else this.pending = () => this.with(f); }
  points(): [number, number][] { try { return this.layer?.points?.() ?? []; } catch { return []; } }
  private drop() { try { this.layer?.destroy(); } catch (e) { console.error('[ndb] layer destroy', e); } this.layer = null; }
  close() { this.gen++; this.pending = null; this.off?.(); this.off = null; this.drop(); }
}

/** Per-beacon live readings and SNR history, kept by the page (the report carries the last few words of every beacon). */
interface Track { words: NdbCopy[]; snr: [number, number][] }

/** The pieces moved into the map (by id) and where each goes. */
// ★★★ beta96 map cards: RINGS / UNHEARD are map controls — the MAP card's ('ctl'), not the bar
// ★★★ beta97 ONE LIST (Stuart 11-27): the beacons are rows of the map's LIST (NAVAIDS — rowsHtml), the tiles are STATS'
//     NDB section (statsSection), HEARD LOG · FIND NDB · STATS sit in MAP CONTROLS; no list of NDB's own any more
const PIECES: Array<[string, 'top' | 'head' | 'over' | 'ctl']> = [['ndbRings', 'ctl'], ['ndbUnheard', 'ctl'], ['ndbTools', 'ctl'], ['ndbCard', 'over']];

export class NdbView {
  private last: NdbReport | null = null;
  private map: NdbMap | null = null;
  private host: NdbHost | null = null;
  private tracks = new Map<number, Track>();
  private seenSeq = 0;
  private sel = 0;
  private found: NdbFound[] = [];
  private prefs = loadPrefs();
  private dlg = '';
  private shown = true;   // ★ the layer's chip (NDB on / off)
  constructor(private h: NdbHooks) {}

  get up(): boolean { return !!this.host; }

  /** ★ NDB went live on the AIR & SEA map: its pieces into the map's slots, its layer onto the map. */
  mount(host: NdbHost) {
    if (this.host === host) return;
    if (this.host) this.park();
    this.host = host;
    for (const [id, where] of PIECES) { const el = document.getElementById(id); if (el) { el.classList.add('ndbPiece'); host.slot(where).appendChild(el); } }
    for (const id of ['ndbRings', 'ndbUnheard', 'ndbLogBtn', 'ndbFindBtn', 'ndbStatsBtn']) $(id).classList.add('btn');
    $('ndbRings').onclick = () => { this.prefs.rings = !this.prefs.rings; this.savePrefs(); this.map?.with((l) => l.setPrefs({ rings: this.prefs.rings })); this.syncToggles(); };
    $('ndbUnheard').onclick = () => { this.prefs.unheard = !this.prefs.unheard; this.savePrefs(); if (this.prefs.unheard) this.h.find(''); this.map?.with((l) => l.setPrefs({ unheard: this.prefs.unheard })); this.syncToggles(); };
    $('ndbLogBtn').onclick = () => this.openLog();
    $('ndbFindBtn').onclick = () => this.openFind();
    $('ndbStatsBtn').onclick = () => { this.openDlg('<h3>RECEPTION STATS</h3><div class="ndbEmpty">Asking the server…</div>'); this.dlg = 'stats'; this.h.stats(); };
    this.syncToggles();
    this.map = new NdbMap((t) => this.note(t));
    this.map.onSelect = (k) => this.select(k ? Number(k) : 0, false);
    this.map.openOn(host, this.h.myPos(), { ...this.prefs, autofit: false });
    host.morePoints = () => (this.shown ? this.map?.points() ?? [] : []);
    this.h.find('');   // ★ the published beacons for the faint "unheard" markers
    this.render();
  }
  /** ★ NDB ended (or the map closed): the pieces back where they wait, the layer off the map. */
  park() {
    const park = document.getElementById('ndbView');
    if (park) for (const [id] of PIECES) { const el = document.getElementById(id); if (el && el.parentElement !== park) park.appendChild(el); }
    this.map?.close(); this.map = null;
    if (this.host) this.host.morePoints = null;
    this.host = null;
    this.closeDlg(); this.closeCard();
    this.last = null; this.tracks.clear(); this.seenSeq = 0; this.sel = 0;
  }
  /** ★ The NDB chip: off = no beacons on the map (the list stays — it is the mode's). */
  setShown(on: boolean) {
    if (on === this.shown) return;
    this.shown = on;
    if (this.last) this.renderMap(this.last);
    if (!on) { this.map?.with((l) => l.select('')); this.closeCard(); }
  }
  private savePrefs() { try { localStorage.setItem(PREF, JSON.stringify(this.prefs)); } catch { /* private window */ } }
  private syncToggles() {
    $('ndbRings').classList.toggle('on', this.prefs.rings); $('ndbRings').setAttribute('aria-pressed', String(this.prefs.rings));
    $('ndbUnheard').classList.toggle('on', this.prefs.unheard); $('ndbUnheard').setAttribute('aria-pressed', String(this.prefs.unheard));
  }
  private note(t: string) { this.host?.note(t); }

  /** ★★★ beta96 THIS SESSION on the AIR & SEA map (mapScope.ts): which beacons are drawn and listed — null = every one. */
  private keep: ((b: NdbBeacon) => boolean) | null = null;
  private raw: NdbReport | null = null;
  setKeep(fn: ((b: NdbBeacon) => boolean) | null) {
    if (!fn && !this.keep) return;
    this.keep = fn;
    if (this.raw) { this.last = fn ? { ...this.raw, beacons: this.raw.beacons.filter(fn) } : this.raw; }
  }
  update(r0: NdbReport) {
    this.raw = r0;
    const r = this.keep ? { ...r0, beacons: r0.beacons.filter(this.keep) } : r0;
    this.last = r;
    const nowS = r.now || Date.now() / 1000;
    for (const c of r.copy) {
      if (c.s <= this.seenSeq) continue;
      const t = this.track(c.id); t.words.push(c); if (t.words.length > 12) t.words.shift();
    }
    this.seenSeq = Math.max(this.seenSeq, ...r.copy.map((c) => c.s), 0);
    for (const b of r.beacons) if (b.active) { const t = this.track(b.id); const lastT = t.snr.length ? t.snr[t.snr.length - 1][0] : 0; if (nowS - lastT >= 10) { t.snr.push([nowS, b.snr]); if (t.snr.length > 90) t.snr.shift(); } }
    this.render();
  }
  private track(id: number): Track { let t = this.tracks.get(id); if (!t) { t = { words: [], snr: [] }; this.tracks.set(id, t); } return t; }
  /** ★ The status line's words (the map's own status line shows them). */
  status(): string { const r = this.last; return r ? `NDB ${khz(r.lo)}–${khz(r.hi)} kHz · ${r.identified} / ${r.carriers} identified${r.starting ? ' · starting…' : ''}` : 'NDB — skimming the beacon band…'; }
  /** The NAVAIDS tab's count. */
  count(): number { return this.last ? ndbRows(this.last).length : 0; }

  private render() {
    const r = this.last;
    if (!r || !this.up) return;
    /* ★★★ beta97 the tiles are STATS' (statsSection) and the rows the LIST's (rowsHtml) */
    const NO_POS = 'This server has no position set — beacons are placed, but distances are unknown.';
    if (r.pos === false) this.note(NO_POS);
    this.renderMap(r);
    if (this.sel) this.renderCard();
  }
  /** ★★★ beta97 THE BEACONS AS ROWS OF THE MAP'S ONE LIST (NAVAIDS): the CW skimmer's order (lowest frequency first),
   *  a row per carrier — its ident (✓ / ≈ / ? coloured by confidence), the station and kHz, the distance, the SNR, the
   *  last few readings; quiet ones dimmed. Keyed `ndb:<id>`; a tap chooses the beacon (main.ts → select). */
  rowsHtml(): string {
    const r = this.last;
    if (!r) return '';
    const rows = ndbRows(r);
    if (!rows.length) return `<div class="trkEmpty">${r.starting || !r.beacons.length ? 'Skimming the whole beacon band — carriers appear as they are found; an ident once several of its repeats agree (about half a minute).' : 'No beacon carriers heard yet.'}</div>`;
    const nowMs = (r.now || Date.now() / 1000) * 1000;
    const COL: Record<string, string> = { conf: '#5BE36B', near: '#E8C547', unl: '#ffb000', cand: '#9fb0c2', sus: '#8fc3ff', unlikely: '#9fb0c2', harm: '#c9a0ff', none: '#9fb0c2' };
    const out = rows.map((b) => {
      const c = ndbIdentCell(b), st = ndbStation(b);
      const live = b.active && (b.lastId === 0 ? true : nowMs - b.last < 60000);
      const words = (this.tracks.get(b.id)?.words ?? []).slice(-5).map((w) => w.text).join(' ');
      const second = [st.name, `${khz(b.hz)} kHz`, b.a1a ? 'A1A' : b.tone ? `tone ${Math.round(b.tone)} Hz` : '', words ? `“${words}”` : b.read ? `reading ${b.read}…` : ''].filter(Boolean).join(' · ');
      const ago = b.last ? Math.max(0, Math.round((nowMs - b.last) / 1000)) : -1;
      const age = ago < 0 ? '' : ago < 5 ? 'now' : ago < 60 ? `${ago} s` : ago < 3600 ? `${Math.round(ago / 60)} min` : `${Math.floor(ago / 3600)} h`;
      const cls = c.cls.split(' ')[0] || 'none';
      return `<div class="trkRow kvRow ndbR${b.active ? '' : ' nopos'}${b.id === this.sel ? ' sel' : ''}" role="button" tabindex="0" data-nofocus="1" data-key="ndb:${b.id}" title="${esc(c.title)}">`
        + `<span class="id"><b><i class="sw" style="background:${COL[cls] ?? '#ffb000'}"></i><span class="nmT">${esc(c.text)}</span>`
        + (st.dist ? `<span class="kv"><span>${esc(st.dist)}</span></span>` : '') + `</b><small>${esc(second)}</small></span>`
        + `<span class="rg">${live ? `<span class="snrChip">${b.snr.toFixed(0)} dB</span>` : ''}</span><span class="ag"><span class="agL">${esc(age)}</span><span class="agS">${esc(age.replace(/^(\d+)\s*([a-z]).*$/i, '$1$2'))}</span></span></div>`;
    }).join('');
    // ★ once, at the foot (Stuart, 2026-10-29) — not on every unnamed beacon. Same words as NdbLiveList.tsx.
    return out + `<div class="trkEmpty ndbFoot">A beacon shown by its ident alone isn’t in our list (OurAirports, FAA, OpenStreetMap) — it may be on another list.</div>`;
  }
  /** ★★★ beta97 the old tiles as STATS' NDB section (CARRIERS · IDENTIFIED · FURTHEST · COVERAGE) */
  statsSection(): { mode: string; rows: Array<{ key: string; label: string; value: string; name: string }> } | null {
    const r = this.last;
    if (!r) return null;
    const t = ndbTiles(r);
    return { mode: 'NDB', rows: [['CARRIERS', t.carriers], ['IDENTIFIED', t.identified], ['FURTHEST', t.furthest], ['COVERAGE', t.coverage]]
      .filter(([, v]) => v).map(([k, v]) => ({ key: `ndb:tile:${k}`, label: k, value: String(v), name: '' })) };
  }

  private renderMap(r: NdbReport) {
    if (!this.map) return;
    const nowMs = (r.now || Date.now() / 1000) * 1000;
    const placed = new Set<string>();
    const beacons = ndbRows(r).filter(ndbOnMap).map((b) => {
      placed.add(`${b.db!.ident}@${Math.round(b.db!.hz / 500)}`);
      const live = b.active && nowMs - b.last < 60000;
      return { key: String(b.id), ident: b.ident || b.db!.ident, label: b.ident ? b.ident : `${b.db!.ident}?`, lat: b.db!.lat, lon: b.db!.lon,
               bars: ndbBars(b.snr, live), stale: !live || !b.ident, kind: b.ident ? (b.db!.k === 'near' ? 'near' : 'match') : 'cand' };
    });
    const unheard = this.found.filter((f) => !placed.has(`${f.ident}@${Math.round(f.hz / 500)}`) && f.hz >= r.lo && f.hz <= r.hi)
      .map((f) => ({ lat: f.lat, lon: f.lon, ident: f.ident }));
    const shown = this.shown;
    this.map.with((l) => { l.update(shown ? [...beacons, ...ndbSusFeed(r)] : [], shown ? unheard : []); const rx = this.h.myPos(); if (rx) l.setReceiver(rx.lat, rx.lon); });
    this.host?.pokeFit();
  }

  /** A row tapped (or a marker): selected on both, its card open, the list row in view. */
  select(id: number, fromList: boolean) {
    this.sel = id;
    if (fromList && id) { this.host?.pauseFit(); this.map?.with((l) => l.focus(String(id))); }
    if (!fromList) this.host?.markRow?.(id ? `ndb:${id}` : '');   // ★ beta97 its row in the ONE LIST lit
    if (id) this.renderCard(); else this.closeCard();
  }
  private closeCard() { const c = document.getElementById('ndbCard'); if (c) c.hidden = true; }
  /** ★ The card (ADS-B's place for it, top right of the map): ident ✓/≈, name, kHz, tone, NM and bearing, SNR now and
   *  its sparkline over the last quarter hour, the last readings, and who else is published on the frequency. */
  private renderCard() {
    const r = this.last;
    const b = r?.beacons.find((x) => x.id === this.sel);
    const card = $('ndbCard');
    if (!b) { card.hidden = true; return; }
    const c = ndbIdentCell(b), st = ndbStation(b);
    const owner = !!this.h.owner?.() && !!this.h.decide;
    const pts = this.tracks.get(b.id)?.snr ?? [];
    let spark = '';
    if (pts.length > 1) {
      const t0 = pts[0][0], t1 = Math.max(t0 + 1, pts[pts.length - 1][0]);
      const mn = Math.min(...pts.map((p) => p[1])), mx = Math.max(mn + 1, ...pts.map((p) => p[1]));
      spark = `<svg class="spark" viewBox="0 0 160 30" preserveAspectRatio="none"><path d="${pts.map((p, i) => `${i ? 'L' : 'M'}${((p[0] - t0) / (t1 - t0)) * 160},${28 - ((p[1] - mn) / (mx - mn)) * 26}`).join(' ')}" fill="none" stroke="#ffb000" stroke-width="1.5"/></svg>`
        + `<div class="dim">SNR ${mn.toFixed(0)}–${mx.toFixed(0)} dB over ${Math.max(1, Math.round((t1 - t0) / 60))} min</div>`;
    }
    const words = (this.tracks.get(b.id)?.words ?? []).slice(-8).map((w) => `<span class="q-${w.q}">${esc(w.text)}</span>`).join(' ');
    card.innerHTML = `<button type="button" class="x" aria-label="Close">✕</button>`
      + `<div class="t"><b class="${c.cls}">${esc(c.text)}</b> ${esc(st.name)}</div>`
      + `<div>${khz(b.hz)} kHz · ${b.a1a ? 'A1A' : b.tone ? `tone ${Math.round(b.tone)} Hz` : 'tone —'}${b.wpm ? ` · ${b.wpm.toFixed(1)} WPM` : ''}</div>`
      + `<div>${st.dist ? `${esc(st.dist)} · ` : ''}${b.snr.toFixed(0)} dB${b.votes ? ` · read ${b.votes}× alike` : ''}</div>`
      + spark + (words ? `<div class="cp">${words}</div>` : '')
      + (b.alt?.length ? `<div class="dim">Also published on this frequency: ${esc(b.alt.map((a) => `${a.ident} ${a.name}${a.km >= 0 ? ` (${nmText(a.km)})` : ''}`).join(', '))}</div>` : '')
      + (b.harm ? `<div class="dim">2nd harmonic of ${esc(b.harm.ident || '')} on ${khz(b.harm.hz)} kHz</div>` : '')
      // ★★ the "Suspected …" clue (Stuart's words), its evidence, and for the OWNER only the confirm / reject keys
      + (b.sus ? `<div class="sus">${esc(ndbSusLabel(b.sus, owner))}</div><div class="dim">${esc(b.sus.why)}</div>` : '')
      + (b.sus?.alt?.length ? `<div class="dim">Or: ${esc(b.sus.alt.map((a) => `${a.name}${a.km >= 0 ? ` (${nmText(a.km)})` : ''}`).join(', '))}</div>` : '')
      + (b.sus && owner ? '<div class="own"><button type="button" data-act="confirm">CONFIRM</button> <button type="button" data-act="reject">NOT THIS ONE</button></div>' : '')
      + (b.unlikely ? `<div class="dim">Unlikely: ${esc(b.unlikely)}.</div>` : '')
      + (b.db?.own ? '<div class="dim">Not in the published list — confirmed by this server\'s owner.</div>' : '')
      + (b.db?.own && owner ? '<div class="own"><button type="button" data-act="forget">UNDO CONFIRM</button></div>' : '')
      + (!ndbOnMap(b) ? `<div class="dim">${b.sus ? 'Not in the published list — on the map as a dashed ring at the suspected airfield.'
                                             : b.unlikely ? 'Not on the map: probably not that beacon.' : 'Not in the published list — not on the map.'}</div>` : '');
    card.hidden = false;
    card.querySelectorAll('.own button').forEach((el) => {
      (el as HTMLButtonElement).onclick = () => this.h.decide?.((el as HTMLElement).dataset.act as 'confirm' | 'reject' | 'forget', b.ident, b.hz, b.sus?.icao ?? '');
    });
    (card.querySelector('.x') as HTMLButtonElement).onclick = () => { this.sel = 0; this.select(0, true); this.map?.with((l) => l.select('')); };
  }

  // ── dialogs (reached from the list's header) ──
  private openDlg(html: string) {
    let d = document.getElementById('ndbDlg');
    if (!d) {
      d = document.createElement('div'); d.id = 'ndbDlg';
      d.innerHTML = '<div id="ndbDlgBox" role="dialog" aria-modal="true"></div>';
      d.onclick = (e) => { if (e.target === d) this.closeDlg(); };
      (this.host?.root ?? document.body).appendChild(d);
    }
    if (this.host && d.parentElement !== this.host.root) this.host.root.appendChild(d);   // ★ the page under the map is inert
    d.hidden = false;
    (d.querySelector('#ndbDlgBox') as HTMLElement).innerHTML = `<div class="ndbDlgHead"><span></span><button id="ndbDlgX" aria-label="Close">✕</button></div>${html}`;
    (d.querySelector('#ndbDlgX') as HTMLButtonElement).onclick = () => this.closeDlg();
  }
  private closeDlg() { const d = document.getElementById('ndbDlg'); if (d) d.hidden = true; this.dlg = ''; }
  private openLog() {
    const r = this.last;
    this.dlg = 'log';
    if (!r || !r.log.length) { this.openDlg('<h3>HEARD LOG</h3><div class="ndbEmpty">No beacons identified yet.</div>'); return; }
    this.openDlg('<h3>HEARD LOG</h3><table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th><th class="num">BEST SNR</th><th>FIRST</th><th>LAST</th></tr>'
      + r.log.map((e) => `<tr><td class="unl">${esc(e.ident)}</td><td>${esc(`${flag(e.cc)} ${e.name}`.trim())}</td><td class="num">${khz(e.hz)}</td>`
        + `<td>${esc(nmText(e.km, e.km >= 0 ? e.brg : undefined))}</td><td class="num">${e.best.toFixed(0)} dB</td><td>${utc(e.first)}</td><td>${utc(e.last)}</td></tr>`).join('')
      + '</table><div class="ndbNote">UTC. The gallery keeps each day\'s log and map.</div>');
  }
  private openFind() {
    this.dlg = 'find';
    this.openDlg(`<h3>FIND AN NDB</h3><input id="ndbQ" placeholder="ident, name or kHz — empty: the nearest" autocomplete="off"><div id="ndbFoundList" class="ndbEmpty">Searching…</div>`
      + `<div class="ndbNote">Every published NDB (OurAirports and FAA, public domain; &copy; OpenStreetMap contributors, ODbL), nearest you first. Tap one heard here to see it on the map.</div>`);
    const inp = document.getElementById('ndbQ') as HTMLInputElement;
    let tmr = 0;
    inp.oninput = () => { clearTimeout(tmr); tmr = window.setTimeout(() => this.h.find(inp.value.trim()), 250); };
    inp.focus();
    this.h.find('');
  }
  /** The server's answer to Find NDB (an empty query also feeds the map's faint unheard beacons). */
  found_(m: any) {
    const list: NdbFound[] = (Array.isArray(m?.list) ? m.list : []).slice(0, 400).map((f: any) => ({
      ident: ident(f.ident), hz: num(f.hz), name: str(f.name, 60), cc: str(f.cc, 2), lat: num(f.lat), lon: num(f.lon), km: num(f.km, -1), brg: num(f.brg), heard: f.heard === true }));
    if (!m?.q) { this.found = list; if (this.last) this.renderMap(this.last); }
    if (this.dlg !== 'find') return;
    const host = document.getElementById('ndbFoundList');
    if (!host) return;
    if (m?.pos === false) { host.textContent = 'This server has no position set, so nothing can be ranked by distance.'; return; }
    if (!list.length) { host.textContent = 'Nothing matches.'; return; }
    host.className = '';
    host.innerHTML = `<table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th></tr>${list.slice(0, 120).map((f) =>
      `<tr${f.heard ? ` class="tap" data-k="${esc(f.ident)}@${Math.round(f.hz / 500)}"` : ''}><td class="${f.heard ? 'conf' : ''}">${f.heard ? '✓ ' : ''}${esc(f.ident)}</td>`
      + `<td>${esc(`${flag(f.cc)} ${f.name}`.trim())}</td><td class="num">${khz(f.hz)}</td><td>${esc(nmText(f.km, f.brg))}</td></tr>`).join('')}</table>`;
    host.querySelectorAll('tr[data-k]').forEach((tr) => {
      (tr as HTMLElement).onclick = () => {
        const k = (tr as HTMLElement).dataset.k;
        const b = this.last?.beacons.find((x) => x.db && `${x.db.ident}@${Math.round(x.db.hz / 500)}` === k);
        if (b) { this.closeDlg(); this.select(b.id, true); }
      };
    });
  }
  /** The server's stats: per hour, by hour of day, each beacon's SNR — with CSV for each. */
  stats_(m: any) {
    if (this.dlg !== 'stats') return;
    const hours: { t: number; heard: number; far: number }[] = (Array.isArray(m?.hours) ? m.hours : []).map((h: any) => ({ t: num(h.t), heard: num(h.heard), far: num(h.far) }));
    const beacons = (Array.isArray(m?.beacons) ? m.beacons : []).slice(0, 400).map((b: any) => ({
      ident: ident(b.ident), hz: num(b.hz), name: str(b.name, 60), cc: str(b.cc, 2), km: num(b.km, -1), minutes: num(b.minutes), mean: num(b.mean), best: num(b.best), last: num(b.last),
      snr: (Array.isArray(b.snr) ? b.snr : []).slice(-300).map((p: any) => [num(p?.[0]), num(p?.[1])] as [number, number]) }));
    if (!hours.length && !beacons.length) { this.openDlg('<h3>RECEPTION STATS</h3><div class="ndbEmpty">No history yet — it builds up a minute at a time while NDB runs (kept in memory, at most 7 days).</div>'); this.dlg = 'stats'; return; }
    const byHod = new Array(24).fill(0).map(() => ({ n: 0, sum: 0 }));
    for (const h of hours) { const d = byHod[new Date(h.t * 1000).getUTCHours()]; d.n++; d.sum += h.heard; }
    const bars = (vals: number[], labels: string[], unit: string) => {
      const mx = Math.max(1, ...vals), W = 600, H = 100, bw = W / Math.max(1, vals.length);
      return `<div class="ndbChart"><svg viewBox="0 0 ${W} ${H + 12}" preserveAspectRatio="none">${vals.map((v, i) => `<rect x="${i * bw + 1}" y="${H - (v / mx) * H}" width="${Math.max(1, bw - 2)}" height="${(v / mx) * H}" fill="rgba(255,176,0,0.7)"><title>${esc(labels[i])}: ${Math.round(v * 10) / 10} ${unit}</title></rect>`).join('')}</svg></div>`;
    };
    const rows = beacons.sort((a: any, b: any) => a.minutes - b.minutes);
    const hl = hours.map((h) => new Date(h.t * 1000).toISOString().slice(0, 13) + 'h');
    this.openDlg(`<h3>RECEPTION STATS</h3><div class="ndbNote">Since ${utc(num(m.since))} UTC on this server (in memory). UTC throughout.</div>`
      + `<h4>BEACONS HEARD PER HOUR <button data-csv="hours">CSV</button></h4>${bars(hours.map((h) => h.heard), hl, 'beacons')}`
      + `<h4>FURTHEST BEACON HEARD PER HOUR (NM)</h4>${bars(hours.map((h) => h.far / 1.852), hl, 'NM')}`
      + `<h4>BY HOUR OF DAY — AVERAGE BEACONS HEARD <button data-csv="hod">CSV</button></h4>${bars(byHod.map((d) => (d.n ? d.sum / d.n : 0)), byHod.map((_, i) => `${i}:00`), 'beacons')}`
      + `<h4>BEACONS, LEAST HEARD FIRST — TAP ONE FOR ITS SNR OVER TIME <button data-csv="beacons">CSV</button></h4><div id="ndbSnrChart"></div>`
      + `<table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th><th class="num">MINUTES</th><th class="num">MEAN SNR</th><th class="num">BEST</th><th>LAST</th></tr>`
      + rows.map((b: any, i: number) => `<tr class="tap" data-i="${i}"><td>${esc(b.ident)}</td><td>${esc(`${flag(b.cc)} ${b.name}`.trim())}</td><td class="num">${khz(b.hz)}</td>`
        + `<td>${esc(nmText(b.km))}</td><td class="num">${b.minutes}</td><td class="num">${b.mean.toFixed(1)}</td><td class="num">${b.best.toFixed(1)}</td><td>${utc(b.last)}</td></tr>`).join('') + '</table>');
    this.dlg = 'stats';
    const box = document.getElementById('ndbDlgBox')!;
    box.querySelectorAll('tr[data-i]').forEach((tr) => {
      (tr as HTMLElement).onclick = () => {
        const b = rows[Number((tr as HTMLElement).dataset.i)];
        const pts: [number, number][] = b.snr;
        if (!pts.length) return;
        const t0 = pts[0][0], t1 = Math.max(t0 + 1, pts[pts.length - 1][0]);
        const mn = Math.min(...pts.map((p) => p[1])), mx = Math.max(mn + 1, ...pts.map((p) => p[1]));
        const path = pts.map((p, i) => `${i ? 'L' : 'M'}${((p[0] - t0) / (t1 - t0)) * 600},${100 - ((p[1] - mn) / (mx - mn)) * 100}`).join(' ');
        document.getElementById('ndbSnrChart')!.innerHTML = `<div class="ndbNote">${esc(b.ident)} — SNR ${mn.toFixed(0)}–${mx.toFixed(0)} dB, ${utc(t0)}–${utc(t1)} UTC</div><div class="ndbChart"><svg viewBox="0 0 600 104" preserveAspectRatio="none"><path d="${path}" fill="none" stroke="#ffb000" stroke-width="2"/></svg></div>`;
      };
    });
    const dl = (name: string, body: string) => { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([body], { type: 'text/csv' })); a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000); };
    box.querySelectorAll('button[data-csv]').forEach((b) => {
      (b as HTMLButtonElement).onclick = () => {
        const k = (b as HTMLElement).dataset.csv;
        if (k === 'hours') dl('ndb-per-hour.csv', csv(['hour_utc', 'beacons_heard', 'furthest_nm'], hours.map((h) => [new Date(h.t * 1000).toISOString(), h.heard, Math.round(h.far / 1.852)])));
        if (k === 'hod') dl('ndb-by-hour-of-day.csv', csv(['hour_utc', 'average_beacons_heard'], byHod.map((d, i) => [i, d.n ? (d.sum / d.n).toFixed(2) : 0])));
        if (k === 'beacons') dl('ndb-beacons.csv', csv(['ident', 'khz', 'station', 'country', 'nm', 'minutes_heard', 'mean_snr_db', 'best_snr_db', 'last_heard_utc'],
          rows.map((x: any) => [x.ident, (x.hz / 1e3).toFixed(1), x.name, x.cc, x.km >= 0 ? Math.round(x.km / 1.852) : '', x.minutes, x.mean.toFixed(1), x.best.toFixed(1), new Date(x.last * 1000).toISOString()])));
      };
    });
  }
}

/** The dialogs' styles (the view itself is styled in index.html, from #adsbView's rules). */
export const NDB_DLG_CSS = `
#ndbDlg { position: fixed; inset: 0; z-index: 1300; background: rgba(0,0,0,0.55); display: flex; align-items: center; justify-content: center; }
#ndbDlg[hidden] { display: none; }
#ndbDlgBox { width: min(760px, calc(100vw - 32px)); max-height: 82vh; overflow-y: auto; background: #0b0703; border: 1px solid rgba(255,176,0,0.4);
  border-radius: 10px; color: #ffb000; font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace; padding: 10px; box-sizing: border-box; }
#ndbDlgBox input { font: inherit; width: 100%; box-sizing: border-box; padding: 6px 8px; background: #120c04; color: #ffd27a; border: 1px solid rgba(255,176,0,0.4); border-radius: 6px; }
#ndbDlgBox h3 { margin: 4px 0 8px; font-size: 13px; letter-spacing: 1.5px; font-weight: 600; }
#ndbDlgBox h4 { margin: 12px 0 4px; font-size: 11px; letter-spacing: 1.5px; font-weight: normal; color: rgba(255,190,40,0.7); }
#ndbDlgBox button { font: inherit; font-size: 12px; padding: 4px 9px; min-height: 30px; color: #ffb000; background: transparent; border: 1px solid rgba(255,176,0,0.35); border-radius: 6px; cursor: pointer; }
.ndbDlgHead { display: flex; gap: 8px; align-items: center; justify-content: space-between; }
.ndbChart svg { width: 100%; height: 110px; display: block; }
.ndbNote { padding: 4px 2px 7px; font-size: 10px; color: rgba(255, 190, 40, 0.5); }
.ndbEmpty { padding: 12px; color: rgba(255, 190, 40, 0.6); }
table.ndbT { width: 100%; border-collapse: collapse; font-size: 12px; }
table.ndbT th { text-align: left; font-weight: normal; font-size: 10px; letter-spacing: 1.5px; color: rgba(255, 190, 40, 0.65); padding: 4px 6px; }
table.ndbT td { padding: 3px 6px; white-space: nowrap; }
table.ndbT tr:nth-child(even) td { background: rgba(255, 176, 0, 0.04); }
table.ndbT tr.tap { cursor: pointer; } table.ndbT tr.tap:hover td { background: rgba(255, 160, 0, 0.12); }
td.conf { color: #8cff8c; font-weight: 600; } td.unl { color: #ffd27a; font-weight: 600; } td.num { text-align: right; font-variant-numeric: tabular-nums; }
@media (max-width: 600px) { table.ndbT td, table.ndbT th { white-space: normal; } }
`;
