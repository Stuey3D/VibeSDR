/**
 * jtty.ts — the web client's JTTY box (V12, 2026-10-26): the WSJT-X 3.2 keyboard mode, decoded on the SERVER
 * (android/app/src/main/cpp/decoders/jtty_decoder.h — every JTTY signal in the listener's passband at once, no time
 * slots). This file only shows what the server sends (vibe_jtty_run.h):
 *
 *   {"type":"jtty_signals","data":{"signals":[{id,hz,audio,snr,q,active,last,first,frames,call,country,text}]}}
 *   {"type":"jtty_spot","data":{callsign,frequency,audio,snr,how,country,timestamp}}
 *
 * ★★ One ROW PER STATION, LOWEST FREQUENCY FIRST (the band's order, as on the spectrum — Stuart's rule for the CW list):
 *    frequency · call · SNR · confidence · last heard, then its recent messages. The message is coloured by the
 *    decoder's confidence: red poor, orange low, amber medium, green high — the CW list's colours.
 * ★ SPOTS below: every call heard, newest first, one row per call — CQ (calling) or QSO (in an exchange).
 * ★ Tap a row or a spot: tune there (USB, the dial 1500 Hz below the station so its tones sit mid-passband).
 * ★ The box keeps the listener's place while it updates (main.ts pins the SPOTS heading, as CW's does).
 */

import { callHtml, wireQrz } from './qrzLink';

export interface JttySignal {
  id: number; hz: number; audio: number; snr: number; q: 'high' | 'medium' | 'low' | 'poor';
  active: boolean; last: number; first: number; frames: number; call: string; country: string; text: string;
}
export interface JttySpot { callsign: string; frequency: number; audio: number; snr: number; how: string; country: string; timestamp: number }

/** ★ The calling frequencies announced for JTTY in the WSJT-X 3.2 release candidates (USB dial, MHz) — PRELIMINARY,
 *  not band-plan allocations (docs/v12/JTTY.md §4). 20 m is the main meeting point. */
export const JTTY_DIALS: { hz: number; label: string }[] = [
  { hz: 1838000, label: '160m' }, { hz: 3575000, label: '80m' }, { hz: 7090000, label: '40m' },
  { hz: 10140000, label: '30m' }, { hz: 14090000, label: '20m' }, { hz: 18100000, label: '17m' },
  { hz: 21090000, label: '15m' }, { hz: 24920000, label: '12m' }, { hz: 28090000, label: '10m' },
  { hz: 50160000, label: '6m' }, { hz: 144160000, label: '2m' },
];

const QS = new Set(['high', 'medium', 'low', 'poor']);
const str = (v: unknown, cap: number) => (typeof v === 'string' ? v.slice(0, cap) : '');
const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);

/** A jtty_signals frame's list, re-checked field by field (it is from the server, but every client checks what it shows). */
export function jttySignalsFromJson(d: any): JttySignal[] | null {
  if (!d || !Array.isArray(d.signals)) return null;
  const out: JttySignal[] = [];
  for (const s of d.signals.slice(0, 80)) {
    if (!s || typeof s !== 'object') continue;
    out.push({
      id: num(s.id), hz: num(s.hz), audio: num(s.audio), snr: num(s.snr), q: QS.has(s.q) ? s.q : 'poor',
      active: s.active === true, last: num(s.last), first: num(s.first), frames: num(s.frames),
      call: str(s.call, 16), country: str(s.country, 40), text: str(s.text, 600),
    });
  }
  return out;
}
export function jttySpotFromJson(d: any): JttySpot | null {
  if (!d || typeof d.callsign !== 'string' || !d.callsign) return null;
  return { callsign: str(d.callsign, 16), frequency: num(d.frequency), audio: num(d.audio), snr: num(d.snr),
           how: d.how === 'CQ' ? 'CQ' : 'QSO', country: str(d.country, 40), timestamp: num(d.timestamp) || Date.now() };
}

/** The spots heard, newest first, one per call (a call heard again moves to the top with its latest frequency). */
export class JttySpots {
  list: JttySpot[] = [];
  push(s: JttySpot) {
    const i = this.list.findIndex((o) => o.callsign === s.callsign);
    // ★ a call once heard calling CQ stays a CQ spot: an exchange heard after does not demote it
    if (i >= 0) { if (this.list[i].how === 'CQ' && s.how !== 'CQ') s = { ...s, how: 'CQ' }; this.list.splice(i, 1); }
    this.list.unshift(s);
    if (this.list.length > 200) this.list.length = 200;
  }
  clear() { this.list.length = 0; }
}

/** Rows lowest frequency first — RF when the server knows it, else the audio tone. A row keeps its place while it lives. */
export function sortJtty(sigs: JttySignal[]): JttySignal[] {
  return [...sigs].sort((a, b) => (a.hz > 0 && b.hz > 0 ? a.hz - b.hz : a.audio - b.audio) || a.id - b.id);
}

/** Where to tune for a station: USB, the dial 1500 Hz below its tone 0 (its four tones then sit at 1500–1594 Hz). */
export function jttyTuneFor(rfHz: number): number { return Math.round(rfHz - 1500); }

export function jttyFreq(s: { hz: number; audio: number }): string {
  return s.hz > 0 ? (s.hz / 1e3).toFixed(2) : `${Math.round(s.audio)} Hz`;
}
export function jttyAge(ms: number, now = Date.now()): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  return s < 60 ? `${s}s` : s < 3600 ? `${Math.round(s / 60)}m` : `${Math.round(s / 3600)}h`;
}

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}

/** ★ The rows. Text off the air goes in through esc() — TEXT5 carries < > & " freely. */
export function renderJttyRows(host: HTMLElement, sigs: JttySignal[], onTune: (hz: number) => void) {
  host.textContent = '';
  if (!sigs.length) {
    const d = document.createElement('div'); d.className = 'idle';
    d.textContent = 'listening — every JTTY signal in your passband appears here as it is decoded (tune USB; 14.090 MHz is the main meeting point)';
    host.appendChild(d);
    return;
  }
  for (const sg of sortJtty(sigs)) {
    const r = document.createElement('div');
    // ★ q-<confidence> colours the message (#jttyList .jtr.q-low .tx …): red poor, orange low, amber medium, green high.
    r.className = 'jtr q-' + sg.q + (sg.active ? '' : ' gone');
    r.innerHTML =
      `<span class="f">${esc(jttyFreq(sg))}</span>` +
      `<span class="c${sg.call ? '' : ' none'}">${sg.call ? callHtml(sg.call, esc) : '—'}</span>` +
      `<span class="s">${sg.snr >= 0 ? '+' : ''}${Math.round(sg.snr)} dB</span>` +
      `<span class="q ${sg.q}">${sg.q}</span>` +
      `<span class="a">${jttyAge(sg.last)}</span>` +
      `<span class="tx"><bdi>${esc(sg.text.trim())}</bdi></span>`;
    r.title = `${sg.call ? sg.call + (sg.country ? ' · ' + sg.country : '') + ' · ' : ''}${jttyFreq(sg)}${sg.hz > 0 ? ' kHz' : ''} · `
            + `SNR ${Math.round(sg.snr)} dB (2500 Hz) · ${sg.frames} frame${sg.frames === 1 ? '' : 's'} · ${sg.q} confidence · `
            + `${sg.active ? 'heard' : 'last heard'} ${jttyAge(sg.last)} ago — tap to tune`;
    wireQrz(r);   // ★ V12: the callsign opens its QRZ.COM page — its own click, not the row's tune
    r.onclick = () => onTune(sg.hz);
    host.appendChild(r);
  }
}

export function renderJttySpots(host: HTMLElement, spots: JttySpot[], fmtTime: (ms: number) => string, onTune: (hz: number) => void) {
  host.textContent = '';
  if (!spots.length) {
    const d = document.createElement('div'); d.className = 'idle';
    d.textContent = 'every callsign heard — calling CQ or in an exchange — is spotted here';
    host.appendChild(d);
    return;
  }
  for (const s of spots.slice(0, 80)) {
    const r = document.createElement('div');
    r.className = 'jts' + (s.how === 'CQ' ? ' cq' : '');
    r.innerHTML = `<span class="t">${esc(fmtTime(s.timestamp))}</span>` +
      `<span class="f">${esc(s.frequency > 0 ? (s.frequency / 1e3).toFixed(2) : `${Math.round(s.audio)} Hz`)}</span>` +
      `<span class="c">${callHtml(s.callsign, esc)}</span>` +
      `<span class="h">${esc(s.how)}</span>` +
      `<span class="ct">${esc(s.country)}</span>`;
    r.title = `${s.callsign} · ${s.how === 'CQ' ? 'calling CQ' : 'in an exchange'} · ${Math.round(s.snr)} dB — tap to tune`;
    wireQrz(r);
    r.onclick = () => onTune(s.frequency);
    host.appendChild(r);
  }
}
