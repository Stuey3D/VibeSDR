/**
 * ndb.ts — the web client's NDB mode (V12, docs/v12/NDB-SKIMMER.md): the map view of every aeronautical beacon the
 * server's skimmer is decoding at once.
 *
 * ★★ THE PAGE'S HALF ONLY. The server decodes (vibe_ndb_service.h): it finds every carrier, reads each ident, confirms it
 *    only when several repeats agree, matches it against the published list (OurAirports, public domain) and sends a
 *    report a second. This file draws it: the map (NdbMap + web/mapkit/ndblayer.js), the live list with its tiles
 *    (CARRIERS · IDENTIFIED · FURTHEST · COVERAGE), the beacon card, the heard log, Find NDB and the stats (NdbView).
 * ★★ THE IDENT COLUMN TELLS THE TRUTH ABOUT ITSELF: ✓ = the ident read by consensus matches the published beacon;
 *    ≈ = read by consensus, one letter off the published one; plain = read by consensus, not in the list; italic + "?"
 *    = NOT READ YET — the published beacon nearest the receiver on that frequency, a guess and labelled as one.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets HTML.
 * ★★ MAP-FIRST, ADS-B'S PATTERN (Stuart, 2026-10-27): the beacons at their published positions on our map, each with a
 *    3-bar signal meter; the live list over the map's corner streams the readings like the CW skimmer's; the waterfall
 *    is throttled behind the map (the server's 1 fps) and the whole band is skimmed at once.
 */
import { probeMapGL, loadMapGLScripts, mapglLoad } from './mapgl';
import { loadLeafletInto } from './leafletAsset';
import { loadVibemapSource } from './chunkCache';

export interface NdbDb { k: 'match' | 'near' | 'cand'; ident: string; name: string; cc: string; hz: number; lat: number; lon: number; km: number; brg: number }
export interface NdbBeacon {
  id: number; hz: number; snr: number; lvl: number; tone: number; tsnr: number; a1a: boolean; wpm: number;
  ident: string; votes: number; ballots: number; read: string; active: boolean; first: number; last: number; idAt: number; lastId: number;
  db?: NdbDb; alt?: { ident: string; name: string; cc: string; km: number }[]; also?: string[];
  harm?: { id: number; ident: string; hz: number }; copy: string;
}
export interface NdbLog { ident: string; hz: number; name: string; cc: string; km: number; brg: number; lat: number; lon: number; best: number; first: number; last: number }
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
    ? { k: d.k, ident: ident(d.ident), name: str(d.name, 60), cc: str(d.cc, 2).replace(/[^A-Z]/g, ''), hz: num(d.hz), lat: num(d.lat), lon: num(d.lon), km: num(d.km, -1), brg: num(d.brg) }
    : undefined;
  const beacons: NdbBeacon[] = (Array.isArray(m.beacons) ? m.beacons : []).slice(0, 200).map((b: any) => ({
    id: num(b.id), hz: num(b.hz), snr: num(b.snr), lvl: num(b.lvl), tone: num(b.tone), tsnr: num(b.tsnr), a1a: b.a1a === true, wpm: num(b.wpm),
    ident: ident(b.ident), votes: num(b.votes), ballots: num(b.ballots), read: ident(b.read), active: b.active === true,
    first: num(b.first), last: num(b.last), idAt: num(b.idAt), lastId: num(b.lastId), db: db(b.db),
    alt: Array.isArray(b.alt) ? b.alt.slice(0, 6).map((a: any) => ({ ident: ident(a.ident), name: str(a.name, 60), cc: str(a.cc, 2), km: num(a.km, -1) })) : undefined,
    also: Array.isArray(b.also) ? b.also.slice(0, 4).map(ident) : undefined,
    harm: b.harm && typeof b.harm === 'object' ? { id: num(b.harm.id), ident: ident(b.harm.ident), hz: num(b.harm.hz) } : undefined,
    copy: str(b.copy, 80).replace(/[^A-Z0-9? ]/g, ''),
  }));
  const log: NdbLog[] = (Array.isArray(m.log) ? m.log : []).slice(0, 300).map((e: any) => ({
    ident: ident(e.ident), hz: num(e.hz), name: str(e.name, 60), cc: str(e.cc, 2), km: num(e.km, -1), brg: num(e.brg),
    lat: num(e.lat), lon: num(e.lon), best: num(e.best), first: num(e.first), last: num(e.last),
  }));
  const copy: NdbCopy[] = (Array.isArray(m.copy) ? m.copy : []).slice(-200).map((c: any) => ({ s: num(c.s), t: num(c.t), hz: num(c.hz), id: num(c.id), text: str(c.text, 12).replace(/[^A-Z0-9? ]/g, ''),
    q: (c.q === 'high' || c.q === 'medium' || c.q === 'poor' ? c.q : 'low') as NdbQ }));
  return {
    type: 'ndb', centre: num(m.centre), lo: num(m.lo), hi: num(m.hi), rate: num(m.rate), now: num(m.now), carriers: num(m.carriers), identified: num(m.identified),
    published: num(m.published), heardIn: num(m.heardIn), pos: m.pos === true ? true : m.pos === false ? false : undefined, far: m.far && typeof m.far === 'object' ? { ident: ident(m.far.ident), km: num(m.far.km) } : undefined,
    bin: num(m.bin), source: str(m.source, 80), starting: m.starting === 1 || m.starting === true, beacons, log, copy,
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
    if (b.db?.k === 'match') return { text: `✓ ${b.ident}`, cls: 'conf', title: `${b.ident} read by ${b.votes || 'folded'} agreeing repeats — matches the published beacon` };
    if (b.db?.k === 'near') return { text: `≈ ${b.ident}`, cls: 'near', title: `${b.ident} read by consensus — the published beacon here is ${b.db.ident}` };
    return { text: b.ident, cls: 'unl', title: `${b.ident} read by consensus — not in the published list` };
  }
  if (b.db?.k === 'cand') return { text: `${b.db.ident}?`, cls: 'cand', title: 'Not read yet — the published beacon nearest you on this frequency (a guess)' };
  return { text: '—', cls: 'none', title: b.read ? `Reading so far: ${b.read} (not yet confirmed)` : 'Carrier — no ident read yet' };
}
/** Station · distance + bearing, from the published match (or the candidate's). */
export function ndbStation(b: NdbBeacon): { name: string; dist: string } {
  const d = b.db;
  if (!d) return { name: b.harm ? 'harmonic' : '', dist: '' };
  const fl = flag(d.cc);
  return { name: `${fl ? fl + ' ' : ''}${d.name}${d.k === 'cand' ? '?' : ''}`, dist: nmText(d.km, d.brg) };
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
export function ndbOnMap(b: NdbBeacon): boolean { return !!b.db && Number.isFinite(b.db.lat) && Number.isFinite(b.db.lon) && !(b.db.lat === 0 && b.db.lon === 0); }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE VIEW — map-first, ADS-B's pattern (Stuart, 2026-10-27): the map is the mode, the live list over its corner, the
//  top bar with EXIT NDB. The markup is #ndbView in index.html (styled with #adsbView's own rules, extended to it).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

const MIN_KEY = 'vibesdr.ndbListMin';
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
}

/** ★ The map: our MapLibre map (the 'aero' profile — airports and runways, where the beacons stand) with the NDB layer
 *  (web/mapkit/ndblayer.js) on it; the Leaflet map where there is no WebGL 2 or no /mapgl/. Made on entry, destroyed on
 *  exit (a MapLibre map holds a WebGL context and tens of MB of tiles — ADS-B's rule). */
export class NdbMap {
  private gen = 0;
  private vm: any = null;
  private lmap: any = null;
  layer: any = null;
  private pending: (() => void) | null = null;
  onSelect: ((key: string | null) => void) | null = null;
  constructor(private host: HTMLElement, private note: (t: string) => void) {}
  open(rx: { lat: number; lon: number } | null, prefs: Record<string, boolean>) {
    if (this.layer) return;
    const my = ++this.gen;
    void (async () => {
      const w = window as any;
      const kit = await probeMapGL();
      if (my !== this.gen) return;
      const { ADSB_LAYER_JS } = await import('./generated/adsbLayerSource');
      const { NDB_LAYER_JS } = await import('./generated/ndbLayerSource');
      if (my !== this.gen) return;
      if (!w.VibeAdsbLayer) runSource(ADSB_LAYER_JS);   // ★ the rings are ADS-B's (VibeAdsbLayer.shared)
      if (!w.VibeNdbLayer) runSource(NDB_LAYER_JS);
      if (!w.VibeNdbLayer) { this.note('The beacon layer did not load — the list still updates.'); return; }
      const mk = (map: any) => {
        this.layer = w.VibeNdbLayer.create(map, { receiver: rx, theme: 'dark', prefs,
          onSelect: (k: string | null) => { try { this.onSelect?.(k); } catch (e) { console.error('[ndb] onSelect', e); } } });
        const p = this.pending; this.pending = null; p?.();
      };
      if (kit && await loadMapGLScripts(kit.base)) {
        if (my !== this.gen) return;
        try {
          const vm = w.VibeMapGL.createNow(this.host, { style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail, profile: 'aero',
            center: rx ? [rx.lon, rx.lat] : [0, 30], zoom: rx ? 6 : 1.5 });
          if (vm) {
            this.vm = vm;
            vm.ready.catch((e: unknown) => console.error('NDB GPU map: the style did not load', e));
            vm.setNight(true);
            vm.map.addControl(new w.maplibregl.NavigationControl({ showCompass: false }), 'top-left');
            mk(vm);
            return;
          }
        } catch (e) {
          console.error('NDB GPU map failed — using the Leaflet map', e);
          try { this.vm?.destroy(); } catch (e2) { console.error('NDB GPU map: tidy-up failed', e2); }
          this.vm = null; this.host.innerHTML = '';
        }
      }
      if (!await loadLeafletInto(document)) { if (my === this.gen) this.note('The map could not be loaded on this server — the list still updates.'); return; }
      if (my !== this.gen) return;
      if (!w.VibeMap) { const { VIBEMAP_JS } = await loadVibemapSource(); if (my !== this.gen) return; if (!w.VibeMap) runSource(VIBEMAP_JS); }
      const L = w.L;
      this.lmap = L.map(this.host, { worldCopyJump: true, preferCanvas: true });
      this.lmap.setView(rx ? [rx.lat, rx.lon] : [30, 0], rx ? 7 : 2);
      if (w.VibeMap) w.VibeMap.attach(this.lmap, { dataBase: '/mapdata/v1/', profile: 'aero' });
      mk(this.lmap);
    })().catch((e) => { console.error('[ndb] the map could not load', e); if (my === this.gen) this.note('The map did not finish loading — the list still updates.'); });
  }
  /** Run now if the layer is there, else as soon as it is. */
  with(f: (layer: any) => void) { if (this.layer) { try { f(this.layer); } catch (e) { console.error('[ndb] layer', e); } } else this.pending = () => this.with(f); }
  close() {
    this.gen++; this.pending = null;
    try { this.layer?.destroy(); } catch (e) { console.error('[ndb] layer destroy', e); }
    this.layer = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[ndb] map destroy', e); }
    this.vm = null;
    try { this.lmap?.remove(); } catch (e) { console.error('[ndb] Leaflet remove', e); }
    this.lmap = null;
  }
}

/** Per-beacon live readings and SNR history, kept by the page (the report carries the last few words of every beacon). */
interface Track { words: NdbCopy[]; snr: [number, number][] }

export class NdbView {
  private last: NdbReport | null = null;
  private map: NdbMap | null = null;
  private tracks = new Map<number, Track>();
  private seenSeq = 0;
  private sel = 0;
  private found: NdbFound[] = [];
  private prefs = loadPrefs();
  private dlg = '';
  private rowsKey = '';
  constructor(private h: NdbHooks) {}

  get up(): boolean { return !$('ndbView').hidden; }

  show() {
    const v = $('ndbView');
    if (!v.hidden) return;
    v.hidden = false;
    document.body.classList.add('adsbMap', 'ndbMap');   // ★ the health pill moves under the bar, as in ADS-B
    let min = false;
    try { min = localStorage.getItem(MIN_KEY) === '1'; } catch { /* private window */ }
    this.setMin(min, false);
    const top = $('ndbTop').getBoundingClientRect().height;
    if (top > 0) document.documentElement.style.setProperty('--adsbTopH', `${Math.round(top)}px`);
    $('ndbExit').onclick = () => this.h.onExit();
    $('ndbLiveHead').onclick = () => this.setMin(!$('ndbLive').classList.contains('min'));
    $('ndbFit').onclick = () => { this.prefs.autofit = true; this.savePrefs(); this.map?.with((l) => { l.setPrefs({ autofit: true }); l.fit(); }); this.syncToggles(); };
    $('ndbRings').onclick = () => { this.prefs.rings = !this.prefs.rings; this.savePrefs(); this.map?.with((l) => l.setPrefs({ rings: this.prefs.rings })); this.syncToggles(); };
    $('ndbUnheard').onclick = () => { this.prefs.unheard = !this.prefs.unheard; this.savePrefs(); if (this.prefs.unheard) this.h.find(''); this.map?.with((l) => l.setPrefs({ unheard: this.prefs.unheard })); this.syncToggles(); };
    $('ndbLogBtn').onclick = () => this.openLog();
    $('ndbFindBtn').onclick = () => this.openFind();
    $('ndbStatsBtn').onclick = () => { this.openDlg('<h3>RECEPTION STATS</h3><div class="ndbEmpty">Asking the server…</div>'); this.dlg = 'stats'; this.h.stats(); };
    this.syncToggles();
    this.map = new NdbMap($('ndbMapHost'), (t) => this.note(t));
    this.map.onSelect = (k) => this.select(k ? Number(k) : 0, false);
    this.map.open(this.h.myPos(), this.prefs);
    this.h.find('');   // ★ the published beacons for the faint "unheard" markers
    this.render();
  }
  hide() {
    const v = $('ndbView');
    if (v.hidden) return;
    this.map?.close(); this.map = null;
    v.hidden = true;
    document.body.classList.remove('ndbMap');
    if (!document.getElementById('adsbView') || (document.getElementById('adsbView') as HTMLElement).hidden) document.body.classList.remove('adsbMap');
    this.closeDlg(); this.closeCard();
    $('ndbList').innerHTML = ''; this.rowsKey = '';
    this.last = null; this.tracks.clear(); this.seenSeq = 0; this.sel = 0;
    this.note('');
  }
  private savePrefs() { try { localStorage.setItem(PREF, JSON.stringify(this.prefs)); } catch { /* private window */ } }
  private syncToggles() {
    $('ndbRings').classList.toggle('on', this.prefs.rings);
    $('ndbUnheard').classList.toggle('on', this.prefs.unheard);
    $('ndbFit').classList.toggle('on', this.prefs.autofit);
  }
  private setMin(min: boolean, save = true) {
    $('ndbLive').classList.toggle('min', min);
    $('ndbLiveHead').setAttribute('aria-expanded', min ? 'false' : 'true');
    $('ndbLiveHead').title = min ? 'Show the beacon list' : 'Hide the list';
    if (save) { try { localStorage.setItem(MIN_KEY, min ? '1' : '0'); } catch { /* this visit only */ } }
  }
  private note(t: string) { const n = $('ndbNote'); n.textContent = t; n.hidden = !t; }

  update(r: NdbReport) {
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

  private render() {
    const r = this.last;
    if (!r || !this.up) return;
    const t = ndbTiles(r);
    $('ndbCnt').textContent = String(r.identified);
    $('ndbStat').textContent = `${khz(r.lo)}–${khz(r.hi)} kHz · ${r.carriers} carriers${r.starting ? ' · starting…' : ''}`;
    $('ndbLiveN').textContent = `${r.identified} / ${r.carriers}`;
    $('ndbTiles').innerHTML = [['CARRIERS', t.carriers], ['IDENTIFIED', t.identified], ['FURTHEST', t.furthest], ['COVERAGE', t.coverage]]
      .map(([k, v]) => `<span class="adsbChip ndbTile" title="${k === 'COVERAGE' ? 'Of the published beacons in this band no further away than the furthest one heard, how many are heard now' : ''}"><span class="k">${k}</span> ${esc(v)}</span>`).join('');
    const NO_POS = 'This server has no position set — beacons are placed, but distances are unknown.';
    if (r.pos === false) this.note(NO_POS); else if ($('ndbNote').textContent === NO_POS) this.note('');
    this.renderList(r);
    this.renderMap(r);
    if (this.sel) this.renderCard();
  }

  /** ★★ THE LIVE LIST — the CW skimmer's: lowest frequency first, a row per carrier, the readings streaming in at its
   *  end coloured by confidence (the health pill's four colours). Rows are updated IN PLACE, so the list stays where the
   *  listener scrolled it; rebuilt only when the set of carriers changes. */
  private renderList(r: NdbReport) {
    const host = $('ndbList');
    const rows = ndbRows(r);
    if (!rows.length) { host.innerHTML = `<div class="adsbEmpty">${r.starting || !r.beacons.length ? 'Skimming the whole beacon band — carriers appear as they are found; an ident once several of its repeats agree (about half a minute for a clear one).' : 'No beacons heard yet.'}</div>`; this.rowsKey = ''; return; }
    const key = rows.map((b) => b.id).join(',');
    if (key !== this.rowsKey) {
      this.rowsKey = key;
      host.innerHTML = rows.map((b) => `<div class="ndbRow" role="button" tabindex="0" data-id="${b.id}"></div>`).join('');
      host.querySelectorAll('.ndbRow').forEach((el) => {
        const id = Number((el as HTMLElement).dataset.id);
        (el as HTMLElement).onclick = () => this.select(id, true);
        (el as HTMLElement).onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); this.select(id, true); } };
      });
    }
    const nowMs = (r.now || Date.now() / 1000) * 1000;
    for (const b of rows) {
      const el = host.querySelector(`.ndbRow[data-id="${b.id}"]`) as HTMLElement | null;
      if (!el) continue;
      const c = ndbIdentCell(b), st = ndbStation(b);
      const live = b.active && (b.lastId === 0 ? true : nowMs - b.last < 60000);
      const words = this.tracks.get(b.id)?.words ?? [];
      const tx = words.length ? words.map((w) => `<span class="q-${w.q}">${esc(w.text)}</span>`).join(' ') : `<span class="dim">${b.read ? esc(`reading ${b.read}…`) : 'listening for the ident…'}</span>`;
      const html = `<span class="f">${khz(b.hz)}</span><span class="c ${c.cls}" title="${esc(c.title)}">${esc(c.text)}</span>`
        + `<span class="n">${esc(st.name)}</span><span class="d">${esc(st.dist)}</span>${barsHtml(ndbBars(b.snr, live))}`
        + `<span class="tx"><bdi>${tx}</bdi></span>`;
      if (el.innerHTML !== html) el.innerHTML = html;
      el.classList.toggle('gone', !b.active);
      el.classList.toggle('sel', b.id === this.sel);
      el.title = `${khz(b.hz)} kHz${b.tone ? ` · tone ${Math.round(b.tone)} Hz` : b.a1a ? ' · A1A (keyed carrier)' : ''} · ${b.snr.toFixed(0)} dB${ndbOnMap(b) ? ' — show on the map' : ' — not in the published list, so not on the map'}`;
    }
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
    this.map.with((l) => { l.update(beacons, unheard); const rx = this.h.myPos(); if (rx) l.setReceiver(rx.lat, rx.lon); });
  }

  /** A row tapped (or a marker): selected on both, its card open, the list row in view. */
  select(id: number, fromList: boolean) {
    this.sel = id;
    if (fromList && id) this.map?.with((l) => l.focus(String(id)));
    $('ndbList').querySelectorAll('.ndbRow').forEach((el) => el.classList.toggle('sel', Number((el as HTMLElement).dataset.id) === id));
    if (!fromList && id) ($('ndbList').querySelector(`.ndbRow[data-id="${id}"]`) as HTMLElement | null)?.scrollIntoView({ block: 'nearest' });
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
      + (!ndbOnMap(b) ? '<div class="dim">Not in the published list — not on the map.</div>' : '');
    card.hidden = false;
    (card.querySelector('.x') as HTMLButtonElement).onclick = () => { this.sel = 0; this.select(0, true); this.map?.with((l) => l.select('')); };
  }

  // ── dialogs (reached from the list's header) ──
  private openDlg(html: string) {
    let d = document.getElementById('ndbDlg');
    if (!d) {
      d = document.createElement('div'); d.id = 'ndbDlg';
      d.innerHTML = '<div id="ndbDlgBox" role="dialog" aria-modal="true"></div>';
      d.onclick = (e) => { if (e.target === d) this.closeDlg(); };
      document.body.appendChild(d);
    }
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
      + `<div class="ndbNote">Every published NDB (OurAirports, public domain), nearest you first. Tap one heard here to see it on the map.</div>`);
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
