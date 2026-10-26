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
  // ── ★★ V12 the Signal pane (2026-10-26): new, optional — an older server sends none of these (vibe_drm_report.h) ──
  so?: number; codeRateA?: number; sdcRate?: number; sdcMer?: number; delayMs?: number; dopplerHz?: number;
  sbr?: boolean; ps?: boolean; concealed?: number; underruns?: number; sdcOk?: number; sdcBad?: number; superFrames?: number;
  lockS?: number[]; reacq?: number; lostWhy?: string; lostAtS?: number; blankedPct?: number; iqGaps?: number;
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

// ── ★★★ V12 THE SIGNAL PANE (Stuart, 2026-10-26: "yes that is what we want") ─────────────────────────────────────────
// DAB+'s advanced pane for DRM: the same layout (amber label, white value, grouped under small headings), the same traffic
// lights where a value has a nominal range, and a plain-English tooltip on every row. The compact box above stays the
// default; this is its second view (main.ts drmSetPane), reached by the SIGNAL key in the box's header.
// ★★ ONE RULE, TWO READERS: the rows are DATA (drmSignalRows), so the app's DrmPanel draws the very same rows — its copy in
//    src/services/drm.ts is held equal to this one by scripts/test_drm_app.ts.
// ★ A row an older server cannot fill is not drawn at all (no dead rows): the pane's new rows need its `reacq` counter.

export type DrmTone = 'ok' | 'warn' | 'bad';
export type DrmSigRow = { h: string } | { k: string; v: string; tone?: DrmTone; tip: string };

/** Each robustness mode's guard interval, ms (Tg at 24 kHz: 64, 128, 128, 176 samples — ES 201 980 table 82). */
const DRM_GUARD_MS: Record<string, number> = { A: 2.67, B: 5.33, C: 5.33, D: 7.33 };
const DRM_MODE_FOR: Record<string, string> = {
  A: 'ground wave and near sky wave', B: 'sky wave — the usual HF mode', C: 'sky wave, harder conditions', D: 'severe delay and Doppler',
};
/** ★ About this MER (dB) or better decodes cleanly at each QAM size with DRM's usual coding — measured, not theory: RRI's
 *  16-QAM played without a bad frame at an MSC MER of 7–9 dB (the decided-symbol MER reads low on a noisy signal). */
const DRM_MER_NEED: Record<number, number> = { 4: 4, 16: 8, 64: 14 };
const DRM_LOSS: Record<string, string> = { fac: 'the FAC failed 16 frames running', frame: 'frame sync lost', freq: 'the frequency was lost' };

const fmt1 = (x: number) => x.toFixed(1);
/** Errors, not passes ("it's easier to read 5 % errors rather than 95 % pass" — Stuart, for DAB). */
function errRow(k: string, bad: number | undefined, good: number | undefined, tip: string): DrmSigRow {
  const b = bad ?? 0, n = b + (good ?? 0);
  if (!n) return { k, v: '—', tip };
  const r = b / n;
  return { k, v: `${b} bad of ${n} (${(r * 100).toFixed(r && r < 0.1 ? 1 : 0)} %)`, tone: r < 0.02 ? 'ok' : r < 0.1 ? 'warn' : 'bad', tip };
}
function merRow(k: string, mer: number | undefined, qam: number | undefined, tip: string): DrmSigRow {
  if (typeof mer !== 'number' || !mer) return { k, v: '—', tip };
  const need = DRM_MER_NEED[qam ?? 16] ?? 12;
  return { k, v: `${fmt1(mer)} dB`, tone: mer >= need ? 'ok' : mer >= need - 3 ? 'warn' : 'bad', tip };
}

/** ★ The Signal pane for `s`, as rows — the web's drmSignalRender and the app's DrmPanel both draw exactly these. */
export function drmSignalRows(s: DrmState | null): DrmSigRow[] {
  const rows: DrmSigRow[] = [];
  const isNew = !!s && typeof s.reacq === 'number';
  rows.push({ h: 'LOCK' });
  rows.push({ k: 'Lock', v: !s || !s.sync ? 'searching' : s.locked ? 'locked' : 'reading the parameters',
              tone: !s || !s.sync ? 'bad' : s.locked ? 'ok' : 'warn',
              tip: 'Locked = two FACs in a row agree: a DRM signal is certainly here and its parameters are known.' });
  if (isNew && s) {
    const t = s.lockS ?? [];
    LIGHTS.forEach(([, name, tip], i) => {
      const v = t[i];
      rows.push({ k: `${name} after`, v: typeof v === 'number' && v >= 0 ? `${fmt1(v)} s` : '—', tone: typeof v === 'number' && v >= 0 ? 'ok' : undefined,
                  tip: `${tip}. Seconds of signal from the start of this lock (the last loss, or tuning in).` });
    });
    const n = s.reacq ?? 0;
    rows.push({ k: 'Re-acquisitions', v: `${n}${n && s.lostWhy ? ` · last: ${DRM_LOSS[s.lostWhy] ?? s.lostWhy}` : ''}`,
                tone: !n ? 'ok' : n < 3 ? 'warn' : 'bad',
                tip: 'Times the lock was lost and searched for again since you chose DRM — fades, interference or a break in the signal.' });
  }
  if (!s || !s.locked) {
    if (s && typeof s.dropped === 'number') rows.push({ k: 'IQ dropped', v: String(s.dropped), tone: s.dropped ? 'warn' : 'ok',
      tip: 'Blocks of radio samples the DRM receiver had to skip because the server fell behind. Should be 0.' });
    return rows;
  }
  const qam = (x?: number) => (x ? `${x}-QAM` : '—');
  const mode = s.robustness || '?';
  const guard = DRM_GUARD_MS[mode];
  rows.push({ h: 'SIGNAL' });
  rows.push({ k: 'Robustness', v: `${mode}${DRM_MODE_FOR[mode] ? ` — ${DRM_MODE_FOR[mode]}` : ''}`,
              tip: 'The transmission mode: how long a guard interval each symbol carries and how dense the pilots are. A, B, C, D are ever more robust against echoes and fading, at the cost of capacity.' });
  rows.push({ k: 'Bandwidth', v: s.bwKhz ? `${s.bwKhz} kHz${typeof s.so === 'number' ? ` (occupancy ${s.so})` : ''}` : '—',
              tip: 'How wide the DRM signal is. DRM on short wave is normally 10 kHz (spectrum occupancy 3).' });
  if (guard) rows.push({ k: 'Guard interval', v: `${guard.toFixed(2)} ms`,
              tip: 'The part of each symbol that soaks up echoes. Echoes arriving within it cause no harm; the delay spread below is measured against it.' });
  rows.push({ k: 'Main channel', v: `${qam(s.mscQam)}${s.codeRate ? ` · rate ${s.codeRate.toFixed(2)}` : ''}${s.codeRateA ? ` (protected part ${s.codeRateA.toFixed(2)})` : ''}`,
              tip: 'The MSC, which carries the programme: its constellation and code rate. Bigger QAM and higher rate = more bits, but it needs a cleaner signal.' });
  rows.push({ k: 'Service channel', v: `${qam(s.sdcQam)}${s.sdcRate ? ` · rate ${s.sdcRate.toFixed(2)}` : ''}`,
              tip: 'The SDC, which carries the station name, codec and multiplex description, once per 1.2 s super frame.' });
  rows.push({ k: 'Interleaving', v: s.interleave === 'long' ? 'long (2 s)' : s.interleave === 'short' ? 'short (0.4 s)' : '—',
              tip: 'How far the programme\'s bits are spread in time. Long interleaving rides out longer fades, but takes 2 s more to start.' });
  rows.push({ h: 'QUALITY' });
  {
    const snr = s.snr;
    rows.push({ k: 'SNR', v: typeof snr === 'number' ? `${fmt1(snr)} dB` : '—', tone: typeof snr !== 'number' ? undefined : snr >= 15 ? 'ok' : snr >= 9 ? 'warn' : 'bad',
                tip: 'Signal-to-noise as the receiver sees it: the modulation error ratio (MER) of the FAC cells, after equalisation.' });
  }
  rows.push(merRow('MSC MER', s.mscMer, s.mscQam, 'Modulation error ratio of the programme\'s cells. With DRM\'s usual coding, about 8 dB decodes 16-QAM cleanly and 14 dB 64-QAM.'));
  if (isNew) rows.push(merRow('SDC MER', s.sdcMer, s.sdcQam, 'Modulation error ratio of the service channel\'s cells.'));
  rows.push({ k: 'Frequency offset', v: typeof s.offsetHz === 'number' ? `${s.offsetHz >= 0 ? '+' : ''}${fmt1(s.offsetHz)} Hz` : '—',
              tip: 'How far the signal sits from the dial. The receiver corrects up to ±1.5 kHz by itself.' });
  rows.push({ k: 'Sample clock', v: typeof s.clockPpm === 'number' ? `${s.clockPpm >= 0 ? '+' : ''}${fmt1(s.clockPpm)} ppm` : '—',
              tip: 'How fast the radio\'s clock runs against the transmitter\'s, in parts per million. The receiver follows it.' });
  if (isNew) {
    const d = s.delayMs;
    rows.push({ k: 'Delay spread', v: typeof d === 'number' && d >= 0 ? `${d.toFixed(2)} ms${guard ? ` of ${guard.toFixed(2)} ms guard` : ''}` : 'measuring…',
                tone: typeof d !== 'number' || d < 0 || !guard ? undefined : d < 0.5 * guard ? 'ok' : d < 0.9 * guard ? 'warn' : 'bad',
                tip: 'How far apart in time the signal\'s echoes (the paths through the ionosphere) arrive: the window holding 95 % of their energy. Within the guard interval they do no harm.' });
    const f = s.dopplerHz;
    rows.push({ k: 'Doppler spread', v: typeof f === 'number' && f >= 0 ? (f < 0.1 ? 'under 0.1 Hz' : `${f.toFixed(2)} Hz`) : 'measuring…',
                tone: typeof f !== 'number' || f < 0 ? undefined : f < 1 ? 'ok' : f < 2 ? 'warn' : 'bad',
                tip: 'How fast the signal fades: the spread of frequencies the moving ionosphere adds. Below 1 Hz is calm; at 2 Hz and above the channel changes faster than the receiver can follow.' });
    if (typeof s.blankedPct === 'number' && s.blankedPct > 0)
      rows.push({ k: 'Pulse blanker', v: `${s.blankedPct.toFixed(s.blankedPct < 1 ? 2 : 1)} % blanked`, tone: s.blankedPct < 1 ? 'ok' : s.blankedPct < 5 ? 'warn' : 'bad',
                  tip: 'Share of the samples zeroed in the last few seconds because a pulse (radar, sparks) stood far above the signal. A few per cent costs little; much more and the signal is drowned.' });
  }
  rows.push({ h: 'ERRORS' });
  rows.push(errRow('FAC', s.facBad, s.facOk, 'Fast Access Channel blocks (one per 400 ms frame) that failed their CRC.'));
  if (isNew) rows.push(errRow('SDC', s.sdcBad, s.sdcOk, 'Service Description Channel blocks (one per 1.2 s super frame) that failed their CRC.'));
  if (typeof s.audioUnits === 'number') rows.push(errRow('Audio frames', s.audioBad, s.audioUnits, 'Audio frames that arrived damaged (their CRC failed) and were not played.'));
  if (typeof s.concealed === 'number') rows.push({ k: 'Concealed', v: String(s.concealed), tone: s.concealed ? 'warn' : 'ok',
    tip: 'Frames the audio decoder could not decode and covered over, so there is no click or gap.' });
  if (isNew && typeof s.iqGaps === 'number') rows.push({ k: 'IQ gaps bridged', v: String(s.iqGaps), tone: 'warn',
    tip: 'Moments the server fell behind and lost a few milliseconds of radio samples before the DRM receiver; each was filled with silence so the lock held.' });
  if (isNew && typeof s.underruns === 'number') rows.push({ k: 'Audio gaps', v: String(s.underruns), tone: s.underruns ? 'warn' : 'ok',
    tip: 'Times the audio ran dry and the server sent silence while waiting for the next super frame.' });
  if (typeof s.dropped === 'number') rows.push({ k: 'IQ dropped', v: String(s.dropped), tone: s.dropped ? 'warn' : 'ok',
    tip: 'Blocks of radio samples the DRM receiver had to skip because the server fell behind. Should be 0.' });
  rows.push({ h: 'AUDIO' });
  rows.push({ k: 'Codec', v: s.codec || '—', tip: 'The audio coding: AAC (DRM\'s own, most stations), HE-AAC (with SBR) or xHE-AAC (the newer, more efficient one).' });
  if (typeof s.sbr === 'boolean') rows.push({ k: 'SBR', v: s.sbr ? 'on' : 'off', tip: 'Spectral band replication: the top of the audio rebuilt from a little side information, so a low bit rate still sounds bright.' });
  if (typeof s.ps === 'boolean') rows.push({ k: 'Parametric stereo', v: s.ps ? 'on' : 'off', tip: 'Stereo sent as mono plus a few bits describing the image — stereo at almost mono\'s bit rate.' });
  rows.push({ k: 'Audio mode', v: s.audioMode || '—', tip: 'Mono or stereo, as the station sends it.' });
  rows.push({ k: 'Sample rate', v: s.audioRate ? `${s.audioRate / 1000} kHz` : '—', tip: 'The codec\'s sampling rate. 12 kHz gives audio up to about 6 kHz (AAC without SBR).' });
  rows.push({ k: 'Bit rate', v: s.bitrate ? `${(s.bitrate / 1000).toFixed(1)} kbit/s` : '—', tip: 'The programme\'s share of the multiplex.' });
  if (s.codec) rows.push({ k: 'Decoder', v: s.playable ? (s.decoder || 'playing') : 'not playable on this server', tone: s.playable ? 'ok' : 'bad',
              tip: s.playable ? 'What decodes the audio on the server.' : (s.why || 'This server has no decoder for this codec.') });
  rows.push({ h: 'STATION' });
  rows.push({ k: 'Label', v: s.label || '—', tip: 'The station\'s name, from the SDC.' });
  rows.push({ k: 'Service id', v: s.serviceId || '—', tip: 'The service\'s 24-bit identifier, from the FAC.' });
  rows.push({ k: 'Programme type', v: s.programme || '—', tip: 'What the station says it is broadcasting.' });
  rows.push({ k: 'Language', v: s.language || (s.lang3 ? s.lang3.toUpperCase() : '—'), tip: 'The programme\'s language.' });
  rows.push({ k: 'Country', v: s.country ? `${flagOf(s.country)} ${s.country.toUpperCase()}` : '—', tip: 'The station\'s country, from the SDC.' });
  rows.push({ k: 'Station clock', v: s.utc ? `${s.utc} UTC` : '—', tip: 'The time the transmitter sends with its signal.' });
  rows.push({ k: 'Text', v: s.text || '—', tip: 'The station\'s text message.' });
  return rows;
}

/** ★ Draw drmSignalRows into `host`. Same skeleton (same headings and labels) → only the values that changed are
 *  rewritten, so a row being read under the pointer keeps its tooltip and the pane does not jump (DAB's dabPatchRows). */
export function drmSignalRender(host: HTMLElement, s: DrmState | null): void {
  const rows = drmSignalRows(s);
  const sig = rows.map((r) => ('h' in r ? `#${r.h}` : r.k)).join('\u0001');
  const val = (r: { v: string; tone?: DrmTone }) => (r.tone ? `<span class="tl ${r.tone}">${esc(r.v)}</span>` : esc(r.v));
  if (host.dataset.sig !== sig) {
    host.dataset.sig = sig;
    host.innerHTML = rows.map((r) => ('h' in r ? `<h4>${esc(r.h)}</h4>`
      : `<div class="row" title="${esc(r.tip)}"><span>${esc(r.k)}</span><span>${val(r)}</span></div>`)).join('');
    return;
  }
  const els = host.querySelectorAll('.row') as NodeListOf<HTMLElement>;
  let i = 0;
  for (const r of rows) {
    if ('h' in r) continue;
    const el = els[i++]; if (!el) break;
    const html = val(r);
    const v = el.lastElementChild as HTMLElement;
    if (v.dataset.src !== html) { v.dataset.src = html; v.innerHTML = html; }
    if (el.title !== r.tip) el.title = r.tip;
  }
}
