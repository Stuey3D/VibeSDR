// drm.ts — V12 DRM: the box a listener reads while their mode is DRM (docs/v12/DRM.md). The server does all the
// receiving (vibe_drm_listener.h) and reports {"type":"drm"} about once per 1.2 s super frame on the spectrum socket;
// this only DRAWS it.
//
// ★★ READABLE, NOT A DUMP (the brief): the station first (its label, its text, language/country/programme), then one
//    plain line of what the signal is ("Robustness B · 10 kHz · 16-QAM · rate 0.62 · long interleave"), the SNR as a
//    number, six sync lights in the order a receiver gains them (time → frequency → frame → FAC → SDC → MSC), and the
//    codec. When the server cannot play the codec, its own sentence says why — prominently, because silence with no
//    reason reads as a broken receiver (AGENTS.md: the user concludes the FEATURE is broken).

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

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c] as string));

/** ISO 3166 alpha-2 → a regional-indicator flag ("ro" → 🇷🇴). */
function flagOf(cc?: string): string {
  if (!cc || !/^[a-z]{2}$/i.test(cc)) return '';
  const u = cc.toUpperCase();
  return String.fromCodePoint(0x1f1e6 + u.charCodeAt(0) - 65, 0x1f1e6 + u.charCodeAt(1) - 65);
}

/** The lights, in the order a receiver gains them. */
const LIGHTS: Array<[keyof DrmState, string, string]> = [
  ['sync', 'Time', 'Symbol timing and robustness mode found (guard-interval correlation)'],
  ['freq', 'Freq', 'Carrier frequency found (the three reference tones)'],
  ['frame', 'Frame', 'Transmission frame found (the time and gain references)'],
  ['fac', 'FAC', 'Fast Access Channel decoded (CRC good) — the signal\'s parameters'],
  ['sdc', 'SDC', 'Service Description Channel decoded (CRC good) — label, codec, multiplex'],
  ['msc', 'MSC', 'Main Service Channel decoded — the programme itself'],
];

/** Fill `host` with the box for `s`. Returns false when there is nothing to show yet. */
export function drmRender(host: HTMLElement, s: DrmState | null): boolean {
  if (!s) { host.innerHTML = '<div class="drmWait">Searching for a DRM signal…</div>'; return true; }
  const lights = LIGHTS.map(([k, name, tip]) =>
    `<span class="drmLight${s[k] ? ' on' : ''}" title="${esc(tip)}">${name}</span>`).join('');
  const parts: string[] = [];
  if (!s.sync) {
    host.innerHTML = `<div class="drmLights">${lights}</div><div class="drmWait">Searching for a DRM signal on this frequency…</div>`;
    return true;
  }
  const label = s.label || (s.locked ? 'DRM station' : 'Locking…');
  const meta = [s.programme, s.language || (s.lang3 ? s.lang3.toUpperCase() : ''), s.country ? `${flagOf(s.country)} ${s.country.toUpperCase()}` : '']
    .filter(Boolean).map((x) => esc(String(x))).join(' · ');
  parts.push(`<div class="drmHead"><span class="drmLabel">${esc(label)}</span>${s.serviceId ? `<span class="drmSid" title="Service identifier">${esc(s.serviceId)}</span>` : ''}</div>`);
  if (meta) parts.push(`<div class="drmMeta">${meta}</div>`);
  if (s.text) parts.push(`<div class="drmText">${esc(s.text)}</div>`);
  parts.push(`<div class="drmLights">${lights}</div>`);
  if (s.locked) {
    const sig = [`Robustness ${esc(s.robustness || '?')}`, s.bwKhz ? `${s.bwKhz} kHz` : '', s.mscQam ? `${s.mscQam}-QAM` : '',
                 s.codeRate ? `rate ${s.codeRate.toFixed(2)}` : '', s.interleave ? `${esc(s.interleave)} interleave` : '']
      .filter(Boolean).join(' · ');
    const snr = typeof s.snr === 'number' ? s.snr : NaN;
    const q = snr >= 15 ? 'good' : snr >= 9 ? 'fair' : 'poor';
    parts.push(`<div class="drmSig"><span class="drmSnr ${q}" title="Decision SNR of the FAC cells (MER)">SNR ${isFinite(snr) ? snr.toFixed(1) : '—'} dB</span>`
             + `<span class="drmSigTxt">${sig}</span></div>`);
    if (s.codec) {
      const rate = s.audioRate ? `${(s.audioRate / 1000).toString()} kHz` : '';
      const br = s.bitrate ? `${(s.bitrate / 1000).toFixed(1)} kbit/s` : '';
      const codec = [esc(s.codec), esc(s.audioMode || ''), rate, br].filter(Boolean).join(' · ');
      parts.push(`<div class="drmCodec"><span class="drmLight${s.playable ? ' on' : ' off'}" title="${s.playable ? 'Playing' : 'Not playable here'}">Audio</span> ${codec}`
               + `${s.decoder ? ` <span class="drmDec">via ${esc(s.decoder)}</span>` : ''}</div>`);
    }
    if (!s.playable && s.why) parts.push(`<div class="drmWhy" role="status">${esc(s.why)}</div>`);
    const small = [typeof s.offsetHz === 'number' ? `offset ${s.offsetHz >= 0 ? '+' : ''}${s.offsetHz.toFixed(1)} Hz` : '',
                   s.utc ? `station clock ${esc(s.utc)} UTC` : '',
                   typeof s.facOk === 'number' ? `FAC ${s.facOk}/${(s.facOk || 0) + (s.facBad || 0)}` : ''].filter(Boolean).join(' · ');
    if (small) parts.push(`<div class="drmSmall">${small}</div>`);
  } else {
    parts.push('<div class="drmWait">A DRM signal is here — reading its parameters…</div>');
  }
  host.innerHTML = parts.join('');
  return true;
}
