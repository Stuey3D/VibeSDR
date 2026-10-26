/* drm.ts — V12 DRM in the app (docs/v12/DRM.md): the box a listener reads while their mode is DRM. The server does all
 * the receiving (vibe_drm_listener.h) and reports {"type":"drm"} about once per 1.2 s super frame on the spectrum
 * socket; the app only DRAWS it (src/components/DrmPanel.tsx).
 *
 * ★★ ONE RULE, TWO READERS: the web client's DRM box (web/client/src/drm.ts drmRender) is the reference. This is the same
 *    box as data — what it says, in the same order, in the same words — and scripts/test_drm_app.ts runs both on the same
 *    reports and holds the text equal.
 * ★★ READABLE, NOT A DUMP: the station first (its label, its text, language/country/programme), then one plain line of
 *    what the signal is, the SNR as a number, six sync lights in the order a receiver gains them, and the codec — with the
 *    server's own sentence, prominently, when it cannot play it (silence with no reason reads as a broken receiver).
 */

export interface DrmState {
  type: 'drm';
  sync?: boolean; freq?: boolean; frame?: boolean; fac?: boolean; sdc?: boolean; msc?: boolean; locked?: boolean;
  robustness?: string; bwKhz?: number; mscQam?: number; sdcQam?: number; interleave?: string; codeRate?: number;
  snr?: number; mscMer?: number; offsetHz?: number; clockPpm?: number;
  serviceId?: string; label?: string; language?: string; lang3?: string; country?: string; programme?: string;
  text?: string; utc?: string;
  codec?: string; audioMode?: string; audioRate?: number; bitrate?: number; playable?: boolean; why?: string; decoder?: string;
  audioUnits?: number; audioBad?: number; facOk?: number; facBad?: number; dropped?: number;
}

/** The report off the wire, checked field by field (the server's, but every client checks what it shows). null = not one. */
export function drmSanitise(m: any): DrmState | null {
  if (!m || typeof m !== 'object') return null;
  const b = (v: unknown) => (v === true ? true : undefined);
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  const s = (v: unknown, cap: number) => (typeof v === 'string' && v ? v.replace(/[\u0000-\u001f\u007f]/g, ' ').slice(0, cap) : undefined);
  const out: DrmState = {
    type: 'drm',
    sync: b(m.sync), freq: b(m.freq), frame: b(m.frame), fac: b(m.fac), sdc: b(m.sdc), msc: b(m.msc), locked: b(m.locked),
    robustness: s(m.robustness, 4), bwKhz: n(m.bwKhz), mscQam: n(m.mscQam), sdcQam: n(m.sdcQam), interleave: s(m.interleave, 12),
    codeRate: n(m.codeRate), snr: n(m.snr), mscMer: n(m.mscMer), offsetHz: n(m.offsetHz), clockPpm: n(m.clockPpm),
    serviceId: s(m.serviceId, 8), label: s(m.label, 64), language: s(m.language, 40), lang3: s(m.lang3, 3),
    country: s(m.country, 2), programme: s(m.programme, 40), text: s(m.text, 400), utc: s(m.utc, 24),
    codec: s(m.codec, 24), audioMode: s(m.audioMode, 24), audioRate: n(m.audioRate), bitrate: n(m.bitrate),
    playable: b(m.playable), why: s(m.why, 300), decoder: s(m.decoder, 24),
    audioUnits: n(m.audioUnits), audioBad: n(m.audioBad), facOk: n(m.facOk), facBad: n(m.facBad), dropped: n(m.dropped),
  };
  for (const k of Object.keys(out) as Array<keyof DrmState>) if (out[k] === undefined) delete out[k];
  return out;
}

/** ISO 3166 alpha-2 → a regional-indicator flag ("ro" → 🇷🇴). */
export function drmFlag(cc?: string): string {
  if (!cc || !/^[a-z]{2}$/i.test(cc)) return '';
  const u = cc.toUpperCase();
  return String.fromCodePoint(0x1f1e6 + u.charCodeAt(0) - 65, 0x1f1e6 + u.charCodeAt(1) - 65);
}

/** The lights, in the order a receiver gains them — the web's LIGHTS. */
export const DRM_LIGHTS: ReadonlyArray<[keyof DrmState, string, string]> = [
  ['sync', 'Time', 'Symbol timing and robustness mode found (guard-interval correlation)'],
  ['freq', 'Freq', 'Carrier frequency found (the three reference tones)'],
  ['frame', 'Frame', 'Transmission frame found (the time and gain references)'],
  ['fac', 'FAC', 'Fast Access Channel decoded (CRC good) — the signal\'s parameters'],
  ['sdc', 'SDC', 'Service Description Channel decoded (CRC good) — label, codec, multiplex'],
  ['msc', 'MSC', 'Main Service Channel decoded — the programme itself'],
];

export type SnrQ = 'good' | 'fair' | 'poor';
export interface DrmView {
  /** Nothing locked yet: the lights (when timing is found) and this line only. */
  wait?: string;
  lights: Array<{ name: string; on: boolean; tip: string }> | null;
  label?: string; sid?: string; meta?: string; text?: string;
  sig?: { snr: string; q: SnrQ; line: string };
  codec?: { playable: boolean; line: string; via: string };
  why?: string;
  small?: string;
}

/** ★ The box for `s` — the web's drmRender, as data, word for word. */
export function drmView(s: DrmState | null): DrmView {
  if (!s) return { wait: 'Searching for a DRM signal…', lights: null };
  const lights = DRM_LIGHTS.map(([k, name, tip]) => ({ name, on: !!s[k], tip }));
  if (!s.sync) return { lights, wait: 'Searching for a DRM signal on this frequency…' };
  const v: DrmView = { lights };
  v.label = s.label || (s.locked ? 'DRM station' : 'Locking…');
  if (s.serviceId) v.sid = s.serviceId;
  const meta = [s.programme, s.language || (s.lang3 ? s.lang3.toUpperCase() : ''), s.country ? `${drmFlag(s.country)} ${s.country.toUpperCase()}` : '']
    .filter(Boolean).map((x) => String(x)).join(' · ');
  if (meta) v.meta = meta;
  if (s.text) v.text = s.text;
  if (s.locked) {
    const sig = [`Robustness ${s.robustness || '?'}`, s.bwKhz ? `${s.bwKhz} kHz` : '', s.mscQam ? `${s.mscQam}-QAM` : '',
                 s.codeRate ? `rate ${s.codeRate.toFixed(2)}` : '', s.interleave ? `${s.interleave} interleave` : '']
      .filter(Boolean).join(' · ');
    const snr = typeof s.snr === 'number' ? s.snr : NaN;
    const q: SnrQ = snr >= 15 ? 'good' : snr >= 9 ? 'fair' : 'poor';
    v.sig = { snr: `SNR ${isFinite(snr) ? snr.toFixed(1) : '—'} dB`, q, line: sig };
    if (s.codec) {
      const rate = s.audioRate ? `${(s.audioRate / 1000).toString()} kHz` : '';
      const br = s.bitrate ? `${(s.bitrate / 1000).toFixed(1)} kbit/s` : '';
      v.codec = { playable: !!s.playable, line: [s.codec, s.audioMode || '', rate, br].filter(Boolean).join(' · '),
                  via: s.decoder ? `via ${s.decoder}` : '' };
    }
    if (!s.playable && s.why) v.why = s.why;
    const small = [typeof s.offsetHz === 'number' ? `offset ${s.offsetHz >= 0 ? '+' : ''}${s.offsetHz.toFixed(1)} Hz` : '',
                   s.utc ? `station clock ${s.utc} UTC` : '',
                   typeof s.facOk === 'number' ? `FAC ${s.facOk}/${(s.facOk || 0) + (s.facBad || 0)}` : ''].filter(Boolean).join(' · ');
    if (small) v.small = small;
  } else {
    v.wait = 'A DRM signal is here — reading its parameters…';
  }
  return v;
}

/** The box's text, in reading order — what VoiceOver says and what the test compares with the web's rendered text. */
export function drmViewText(v: DrmView): string[] {
  const out: string[] = [];
  if (v.label) out.push(v.label + (v.sid ? ` ${v.sid}` : ''));
  if (v.meta) out.push(v.meta);
  if (v.text) out.push(v.text);
  if (v.lights) out.push(v.lights.map((l) => l.name).join(''));
  if (v.sig) out.push(`${v.sig.snr}${v.sig.line}`);
  if (v.codec) out.push(`Audio ${v.codec.line}${v.codec.via ? ` ${v.codec.via}` : ''}`);
  if (v.why) out.push(v.why);
  if (v.small) out.push(v.small);
  if (v.wait) out.push(v.wait);
  return out;
}

/** The SNR's colour by quality (the web's .drmSnr.good/.fair/.poor) and a light's when on. */
export const DRM_SNR_COLOURS: Record<SnrQ, string> = { good: '#5BE36B', fair: '#E8C547', poor: '#FF4B4B' };
export const DRM_LIGHT_OFF = '#FF4B4B';   // ★ the Audio light when the server cannot play the codec (the web's .drmLight.off)
export const DRM_LIGHT_ON = '#5BE36B';

/** ★ Does this server offer DRM — its vibeserver.json `modes` lists "drm" (an older server has no such mode: no key). */
export function drmOffered(modes: readonly string[] | undefined): boolean {
  return !!modes && modes.includes('drm');
}
