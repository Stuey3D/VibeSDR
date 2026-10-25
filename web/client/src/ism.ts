/* ism.ts — V12 ISM mode in the web client (docs/v12/ISM-DECODER.md): the report's types, the live device list (newest
 * first, one line per device, a filter by kind, last-heard times, units), and the box that shows it over the lower part
 * of the waterfall. The waterfall stays live above it — the bursts on it are half of what the mode is for.
 *
 * ★★★ PRIVACY IS THE SERVER'S (vibe_ism_service.h): a TPMS, a key fob or a fixed-code remote arrives already as
 *     "TPMS #2" — no ID, no code. Nothing here could show one; nothing here tries.
 * ★ The server's `line` is metric with PSI for tyres. A listener who picks mph or kPa/bar gets the line rebuilt here from
 *   the numeric fields (ismLine) — one rule, the same order as the server's readable(). */

export interface IsmDevice {
  key: string; kind: string; make: string; model: string; name: string; line: string;
  nick?: number; id?: string; ch?: number;
  temp?: number; hum?: number; wind?: number; gust?: number; dir?: number; rain?: number; rainRate?: number;
  kpa?: number; uv?: number; watts?: number; baro?: number; batt?: number; button?: string; note?: string; extra?: string;
  freq: number; level: number; snr: number; seen: number; age: number; count: number;
}
export interface IsmBand { id: string; name: string; hz: number }
export interface IsmReport {
  type: 'ism'; band?: string; bandName?: string; centre: number; custom?: number; rate?: number; now?: number;
  bursts?: number; decoded?: number; unknown?: number; noise?: number; dropped?: number; capture?: number;
  gain?: number; gainAuto?: number;
  devices: IsmDevice[];
}
export interface IsmUnits { mph: boolean; pressure: 'psi' | 'kpa' | 'bar' }

/** The kinds the filter offers, in this order (Stuart's list). */
export const ISM_KINDS: Array<{ id: string; label: string }> = [
  { id: 'all', label: 'ALL' }, { id: 'weather', label: 'WEATHER' }, { id: 'tpms', label: 'TPMS' },
  { id: 'sensor', label: 'SENSOR' }, { id: 'doorbell', label: 'DOORBELL' }, { id: 'energy', label: 'ENERGY' },
  { id: 'keyfob', label: 'KEY FOB' },
];

/** A report from the wire, sanitised: only plain fields of the types we read, at most 300 devices. */
export function ismSanitise(msg: any): IsmReport | null {
  if (!msg || msg.type !== 'ism') return null;
  const num = (v: any) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  const str = (v: any, n = 120) => (typeof v === 'string' ? v.slice(0, n) : undefined);
  const devices: IsmDevice[] = [];
  for (const d of Array.isArray(msg.devices) ? msg.devices.slice(0, 300) : []) {
    if (!d || typeof d !== 'object' || typeof d.line !== 'string') continue;
    devices.push({
      key: str(d.key, 24) ?? '', kind: str(d.kind, 16) ?? 'sensor', make: str(d.make, 40) ?? '', model: str(d.model, 60) ?? '',
      name: str(d.name, 80) ?? '', line: str(d.line, 300) ?? '', nick: num(d.nick), id: str(d.id, 16), ch: num(d.ch),
      temp: num(d.temp), hum: num(d.hum), wind: num(d.wind), gust: num(d.gust), dir: num(d.dir), rain: num(d.rain),
      rainRate: num(d.rainRate), kpa: num(d.kpa), uv: num(d.uv), watts: num(d.watts), baro: num(d.baro), batt: num(d.batt),
      button: str(d.button, 16), note: str(d.note, 40), extra: str(d.extra, 60),
      freq: num(d.freq) ?? 0, level: num(d.level) ?? -99, snr: num(d.snr) ?? 0, seen: num(d.seen) ?? 0, age: num(d.age) ?? 0,
      count: num(d.count) ?? 1,
    });
  }
  return {
    type: 'ism', band: str(msg.band, 8), bandName: str(msg.bandName, 40), centre: num(msg.centre) ?? 0, custom: num(msg.custom),
    rate: num(msg.rate), now: num(msg.now), bursts: num(msg.bursts), decoded: num(msg.decoded), unknown: num(msg.unknown),
    noise: num(msg.noise), dropped: num(msg.dropped), capture: num(msg.capture), gain: num(msg.gain), gainAuto: num(msg.gainAuto),
    devices,
  };
}

const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
export function compass(deg: number): string { return COMPASS[Math.floor((((deg % 360) + 360 + 11.25) % 360) / 22.5) & 15]; }

/** The readable line in the listener's units — the server's readable() order exactly. With the defaults (km/h, PSI) it
 *  is the server's own line, which is used as sent. */
export function ismLine(d: IsmDevice, u: IsmUnits): string {
  if (!u.mph && u.pressure === 'psi') return d.line;
  const secret = d.kind === 'tpms' || d.kind === 'keyfob' || d.kind === 'doorbell' || d.kind === 'suspectfob';
  const p: string[] = [d.name];
  if (secret && d.kind === 'tpms' && d.model) p.push(d.model);
  if (secret && d.kind === 'keyfob' && d.make) p.push(d.make);
  if (d.kind === 'tpms' && d.kpa !== undefined)
    p.push(u.pressure === 'kpa' ? `${Math.round(d.kpa)} kPa` : u.pressure === 'bar' ? `${(d.kpa / 100).toFixed(2)} bar` : `${Math.round(d.kpa * 0.1450377)} PSI`);
  if (d.temp !== undefined) p.push(`${d.kind === 'tpms' ? Math.round(d.temp) : d.temp.toFixed(1)} °C`);
  if (d.hum !== undefined) p.push(`${Math.round(d.hum)} % RH`);
  const sp = (kmh: number) => (u.mph ? `${Math.round(kmh / 1.609344)} mph` : `${Math.round(kmh)} km/h`);
  if (d.wind !== undefined) p.push(`wind ${sp(d.wind)}${d.dir !== undefined ? ' ' + compass(d.dir) : ''}`);
  if (d.gust !== undefined) p.push(`gust ${sp(d.gust)}`);
  if (d.rain !== undefined) p.push(`rain ${d.rain.toFixed(1)} mm`);
  if (d.rainRate !== undefined && d.rainRate > 0) p.push(`${d.rainRate.toFixed(1)} mm/h`);
  if (d.uv !== undefined) p.push(`UV ${Math.round(d.uv)}`);
  if (d.baro !== undefined) p.push(`${Math.round(d.baro)} hPa`);
  if (d.watts !== undefined) p.push(d.watts >= 1000 ? `${(d.watts / 1000).toFixed(2)} kW` : `${Math.round(d.watts)} W`);
  if (d.kind === 'keyfob' || d.kind === 'doorbell' || d.kind === 'suspectfob') p.push(d.button ? `button ${d.button}` : d.note === 'learn' ? 'learn' : 'button press');
  if (d.extra) p.push(d.extra);
  if (d.batt === 1) p.push('battery OK'); else if (d.batt === 0) p.push('battery low');
  if (!secret && d.ch !== undefined) p.push(`ch ${d.ch}`);
  return p.join(' · ');
}

/** "now", "12 s", "3 min", "1 h 5 min" — how long since the device was last heard. */
export function ismAgo(sec: number): string {
  if (!(sec >= 0) || sec < 2) return 'now';
  if (sec < 60) return `${Math.round(sec)} s`;
  if (sec < 3600) return `${Math.round(sec / 60)} min`;
  const h = Math.floor(sec / 3600), m = Math.round((sec % 3600) / 60);
  return m ? `${h} h ${m} min` : `${h} h`;
}

/** The status line: "433.92 MHz ISM · 6 devices · 41 bursts (3 unidentified) · gain 33.8 dB auto · capture 100 %". */
export function ismStatus(r: IsmReport): string {
  const parts: string[] = [];
  parts.push(r.custom ? `${(r.centre / 1e6).toFixed(3)} MHz` : (r.bandName || 'ISM'));
  parts.push(`${r.devices.length} ${r.devices.length === 1 ? 'device' : 'devices'}`);
  if (r.bursts !== undefined) parts.push(`${r.bursts} bursts${r.unknown ? ` (${r.unknown} unidentified)` : ''}`);
  /* ★ THE GAIN ISM IS ACTUALLY USING (Stuart, 2026-10-25: "the gain is indicating 0dB but I know that not to be the case").
   *  ISM sets the radio's gain itself (vibe_ism_service.h GainRule: 60 % up the radio's own list, down only on clipping)
   *  and the dongle AGC off, so the ordinary gain readout has nothing true to say — the report's `gain` is the figure. */
  if (r.gain !== undefined && r.gain >= 0) parts.push(`gain ${r.gain.toFixed(1)} dB${r.gainAuto === 1 ? ' auto' : ''}`);
  if (r.capture !== undefined && r.capture >= 0) parts.push(`capture ${r.capture} %`);
  return parts.join(' · ');
}

function esc(s: string): string { return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }

/** The list's HTML: newest first (the server's order), filtered by kind. */
export function ismListHtml(r: IsmReport, filter: string, u: IsmUnits): string {
  // ★ KEY FOB shows the suspected ones too (vibe_ism_service.h FobWatch): a car key we cannot name is still a key fob
  const rows = r.devices.filter((d) => filter === 'all' || d.kind === filter || (filter === 'keyfob' && d.kind === 'suspectfob'));
  if (!rows.length) {
    const what = filter === 'all' ? 'No sensors heard yet' : `Nothing of that kind heard yet`;
    return `<div class="ismEmpty">${what} — most send every 30–60 s; tyres only while the car moves.</div>`;
  }
  return rows.map((d) => {
    const mhz = (d.freq / 1e6).toFixed(3);
    const tip = `${mhz} MHz · ${d.snr.toFixed(0)} dB over the noise · heard ${d.count} ${d.count === 1 ? 'time' : 'times'}`;
    return `<div class="ismRow k-${esc(d.kind)}" data-key="${esc(d.key)}" title="${esc(tip)}">`
         + `<span class="ismAgo">${esc(ismAgo(d.seen))}</span><span class="ismTxt">${esc(ismLine(d, u))}</span></div>`;
  }).join('');
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE BOX — over the lower part of the waterfall; header: EXIT ISM, ◀ band ▶, the status; the filter chips; the list.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const CSS = `
#ismBox { position: fixed; left: 50%; transform: translateX(-50%); bottom: 96px; z-index: 30; width: min(760px, calc(100vw - 32px));
  max-height: 44vh; display: flex; flex-direction: column; background: rgba(9, 6, 2, 0.94); border: 1px solid rgba(255, 176, 0, 0.35);
  border-radius: 10px; color: #ffb000; font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace; box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
#ismBox[hidden] { display: none; }
#ismHead { display: flex; align-items: center; gap: 8px; padding: 7px 9px; border-bottom: 1px solid rgba(255, 176, 0, 0.18); flex-wrap: wrap; }
#ismHead button, #ismChips button { font: inherit; font-size: 12px; letter-spacing: 1px; padding: 5px 9px; min-height: 30px; color: #ffb000;
  background: transparent; border: 1px solid rgba(255, 176, 0, 0.35); border-radius: 6px; cursor: pointer; }
#ismHead button:hover, #ismChips button:hover { background: rgba(255, 160, 0, 0.14); }
#ismHead button:disabled { opacity: 0.35; cursor: default; }
#ismExit { font-weight: 600; border-color: rgba(255, 184, 51, 0.55) !important; }
#ismBandName { font-size: 14px; letter-spacing: 1.5px; min-width: 9.5em; text-align: center; }
#ismStat { flex: 1 1 12em; min-width: 0; font-size: 11px; color: rgba(255, 190, 40, 0.7); text-align: right; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#ismChips { display: flex; gap: 6px; padding: 6px 9px; flex-wrap: nowrap; overflow-x: auto; scrollbar-width: none; border-bottom: 1px solid rgba(255, 176, 0, 0.12); }
#ismChips::-webkit-scrollbar { display: none; }
#ismChips button { flex: 0 0 auto; }
#ismChips button.on { background: rgba(255, 176, 0, 0.85); color: #0b0b0b; }
#ismChips .ismUnits { margin-left: auto; }
#ismList { overflow-y: auto; padding: 4px 0; }
.ismRow { display: flex; gap: 10px; padding: 3px 10px; }
.ismRow:nth-child(odd) { background: rgba(255, 176, 0, 0.04); }
.ismAgo { flex: 0 0 4.6em; text-align: right; color: rgba(255, 190, 40, 0.6); font-variant-numeric: tabular-nums; }
.ismTxt { flex: 1 1 auto; min-width: 0; }
.ismEmpty { padding: 12px; color: rgba(255, 190, 40, 0.6); }
@media (max-width: 600px) { #ismBox { font-size: 12px; } #ismHead button, #ismChips button { font-size: 11px; padding: 4px 7px; min-height: 30px; }
  #ismStat { flex-basis: 100%; text-align: left; } .ismAgo { flex-basis: 3.8em; } }
`;

const PREF_KEY = 'vibesdr.ismPrefs';
function loadPrefs(): { filter: string; units: IsmUnits } {
  try {
    const j = JSON.parse(localStorage.getItem(PREF_KEY) || '{}');
    const filter = ISM_KINDS.some((k) => k.id === j.filter) ? j.filter : 'all';
    const pressure = j.pressure === 'kpa' || j.pressure === 'bar' ? j.pressure : 'psi';
    return { filter, units: { mph: j.mph === true, pressure } };
  } catch { return { filter: 'all', units: { mph: false, pressure: 'psi' } }; }
}

export class IsmBox {
  private el: HTMLElement | null = null;
  private last: IsmReport | null = null;
  private prefs = loadPrefs();
  private lastHtml = '';
  /** EXIT ISM, and the band steps (±1). The page decides what they send. */
  constructor(private onExit: () => void, private onStep: (d: number) => void) {}

  private mount(): HTMLElement {
    if (this.el) return this.el;
    if (!document.getElementById('ismCss')) {
      const st = document.createElement('style'); st.id = 'ismCss'; st.textContent = CSS; document.head.appendChild(st);
    }
    const el = document.createElement('div');
    el.id = 'ismBox'; el.hidden = true;
    el.setAttribute('role', 'region'); el.setAttribute('aria-label', 'ISM sensors');
    el.setAttribute('data-navbox', '');
    el.innerHTML = `<div id="ismHead"><button id="ismExit" title="Leave ISM — the receiver goes back to where it was">EXIT ISM</button>`
      + `<button id="ismDown" title="Previous band" aria-label="Previous band">◀</button><span id="ismBandName"></span>`
      + `<button id="ismUp" title="Next band" aria-label="Next band">▶</button><span id="ismStat"></span></div>`
      + `<div id="ismChips"></div><div id="ismList" aria-live="polite"></div>`;
    document.body.appendChild(el);
    (el.querySelector('#ismExit') as HTMLButtonElement).onclick = () => this.onExit();
    (el.querySelector('#ismDown') as HTMLButtonElement).onclick = () => this.onStep(-1);
    (el.querySelector('#ismUp') as HTMLButtonElement).onclick = () => this.onStep(1);
    this.el = el;
    this.drawChips();
    return el;
  }
  private savePrefs() {
    try { localStorage.setItem(PREF_KEY, JSON.stringify({ filter: this.prefs.filter, mph: this.prefs.units.mph, pressure: this.prefs.units.pressure })); } catch { /* private window */ }
  }
  private drawChips() {
    const c = this.el?.querySelector('#ismChips') as HTMLElement | null;
    if (!c) return;
    const u = this.prefs.units;
    c.innerHTML = ISM_KINDS.map((k) => `<button data-k="${k.id}" class="${this.prefs.filter === k.id ? 'on' : ''}">${k.label}</button>`).join('')
      + `<button class="ismUnits" data-u="speed" title="Wind speed units">${u.mph ? 'MPH' : 'KM/H'}</button>`
      + `<button data-u="pressure" title="Tyre pressure units">${u.pressure.toUpperCase()}</button>`;
    c.querySelectorAll('button').forEach((b) => {
      (b as HTMLButtonElement).onclick = () => {
        const k = (b as HTMLElement).dataset.k, uu = (b as HTMLElement).dataset.u;
        if (k) this.prefs.filter = k;
        if (uu === 'speed') this.prefs.units.mph = !this.prefs.units.mph;
        if (uu === 'pressure') this.prefs.units.pressure = u.pressure === 'psi' ? 'kpa' : u.pressure === 'kpa' ? 'bar' : 'psi';
        this.savePrefs(); this.drawChips(); this.lastHtml = ''; if (this.last) this.update(this.last, this.bandsCan);
      };
    });
  }
  private bandsCan: { down: boolean; up: boolean } = { down: true, up: true };

  show() { this.mount().hidden = false; this.place(); if (!this.onResize) { this.onResize = () => this.place(); window.addEventListener('resize', this.onResize); } }
  private onResize: (() => void) | null = null;
  /** ★ Just above the control deck, whatever its height (a phone's deck is ~4× a desktop's): the box must never sit over
   *  the controls, and on a short screen it shrinks rather than covering the waterfall entirely. */
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
    el.style.maxHeight = `${Math.round(Math.max(150, (top - gap - 70) * 0.62))}px`;
  }
  hide() {
    if (this.el) this.el.hidden = true; this.last = null; this.lastHtml = '';
    if (this.onResize) { window.removeEventListener('resize', this.onResize); this.onResize = null; }
  }
  get visible() { return !!this.el && !this.el.hidden; }

  /** A report. `can`: whether a band exists below / above (the arrows grey at the ends — the server clamps). */
  update(r: IsmReport, can: { down: boolean; up: boolean }) {
    const el = this.mount();
    this.last = r; this.bandsCan = can;
    (el.querySelector('#ismBandName') as HTMLElement).textContent = r.custom ? `${(r.centre / 1e6).toFixed(3)} MHz` : (r.bandName || 'ISM');
    (el.querySelector('#ismStat') as HTMLElement).textContent = ismStatus(r);
    (el.querySelector('#ismDown') as HTMLButtonElement).disabled = !can.down;
    (el.querySelector('#ismUp') as HTMLButtonElement).disabled = !can.up;
    this.place();
    const html = ismListHtml(r, this.prefs.filter, this.prefs.units);
    if (html !== this.lastHtml) { (el.querySelector('#ismList') as HTMLElement).innerHTML = html; this.lastHtml = html; }
  }
}
