/**
 * dsc.ts — the web client's DSC box (V12, 2026-10-26): maritime Digital Selective Calling, decoded on the SERVER
 * (android/app/src/main/cpp/decoders/dsc_decoder.h, ITU-R M.493). This file only shows what the server sends.
 *
 *   {"type":"dsc_call","data":{id, utc, rf, repeat, format, category, level, self, selfCountry, to?, distressId?,
 *     nature?, lat?, lon?, posText?, time?, tc1?, tc2?, freqs?, number?, eos, check, quality, band, line}}
 *
 * ★★ READABLE, NEVER A SYMBOL DUMP: each call is the server's own line ("DISTRESS · MMSI 235012345 (United Kingdom) ·
 *    Sinking · 50°12'N 004°10'W · 14:32 UTC · …"), coloured by what it is — distress red, urgency orange, safety amber,
 *    routine plain — newest first. A transmission's repeats (a distress alert goes five times) are one row, "×5".
 * ★ A call that sent a POSITION gets MAP: the position pinned on this server's own map (the same GPU map / Leaflet
 *   fallback the spots map uses), with this receiver's house beside it.
 * ★ The channel keys tune the DSC frequencies as a DSC receiver does: USB with the dial 1.7 kHz below (2185.8 for
 *   2187.5), or channel 70 in NFM — only those this radio can reach. The box itself carries the keys for the band the dial
 *   is on (distress + routine, ITU-R M.541) and, when the dial sits ON an assigned frequency, a one-tap TUNE … USB.
 */
import { mapglVendor, type MapGLKit } from './mapgl';
import { DSC_DISTRESS, dscTuneFor as sharedTuneFor, dscCentreFor } from '../../../src/utils/dscChannels';
export { DSC_DISTRESS, DSC_ROUTINE, DSC_ALL, dscBandOf, dscBandLabel, dscBandChannels, dscChannelOn, dscChannelsForDial,
         dscReachable, dscTuneOffer, dscKeyTitle, dscDialText, type DscBand, type DscChannel } from '../../../src/utils/dscChannels';

export interface DscCall {
  id: number; utc: number; rf: number; repeat: number;
  format: string; category: string; level: number;
  self: string; selfCountry: string; to: string; toCountry: string;
  distressId: string; nature: string; lat: number | null; lon: number | null; posText: string; time: string;
  tc1: string; tc2: string; freqs: string[]; number: string; eos: string; check: 'ok' | 'corrected' | 'unconfirmed';
  quality: number; band: 'hf' | 'vhf'; line: string;
}

/** The DSC distress and safety frequencies (ITU RR Appendix 15) and channel 70. ★ The routine channels (ITU-R M.541)
 *  and every rule about where to tune are src/utils/dscChannels.ts — one table, read by the app too. */
export const DSC_CHANNELS: { hz: number; label: string; vhf: boolean }[] = DSC_DISTRESS.map(({ hz, label, vhf }) => ({ hz, label, vhf }));

/** Where to tune for a DSC channel: USB 1.7 kHz below, or channel 70 in NFM. */
export function dscTuneFor(ch: { hz: number; vhf: boolean }): { hz: number; mode: string } {
  return sharedTuneFor(ch);
}

/** The attach parameters for the listener's dial and mode. ★ The audio centre is worked out from the NEAREST DSC
 *  channel, distress or routine (dscCentreFor), so a dial a little off the usual 1.7 kHz (2186.0 USB) still decodes;
 *  anywhere else, 1700 Hz. */
export function dscParams(dialHz: number, mode: string): Record<string, unknown> {
  const m = (mode || '').toLowerCase();
  if (m === 'nfm' || m === 'fm' || Math.abs(dialHz - 156525000) < 25000) return { band: 'vhf' };
  return { band: 'hf', center_frequency: dscCentreFor(dialHz, m) };
}

/** A dsc_call's data, re-checked field by field (it is from the server, but every client checks what it shows). */
export function dscFromJson(d: any): DscCall | null {
  if (!d || typeof d !== 'object' || typeof d.line !== 'string') return null;
  const s = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  const nn = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
  const lat = nn(d.lat), lon = nn(d.lon);
  return {
    id: n(d.id), utc: n(d.utc) || Date.now(), rf: n(d.rf), repeat: Math.max(1, n(d.repeat)),
    format: s(d.format, 40), category: s(d.category, 20), level: Math.max(0, Math.min(3, n(d.level))),
    self: s(d.self, 9), selfCountry: s(d.selfCountry, 60), to: s(d.to, 9), toCountry: s(d.toCountry, 60),
    distressId: s(d.distressId, 9), nature: s(d.nature, 60),
    lat: lat !== null && Math.abs(lat) <= 90 ? lat : null, lon: lon !== null && Math.abs(lon) <= 180 ? lon : null,
    posText: s(d.posText, 60), time: s(d.time, 8), tc1: s(d.tc1, 60), tc2: s(d.tc2, 80),
    freqs: Array.isArray(d.freqs) ? d.freqs.slice(0, 4).map((x: unknown) => s(x, 24)) : [], number: s(d.number, 20),
    eos: s(d.eos, 20), check: d.check === 'corrected' ? 'corrected' : d.check === 'unconfirmed' ? 'unconfirmed' : 'ok',
    quality: n(d.quality), band: d.band === 'vhf' ? 'vhf' : 'hf', line: s(d.line, 600),
  };
}

export const DSC_LEVEL_CLASS = ['routine', 'safety', 'urgency', 'distress'];

/** The calls heard while the box is open, newest first; a repeat updates its row. */
export class DscCalls {
  list: DscCall[] = [];
  push(c: DscCall): void {
    const i = this.list.findIndex((o) => o.id === c.id && o.rf === c.rf);
    if (i >= 0) this.list.splice(i, 1);
    this.list.unshift(c);
    if (this.list.length > 300) this.list.length = 300;
  }
  clear(): void { this.list.length = 0; }
}

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
function hhmmss(ms: number): string {
  const d = new Date(ms);
  const p = (x: number) => String(x).padStart(2, '0');
  return `${p(d.getUTCHours())}:${p(d.getUTCMinutes())}:${p(d.getUTCSeconds())}z`;
}
export function dscFreqText(hz: number): string {
  if (!(hz > 0)) return '';
  return hz >= 30e6 ? `${(hz / 1e6).toFixed(3)} MHz` : `${(hz / 1e3).toFixed(1)} kHz`;
}

/** The rows. `onMap` is given the call whose MAP was pressed. */
export function renderDscRows(host: HTMLElement, calls: DscCall[], idle: string, onMap: (c: DscCall) => void): void {
  host.textContent = '';
  if (!calls.length) {
    const d = document.createElement('div'); d.className = 'idle'; d.textContent = idle;
    host.appendChild(d);
    return;
  }
  for (const c of calls) {
    const r = document.createElement('div');
    r.className = `dscr ${DSC_LEVEL_CLASS[c.level] || 'routine'}${c.check === 'unconfirmed' ? ' unconf' : ''}`;
    const head = `<span class="t">${hhmmss(c.utc)}</span><span class="f">${esc(dscFreqText(c.rf))}</span>`
      + (c.repeat > 1 ? `<span class="x">×${c.repeat}</span>` : '')
      + (c.check === 'corrected' ? '<span class="ck" title="One or two symbols were put right by the call\'s own check character">corrected</span>' : '')
      + (c.check === 'unconfirmed' ? '<span class="ck" title="Passed its check character, but some symbols were in doubt">unconfirmed</span>' : '');
    r.innerHTML = `<div class="h">${head}</div><div class="l"></div>`;
    (r.querySelector('.l') as HTMLElement).textContent = c.line;
    if (c.lat !== null && c.lon !== null) {
      const b = document.createElement('button');
      b.className = 'btn dscMap'; b.textContent = 'MAP';
      b.title = `${c.posText} — show on the map`;
      b.onclick = (e) => { e.stopPropagation(); onMap(c); };
      (r.querySelector('.h') as HTMLElement).appendChild(b);
    }
    r.title = `${c.format}${c.category ? ' · ' + c.category : ''} · read ${c.check}${c.quality ? ` · quality ${Math.round(c.quality * 100)} %` : ''}`;
    host.appendChild(r);
  }
}

export interface DscPin { lat: number; lon: number; label: string; level: number }
const PIN_COLOUR = ['#9fb3c8', '#E8C547', '#FF8A3D', '#FF4B4B'];

/** The map window's page: the pins, this receiver's house, the GPU map with the Leaflet fallback — every file from
 *  THIS server (never a CDN), as the spots map does. */
export function dscMapHtml(kit: MapGLKit | null, VIBEMAP_JS: string, pins: DscPin[], me: { lat: number; lon: number } | null,
                           leaflet: { js: string; css: string; jsSri: string; cssSri: string } | null, dataBase: string): string {
  const ES = '<' + '/script>';
  const safe = (v: unknown) => JSON.stringify(v).replace(/</g, '\\u003c');
  const glHead = kit ? `<link rel="stylesheet" href="${mapglVendor(kit.base, 'maplibre-gl.css')}">
<script src="${mapglVendor(kit.base, 'maplibre-gl.js')}">${ES}
<script src="${mapglVendor(kit.base, 'pmtiles.js')}">${ES}
<script src="${mapglVendor(kit.base, 'vibemapgl.js')}">${ES}
<script src="${mapglVendor(kit.base, 'vibemapgl-compat.js')}">${ES}` : '';
  const clean = pins.filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon) && Math.abs(p.lat) <= 90 && Math.abs(p.lon) <= 180)
    .slice(0, 200).map((p) => ({ lat: p.lat, lon: p.lon, label: String(p.label).slice(0, 400), colour: PIN_COLOUR[Math.max(0, Math.min(3, p.level))] }));
  return `<!doctype html><meta charset="utf-8"><title>VibeSDR — DSC positions</title>
${glHead}
<style>
  html,body{height:100%;margin:0;background:#080601;font:12px ui-monospace,"SF Mono",Menlo,monospace;color:#ffb833}
  #m{position:absolute;inset:0}
  #note{position:absolute;z-index:1000;left:10px;bottom:10px;max-width:min(560px,calc(100% - 40px));background:rgba(8,6,2,0.94);
    border:1px solid rgba(255,160,0,0.30);border-radius:8px;padding:8px 10px;line-height:1.4}
  .pop{font:12px ui-monospace,Menlo,monospace;max-width:320px;white-space:normal}
</style>
<div id="m"></div><div id="note"></div>
<script>
const PINS = ${safe(clean)};
const me = ${safe(me)};
const GL = ${kit ? safe(kit) : 'null'};
const VIBEMAP_SRC = ${safe(VIBEMAP_JS)};
const LEAFLET = ${safe(leaflet)};
const GL_DATA_BASE = ${safe(dataBase)};
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
document.getElementById('note').innerHTML = PINS.length === 1 ? esc(PINS[0].label)
  : PINS.length + ' positions sent in DSC calls — tap a pin for its call.';
let L = null, map = null, vm = null;
var RX_HOUSE = '<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 22 22"><polygon points="11,1 21,10 17,10 17,21 13,21 13,15 9,15 9,21 5,21 5,10 1,10" fill="rgba(255,200,60,0.85)" stroke="rgba(0,0,0,0.5)" stroke-width="1"/></svg>';
const first = PINS[0] || me || { lat: 50, lon: 0 };
function bootGPU() {
  if (!GL || !window.VibeMapGL || !window.VibeMapGLCompat) return false;
  try {
    const load = (path, kind) => fetch(path).then((r) => r.ok ? (kind === 'text' ? r.text() : r.arrayBuffer()) : null).catch(() => null);
    vm = VibeMapGL.createNow(document.getElementById('m'), { style: GL.style, load, base: GL.base, rangeBase: GL.base, detail: GL.detail,
      profile: 'spots', center: [first.lon, first.lat], zoom: (PINS.length === 1 ? 6 : 4) - 1 });
    if (!vm) return false;
    L = VibeMapGLCompat.install(vm);
    vm.ready.catch((e) => console.error('GPU map: the style did not load', e));
    L.control.zoom({ position: 'topleft' }).addTo();
    return true;
  } catch (e) { console.error('GPU map unavailable — using the Leaflet map', e); vm = null; L = null; return false; }
}
function draw() {
  map = L.map('m', { worldCopyJump: true, preferCanvas: true });
  if (!vm) map.setView([first.lat, first.lon], PINS.length === 1 ? 6 : 4);
  if (me) L.marker([me.lat, me.lon], { zIndexOffset: 1000, icon: L.divIcon({ html: RX_HOUSE, className: '', iconSize: [22, 22],
    iconAnchor: [11, 22], popupAnchor: [0, -20] }) }).addTo(map).bindPopup('<div class="pop"><b>RX</b><br>This receiver</div>');
  for (const p of PINS) L.circleMarker([p.lat, p.lon], { radius: 9, color: '#000000aa', weight: 2, fillColor: p.colour, fillOpacity: 0.9 })
    .addTo(map).bindPopup('<div class="pop">' + esc(p.label) + '</div>');
}
function bootLeaflet() {
  if (!LEAFLET) { document.getElementById('note').textContent = 'The map could not be loaded.'; return; }
  const css = document.createElement('link'); css.rel = 'stylesheet'; css.href = LEAFLET.css; css.integrity = LEAFLET.cssSri;
  document.head.appendChild(css);
  const js = document.createElement('script'); js.src = LEAFLET.js; js.integrity = LEAFLET.jsSri;
  js.onload = () => {
    L = window.L;
    if (!window.VibeMap) { const s = document.createElement('script'); s.textContent = VIBEMAP_SRC; document.head.appendChild(s); }
    draw();
    window.VibeMap.attach(map, { dataBase: GL_DATA_BASE, profile: 'spots' });
  };
  js.onerror = () => { document.getElementById('note').textContent = 'The map could not be loaded from this server.'; };
  document.head.appendChild(js);
}
if (bootGPU()) draw(); else bootLeaflet();
${ES}`;
}
