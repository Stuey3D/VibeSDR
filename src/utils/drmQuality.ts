/**
 * drmQuality — how well a DRM station is RECEIVED, as three bars and a sentence (app + web): DAB's reception meter
 * (dabQuality.ts) for DRM.
 *
 * ★★★ WHY (Stuart, 2026-11-12, Music4Joy in xHE-AAC on air): "the decoder states 17-20db SNR on the server which was
 *     registering a S9+20 signal and on the R36S the main signal meter was reading 42db SNR but it was dropping out more
 *     than the server … This is the same issue we had with the DAB MER not matching the standard signal meter and why we
 *     made a DAB specific signal readout." Both numbers are right and they measure different things:
 *       • the signal meter's SNR is the carrier over the noise floor beside it — strength;
 *       • the decoder's SNR is the FAC cells' MER after equalisation — what is LEFT once the receiver has undone the
 *         echoes and the fading, so multipath, Doppler and phase noise all count against it.
 *     A strong, smeared signal reads 42 dB on the first and 17 on the second, and the second is the one that decides
 *     whether the audio plays. So in DRM, as in DAB, the signal bar's slot carries the decoder's verdict and the box
 *     where SNR / S-units sit carries the decoder's own SNR.
 *
 * ★★ ONE RULE, ONE COPY: the app (SDRScreen → ControlsBar → DabMeter) and the web client (web/client/src/mobile.ts
 *    paintDab) both import this file; handheld-ui/src/drm_quality.h is its port for the R36S's pill, held to the same
 *    cases by scripts/test_drm_quality.ts's table. The SHAPE is DabQuality's, so the very same meter draws it.
 * ★★ NOTHING HERE IS INVENTED. Every input is a field the server already sends in its `drm` report (vibe_drm_report.h)
 *    and the Signal pane already shows (src/services/drm.ts drmSignalRows); the thresholds are that pane's own ok / warn /
 *    bad lines: errors as errRow (2 % / 10 %), MER against the QAM as merRow (DRM_MER_NEED: about 8 dB decodes 16-QAM,
 *    14 dB 64-QAM — measured on RRI, which played without a bad frame at 7–9 dB), the SNR as its row (15 / 9 dB).
 * ★★ AUDIO EVIDENCE OUTRANKS PREDICTION (DAB's rule): once audio frames are arriving, what happened to THEM is the
 *    answer; before, the level is predicted from the FAC's errors and the programme's MER against its QAM. The MER against
 *    the QAM stays a ceiling, as DAB's raw BER does: clean frames say the last few seconds survived, the margin says how
 *    the next few will.
 */
import type { DabLevel, DabQuality } from './dabQuality.ts';
import { HOLD_DOWN, HOLD_UP } from './dabQuality.ts';

/** The report fields the meter reads — a subset of DrmState (src/services/drm.ts, web/client/src/drm.ts). */
export interface DrmQualityReport {
  sync?: boolean; locked?: boolean; msc?: boolean;
  snr?: number; mscMer?: number; mscQam?: number;
  facOk?: number; facBad?: number;
  audioUnits?: number; audioBad?: number;
  codec?: string; playable?: boolean;
}

/** DabQuality's shape (so DabMeter, the web's pill and the R36S's draw it unchanged), plus the sentence that says why
 *  this number is not the signal meter's. */
export interface DrmQuality extends DabQuality {
  /** ★ Always set: the plain words for the tooltip / accessibility hint. */
  explain: string;
}

/** ★★ The plain words (Stuart: a user must understand why it differs from the S-meter). One copy: web, app and R36S. */
export const DRM_SNR_EXPLAIN =
  'Decoded SNR: measured by the DRM decoder after it has undone the echoes and fading, so it counts what spoils the '
  + 'signal as well as the noise. It reads lower than the signal meter’s SNR, which only compares the signal’s '
  + 'strength with the noise beside it — both are right, and this one decides whether the audio plays.';

const LABEL: Record<DabLevel, string> = { 0: 'No DRM signal', 1: 'DRM weak', 2: 'DRM moderate', 3: 'DRM strong' };
const SHORT: Record<DabLevel, string> = { 0: 'No signal', 1: 'Weak', 2: 'Moderate', 3: 'Strong' };
const ADVICE: Record<DabLevel, string> = {
  0: 'Searching for a DRM signal',
  1: 'No or heavily broken audio',
  2: 'Expect occasional audio break-ups',
  3: 'Clear audio',
};
/** Found (timing locked) but not yet decoded — the FAC is being read. Still level 0: nothing plays yet. */
export const DRM_FOUND_LABEL = 'DRM found', DRM_FOUND_ADVICE = 'Reading its parameters';
/** Locked, but this server has no decoder for the station's codec (the box says why, in the server's words). */
export const DRM_UNPLAYABLE_ADVICE = 'This server cannot play its audio';

/** ★ Shown between entering DRM (or retuning) and the first report — never the last station's verdict. */
export const DRM_SEARCHING: DrmQuality = {
  level: 0, label: LABEL[0], short: SHORT[0], advice: ADVICE[0], explain: DRM_SNR_EXPLAIN,
};

/** How much history the meter judges: ~5 reports at the server's one per 1.2 s super frame. */
export const DRM_WINDOW_MS = 6000;

/** ★ The Signal pane's merRow: about this MSC MER (dB) or better decodes cleanly at each QAM size (src/services/drm.ts). */
export const DRM_MER_NEED: Record<number, number> = { 4: 4, 16: 8, 64: 14 };
/** Error rates (FAC blocks, audio frames): the pane's errRow — ok < 2 %, bad ≥ 10 %. Audio as DAB's AUs: 1 % / 8 %. */
export const FAC_BAD_MODERATE = 0.02, FAC_BAD_WEAK = 0.10;
export const AUDIO_BAD_STRONG = 0.01, AUDIO_BAD_WEAK = 0.08;
/** Enough to judge on: ~3 frames of FAC (400 ms each), a handful of audio frames. */
const MIN_FAC = 3, MIN_AUDIO = 5;

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const pct = (f: number) => {
  const p = f * 100;
  return p >= 10 ? `${Math.round(p)}` : p >= 1 ? p.toFixed(1).replace(/\.0$/, '') : p.toFixed(1);
};

/** The window's evidence, oldest sample to newest. Exported for the tests. */
export interface DrmWindow {
  sync: boolean; locked: boolean;
  snr?: number; mscMer?: number; mscQam?: number;
  facIn: number; facBad: number;
  audioIn?: number; audioBad?: number;
  unplayable: boolean;
}

/** ★ The verdict for one window, with no memory — the pure core the meter wraps in hysteresis. */
export function classifyDrmWindow(w: DrmWindow): { level: DabLevel; detail?: string } {
  if (!w.locked) return { level: 0, detail: w.sync ? 'found' : 'searching' };
  const need = DRM_MER_NEED[w.mscQam ?? 16] ?? 12;
  const merDetail = (m: number) => `MSC MER ${m.toFixed(1)} dB${w.mscQam ? ` (${w.mscQam}-QAM needs ${need})` : ''}`;

  // ── Audio evidence: the frames the listener would hear ──
  if (w.audioIn !== undefined && w.audioIn >= MIN_AUDIO) {
    const lost = Math.min(w.audioBad ?? 0, w.audioIn) / w.audioIn;
    let a: { level: DabLevel; detail?: string } =
      { level: lost >= AUDIO_BAD_WEAK ? 1 : lost >= AUDIO_BAD_STRONG ? 2 : 3, detail: lost > 0 ? `${pct(lost)} % audio lost` : undefined };
    // ★ The margin is a ceiling, as DAB's raw BER: clean frames on a programme living below its QAM's need are luck.
    if (a.level === 3 && w.mscMer !== undefined && w.mscMer > 0 && w.mscMer < need) a = { level: 2, detail: merDetail(w.mscMer) };
    return a;
  }

  // ── Prediction: no audio decoding yet (the MSC not reached, or a codec this server cannot play) ──
  let level: DabLevel = 3; let detail: string | undefined;
  const worse = (l: DabLevel, d: string) => { if (l < level) { level = l; detail = d; } };
  if (w.facIn >= MIN_FAC) {
    const e = Math.min(w.facBad, w.facIn) / w.facIn;
    if (e >= FAC_BAD_WEAK) worse(1, `FAC ${pct(e)} % errors`);
    else if (e >= FAC_BAD_MODERATE) worse(2, `FAC ${pct(e)} % errors`);
  }
  if (w.mscMer !== undefined && w.mscMer > 0) {
    if (w.mscMer < need - 3) worse(1, merDetail(w.mscMer));
    else if (w.mscMer < need) worse(2, merDetail(w.mscMer));
  } else if (w.snr !== undefined) {
    // ★ No MSC MER yet: the SNR row's own lines (15 / 9 dB)
    if (w.snr < 9) worse(1, `decoded SNR ${Math.round(w.snr)} dB`);
    else if (w.snr < 15) worse(2, `decoded SNR ${Math.round(w.snr)} dB`);
  }
  return { level, detail };
}

interface Sample { t: number; facOk?: number; facBad?: number; audioUnits?: number; audioBad?: number; snr?: number; mscMer?: number }

/**
 * ★ The meter: feed it every `drm` report; it keeps the window and the hysteresis. One per client.
 *   `reset()` on leaving DRM and on a retune — no verdict may outlive the station (DAB's rule).
 */
export class DrmQualityMeter {
  private samples: Sample[] = [];
  private shown: DabLevel | null = null;
  private pending: { level: DabLevel; n: number } | null = null;
  private last: DrmQuality | null = null;

  reset(): void { this.samples = []; this.shown = null; this.pending = null; this.last = null; }

  /** The current verdict without pushing anything (null before the first report). */
  current(): DrmQuality | null { return this.last; }

  push(r: DrmQualityReport, nowMs: number): DrmQuality {
    const s: Sample = {
      t: nowMs, facOk: num(r.facOk), facBad: num(r.facBad), audioUnits: num(r.audioUnits), audioBad: num(r.audioBad),
      snr: r.locked ? num(r.snr) : undefined, mscMer: r.locked ? num(r.mscMer) : undefined,
    };
    // ★ A counter that went BACKWARDS is a new receiver (a retune, a re-acquisition's codec reset): start again.
    const prev = this.samples[this.samples.length - 1];
    if (prev && (['facOk', 'facBad', 'audioUnits', 'audioBad'] as const)
        .some(k => prev[k] !== undefined && s[k] !== undefined && (s[k] as number) < (prev[k] as number))) {
      this.samples = [];
    }
    // ★ Not locked: nothing of the last lock is evidence about the next.
    if (!r.locked) this.samples = [];
    this.samples.push(s);
    while (this.samples.length > 2 && nowMs - this.samples[1].t >= DRM_WINDOW_MS) this.samples.shift();

    const a = this.samples[0], b = s;
    const d = (k: 'facOk' | 'facBad' | 'audioUnits' | 'audioBad') => {
      const x = a[k], y = b[k];
      if (y === undefined) return 0;
      if (this.samples.length === 1 || x === undefined) return 0;   // ★ session totals are never the evidence
      return Math.max(0, y - x);
    };
    const mean = (k: 'snr' | 'mscMer') => {
      const v = this.samples.map(x => x[k]).filter((x): x is number => x !== undefined && x !== 0);
      return v.length ? v.reduce((p, q) => p + q, 0) / v.length : undefined;
    };
    const facIn = d('facOk') + d('facBad');
    const hasAudio = s.audioUnits !== undefined && this.samples.length > 1;
    const win: DrmWindow = {
      sync: !!r.sync, locked: !!r.locked,
      snr: mean('snr'), mscMer: mean('mscMer'), mscQam: num(r.mscQam),
      facIn, facBad: d('facBad'),
      // ★ audioUnits counts the frames DECODED, audioBad the ones that failed their CRC: together, the frames due
      ...(hasAudio ? { audioIn: d('audioUnits') + d('audioBad'), audioBad: d('audioBad') } : {}),
      unplayable: !!r.locked && !!r.codec && r.playable !== true,
    };
    const v = classifyDrmWindow(win);

    // ── Hysteresis (DAB's: down in 2 reports, up in 3) ──
    if (this.shown === null) this.shown = v.level;
    else if (v.level !== this.shown) {
      this.pending = this.pending && this.pending.level === v.level ? { level: v.level, n: this.pending.n + 1 } : { level: v.level, n: 1 };
      if (this.pending.n >= (v.level < this.shown ? HOLD_DOWN : HOLD_UP)) { this.shown = v.level; this.pending = null; }
    } else this.pending = null;
    // ★ Losing the lock is not "a change to be proved": the bars go out with the signal.
    if (!r.locked) { this.shown = 0; this.pending = null; }

    const lvl = this.shown;
    const found = lvl === 0 && !!r.sync;
    const detail = lvl === v.level ? v.detail : undefined;
    const showDetail = detail && lvl !== 0 ? detail : (lvl !== 0 && win.snr !== undefined ? `decoded SNR ${Math.round(win.snr)} dB` : undefined);
    this.last = {
      level: lvl,
      label: found ? DRM_FOUND_LABEL : LABEL[lvl],
      short: found ? 'Locking' : SHORT[lvl],
      advice: found ? DRM_FOUND_ADVICE : lvl !== 0 && win.unplayable ? DRM_UNPLAYABLE_ADVICE : ADVICE[lvl],
      ...(showDetail ? { detail: showDetail } : {}),
      // ★ the decoder's own SNR (the FAC's MER) — the box's reading, where SNR sits for every other mode
      ...(r.locked && win.snr !== undefined ? { merDb: win.snr } : {}),
      explain: DRM_SNR_EXPLAIN,
    };
    return this.last;
  }
}

/** The one-line text the bar shows: "DRM strong · Clear audio · decoded SNR 18 dB". */
export function drmQualityLine(q: DabQuality, withDetail = true): string {
  return [q.label, q.advice, withDetail ? q.detail : undefined].filter(Boolean).join(' · ');
}

/** ★ The box's reading in DRM: the decoder's SNR ("SNR 18 dB"), or the verdict's one word before there is one. */
export function drmBoxReading(q: DabQuality): string {
  return q.merDb !== undefined ? `SNR ${Math.round(q.merDb)} dB` : q.short;
}
