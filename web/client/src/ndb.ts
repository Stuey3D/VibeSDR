/**
 * ndb.ts — the web client's NDB mode (V12, docs/v12/NDB-SKIMMER.md): the box over the waterfall that shows every
 * aeronautical beacon the server's skimmer is decoding at once.
 *
 * ★★ THE PAGE'S HALF ONLY. The server decodes (vibe_ndb_service.h): it finds every carrier, reads each ident, confirms it
 *    only when several repeats agree, matches it against the published list (OurAirports, public domain) and sends a
 *    report a second. This file draws it: the tiles (CARRIERS · IDENTIFIED · FURTHEST · COVERAGE), a spectrum strip with
 *    the idents over their carriers, the beacon table, a map, the live copy, the heard log, Find NDB and the stats.
 * ★★ THE IDENT COLUMN TELLS THE TRUTH ABOUT ITSELF: ✓ = the ident read by consensus matches the published beacon;
 *    ≈ = read by consensus, one letter off the published one; plain = read by consensus, not in the list; italic + "?"
 *    = NOT READ YET — the published beacon nearest the receiver on that frequency, a guess and labelled as one.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets HTML.
 * ★ The listener keeps their VFO in NDB mode: a row tapped tunes to the beacon (onTune) so it can be heard.
 */
import { probeMapGL, loadMapGLScripts, mapglLoad } from './mapgl';

export interface NdbDb { k: 'match' | 'near' | 'cand'; ident: string; name: string; cc: string; hz: number; lat: number; lon: number; km: number; brg: number }
export interface NdbBeacon {
  id: number; hz: number; snr: number; lvl: number; tone: number; tsnr: number; a1a: boolean; wpm: number;
  ident: string; votes: number; ballots: number; read: string; active: boolean; first: number; last: number; idAt: number; lastId: number;
  db?: NdbDb; alt?: { ident: string; name: string; cc: string; km: number }[]; also?: string[];
  harm?: { id: number; ident: string; hz: number }; copy: string;
}
export interface NdbLog { ident: string; hz: number; name: string; cc: string; km: number; brg: number; lat: number; lon: number; best: number; first: number; last: number }
export interface NdbCopy { s: number; t: number; hz: number; id: number; text: string }
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
  const copy: NdbCopy[] = (Array.isArray(m.copy) ? m.copy : []).slice(-60).map((c: any) => ({ s: num(c.s), t: num(c.t), hz: num(c.hz), id: num(c.id), text: str(c.text, 12).replace(/[^A-Z0-9? ]/g, '') }));
  return {
    type: 'ndb', centre: num(m.centre), lo: num(m.lo), hi: num(m.hi), rate: num(m.rate), now: num(m.now), carriers: num(m.carriers), identified: num(m.identified),
    published: num(m.published), heardIn: num(m.heardIn), pos: m.pos === true, far: m.far && typeof m.far === 'object' ? { ident: ident(m.far.ident), km: num(m.far.km) } : undefined,
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
  return { name: `${fl ? fl + ' ' : ''}${d.name}${d.k === 'cand' ? '?' : ''}`, dist: d.km >= 0 ? `${Math.round(d.km)} km ${compass(d.brg)}` : '' };
}
/** The four tiles. */
export function ndbTiles(r: NdbReport): { carriers: string; identified: string; furthest: string; coverage: string } {
  return {
    carriers: String(r.carriers),
    identified: String(r.identified),
    furthest: r.far ? `${r.far.ident} ${Math.round(r.far.km)} km` : '—',
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
/** Rows of the beacon table, ordered: identified first (strongest first), then carriers. */
export function ndbRows(r: NdbReport): NdbBeacon[] {
  const live = r.beacons.filter((b) => b.active || b.ident);
  return live.sort((a, b) => (Number(!!b.ident && !b.harm) - Number(!!a.ident && !a.harm)) || Number(b.active) - Number(a.active) || b.snr - a.snr);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE BOX
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const CSS = `
#ndbBox { position: fixed; left: 50%; transform: translateX(-50%); bottom: 96px; z-index: 30; width: min(980px, calc(100vw - 32px));
  max-height: 60vh; display: flex; flex-direction: column; background: rgba(9, 6, 2, 0.95); border: 1px solid rgba(255, 176, 0, 0.35);
  border-radius: 10px; color: #ffb000; font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace; box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
#ndbBox[hidden] { display: none; }
#ndbBox button { font: inherit; font-size: 12px; letter-spacing: 1px; padding: 5px 9px; min-height: 30px; color: #ffb000;
  background: transparent; border: 1px solid rgba(255, 176, 0, 0.35); border-radius: 6px; cursor: pointer; }
#ndbBox button:hover { background: rgba(255, 160, 0, 0.14); }
#ndbBox button.on { background: rgba(255, 176, 0, 0.85); color: #0b0b0b; }
#ndbHead { display: flex; align-items: center; gap: 8px; padding: 7px 9px; border-bottom: 1px solid rgba(255, 176, 0, 0.18); flex-wrap: wrap; }
#ndbExit { font-weight: 600; }
#ndbStat { flex: 1 1 10em; min-width: 0; font-size: 11px; color: rgba(255, 190, 40, 0.7); text-align: right; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#ndbTiles { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; padding: 6px 9px; }
.ndbTile { border: 1px solid rgba(255, 176, 0, 0.2); border-radius: 6px; padding: 3px 8px; min-width: 0; }
.ndbTile b { display: block; font-size: 16px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #ffd27a; }
.ndbTile span { font-size: 10px; letter-spacing: 1.5px; color: rgba(255, 190, 40, 0.65); }
#ndbSpec { height: 64px; margin: 0 9px; border-bottom: 1px solid rgba(255, 176, 0, 0.12); }
#ndbSpec svg { width: 100%; height: 100%; display: block; }
#ndbTabs { display: flex; gap: 6px; padding: 6px 9px; overflow-x: auto; scrollbar-width: none; }
#ndbTabs::-webkit-scrollbar { display: none; }
#ndbTabs button { flex: 0 0 auto; }
#ndbPane { overflow-y: auto; min-height: 120px; }
#ndbMapHost { height: 320px; position: relative; }
#ndbMapCtl { display: flex; gap: 6px; flex-wrap: wrap; padding: 6px 9px; align-items: center; }
#ndbMapCtl select { font: inherit; font-size: 12px; background: #120c04; color: #ffb000; border: 1px solid rgba(255,176,0,0.35); border-radius: 6px; min-height: 30px; }
table.ndbT { width: 100%; border-collapse: collapse; font-size: 12px; }
table.ndbT th { position: sticky; top: 0; background: #0d0904; text-align: left; font-weight: normal; font-size: 10px; letter-spacing: 1.5px;
  color: rgba(255, 190, 40, 0.65); padding: 4px 6px; }
table.ndbT td { padding: 3px 6px; white-space: nowrap; }
table.ndbT tr:nth-child(even) td { background: rgba(255, 176, 0, 0.04); }
table.ndbT tr.tap { cursor: pointer; } table.ndbT tr.tap:hover td { background: rgba(255, 160, 0, 0.12); }
table.ndbT tr.gone td { opacity: 0.5; }
td.conf { color: #8cff8c; font-weight: 600; } td.near { color: #ffe066; } td.unl { color: #ffd27a; font-weight: 600; }
td.cand { font-style: italic; color: rgba(255, 190, 40, 0.6); } td.none { color: rgba(255, 190, 40, 0.4); } td.harm { color: #c9a0ff; }
td.cp { max-width: 18em; overflow: hidden; text-overflow: ellipsis; color: rgba(255, 210, 122, 0.8); }
td.num { text-align: right; font-variant-numeric: tabular-nums; }
.ndbEmpty { padding: 12px; color: rgba(255, 190, 40, 0.6); }
.ndbNote { padding: 4px 9px 7px; font-size: 10px; color: rgba(255, 190, 40, 0.5); }
#ndbDlg { position: fixed; inset: 0; z-index: 40; background: rgba(0,0,0,0.55); display: flex; align-items: center; justify-content: center; }
#ndbDlg[hidden] { display: none; }
#ndbDlgBox { width: min(760px, calc(100vw - 32px)); max-height: 82vh; overflow-y: auto; background: #0b0703; border: 1px solid rgba(255,176,0,0.4);
  border-radius: 10px; color: #ffb000; font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace; padding: 10px; }
#ndbDlgBox input { font: inherit; width: 100%; box-sizing: border-box; padding: 6px 8px; background: #120c04; color: #ffd27a; border: 1px solid rgba(255,176,0,0.4); border-radius: 6px; }
#ndbDlgBox h3 { margin: 4px 0 8px; font-size: 13px; letter-spacing: 1.5px; font-weight: 600; }
#ndbDlgBox h4 { margin: 12px 0 4px; font-size: 11px; letter-spacing: 1.5px; font-weight: normal; color: rgba(255,190,40,0.7); }
#ndbDlgBox button { font: inherit; font-size: 12px; padding: 4px 9px; min-height: 30px; color: #ffb000; background: transparent; border: 1px solid rgba(255,176,0,0.35); border-radius: 6px; cursor: pointer; }
.ndbDlgHead { display: flex; gap: 8px; align-items: center; justify-content: space-between; }
.ndbChart svg { width: 100%; height: 110px; display: block; }
@media (max-width: 600px) { #ndbBox { font-size: 12px; max-height: 66vh; } #ndbTiles { grid-template-columns: repeat(2, 1fr); }
  .ndbTile b { font-size: 14px; } #ndbStat { flex-basis: 100%; text-align: left; } #ndbMapHost { height: 240px; }
  table.ndbT td.hideS, table.ndbT th.hideS { display: none; } }
`;

type Tab = 'beacons' | 'map' | 'copy' | 'log';
const PREF = 'vibesdr.ndbPrefs';
function prefs(): { tab: Tab; identOnly: boolean; win: number; rings: boolean; unheard: boolean; autofit: boolean } {
  const d = { tab: 'beacons' as Tab, identOnly: false, win: 3600, rings: true, unheard: true, autofit: true };
  try { return { ...d, ...JSON.parse(localStorage.getItem(PREF) || '{}') }; } catch { return d; }
}

export interface NdbHooks {
  onExit: () => void;
  onTune: (hz: number) => void;
  find: (q: string) => void;
  stats: () => void;
  myPos: () => { lat: number; lon: number } | null;
}

export class NdbBox {
  private el: HTMLElement | null = null;
  private last: NdbReport | null = null;
  private p = prefs();
  private lastHtml = '';
  private copy: NdbCopy[] = [];
  private map: NdbMap | null = null;
  private found: NdbFound[] = [];
  private onResize: (() => void) | null = null;
  constructor(private h: NdbHooks) {}

  private save() { try { localStorage.setItem(PREF, JSON.stringify(this.p)); } catch { /* private window */ } }
  private q<T extends HTMLElement>(sel: string): T { return this.el!.querySelector(sel) as T; }

  private mount(): HTMLElement {
    if (this.el) return this.el;
    if (!document.getElementById('ndbCss')) { const st = document.createElement('style'); st.id = 'ndbCss'; st.textContent = CSS; document.head.appendChild(st); }
    const el = document.createElement('div');
    el.id = 'ndbBox'; el.hidden = true;
    el.setAttribute('role', 'region'); el.setAttribute('aria-label', 'NDB beacons'); el.setAttribute('data-navbox', '');
    el.innerHTML = `<div id="ndbHead"><button id="ndbExit" title="Leave NDB — the receiver goes back to where it was">EXIT NDB</button>`
      + `<button id="ndbFind" title="Search every published NDB, nearest first">FIND NDB</button><button id="ndbStats" title="Reception stats">STATS</button>`
      + `<button id="ndbOnly" title="Show only identified beacons">IDENTIFIED ONLY</button><span id="ndbStat"></span></div>`
      + `<div id="ndbTiles"></div><div id="ndbSpec" title="The beacon band: each carrier, its ident above it"></div>`
      + `<div id="ndbTabs"><button data-t="beacons">BEACONS</button><button data-t="map">MAP</button><button data-t="copy">LIVE COPY</button><button data-t="log">HEARD LOG</button></div>`
      + `<div id="ndbPane" aria-live="polite"></div>`
      + `<div class="ndbNote">✓ read by consensus, matches the published beacon · ≈ one letter off it · <i>italic?</i> not read yet — the nearest published beacon on that frequency. Tap a row to listen. Beacon data: OurAirports (public domain).</div>`;
    document.body.appendChild(el);
    this.el = el;
    this.q<HTMLButtonElement>('#ndbExit').onclick = () => this.h.onExit();
    this.q<HTMLButtonElement>('#ndbFind').onclick = () => this.openFind();
    this.q<HTMLButtonElement>('#ndbStats').onclick = () => { this.openDlg('<h3>RECEPTION STATS</h3><div class="ndbEmpty">Asking the server…</div>'); this.h.stats(); };
    this.q<HTMLButtonElement>('#ndbOnly').onclick = () => { this.p.identOnly = !this.p.identOnly; this.save(); this.redraw(); };
    el.querySelectorAll('#ndbTabs button').forEach((b) => {
      (b as HTMLButtonElement).onclick = () => { this.p.tab = (b as HTMLElement).dataset.t as Tab; this.save(); this.redraw(); };
    });
    return el;
  }
  show() { this.mount().hidden = false; this.place(); if (!this.onResize) { this.onResize = () => this.place(); window.addEventListener('resize', this.onResize); } }
  /** ★ Just above the control deck (ISM's rule): never over the controls; a short screen shrinks it. */
  private place() {
    const el = this.el;
    if (!el || el.hidden) return;
    let top = window.innerHeight;
    for (const id of ['mFreqBox', 'mVfoDown', 'mVfoUp', 'mZoomIn', 'mStep']) {
      const r = document.getElementById(id)?.getBoundingClientRect();
      if (r && r.height > 0) top = Math.min(top, r.top);
    }
    const gap = top < window.innerHeight ? 18 : 16;
    el.style.bottom = `${Math.round(window.innerHeight - top + gap)}px`;
    el.style.maxHeight = `${Math.round(Math.max(200, (top - gap - 60) * 0.8))}px`;
  }
  hide() {
    if (this.el) this.el.hidden = true;
    this.last = null; this.lastHtml = ''; this.copy = [];
    this.map?.close(); this.map = null;
    this.closeDlg();
    if (this.onResize) { window.removeEventListener('resize', this.onResize); this.onResize = null; }
  }
  get visible() { return !!this.el && !this.el.hidden; }

  update(r: NdbReport) {
    this.mount();
    this.last = r;
    // the live copy: merged by sequence number (the report carries the last 40)
    const seen = new Set(this.copy.map((c) => c.s));
    for (const c of r.copy) if (!seen.has(c.s)) this.copy.push(c);
    if (this.copy.length > 300) this.copy.splice(0, this.copy.length - 300);
    this.redraw();
  }

  private redraw() {
    const r = this.last;
    if (!this.el || !r) return;
    const t = ndbTiles(r);
    this.q('#ndbStat').textContent = `${khz(r.lo)}–${khz(r.hi)} kHz${r.starting ? ' · starting…' : ''}${r.pos === false ? ' · no receiver position set — distances unknown' : ''}`;
    this.q('#ndbTiles').innerHTML = [['CARRIERS', t.carriers], ['IDENTIFIED', t.identified], ['FURTHEST', t.furthest], ['COVERAGE', t.coverage]]
      .map(([k, v]) => `<div class="ndbTile" title="${k === 'COVERAGE' ? 'Of the published beacons in this band no further away than the furthest one heard, how many are heard now' : ''}"><b>${esc(v)}</b><span>${k}</span></div>`).join('');
    this.q('#ndbOnly').classList.toggle('on', this.p.identOnly);
    this.el.querySelectorAll('#ndbTabs button').forEach((b) => b.classList.toggle('on', (b as HTMLElement).dataset.t === this.p.tab));
    this.drawSpec(r);
    this.place();
    if (this.p.tab === 'map') { this.drawMapPane(r); return; }
    this.map?.close(); this.map = null;
    const html = this.p.tab === 'beacons' ? this.tableHtml(r) : this.p.tab === 'copy' ? this.copyHtml() : this.logHtml(r);
    if (html !== this.lastHtml) {
      const pane = this.q('#ndbPane');
      pane.innerHTML = html; this.lastHtml = html;
      pane.querySelectorAll('tr[data-hz]').forEach((tr) => { (tr as HTMLElement).onclick = () => this.h.onTune(Number((tr as HTMLElement).dataset.hz)); });
    }
  }

  private drawSpec(r: NdbReport) {
    const lo = r.lo, hi = r.hi;
    if (!(hi > lo)) return;
    const W = 1000, H = 64;
    const x = (hz: number) => ((hz - lo) / (hi - lo)) * W;
    let s = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" role="img" aria-label="Carriers across the band">`;
    for (let k = Math.ceil(lo / 50e3) * 50e3; k <= hi; k += 50e3) s += `<line x1="${x(k)}" x2="${x(k)}" y1="${H - 10}" y2="${H}" stroke="rgba(255,176,0,0.3)"/><text x="${x(k) + 2}" y="${H - 1}" font-size="9" fill="rgba(255,190,40,0.5)">${k / 1e3}</text>`;
    for (const b of r.beacons) {
      if (!b.active) continue;
      const h = Math.max(4, Math.min(40, b.snr * 1.3));
      const col = b.harm ? '#c9a0ff' : b.ident ? '#8cff8c' : 'rgba(255,176,0,0.55)';
      s += `<line x1="${x(b.hz)}" x2="${x(b.hz)}" y1="${H - 12}" y2="${H - 12 - h}" stroke="${col}" stroke-width="2"/>`;
      if (b.ident) s += `<text x="${x(b.hz)}" y="${H - 14 - h}" font-size="10" text-anchor="middle" fill="${col}">${esc(b.ident)}</text>`;
    }
    this.q('#ndbSpec').innerHTML = s + '</svg>';
  }

  private tableHtml(r: NdbReport): string {
    let rows = ndbRows(r);
    if (this.p.identOnly) rows = rows.filter((b) => !!b.ident);
    if (!rows.length) return `<div class="ndbEmpty">${r.starting || !r.beacons.length ? 'Listening across the band — beacons appear as their carriers are found; an ident is shown once several of its repeats agree (about half a minute for a clear one).' : 'Nothing identified yet.'}</div>`;
    const head = `<tr><th>kHz</th><th>IDENT</th><th>STATION</th><th>DISTANCE</th><th class="num">SNR</th><th class="num hideS">TONE</th><th class="num hideS">WPM</th><th class="hideS">COPY</th></tr>`;
    const body = rows.map((b) => {
      const c = ndbIdentCell(b), st = ndbStation(b);
      const alt = b.alt?.length ? ` · also on this frequency: ${b.alt.map((a) => `${a.ident} ${a.name}${a.km >= 0 ? ` ${Math.round(a.km)} km` : ''}`).join(', ')}` : '';
      return `<tr class="tap${b.active ? '' : ' gone'}" data-hz="${Math.round(b.hz)}" title="${esc(`Tap to listen on ${khz(b.hz)} kHz${alt}`)}">`
        + `<td class="num">${khz(b.hz)}</td><td class="${c.cls}" title="${esc(c.title)}">${esc(c.text)}${b.also?.length ? ` <small>(also ${esc(b.also.join(', '))})</small>` : ''}</td>`
        + `<td>${esc(st.name)}</td><td>${esc(st.dist)}</td><td class="num">${b.snr.toFixed(0)} dB</td>`
        + `<td class="num hideS">${b.a1a ? 'A1A' : b.tone ? `${Math.round(b.tone)} Hz` : ''}</td><td class="num hideS">${b.wpm ? b.wpm.toFixed(1) : ''}</td>`
        + `<td class="cp hideS">${esc(b.copy)}</td></tr>`;
    }).join('');
    return `<table class="ndbT">${head}${body}</table>`;
  }
  private copyHtml(): string {
    if (!this.copy.length) return '<div class="ndbEmpty">Nothing decoded yet.</div>';
    const byId = new Map((this.last?.beacons ?? []).map((b) => [b.id, b]));
    return `<table class="ndbT"><tr><th>UTC</th><th>kHz</th><th>READ</th><th>IDENT</th></tr>${this.copy.slice().reverse().slice(0, 150).map((c) => {
      const b = byId.get(c.id);
      return `<tr class="tap" data-hz="${Math.round(c.hz)}"><td>${utc(c.t / 1000)}</td><td class="num">${khz(c.hz)}</td><td>${esc(c.text)}</td><td>${esc(b?.ident || '')}</td></tr>`;
    }).join('')}</table>`;
  }
  private logHtml(r: NdbReport): string {
    if (!r.log.length) return '<div class="ndbEmpty">No beacons identified yet.</div>';
    return `<table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th><th class="num">BEST SNR</th><th class="hideS">FIRST HEARD</th><th>LAST HEARD</th></tr>`
      + r.log.map((e) => `<tr class="tap" data-hz="${Math.round(e.hz)}"><td class="unl">${esc(e.ident)}</td><td>${esc(`${flag(e.cc)} ${e.name}`.trim())}</td>`
        + `<td class="num">${khz(e.hz)}</td><td>${e.km >= 0 ? `${Math.round(e.km)} km ${compass(e.brg)}` : ''}</td><td class="num">${e.best.toFixed(0)} dB</td>`
        + `<td class="hideS">${utc(e.first)}</td><td>${utc(e.last)}</td></tr>`).join('') + '</table>';
  }

  // ── the map ──
  private drawMapPane(r: NdbReport) {
    const pane = this.q('#ndbPane');
    if (!pane.querySelector('#ndbMapHost')) {
      this.lastHtml = '';
      const wins: [number, string][] = [[600, '10 min'], [1800, '30 min'], [3600, '1 h'], [10800, '3 h'], [21600, '6 h'], [43200, '12 h'], [86400, '24 h'], [604800, '7 days'], [0, 'all']];
      pane.innerHTML = `<div id="ndbMapCtl"><button data-m="autofit">AUTOFIT</button><button data-m="rings">RINGS</button><button data-m="unheard">UNHEARD</button>`
        + `<label>HEARD IN THE LAST <select id="ndbWin">${wins.map(([v, l]) => `<option value="${v}"${v === this.p.win ? ' selected' : ''}>${l}</option>`).join('')}</select></label></div>`
        + `<div id="ndbMapHost"></div>`;
      pane.querySelectorAll('#ndbMapCtl button').forEach((b) => {
        (b as HTMLButtonElement).onclick = () => {
          const m = (b as HTMLElement).dataset.m as 'autofit' | 'rings' | 'unheard';
          this.p[m] = !this.p[m]; this.save();
          if (m === 'unheard' && this.p.unheard) this.h.find('');
          this.drawMapPane(this.last!);
        };
      });
      (pane.querySelector('#ndbWin') as HTMLSelectElement).onchange = (e) => { this.p.win = Number((e.target as HTMLSelectElement).value); this.save(); this.drawMapPane(this.last!); };
      this.map = new NdbMap(pane.querySelector('#ndbMapHost') as HTMLElement);
      this.map.open(this.h.myPos());
      if (this.p.unheard && !this.found.length) this.h.find('');
    }
    pane.querySelectorAll('#ndbMapCtl button').forEach((b) => b.classList.toggle('on', !!this.p[(b as HTMLElement).dataset.m as 'autofit']));
    const now = r.now || Date.now() / 1000;
    const heard = r.log.filter((e) => e.km >= 0 && (this.p.win === 0 || now - e.last <= this.p.win));
    const keys = new Set(r.log.map((e) => `${e.ident}@${Math.round(e.hz / 500)}`));
    const unheard = this.p.unheard ? this.found.filter((f) => !keys.has(`${f.ident}@${Math.round(f.hz / 500)}`) && f.hz >= r.lo && f.hz <= r.hi) : [];
    this.map?.update(heard, unheard, { rings: this.p.rings, autofit: this.p.autofit, rx: this.h.myPos(), nowS: now });
  }

  // ── dialogs ──
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
  private dlg = '';
  private openFind() {
    this.dlg = 'find';
    this.openDlg(`<h3>FIND AN NDB</h3><input id="ndbQ" placeholder="ident, name or kHz — empty: the nearest" autocomplete="off"><div id="ndbFoundList" class="ndbEmpty">Searching…</div>`
      + `<div class="ndbNote">Searches every published NDB (OurAirports, public domain), nearest you first. Tap one in the band to listen.</div>`);
    const inp = document.getElementById('ndbQ') as HTMLInputElement;
    let tmr = 0;
    inp.oninput = () => { clearTimeout(tmr); tmr = window.setTimeout(() => this.h.find(inp.value.trim()), 250); };
    inp.focus();
    this.h.find('');
  }
  /** The server's answer to Find NDB (also feeds the map's unheard beacons when the query is empty). */
  found_(m: any) {
    const list: NdbFound[] = (Array.isArray(m?.list) ? m.list : []).slice(0, 400).map((f: any) => ({
      ident: ident(f.ident), hz: num(f.hz), name: str(f.name, 60), cc: str(f.cc, 2), lat: num(f.lat), lon: num(f.lon), km: num(f.km, -1), brg: num(f.brg), heard: f.heard === true }));
    if (!m?.q) { this.found = list; if (this.p.tab === 'map' && this.last) this.drawMapPane(this.last); }
    if (this.dlg !== 'find') return;
    const host = document.getElementById('ndbFoundList');
    if (!host) return;
    if (m?.pos === false) { host.textContent = 'This server has no position set, so nothing can be ranked by distance.'; return; }
    if (!list.length) { host.textContent = 'Nothing matches.'; return; }
    const lo = this.last?.lo ?? 0, hi = this.last?.hi ?? 0;
    host.className = '';
    host.innerHTML = `<table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th><th></th></tr>${list.slice(0, 120).map((f) => {
      const inBand = f.hz >= lo && f.hz <= hi;
      return `<tr class="${inBand ? 'tap' : ''}" ${inBand ? `data-hz="${Math.round(f.hz)}"` : ''}><td class="${f.heard ? 'conf' : ''}">${f.heard ? '✓ ' : ''}${esc(f.ident)}</td>`
        + `<td>${esc(`${flag(f.cc)} ${f.name}`.trim())}</td><td class="num">${khz(f.hz)}</td><td>${Math.round(f.km)} km ${compass(f.brg)}</td><td>${inBand ? '' : '<small>outside the band</small>'}</td></tr>`;
    }).join('')}</table>`;
    host.querySelectorAll('tr[data-hz]').forEach((tr) => { (tr as HTMLElement).onclick = () => { this.h.onTune(Number((tr as HTMLElement).dataset.hz)); this.closeDlg(); }; });
  }
  /** The server's stats: per hour, by hour of day, each beacon's SNR — with CSV for each. */
  stats_(m: any) {
    const hours: { t: number; heard: number; far: number }[] = (Array.isArray(m?.hours) ? m.hours : []).map((h: any) => ({ t: num(h.t), heard: num(h.heard), far: num(h.far) }));
    const beacons = (Array.isArray(m?.beacons) ? m.beacons : []).slice(0, 400).map((b: any) => ({
      ident: ident(b.ident), hz: num(b.hz), name: str(b.name, 60), cc: str(b.cc, 2), km: num(b.km, -1), minutes: num(b.minutes), mean: num(b.mean), best: num(b.best), last: num(b.last),
      snr: (Array.isArray(b.snr) ? b.snr : []).slice(-300).map((p: any) => [num(p?.[0]), num(p?.[1])] as [number, number]) }));
    if (!hours.length && !beacons.length) { this.openDlg('<h3>RECEPTION STATS</h3><div class="ndbEmpty">No history yet — it builds up a minute at a time while NDB runs (kept in memory, at most 7 days).</div>'); return; }
    const byHod = new Array(24).fill(0).map(() => ({ n: 0, sum: 0 }));
    for (const h of hours) { const d = byHod[new Date(h.t * 1000).getUTCHours()]; d.n++; d.sum += h.heard; }
    const bars = (vals: number[], labels: string[], unit: string) => {
      const mx = Math.max(1, ...vals), W = 600, H = 100, bw = W / Math.max(1, vals.length);
      return `<div class="ndbChart"><svg viewBox="0 0 ${W} ${H + 12}" preserveAspectRatio="none">${vals.map((v, i) => `<rect x="${i * bw + 1}" y="${H - (v / mx) * H}" width="${Math.max(1, bw - 2)}" height="${(v / mx) * H}" fill="rgba(255,176,0,0.7)"><title>${esc(labels[i])}: ${Math.round(v * 10) / 10} ${unit}</title></rect>`).join('')}</svg></div>`;
    };
    const rows = beacons.sort((a: any, b: any) => a.minutes - b.minutes);
    this.openDlg(`<h3>RECEPTION STATS</h3><div class="ndbNote">Since ${utc(num(m.since))} UTC on this server (in memory). UTC throughout.</div>`
      + `<h4>BEACONS HEARD PER HOUR <button data-csv="hours">CSV</button></h4>${bars(hours.map((h) => h.heard), hours.map((h) => new Date(h.t * 1000).toISOString().slice(0, 13) + 'h'), 'beacons')}`
      + `<h4>FURTHEST BEACON HEARD PER HOUR (km)</h4>${bars(hours.map((h) => h.far), hours.map((h) => new Date(h.t * 1000).toISOString().slice(0, 13) + 'h'), 'km')}`
      + `<h4>BY HOUR OF DAY — AVERAGE BEACONS HEARD <button data-csv="hod">CSV</button></h4>${bars(byHod.map((d) => (d.n ? d.sum / d.n : 0)), byHod.map((_, i) => `${i}:00`), 'beacons')}`
      + `<h4>BEACONS, LEAST HEARD FIRST — TAP ONE FOR ITS SNR OVER TIME <button data-csv="beacons">CSV</button></h4><div id="ndbSnrChart"></div>`
      + `<table class="ndbT"><tr><th>IDENT</th><th>STATION</th><th>kHz</th><th>DISTANCE</th><th class="num">MINUTES</th><th class="num">MEAN SNR</th><th class="num">BEST</th><th>LAST</th></tr>`
      + rows.map((b: any, i: number) => `<tr class="tap" data-i="${i}"><td>${esc(b.ident)}</td><td>${esc(`${flag(b.cc)} ${b.name}`.trim())}</td><td class="num">${khz(b.hz)}</td>`
        + `<td>${b.km >= 0 ? `${Math.round(b.km)} km` : ''}</td><td class="num">${b.minutes}</td><td class="num">${b.mean.toFixed(1)}</td><td class="num">${b.best.toFixed(1)}</td><td>${utc(b.last)}</td></tr>`).join('') + '</table>');
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
        if (k === 'hours') dl('ndb-per-hour.csv', csv(['hour_utc', 'beacons_heard', 'furthest_km'], hours.map((h) => [new Date(h.t * 1000).toISOString(), h.heard, Math.round(h.far)])));
        if (k === 'hod') dl('ndb-by-hour-of-day.csv', csv(['hour_utc', 'average_beacons_heard'], byHod.map((d, i) => [i, d.n ? (d.sum / d.n).toFixed(2) : 0])));
        if (k === 'beacons') dl('ndb-beacons.csv', csv(['ident', 'khz', 'station', 'country', 'km', 'minutes_heard', 'mean_snr_db', 'best_snr_db', 'last_heard_utc'],
          rows.map((x: any) => [x.ident, (x.hz / 1e3).toFixed(1), x.name, x.cc, Math.round(x.km), x.minutes, x.mean.toFixed(1), x.best.toFixed(1), new Date(x.last * 1000).toISOString()])));
      };
    });
  }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE MAP — our MapLibre map (the same renderer, style and packs as ADS-B: /mapgl/), beacons as a GeoJSON layer;
//  where there is no WebGL 2 or no /mapgl/, a note says so and the table carries on.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
function ringGeo(lat: number, lon: number, km: number): [number, number][] {
  const pts: [number, number][] = [];
  const R = 6371, d = km / R, p1 = lat * Math.PI / 180, l1 = lon * Math.PI / 180;
  for (let i = 0; i <= 96; i++) {
    const b = (i / 96) * 2 * Math.PI;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(b));
    const l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    pts.push([((l2 * 180 / Math.PI + 540) % 360) - 180, p2 * 180 / Math.PI]);
  }
  return pts;
}
export class NdbMap {
  private vm: any = null;
  private gen = 0;
  private pending: (() => void) | null = null;
  private fitted = '';
  constructor(private host: HTMLElement) {}
  open(rx: { lat: number; lon: number } | null) {
    const my = ++this.gen;
    void (async () => {
      const w = window as any;
      const kit = await probeMapGL();
      if (my !== this.gen) return;
      if (!kit || !(await loadMapGLScripts(kit.base)) || my !== this.gen) {
        this.host.innerHTML = '<div class="ndbEmpty">The map needs WebGL 2 and this server\'s map files — the table and the heard log carry on.</div>';
        return;
      }
      const vm = w.VibeMapGL.createNow(this.host, { style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail, profile: 'aero',
        center: rx ? [rx.lon, rx.lat] : [0, 30], zoom: rx ? 5 : 1.5 });
      if (!vm) { this.host.innerHTML = '<div class="ndbEmpty">The map could not start in this browser.</div>'; return; }
      this.vm = vm;
      try { vm.map.addControl(new w.maplibregl.NavigationControl({ showCompass: false }), 'top-left'); } catch (e) { console.error('[ndb] map control', e); }
      await vm.ready.catch((e: unknown) => console.error('[ndb] map style', e));
      if (my !== this.gen) return;
      const m = vm.map;
      const empty = { type: 'FeatureCollection', features: [] };
      for (const id of ['ndbRings', 'ndbLinks', 'ndbUnheard', 'ndbHeard', 'ndbRx']) m.addSource(id, { type: 'geojson', data: empty });
      m.addLayer({ id: 'ndbRings', type: 'line', source: 'ndbRings', paint: { 'line-color': 'rgba(255,176,0,0.35)', 'line-dasharray': [2, 3] } });
      m.addLayer({ id: 'ndbRingLbl', type: 'symbol', source: 'ndbRings', layout: { 'symbol-placement': 'line', 'text-field': ['get', 'label'], 'text-size': 10 }, paint: { 'text-color': 'rgba(255,190,40,0.7)' } });
      m.addLayer({ id: 'ndbLinks', type: 'line', source: 'ndbLinks', paint: { 'line-color': 'rgba(255,176,0,0.3)', 'line-width': 1 } });
      m.addLayer({ id: 'ndbUnheard', type: 'circle', source: 'ndbUnheard', paint: { 'circle-radius': 3, 'circle-color': 'rgba(160,160,160,0.7)' } });
      m.addLayer({ id: 'ndbUnheardLbl', type: 'symbol', source: 'ndbUnheard', minzoom: 6, layout: { 'text-field': ['get', 'ident'], 'text-size': 10, 'text-offset': [0, 1.1] }, paint: { 'text-color': 'rgba(190,190,190,0.8)' } });
      m.addLayer({ id: 'ndbHeard', type: 'circle', source: 'ndbHeard', paint: { 'circle-radius': 6, 'circle-color': '#ffb000', 'circle-stroke-color': '#000', 'circle-stroke-width': 1, 'circle-opacity': ['get', 'op'] } });
      m.addLayer({ id: 'ndbHeardLbl', type: 'symbol', source: 'ndbHeard', layout: { 'text-field': ['get', 'label'], 'text-size': 12, 'text-offset': [0, 1.3], 'text-allow-overlap': false }, paint: { 'text-color': '#ffd27a', 'text-halo-color': '#000', 'text-halo-width': 1 } });
      m.addLayer({ id: 'ndbRx', type: 'circle', source: 'ndbRx', paint: { 'circle-radius': 5, 'circle-color': '#fff', 'circle-stroke-color': '#000', 'circle-stroke-width': 2 } });
      const p = this.pending; this.pending = null; p?.();
    })().catch((e) => { console.error('[ndb] the map could not load', e); });
  }
  update(heard: NdbLog[], unheard: NdbFound[], o: { rings: boolean; autofit: boolean; rx: { lat: number; lon: number } | null; nowS: number }) {
    const apply = () => {
      const m = this.vm?.map;
      if (!m || !m.getSource('ndbHeard')) { this.pending = apply; return; }
      const fc = (features: any[]) => ({ type: 'FeatureCollection', features });
      const far = Math.max(50, ...heard.map((h) => h.km));
      m.getSource('ndbHeard').setData(fc(heard.map((h) => ({ type: 'Feature', geometry: { type: 'Point', coordinates: [h.lon, h.lat] },
        properties: { ident: h.ident, label: `${h.ident} ${(h.hz / 1e3).toFixed(1)}`, op: o.nowS - h.last < 300 ? 1 : 0.5 } }))));
      m.getSource('ndbUnheard').setData(fc(unheard.map((u) => ({ type: 'Feature', geometry: { type: 'Point', coordinates: [u.lon, u.lat] }, properties: { ident: u.ident } }))));
      if (o.rx) {
        m.getSource('ndbRx').setData(fc([{ type: 'Feature', geometry: { type: 'Point', coordinates: [o.rx.lon, o.rx.lat] }, properties: {} }]));
        m.getSource('ndbLinks').setData(fc(heard.map((h) => ({ type: 'Feature', geometry: { type: 'LineString', coordinates: [[o.rx!.lon, o.rx!.lat], [h.lon, h.lat]] }, properties: {} }))));
        const steps = [25, 50, 100, 200, 250, 500, 1000, 2000, 5000];
        const step = steps.find((s) => far / s <= 4) ?? 5000;
        const rings: any[] = [];
        if (o.rings) for (let k = step; k <= far * 1.1 + step; k += step) rings.push({ type: 'Feature', geometry: { type: 'LineString', coordinates: ringGeo(o.rx.lat, o.rx.lon, k) }, properties: { label: `${k} km` } });
        m.getSource('ndbRings').setData(fc(rings));
        const key = heard.map((h) => h.ident).sort().join(',');
        if (o.autofit && heard.length && key !== this.fitted) {
          this.fitted = key;
          const lons = [o.rx.lon, ...heard.map((h) => h.lon)], lats = [o.rx.lat, ...heard.map((h) => h.lat)];
          try { m.fitBounds([[Math.min(...lons), Math.min(...lats)], [Math.max(...lons), Math.max(...lats)]], { padding: 40, maxZoom: 9, duration: 0 }); } catch (e) { console.error('[ndb] fitBounds', e); }
        }
      }
    };
    apply();
  }
  close() {
    this.gen++; this.pending = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[ndb] map destroy', e); }
    this.vm = null;
  }
}
