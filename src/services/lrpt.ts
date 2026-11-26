// lrpt.ts — METEOR-M LRPT weather-satellite pictures (V12, 2026-11-19; docs/v12/LRPT.md): what the app and the web page
// both need to start the decoder and read its status. The picture itself comes as the SSTV frames (0x07 start · 0x01
// line · 0x05 complete), a half-size colour preview growing during the pass; the full-size PNGs go to the gallery.

import { parseLrptGaps, type LrptGap } from '../utils/lrptGaps';

/** {"type":"lrpt_status","data":{…}} once a second (vibe_lrpt_run.h Run::status). */
export interface LrptStatus {
  carrier: boolean;      // the demodulator is locked to the satellite's carrier
  sync: boolean;         // frames are being found
  mode: string;          // "QPSK" | "OQPSK" (AUTO alternates until frames decode)
  diff: number;          // -1 not yet known, 0 plain, 1 differentially coded
  symbolRate: number;
  lock: number;          // constellation lock, 0–100 (≈ 64 on noise) — a developer figure, not shown
  /** ★★ beta91: the signal over the noise, dB — (S+N)/N from the demodulator's own Es/N0, what the waterfall shows over
   *  its floor at the top of the signal; ≈ 0 on noise. undefined from an older server. */
  snrDb?: number;
  needDb?: number;       // what it takes to decode (≈ +4 dB since lrpt-sens 2026-11-24; +5 before)
  saved: number;         // passes filed to the gallery since the decoder started
  inRange: number;       // the satellite engine: 1 above the horizon, 0 not, −1 no pick
  offsetHz: number;      // the carrier's offset from the dial (the Doppler left over)
  frames: number;        // frames that passed Reed–Solomon
  bad: number;           // frames that did not
  packets: number;
  lines: number;         // picture lines so far (the gaps included — placed by time)
  /** ★ beta95 of those, the lines with picture, and the lines of no signal between them (drawn hatched and labelled
   *  "NO SIGNAL hh:mm-hh:mm" in the picture itself, vibe_lrpt_run.h gapart). undefined from an older server. */
  decoded?: number; gapLines?: number;
  /** ★★★ beta96 the live preview's gaps, merged (vibe_lrpt_run.h gapart::mergeGaps) — the clients draw each as one short
   *  band (src/utils/lrptGaps.ts). [] from an older server or with none. */
  gaps: LrptGap[];
  channels: string[];    // live MSU-MR channels ("1 visible", "2 near-IR", "5 thermal" …)
  why?: string;          // a reason it cannot run here (the IQ slice's rate)
}

export function parseLrptStatus(d: unknown): LrptStatus | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  const n = (k: string) => (typeof o[k] === 'number' && isFinite(o[k] as number) ? (o[k] as number) : 0);
  return {
    carrier: o.carrier === true,
    sync: o.sync === true,
    mode: typeof o.mode === 'string' ? (o.mode as string).slice(0, 8) : '',
    diff: n('diff'),
    symbolRate: n('symbolRate'),
    lock: n('lock'),
    snrDb: typeof o.snrDb === 'number' && isFinite(o.snrDb) ? Math.max(-9.9, Math.min(60, o.snrDb)) : undefined,
    needDb: typeof o.needDb === 'number' && isFinite(o.needDb) ? Math.max(0, Math.min(30, o.needDb)) : undefined,
    saved: Math.max(0, Math.round(n('saved'))),
    inRange: typeof o.inRange === 'number' ? Math.max(-1, Math.min(1, Math.round(o.inRange))) : -1,
    offsetHz: n('offsetHz'),
    frames: Math.max(0, Math.round(n('frames'))),
    bad: Math.max(0, Math.round(n('bad'))),
    packets: Math.max(0, Math.round(n('packets'))),
    lines: Math.max(0, Math.round(n('lines'))),
    decoded: typeof o.decoded === 'number' && isFinite(o.decoded) ? Math.max(0, Math.round(o.decoded)) : undefined,
    gapLines: typeof o.gapLines === 'number' && isFinite(o.gapLines) ? Math.max(0, Math.round(o.gapLines)) : undefined,
    gaps: parseLrptGaps(o.gaps),
    channels: Array.isArray(o.channels) ? (o.channels as unknown[]).filter((c) => typeof c === 'string').map((c) => (c as string).slice(0, 24)).slice(0, 6) : [],
    why: typeof o.why === 'string' && o.why ? (o.why as string).slice(0, 160) : undefined,
  };
}

/** "signal +3.2 dB over the noise (decodes from about +3, best above +4)" — empty from an older server that does not
 *  measure it. ★★ beta95 (Stuart's 11-25 pass on the Sony, vertical + LNA: it locked at about +3.0 and decoded there) —
 *  "needs about +4" was wrong: decoding starts about a dB under the server's figure and is best above it. */
export function lrptSignalText(st: LrptStatus | null): string {
  if (!st || st.snrDb === undefined) return '';
  const v = st.snrDb, need = st.needDb ?? 5;
  return `signal ${v >= 0 ? '+' : '−'}${Math.abs(v).toFixed(1)} dB over the noise (decodes from about +${Math.max(1, need - 1).toFixed(0)}, best above +${need.toFixed(0)})`;
}
/** ★ beta95 "320 lines (336 of no signal)" — decoded lines apart from the gap's; an older server's plain count. */
export function lrptLinesText(st: LrptStatus): string {
  if (st.decoded === undefined || st.gapLines === undefined || st.gapLines <= 0) return `${st.lines} lines`;
  return `${st.decoded} lines decoded · ${st.gapLines} of no signal`;
}

/** The decoder box's status line — and the SATELLITE window's, so it is always the LIVE state (a pass filed earlier is
 *  said as such, never left standing as "picture complete" while the receiver hunts again). */
export function lrptStatusText(st: LrptStatus | null): string {
  if (!st) return 'waiting for a METEOR pass';
  if (st.why) return st.why;
  const sig = lrptSignalText(st);
  if (st.lines > 0) {
    const ch = st.channels.length ? ` · channels ${st.channels.map((c) => c.split(' ')[0]).join(', ')}` : '';
    /* ★★ beta91: a gap in the frames is a gap in the picture, not its end (the server keeps the pass open while the
     *  satellite is up) — so "signal lost" says what the receiver is doing about it. */
    return `${lrptLinesText(st)}${ch} · ${st.frames} frames${st.sync ? '' : ` · between frames, still listening${sig ? ' · ' + sig : ''}`}`;
  }
  const filed = st.saved > 0 ? 'last pass saved to the gallery · ' : '';
  if (st.sync) return `${filed}${st.mode} locked · frames ${st.frames}${st.bad ? ` (${st.bad} unreadable)` : ''}`;
  /* ★★ beta91 (Stuart's Meteor-M2-4 pass, 2026-11-22): the old "lock N % (80 % holds it)" was a constellation figure
   *  that sat at 64 % on noise and never reached 80 on a decoding OQPSK signal. Now the signal over the noise, as the
   *  waterfall shows it at the top of the signal (≈ 0 on noise), and what decoding needs — something a listener can act
   *  on (a better aerial, a higher pass). */
  if (sig) return `${filed}listening for a METEOR satellite · ${sig}`;
  if (st.carrier) return `${filed}carrier found (${st.mode}, ${st.offsetHz >= 0 ? '+' : ''}${st.offsetHz} Hz) · looking for frames`;
  return `${filed}listening for a METEOR satellite`;
}

export const LRPT_HINT =
  'METEOR-M weather satellite pictures (LRPT): pick a METEOR in SATELLITE (TUNES THE RADIO ITSELF) while one passes over — ' +
  'the radio tunes and follows its Doppler by itself (a pass lasts up to 15 minutes; a 137 MHz QFH or V-dipole aerial ' +
  "helps a lot). The picture grows during the pass; each pass's channels and colour composite are saved to the gallery as PNG.";

/** The attach parameters. The symbol rate is the satellite's (72000, or 80000 for the 80k mode) when its entry is known
 *  (the satellite engine, sats.json); without one nothing is sent and the server tries both (AUTO). The rest AUTO. */
export function lrptAttachParams(symbolRate?: number): Record<string, unknown> {
  const p: Record<string, unknown> = { modulation: 'auto', diff: 'auto' };
  if (typeof symbolRate === 'number' && symbolRate > 0) p.symbol_rate = symbolRate >= 76000 ? 80000 : 72000;
  return p;
}
