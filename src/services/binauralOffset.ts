/**
 * binauralOffset.ts — V12 BINAURAL TUNING: how far off tune the signal is, per mode, and the hand-off to the
 * native spatialiser (vibedsp/binaural.h, the same C++ on iOS and Android).
 *
 * Stuart, 2026-10-16: "If you are below the signal and need to tune up, signal to the right of the VFO, the audio
 * will pan to the right ear and pan back to centre the closer you get to being on tune, above it and vice versa."
 *
 * ★★ THE OFFSET IS SIGNED Hz: the signal MINUS where it should be. + = the signal is ABOVE the dial → tune up →
 *    the audio leans RIGHT. Every source below speaks that one convention.
 *
 * ★★ WHERE EACH MODE'S OFFSET COMES FROM:
 *    • USB / LSB  — NATIVE, from the audio: the voice-harmonic estimator (ssb_offset.h, FAST profile) on its own
 *                   worker thread. An SSB signal has no carrier for a spectrum to see; the voice is the evidence.
 *                   ★★ PLUS A COARSE CUE FROM HERE (SsbCoarse): the estimator reaches ±300 Hz, so arriving 500 Hz
 *                   off a voice it never locked gave no lean at all (Stuart, 2026-10-17: "if i am tuning up and end
 *                   up 500Hz below it wont trigger?"). The waterfall shows where the voice's band STARTS, a few kHz
 *                   either way, and the carrier is ~180 Hz below that start (above it on LSB) — good to ±~150-200 Hz.
 *                   ★ Since 2026-10-18 it finds the transmitter's FILTER EDGE (SsbCoarse.onFrame), measured on real
 *                   voices swept at 100/500 Hz clicks (scripts/test_binauralSweep.ts).
 *                   Native uses it only where its own answer is missing or out of reach (BinauralTuning::setSsbHint).
 *    • AM / SAM   — HERE, the carrier: the strongest bin in the passband, against the DIAL.
 *    • CW         — HERE, the CW signal's peak in the spectrum, against the PASSBAND CENTRE.
 *        ★★ NOT "received tone vs the set CW pitch", which is what was first asked for. The pitch is not ours to
 *           know: VibeServer beats CW at 600 Hz and treats CWL as USB (local_sdr_shim setBandwidth), a Kiwi's CW
 *           passband is 300-700 Hz audio (500), UberSDR's comes from radiod's preset, OpenWebRX's from its own
 *           demodulator — and none of them reports it. A wrong pitch is a CONSTANT error: an on-tune signal
 *           would sit permanently in one ear, the exact failure the dead zone exists to prevent. The RF peak
 *           against the passband centre is in the app's own model on every backend, sign included.
 *        ★ The price is resolution: a spectrum bin, not a 2 Hz FFT of the tone. The dead zone grows with the bin
 *          width so a peak that is merely quantised never leans; zoom in and the aid gets sharper.
 *    • NFM / FM / WFM — HERE, the power CENTROID of the signal inside the passband, against the dial. An FM
 *                   carrier swings with its modulation, so the centroid (smoothed over frames) is where the
 *                   discriminator's DC would sit. WFM is faded out natively as stereo locks.
 *    • Data modes ride USB/LSB/NFM (dataModes.ts: RTTY/SSTV/WEFAX/packet use a real sideband) and take that mode's source: NFM-borne ones
 *      get the centroid; SSB-borne data tones are not a voice, so the estimator's gate refuses them and the audio
 *      simply stays centred — never a wrong lean.
 *    • DAB and every whole-profile mode — none.
 *
 * ★★ ONE BEHAVIOUR, TWO CUES (until 476 they were two exclusive STYLES, GUIDE or SPREAD; Stuart on 476: "when on
 *    signal it presents in mono as it would before and then pan left and right based on signal and audio response
 *    when off tune"). ON TUNE the audio is untouched. OFF TUNE, by one weight (0 in the dead zone, 1 at full scale):
 *    the LEAN (all of the above) says which way to tune, and on SSB and CW — where the audio IS the RF spectrum
 *    shifted — the SPREAD places each audio frequency by where it lies in the passband, higher RF to the right in
 *    every sideband. Native does both (BinauralSpread, driven by the same weight as the lean); this file only says
 *    which map the mode has: spreadKindFor(), sent by the tracker by itself whenever binaural is on. AM, FM and DAB
 *    have no map — their audio is not a slice of spectrum — so they get the lean alone. There is no choice to offer.
 *
 * ★ PURE apart from the sink it is handed (no React Native import), so scripts/test_binauralOffset.ts runs it
 *   under plain node. (sdrProtocol.ts is type-only imports, so it loads there too — hence the .ts extension.)
 */
import { MODE_BANDWIDTHS } from './sdrProtocol.ts';

/** Native source ids — mirror BinauralTuning::Source in vibedsp/binaural.h. */
export const BIN_SRC_NONE = 0;
export const BIN_SRC_GIVEN = 1;
export const BIN_SRC_SSB_USB = 2;
export const BIN_SRC_SSB_LSB = 3;
/** Native stereo hints — mirror BinauralTuning::Stereo. */
export const BIN_STEREO_MONO = 0;
export const BIN_STEREO_LOCKED = 1;
export const BIN_STEREO_UNKNOWN = 2;

export type BinauralKind = 'none' | 'ssb' | 'peak' | 'centroid';

export interface BinauralPlan {
  kind: BinauralKind;
  source: number;
  /** 0 pan inside ±deadHz … full lean at ±fullHz. */
  deadHz: number;
  fullHz: number;
  /** Where the signal SHOULD be, relative to the dial (Hz): 0 for a carrier, the passband centre for CW. */
  refHz: number;
}

/** Spread maps — mirror BinauralSpread::Kind in vibedsp/binaural.h. 0 = none (AM, FM, DAB: the lean alone). */
export const BIN_SPREAD_OFF = 0;
export const BIN_SPREAD_USB = 1;
export const BIN_SPREAD_LSB = 2;
export const BIN_SPREAD_CW_UPPER = 3;
export const BIN_SPREAD_CW_LOWER = 4;
/** ★★ THE SPREAD's FIXED SCALE = the DEFAULT USB passband (Stuart: "treat the 0-2.7K or whatever our default is as the
 *  default stereo separation so good width"): its low edge full left, its high edge full right. Read from the
 *  table the protocol uses, so the two cannot drift apart. It never follows the listener's filter — a signal keeps
 *  its place when the passband is widened or narrowed. */
export const BIN_SPREAD_EDGES: readonly [number, number] = [MODE_BANDWIDTHS.usb[0], MODE_BANDWIDTHS.usb[1]];

/**
 * The spread map for a mode, or BIN_SPREAD_OFF where a spread means nothing. USB/LSB by sideband (data modes ride
 * these too). CW: upper or lower by WHERE THE PASSBAND SITS — a Kiwi's CWL passband is below the dial — and, when
 * it straddles the dial (VibeServer/UberSDR ±200 Hz), by the mode's name. ★ Not perfect: VibeServer beats CWL as
 * USB (local_sdr_shim), so there a CWL signal's left and right come out swapped. The tuned signal is centred either
 * way (the note is measured), so the cost is only which side a neighbour sits.
 */
export function spreadKindFor(mode: string | undefined | null, lo: number, hi: number): number {
  const m = String(mode ?? '').toLowerCase();
  switch (m) {
    case 'usb': return BIN_SPREAD_USB;
    case 'lsb': return BIN_SPREAD_LSB;
    case 'cw': case 'cwu': case 'cwl': {
      const mid = (lo + hi) / 2;
      if (Number.isFinite(mid) && mid < -50) return BIN_SPREAD_CW_LOWER;
      if (Number.isFinite(mid) && mid > 50) return BIN_SPREAD_CW_UPPER;
      return m === 'cwl' ? BIN_SPREAD_CW_LOWER : BIN_SPREAD_CW_UPPER;
    }
    default: return BIN_SPREAD_OFF;
  }
}

/** The smallest dial change that counts as a move (mirrors BinauralTuning::kDialMoveHz). */
export const BIN_DIAL_MOVE_HZ = 2;
/** The base dead zone: on-tune audio must not wander (the brief: about ±15 Hz). */
export const BIN_DEAD_HZ = 15;
/**
 * ★★★ SSB's OWN dead zone: ±50 Hz, not the ±15 Hz the spectrum sources use (Stuart on 477: "deep male voice in the
 * right, higher pitched female voice in the left when they are talking to each other and the panning left and right
 * constantly"). The SSB offset is measured from the VOICE, and a voice cannot be measured to ±15 Hz — the estimator
 * scatters by tens of Hz from one over to the next, a child's voice more than a man's, and the stations of a QSO sit
 * a few tens of Hz apart anyway. Natively a further hysteresis applies (BinauralTuning::kSsbEnterHz, 90 Hz): an
 * estimator answer must pass it to START a lean; a dial move leans at once. Full scale is BIN_FULL_SSB_HZ.
 */
export const BIN_DEAD_SSB_HZ = 50;
/** ★★ SSB's full lean at 1 kHz off (Stuart on 479, 2026-10-17: "the pan doesn't need to be as aggressive when slightly
 *  off tune — about half what it is for 500 Hz, and then 1 kHz has the full pan"). The same smoothstep from the ±50 Hz
 *  dead zone gives 46 % at 500 Hz, 13 % at 200 Hz. Possible since the two-stage estimator reaches ±1200 Hz; through
 *  build 478 it searched ±300 Hz, which is why full scale sat there. */
export const BIN_FULL_SSB_HZ = 1000;
/** ★ Full lean at 300 Hz off for CW: 300 Hz is the edge of a typical CW filter — past it you are hearing a different
 *  signal, not a mistuned one. (SSB shared it until 2026-10-17 — see BIN_FULL_SSB_HZ.) */
export const BIN_FULL_NARROW_HZ = 300;

/** Is binaural tuning meaningful in this mode at all? (The audio menu hides the row when not.) */
export function binauralModeOk(mode: string | undefined | null): boolean {
  return binauralPlan(mode, -1, 1, 0).kind !== 'none';
}

/**
 * Which source, and the scale. `binHz` is the spectrum's bin width — a spectrum source cannot resolve finer than
 * that, so its dead zone is at least ~0.6 bin (a quantised peak must never lean) and the full scale is kept at
 * least three dead zones wide so there is a ramp to hear.
 */
export function binauralPlan(mode: string | undefined | null, lo: number, hi: number, binHz: number): BinauralPlan {
  const m = String(mode ?? '').toLowerCase();
  const half = Math.max(1, (Math.abs(hi - lo)) / 2);
  const res = Number.isFinite(binHz) && binHz > 0 ? 0.6 * binHz : 0;
  const plan = (kind: BinauralKind, source: number, dead: number, full: number, refHz = 0): BinauralPlan => {
    const d = Math.max(BIN_DEAD_HZ, dead);
    return { kind, source, deadHz: d, fullHz: Math.max(full, 3 * d), refHz };
  };
  switch (m) {
    case 'usb': return plan('ssb', BIN_SRC_SSB_USB, BIN_DEAD_SSB_HZ, BIN_FULL_SSB_HZ);
    case 'lsb': return plan('ssb', BIN_SRC_SSB_LSB, BIN_DEAD_SSB_HZ, BIN_FULL_SSB_HZ);
    case 'cw': case 'cwu': case 'cwl':
      return plan('peak', BIN_SRC_GIVEN, res, BIN_FULL_NARROW_HZ, (lo + hi) / 2);
    // ★ AM: full lean at a fifth of the passband (±5 kHz → 1 kHz). An AM carrier a few hundred Hz off is still
    //   perfectly listenable, so the ramp is gentler than SSB's — but SAM, which locks to the carrier, rewards
    //   being close, and the same scale serves both.
    case 'am': case 'sam': case 'amn': case 'amw':
      return plan('peak', BIN_SRC_GIVEN, res, Math.max(BIN_FULL_NARROW_HZ, 0.2 * half));
    // ★ FM: the centroid wobbles with the modulation, so the dead zone is also a slice of the passband (4 %:
    //   ±200 Hz on NFM, ±4 kHz on broadcast FM) and full lean is at half the passband.
    case 'fm': case 'nfm': case 'wfm':
      return plan('centroid', BIN_SRC_GIVEN, Math.max(res, 0.04 * half), 0.5 * half);
    default:
      return { kind: 'none', source: BIN_SRC_NONE, deadHz: BIN_DEAD_HZ, fullHz: BIN_FULL_NARROW_HZ, refHz: 0 };
  }
}

export interface SpecGeom {
  /** The ACTUAL centre of these bins (SDRStatus.trueCenterHz ?? centerHz), Hz. */
  centerHz: number;
  bwHz: number;
  /** The dial, Hz. */
  tuneHz: number;
  /** Passband edges relative to the dial, Hz. */
  lo: number;
  hi: number;
}

export interface OffsetReading { ok: boolean; hz: number }

const lin = (db: number) => Math.pow(10, db / 10);

/**
 * One spectrum frame → the offset (Hz, signal minus where it should be). ok=false when nothing in the passband
 * stands clear of the noise — that is "no lean", never a guess.
 * ★ Bin k sits at centre + (k − n/2)·binHz, the convention watchProvider's crop uses on the same frames.
 */
export function spectrumOffset(bins: ArrayLike<number>, g: SpecGeom, kind: BinauralKind, refHz = 0): OffsetReading {
  const n = bins.length;
  if (n < 8 || !(g.bwHz > 0) || (kind !== 'peak' && kind !== 'centroid')) return { ok: false, hz: 0 };
  const binHz = g.bwHz / n;
  const toBin = (f: number) => (f - g.centerHz) / binHz + n / 2;
  let a = Math.ceil(toBin(g.tuneHz + Math.min(g.lo, g.hi)));
  let b = Math.floor(toBin(g.tuneHz + Math.max(g.lo, g.hi)));
  a = Math.max(1, a); b = Math.min(n - 2, b);
  if (b - a < 2) return { ok: false, hz: 0 };
  const freqOf = (k: number) => g.centerHz + (k - n / 2) * binHz;

  if (kind === 'peak') {
    // The strongest bin, which must stand ≥ 10 dB over the passband's median — a carrier or a CW mark does;
    // noise and speech sidebands do not.
    let pk = a;
    for (let k = a; k <= b; k++) if (bins[k] > bins[pk]) pk = k;
    const vals: number[] = [];
    for (let k = a; k <= b; k++) vals.push(bins[k]);
    vals.sort((x, y) => x - y);
    const med = vals[vals.length >> 1];
    if (!(bins[pk] - med >= 10)) return { ok: false, hz: 0 };
    // Parabolic refinement on the dB values: a fraction of a bin, so a peak between two bins reads between them.
    const y0 = bins[pk - 1], y1 = bins[pk], y2 = bins[pk + 1];
    const den = y0 - 2 * y1 + y2;
    let d = den < 0 ? 0.5 * (y0 - y2) / den : 0;
    if (!Number.isFinite(d)) d = 0;
    d = Math.max(-0.5, Math.min(0.5, d));
    const hz = freqOf(pk + d) - (g.tuneHz + refHz);
    return Number.isFinite(hz) ? { ok: true, hz } : { ok: false, hz: 0 };
  }

  // Centroid: the floor is the WHOLE frame's lower quartile (the passband may be full of signal); weights are
  // the power above twice that floor (3 dB), so the noise inside the passband does not drag it to the centre.
  const all = Array.from(bins as ArrayLike<number>).filter(Number.isFinite).sort((x, y) => x - y);
  if (all.length < 8) return { ok: false, hz: 0 };
  const floorLin = lin(all[Math.floor(all.length / 4)]);
  let w = 0, wf = 0, pkDb = -Infinity;
  for (let k = a; k <= b; k++) {
    const v = bins[k];
    if (!Number.isFinite(v)) continue;
    if (v > pkDb) pkDb = v;
    const e = lin(v) - 2 * floorLin;
    if (e > 0) { w += e; wf += e * freqOf(k); }
  }
  // ★ Something must actually be there: a peak 10 dB over the floor, and the excess carrying at least a few
  //   bins' worth of floor power (one hot bin of noise is not a station).
  if (!(pkDb - 10 * Math.log10(floorLin) >= 10) || !(w > 4 * floorLin)) return { ok: false, hz: 0 };
  const hz = wf / w - (g.tuneHz + refHz);
  return Number.isFinite(hz) ? { ok: true, hz } : { ok: false, hz: 0 };
}

// ── SSB: the coarse cue from the waterfall ─────────────────────────────────────────────────────────

/** Bins coarser than this cannot place a voice's edge well enough to be worth a lean: leave it to the estimator.
 *  ★ MEASURED 2026-10-18 on the composite sweep (scripts/test_binauralSweep.ts, real voices): at 23-94 Hz bins the cue
 *  points the right way on 99-100 % of frames; at 188 Hz still 97-100 %, but it then reads an ON-TUNE voice as 250+ Hz
 *  off on a fifth to most of the on-tune frames (it cannot tell on tune from a bin or two off), and holding back the near
 *  readings cost half its coverage. So it stays 100: a phone view wider than ~100 kHz (1024 bins) gives no cue — zoom
 *  in to sweep a band with the lean. */
export const COARSE_MAX_BIN_HZ = 100;
/** ★★★ …but the SECOND VOICE (setSsbHint2 → native places each voice by its own frequency) is read up to this: there is
 *  no whole-image lean in the crowded state, so the on-tune misreading that sets COARSE_MAX_BIN_HZ does not matter to it.
 *  Between the two, the cue's FIRST reading is used only to find the second one, and never sent (no lean). */
export const COARSE_CROWD_MAX_BIN_HZ = 100;
/** ★ Where the voice's band starts, above the suppressed carrier. Transmit filters cut their low end at ~100-400 Hz.
 *  ★ 180, not the textbook 300 (2026-10-18): measured on real voices (the composite sweep), 300 put the carrier 120-160 Hz
 *  too far from the voice on every zoom — the edge found 6 dB over the floor sits on the filter's skirt, and many
 *  transmitters cut lower than 300. At 180 the median error is within ±40 Hz. A contest-style 400 Hz cut reads ~220 Hz
 *  too far off; native takes kCoarseShrinkHz off every cue's lean for that reason (binaural.h). */
export const COARSE_VOICE_LOW_HZ = 180;
/** The window: the passband, plus this either side — enough to see the start of a voice the dial is well off. */
export const COARSE_MARGIN_HZ = 3000;
/** A voice must fill at least this much of the passband to be the one you are hearing (a reading, or a held one). */
export const COARSE_MIN_OVERLAP_HZ = 300;
/** A voice's averaged band stands this far over the floor (dB). */
export const COARSE_OVER_DB = 6;
/** The carrier side of a voice's edge is quiet — no more than this over the floor (dB)… */
export const COARSE_QUIET_DB = 3;
/** …for this far (Hz): below the transmitter's filter there is nothing… */
export const COARSE_QUIET_HZ = 150;
/** …once past the filter's own skirt, up to this wide (Hz). */
export const COARSE_SKIRT_HZ = 80;
/** Just inside the edge (this many Hz) the voice's low audio reaches… */
export const COARSE_TOP_HZ = 600;
/** …this far over the floor (dB)… */
export const COARSE_TOP_DB = 9;
/** …and is no more than this below anything further into the voice (dB). */
export const COARSE_TOP_MARGIN_DB = 3;
/** Time-averaging per frame (~10 frames/s → about a second). */
export const COARSE_ALPHA = 0.1;
/** Frames a cell must have averaged before it counts. */
export const COARSE_MIN_FRAMES = 8;

export interface CoarseReading {
  ok: boolean;
  /** The voice's suppressed carrier, ABSOLUTE Hz. */
  carrierHz: number;
  /** true = the carrier-side edge was off the window: only the DIRECTION is sure, the size is at least this. */
  bound: boolean;
  /** ★★★ A SECOND voice filling the passband too (≥ COARSE_MIN_OVERLAP_HZ), its carrier ≥ COARSE_SECOND_APART_HZ from the
   *  first: ABSOLUTE Hz, NaN when there is none. Native then places each voice by its frequency instead of leaning the
   *  whole sound toward one (BinauralTuning::setSsbHint2 — Stuart on 483: "in between 2 signals … blending them both and
   *  swirling them about between the ears"). */
  secondHz?: number;
}

/** No voice reaches further than this above its carrier (a generous transmitter's top cut). */
export const COARSE_VOICE_TOP_HZ = 3000;
/** Energy beyond that must cover at least this much (Hz, a second's average over COARSE_OVER_DB) to be a second voice. */
export const COARSE_SECOND_MIN_HZ = 300;
/** Two voices' carriers this far apart are two voices, not one read twice (mirrors BinauralSpread::kCrowdSidesApartHz). */
export const COARSE_SECOND_APART_HZ = 300;
/** separate(): a voice's band is widened by this (plus two bins) before another voice's energy counts as outside it —
 *  the filter's skirt, a band running past 2.7 kHz, and the cue's own error. */
export const COARSE_SEPARATE_MARGIN_HZ = 150;
/** separate(): the held average decays by this a frame (~10 a second): −6 dB in about 8 s — a QSO's over and its pause,
 *  as long as native holds its crowded judgement (BinauralSpread::kCrowdHoldSec). (−6 dB in 4 s let a quiet partner's
 *  energy drop out mid-over: test_binauralOffset's QSO at ∓600 Hz went 2.2 s unstated.) */
export const COARSE_HOLD_DECAY = 0.983;
/** separate(): each voice's own energy, outside the other's widened band, must cover this much of the passband (Hz).
 *  On tune, the widened band covers all but (the cue's error − the widening) of it: ≤ 40 Hz at the worst measured
 *  on-tune error (+234 Hz). Two voices ∓600 Hz either side leave the upper one ~300 Hz of its own once the cue's
 *  ~100 Hz error is counted, so 300 was too tight. */
export const COARSE_SEPARATE_MIN_HZ = 200;

/**
 * ★★ WHERE THE SSB VOICE IS, FROM THE WATERFALL. A voice is speech, so any one frame is ragged; averaged over a
 * second it is a solid band from ~300 Hz above its carrier to ~2.7 kHz (below it, on LSB). The CARRIER side of that
 * band is the sharp one — the transmitter's filter — so that is the edge we find; the far side just fades.
 *   • Averaged in ABSOLUTE RF cells (one per bin), not dial-relative ones: the signal stays put when the knob turns,
 *     so the average survives a retune and the answer is right again on the next frame.
 *   • The floor is the window's lower fifth (the window is ~8.6 kHz; one voice fills under a third of it).
 *   • The band chosen is the one overlapping the passband most — the voice you are HEARING part of — and it must
 *     overlap by 300 Hz and be 500-4000 Hz wide (a carrier is narrower; an AM station wider).
 *   • Its carrier-side edge off the window → only the direction is sure; reported as a bound (`bound`), which is
 *     still past full lean (300 Hz), so it leans the right way at once.
 *   • Bins over COARSE_MAX_BIN_HZ, or nothing over the floor → no reading. Never a guess.
 */
export class SsbCoarse {
  /** The coarsest bins read at all (test instrument: scripts/test_binauralSweep.ts measures past it). */
  static maxBinHz = COARSE_CROWD_MAX_BIN_HZ;
  private gridHz = 0;
  private startHz = 0;           // the absolute frequency of cell 0 (a multiple of gridHz)
  private pow = new Float64Array(0);
  private cnt = new Uint16Array(0);
  /** Each cell's average, held: the larger of it now and its held value decayed by COARSE_HOLD_DECAY a frame — so a voice
   *  that spoke in the last few seconds is still there (the other station of a QSO, between its overs). separate(). */
  private hold = new Float64Array(0);
  private floorNow = 0;
  private lastG: SpecGeom | null = null;

  reset(): void { this.gridHz = 0; this.pow = new Float64Array(0); this.cnt = new Uint16Array(0); this.hold = new Float64Array(0); this.floorNow = 0; this.lastG = null; }

  /** One frame. `lsb` = the voice is below its carrier. */
  onFrame(bins: ArrayLike<number>, g: SpecGeom, lsb: boolean): CoarseReading {
    const none: CoarseReading = { ok: false, carrierHz: 0, bound: false };
    const n = bins.length;
    if (n < 8 || !(g.bwHz > 0) || !Number.isFinite(g.tuneHz)) return none;
    const binHz = g.bwHz / n;
    if (binHz > SsbCoarse.maxBinHz) { this.reset(); return none; }
    if (!(Math.abs(binHz - this.gridHz) <= 0.01 * binHz)) {
      this.gridHz = binHz; this.pow = new Float64Array(0); this.cnt = new Uint16Array(0); this.hold = new Float64Array(0);
    }
    const lo = Math.min(g.lo, g.hi), hi = Math.max(g.lo, g.hi);
    const grid = this.gridHz;
    const start = Math.floor((g.tuneHz + lo - COARSE_MARGIN_HZ) / grid) * grid;
    const cells = Math.ceil((hi - lo + 2 * COARSE_MARGIN_HZ) / grid) + 1;
    // Slide the average to the window's new place (a retune): cells still in view keep what they have learnt.
    if (this.pow.length !== cells || start !== this.startHz) {
      const np = new Float64Array(cells), nc = new Uint16Array(cells), nh = new Float64Array(cells);
      const shift = Math.round((start - this.startHz) / grid);
      for (let i = 0; i < cells; i++) {
        const j = i + shift;
        if (j >= 0 && j < this.pow.length) { np[i] = this.pow[j]; nc[i] = this.cnt[j]; nh[i] = this.hold[j] ?? 0; }
      }
      this.pow = np; this.cnt = nc; this.hold = nh; this.startHz = start;
    }
    for (let i = 0; i < cells; i++) {
      const k = Math.round((start + i * grid - g.centerHz) / binHz + n / 2);
      if (k < 0 || k >= n) continue;
      const v = bins[k];
      if (!Number.isFinite(v)) continue;
      const p = Math.pow(10, v / 10);
      this.pow[i] = this.cnt[i] === 0 ? p : this.pow[i] + COARSE_ALPHA * (p - this.pow[i]);
      if (this.cnt[i] < 60000) this.cnt[i]++;
    }
    for (let i = 0; i < cells; i++) this.hold[i] = Math.max(this.pow[i], this.hold[i] * COARSE_HOLD_DECAY);
    // The floor: the lower fifth of the matured cells.
    const ripe: number[] = [];
    for (let i = 0; i < cells; i++) if (this.cnt[i] >= COARSE_MIN_FRAMES) ripe.push(this.pow[i]);
    if (ripe.length < cells / 2) return none;
    ripe.sort((a, b) => a - b);
    const floor = ripe[Math.floor(ripe.length / 10)];
    if (!(floor > 0)) return none;
    this.floorNow = floor; this.lastG = g;
    /* ★★★ FIND THE CARRIER-SIDE EDGE ITSELF, not runs over a threshold (2026-10-18, the sweep test on real voices).
     *  Averaged over a second, real speech is RAGGED: one voice's band breaks into pieces 300-500 Hz apart (between
     *  formants, between words), and the old rule — the longest run over the floor, bridging only 150 Hz gaps — took a
     *  piece from the middle of the voice for its edge: on the off-air 40 m sweep the cue pointed the wrong way on ~20 %
     *  of frames, and its 90th-percentile error was ~700 Hz even with the voices laid out cleanly.
     *  What does not break up is the TRANSMITTER'S FILTER: below the carrier side of a voice there is nothing, and the
     *  voice's strongest energy (its low audio, 200-1000 Hz) sits right against that edge. So an edge is believed where:
     *    A. a cell COARSE_OVER_DB over the floor has quiet (≤ COARSE_QUIET_DB) for COARSE_QUIET_HZ on its carrier side;
     *    B. the COARSE_TOP_HZ just inside it reach COARSE_TOP_DB — the voice's low audio is there…
     *    C. …and are no more than COARSE_TOP_MARGIN_DB below anything further into the voice (a piece from the middle
     *       of a voice has its loudest part further in, toward the low audio it is cut off from);
     *    D. the band it opens holds at least 500 Hz over COARSE_OVER_DB (a lone carrier or a CW signal does not).
     *  The voice chosen is the one whose band fills the passband most — the one you are hearing. Worked in "voice
     *  space": an LSB row is mirrored so a voice always lies above its edge. */
    const dbOver = (i: number) => (this.cnt[i] >= COARSE_MIN_FRAMES ? 10 * Math.log10(Math.max(this.pow[i], 1e-30) / floor) : NaN);
    const at = (j: number) => dbOver(lsb ? cells - 1 - j : j);            // voice space → the cell
    const kq = Math.max(1, Math.round(COARSE_QUIET_HZ / grid));
    const kTop = Math.max(1, Math.round(COARSE_TOP_HZ / grid));
    const kVoice = Math.max(kTop + 1, Math.round(2700 / grid));
    let best = -1, bestCarrier = 0, bestE = -1;
    const found: Array<[number, number]> = [];   // [overlap, carrier] of every voice that fills the passband enough
    const edges: number[] = [];                  // every voice edge found (voice-space cell), in the passband or not
    const ks = Math.round(COARSE_SKIRT_HZ / grid);
    for (let e = kq + ks; e < cells; e++) {
      const de = at(e);
      if (!(de >= COARSE_OVER_DB) || at(e - 1) >= COARSE_OVER_DB) continue;   // the first loud cell of a step
      // A: quiet on the carrier side, past the filter's own skirt — on average (a fine bin's own noise is ±2-3 dB)
      let qs = 0, qn = 0, qmax = -Infinity;
      for (let j = e - ks - kq; j < e - ks; j++) { const d = at(j); if (!Number.isFinite(d)) { qn = -1; break; } qs += d; qn++; qmax = Math.max(qmax, d); }
      if (qn <= 0 || qs / qn > COARSE_QUIET_DB || qmax >= COARSE_OVER_DB) continue;
      let top = -Infinity, rest = -Infinity, cover = 0;
      for (let j = e; j < Math.min(cells, e + kVoice); j++) {
        const d = at(j);
        if (!Number.isFinite(d)) continue;
        if (j < e + kTop) top = Math.max(top, d); else rest = Math.max(rest, d);
        if (d >= COARSE_OVER_DB) cover++;
      }
      if (!(top >= COARSE_TOP_DB) || top < rest - COARSE_TOP_MARGIN_DB || cover * grid < 500) continue;   // B, C, D
      // The edge: where the step crosses COARSE_OVER_DB, between the quiet cell and the loud one (linear in dB).
      edges.push(e);
      const dq = at(e - 1);
      const t = de > dq ? (de - COARSE_OVER_DB) / (de - dq) : 0.5;            // 0 = at the loud cell, 1 = at the quiet one
      const iIn = lsb ? cells - 1 - e : e, iOut = lsb ? cells - e : e - 1;
      const edgeHz = start + (iIn + (iOut - iIn) * Math.max(0, Math.min(1, t))) * grid;
      // ★ A coarse bin averages the step across its width, so the crossing reads up to half a bin outside the true edge
      //   (measured: the cue's median error grew from ~−60 Hz at 23-47 Hz bins to −150…−270 Hz at 188 Hz). Half of that
      //   is given back — never more: an overstated cue is the one that could lean an on-tune voice.
      const binComp = 0.5 * Math.max(0, grid - 50);
      const carrier = lsb ? edgeHz + COARSE_VOICE_LOW_HZ - binComp : edgeHz - COARSE_VOICE_LOW_HZ + binComp;
      const ov = coarseVoiceOverlap(carrier, lsb, g);
      if (ov >= COARSE_MIN_OVERLAP_HZ) found.push([ov, carrier]);
      if (ov >= COARSE_MIN_OVERLAP_HZ && ov > best) { best = ov; bestCarrier = carrier; bestE = e; }
    }
    if (best < 0) return none;
    // ★★★ The second voice: the one filling the passband most after the first, apart from it (and really a second voice:
    //     separate(), at the end).
    let second = NaN, secondOv = -1;
    for (const [ov, c] of found) if (Math.abs(c - bestCarrier) >= COARSE_SECOND_APART_HZ && ov > secondOv) { secondOv = ov; second = c; }
    /* ★★★ …or one whose edge the first one HIDES. Two voices less than a voice's width apart overlap, and the upper one's
     *     carrier-side edge falls inside the lower one's band (USB; mirrored, LSB) — there is no quiet below it to find. But
     *     no voice reaches more than COARSE_VOICE_TOP_HZ above its carrier: energy SUSTAINED beyond that (the cells are a
     *     second's average, so noise does not pass COARSE_OVER_DB) is another voice. Its far edge (its transmitter's top
     *     cut, ~2700 Hz above its carrier) places it. */
    /* ★★★ …and only when no voice of its own is SEEN there. A hidden edge is hidden because the second voice overlaps the
     *     first; energy beyond the first's band that has its OWN transmitter edge (one the search above found, wherever it
     *     sits — `edges`, before the overlap test) is a SEPARATE voice, already judged at its true carrier.
     *     ★ 2026-10-18 (Stuart on 484, the zoom-in lean): parked on a lone LSB voice with a neighbour 3.2 kHz below — whose
     *       whole band is outside the passband — this rule read the neighbour's band as a hidden voice 1794 Hz below the
     *       dial, overlapping the passband: CROWDED, and the on-tune voice was spread by frequency. */
    if (!Number.isFinite(second) && bestE >= 0) {
      const k0 = bestE + Math.round((COARSE_VOICE_TOP_HZ + COARSE_SKIRT_HZ - COARSE_VOICE_LOW_HZ) / grid);
      const kLen = Math.round(2700 / grid);
      let occ = 0, top = -1, run = 0;
      for (let j = k0; j < Math.min(cells, k0 + kLen); j++) {
        const d = at(j);
        if (d >= COARSE_OVER_DB) { occ++; run = 0; top = j; }
        else if (Number.isFinite(d) && ++run * grid > 300) break;   // a gap of 300 Hz: the band has ended
      }
      const own = edges.some((e2) => e2 > bestE + Math.round(1000 / grid) && e2 <= top);
      if (occ * grid >= COARSE_SECOND_MIN_HZ && top >= 0 && !own) {
        const topHz = start + (lsb ? cells - 1 - top : top) * grid;
        const c2 = lsb ? topHz + 2700 : topHz - 2700;
        if (Math.abs(c2 - bestCarrier) >= COARSE_SECOND_APART_HZ && coarseVoiceOverlap(c2, lsb, g) >= COARSE_MIN_OVERLAP_HZ) second = c2;
      }
    }
    if (Number.isFinite(second) && !this.separate(bestCarrier, second, lsb)) second = NaN;
    return { ok: true, carrierHz: bestCarrier, bound: false, secondHz: second };
  }

  /**
   * ★★★ ARE THESE TWO VOICES REALLY TWO? (2026-10-18, Stuart on 484: "when i zoomed in without tuning it panned over
   * slightly" — on tune, right, "a bassy male voice".) Each must have shown energy, within the last few seconds (the held
   * average), IN THE PASSBAND where the OTHER cannot reach — outside the other's band (COARSE_VOICE_LOW_HZ…2700 Hz from
   * its carrier, widened by COARSE_SEPARATE_MARGIN_HZ and two bins for the cue's own error) — over at least
   * COARSE_SEPARATE_MIN_HZ. A voice you are ON fills the passband, so nothing can be outside it there: a lone on-tune voice is
   * never two, whatever a gap in its ragged spectrum or a neighbour beyond the passband looks like. Between two stations,
   * each has its own part of the passband — and in a QSO either side, each shows it while it talks, held between overs.
   * Measured: parked on lone voices, the zoom-in found a "second" 1794 Hz below the dial (a neighbour wholly outside the
   * passband, read as hidden) — CROWDED, and native spread the on-tune voice by audio frequency: a bassy voice's energy
   * is low audio, which on LSB is the higher RF, so the image moved right by 0.9-1.8 dB.
   */
  separate(c1: number, c2: number, lsb: boolean): boolean {
    const g = this.lastG;
    if (!g || !(this.floorNow > 0) || !(this.gridHz > 0) || !Number.isFinite(c1) || !Number.isFinite(c2)) return false;
    const grid = this.gridHz;
    const m = COARSE_SEPARATE_MARGIN_HZ + 2 * grid;
    const pLo = g.tuneHz + Math.min(g.lo, g.hi), pHi = g.tuneHz + Math.max(g.lo, g.hi);
    const band = (c: number, w: number): [number, number] =>
      lsb ? [c - 2700 - w, c - COARSE_VOICE_LOW_HZ + w] : [c + COARSE_VOICE_LOW_HZ - w, c + 2700 + w];
    const thr = this.floorNow * Math.pow(10, COARSE_OVER_DB / 10);
    const own = (a: number, b: number) => {   // energy of voice a, in the passband, outside voice b's (widened) band
      const [aLo, aHi] = band(a, 0), [bLo, bHi] = band(b, m);
      let n = 0;
      for (let i = 0; i < this.hold.length; i++) {
        if (this.cnt[i] < COARSE_MIN_FRAMES) continue;
        const f = this.startHz + i * grid;
        if (f < pLo || f > pHi || f < aLo || f > aHi || (f >= bLo && f <= bHi)) continue;
        if (this.hold[i] >= thr) n++;
      }
      return n * grid >= COARSE_SEPARATE_MIN_HZ;
    };
    return own(c1, c2) && own(c2, c1);
  }
}

/** How much of the passband (Hz) a voice whose suppressed carrier is `carrierHz` fills — its band taken as the usual
 *  COARSE_VOICE_LOW_HZ…2700 Hz above the carrier (below it on LSB). */
export function coarseVoiceOverlap(carrierHz: number, lsb: boolean, g: SpecGeom): number {
  const vLo = lsb ? carrierHz - 2700 : carrierHz + COARSE_VOICE_LOW_HZ;
  const vHi = lsb ? carrierHz - COARSE_VOICE_LOW_HZ : carrierHz + 2700;
  const pLo = g.tuneHz + Math.min(g.lo, g.hi), pHi = g.tuneHz + Math.max(g.lo, g.hi);
  return Math.min(vHi, pHi) - Math.max(vLo, pLo);
}

/** What the tracker drives — VibePowerModule's binaural methods on both platforms (optional: older natives). */
export interface BinauralSink {
  setBinauralSource?: (source: number, deadHz: number, fullHz: number) => void;
  setBinauralOffset?: (hz: number, valid: boolean) => void;
  setBinauralStereo?: (hint: number) => void;
  setBinauralDial?: (hz: number) => void;
  /** The coarse SSB cue: the voice's carrier, ABSOLUTE Hz (absolute, so it cannot race the dial). */
  setBinauralSsbHint?: (carrierHz: number, valid: boolean) => void;
  /** A SECOND voice the waterfall sees in the passband (ABSOLUTE Hz; valid false = none) — native judges it crowded. */
  setBinauralSsbHint2?: (carrierHz: number, valid: boolean) => void;
  /** The mode's spread map (BIN_SPREAD_OFF = none: the lean alone); edges = BIN_SPREAD_EDGES. How much spread is
   *  native's business — the same off-tune weight as the lean, none on tune. */
  setBinauralSpread?: (kind: number, edgeLoHz: number, edgeHiHz: number) => void;
}

/**
 * ★ Per-frame driver: called from the spectrum callback (~10 frames a second) while the setting is on. Chooses
 * the source, measures, smooths, holds through a CW space, and tells native ONLY WHAT CHANGED — at most one
 * offset message per 100 ms, so the bridge carries a trickle, never a frame-rate stream.
 */
export class BinauralTracker {
  /** A spectrum reading survives this long without a fresh one: a CW space, a word gap on AM. */
  static readonly HOLD_MS = 1500;
  /** ★ AM: a carrier reading survives this long through a fade while the DIAL IS STILL (a carrier does not wander; a
   *  move resets it). Short of it, a deep fade of a few seconds let the lean fall to centre and swing back. */
  static readonly AM_HOLD_MS = 8000;
  /** A peak further than this many bins (or PEAK_JUMP_MIN_HZ) from the established carrier is a jump… */
  static readonly PEAK_JUMP_BINS = 3;
  static readonly PEAK_JUMP_MIN_HZ = 60;
  /** …believed only after this many frames there in a row (~1 s): a new carrier that stays, not a fade's blip. */
  static readonly PEAK_PERSIST = 10;
  private peakAbs = NaN;
  private peakCand = NaN;
  private peakCandN = 0;
  static readonly PUSH_MS = 100;
  /** ★ A coarse SSB reading survives this long without a fresh one: the pause between overs. (The average itself
   *  fades below the threshold within a second or two of silence.) */
  static readonly COARSE_HOLD_MS = 4000;
  /** ★★★ A second voice counts only once seen in this many readings (half a second)… */
  static readonly SECOND_MIN_FRAMES = 5;
  /** …and filling this much of the passband (Hz): a neighbour just touching its edge is not "between two signals". */
  static readonly SECOND_MIN_OVERLAP_HZ = 600;
  private sink: BinauralSink;
  private spreadSent = -1;
  private coarse = new SsbCoarse();
  private coarseKey = '';
  private coarseHz = NaN;
  private coarseAt = -Infinity;
  private coarseSentHz = NaN;
  private coarseSentOk = false;
  private coarseSentAt = -Infinity;
  /** Voices the waterfall has seen lately (each reading's first AND second), merged within COARSE_SECOND_APART_HZ (a hidden voice is placed from its far edge, ±200 Hz). */
  private seen: Array<{ hz: number; at: number; n: number }> = [];
  private secondSentHz = NaN;
  /** The waterfall's voices as last judged (ABSOLUTE Hz, NaN = none) — what the cue and the second say, whether or not
   *  the zoom lets the first be sent. A test instrument (scripts/test_binauralSweep.ts); nothing reads it in the app. */
  firstHz = NaN;
  secondHz = NaN;
  private secondSentOk = false;
  private secondSentAt = -Infinity;
  private srcKey = '';
  private dial = NaN;
  private stereo = -1;
  private smooth = NaN;
  private lastOkAt = -Infinity;
  private sentHz = NaN;
  private sentOk = false;
  private sentAt = -Infinity;

  constructor(sink: BinauralSink) { this.sink = sink; }

  /** Forget everything (the setting switched off, a new connection): the next frame states it all afresh. */
  reset(): void {
    this.srcKey = ''; this.dial = NaN; this.stereo = -1; this.smooth = NaN;
    this.peakAbs = NaN; this.peakCand = NaN; this.peakCandN = 0;
    this.lastOkAt = -Infinity; this.sentHz = NaN; this.sentOk = false; this.sentAt = -Infinity;
    this.spreadSent = -1; this.coarse.reset(); this.coarseKey = ''; this.coarseHz = NaN; this.coarseAt = -Infinity;
    this.coarseSentHz = NaN; this.coarseSentOk = false; this.coarseSentAt = -Infinity;
    this.seen = []; this.firstHz = NaN; this.secondHz = NaN; this.secondSentHz = NaN; this.secondSentOk = false; this.secondSentAt = -Infinity;
  }

  setStereo(hint: number): void {
    if (hint === this.stereo) return;
    this.stereo = hint;
    this.sink.setBinauralStereo?.(hint);
  }

  /** One spectrum frame. `mode`, `lo`, `hi` are the demodulator's; `now` in ms (injectable for the test). */
  onFrame(bins: ArrayLike<number>, g: SpecGeom, mode: string, now: number = Date.now()): OffsetReading {
    const binHz = bins.length > 0 ? g.bwHz / bins.length : 0;
    const p = binauralPlan(mode, g.lo, g.hi, binHz);
    // Source + scale: re-sent only when they change (rounded, so bin-width jitter does not chatter).
    const key = `${p.source}|${Math.round(p.deadHz)}|${Math.round(p.fullHz)}`;
    if (key !== this.srcKey) {
      const kindChanged = this.srcKey.split('|')[0] !== String(p.source);
      this.srcKey = key;
      this.sink.setBinauralSource?.(p.source, p.deadHz, p.fullHz);
      if (kindChanged) {
        this.smooth = NaN; this.lastOkAt = -Infinity; this.sentHz = NaN; this.sentOk = false;
        // ★ Native forgets the coarse cue on a change of source, so it must be stated afresh (and the average
        //   belongs to the old sideband's reading anyway).
        this.coarseKey = ''; this.coarseSentHz = NaN; this.coarseSentOk = false; this.coarseSentAt = -Infinity;
        this.seen = []; this.secondSentHz = NaN; this.secondSentOk = false; this.secondSentAt = -Infinity;
      }
    }
    // ★★ A MOVE IS 2 Hz OR MORE. radiod stamps each frame's frequency with ±1 Hz of jitter (see the setStatus
    //    gate in SDRScreen); passed on, every frame would read as a retune and native would restart the SSB
    //    estimator twenty times a second — it would never finish one.
    if (Number.isFinite(g.tuneHz) && !(Math.abs(g.tuneHz - this.dial) < BIN_DIAL_MOVE_HZ)) {
      // The dial moved: a smoothed spectrum reading is of the OLD position — start the smoothing again.
      // (Native carries the SSB answer across the move itself.)
      if (Number.isFinite(this.dial)) this.smooth = NaN;
      this.peakAbs = NaN; this.peakCand = NaN; this.peakCandN = 0;   // a retune: the first carrier seen is taken at once
      this.dial = g.tuneHz;
      this.sink.setBinauralDial?.(g.tuneHz);
    }
    // ★★ The mode's spread map, stated by itself (there is no style to choose any more) and only when it changes.
    const kind = spreadKindFor(mode, g.lo, g.hi);
    if (kind !== this.spreadSent) {
      this.spreadSent = kind;
      this.sink.setBinauralSpread?.(kind, BIN_SPREAD_EDGES[0], BIN_SPREAD_EDGES[1]);
    }
    if (p.kind === 'ssb') {
      // ★ Always: the coarse cue drives the off-tune weight on SSB, and with it the spread as well as the lean.
      this.coarseFrame(bins, g, p.source === BIN_SRC_SSB_LSB, now);
      return { ok: false, hz: 0 };
    }
    if (p.kind !== 'peak' && p.kind !== 'centroid') return { ok: false, hz: 0 };

    const r = spectrumOffset(bins, g, p.kind, p.refHz);
    // ★★ AM THROUGH A FADE (2026-10-18, Stuart on 482, a fading 6105 kHz broadcast: "I get the odd pan left to right
    //    before it settles again"). In a selective fade the CARRIER drops while its sidebands — or the station next door
    //    — do not, and for a frame or two the tallest bin is one of those: the lean jumped to it and back. A carrier does
    //    not move while the dial is still, so once one is ESTABLISHED a reading far from it is believed only if it holds
    //    there for PEAK_PERSIST frames (a new carrier that stays); a blip is no reading at all, and the last good one is
    //    held (AM_HOLD_MS) through the fade. A retune clears all of it (the move resets below), so tuning stays immediate.
    if (r.ok && p.kind === 'peak') {
      const abs = g.tuneHz + p.refHz + r.hz;
      const jump = Math.max(BinauralTracker.PEAK_JUMP_BINS * binHz, BinauralTracker.PEAK_JUMP_MIN_HZ);
      if (!Number.isFinite(this.peakAbs) || Math.abs(abs - this.peakAbs) <= jump) {
        this.peakAbs = abs; this.peakCandN = 0;
      } else {
        if (Number.isFinite(this.peakCand) && Math.abs(abs - this.peakCand) <= jump) this.peakCandN++;
        else { this.peakCand = abs; this.peakCandN = 1; }
        if (this.peakCandN >= BinauralTracker.PEAK_PERSIST) {   // it stayed: a new carrier (no move came through)
          this.peakAbs = abs; this.peakCandN = 0; this.smooth = NaN;
        } else {
          r.ok = false;                                         // a blip in a fade: not a reading
        }
      }
    }
    if (r.ok) {
      // A peak is already sharp (light smoothing); a centroid wobbles with the modulation (heavier).
      const a = p.kind === 'peak' ? 0.5 : 0.25;
      this.smooth = Number.isFinite(this.smooth) ? this.smooth + a * (r.hz - this.smooth) : r.hz;
      this.lastOkAt = now;
    }
    const am = /^(am|sam|amn|amw)$/.test(String(mode).toLowerCase());
    const ok = Number.isFinite(this.smooth) && now - this.lastOkAt <= (am ? BinauralTracker.AM_HOLD_MS : BinauralTracker.HOLD_MS);
    const hz = ok ? this.smooth : 0;
    const changed = ok !== this.sentOk || (ok && !(Math.abs(hz - this.sentHz) < 1));
    if (changed && now - this.sentAt >= BinauralTracker.PUSH_MS) {
      this.sink.setBinauralOffset?.(hz, ok);
      this.sentHz = hz; this.sentOk = ok; this.sentAt = now;
    }
    return { ok, hz };
  }

  /** SSB: the waterfall's coarse cue → native, held through a pause, at most one message per PUSH_MS. */
  private coarseFrame(bins: ArrayLike<number>, g: SpecGeom, lsb: boolean, now: number): void {
    const key = lsb ? 'lsb' : 'usb';
    if (key !== this.coarseKey) { this.coarseKey = key; this.coarse.reset(); this.coarseHz = NaN; this.coarseAt = -Infinity; this.seen = []; }
    const r = this.coarse.onFrame(bins, g, lsb);
    if (r.ok) { this.coarseHz = r.carrierHz; this.coarseAt = now; }
    // ★★★ THE SECOND VOICE (2026-10-18): every voice seen lately — a reading's first and its second — held like the first
    //     (COARSE_HOLD_MS, while it still fills the passband). In a QSO either side of the dial the two take turns and
    //     each fades from the average while the other talks, so "the other one" is whichever was seen last apart from the
    //     first cue. The DIAL is not involved: the cells are absolute, and a retune drops what leaves the passband.
    if (r.ok) {
      for (const hz of [r.carrierHz, r.secondHz ?? NaN]) {
        if (!Number.isFinite(hz)) continue;
        const v = this.seen.find((x) => Math.abs(x.hz - hz) < COARSE_SECOND_APART_HZ);   // closer than that: the same voice
        if (v) { v.hz = hz; v.at = now; v.n++; } else this.seen.push({ hz, at: now, n: 1 });
      }
    }
    this.seen = this.seen.filter((x) => now - x.at <= BinauralTracker.COARSE_HOLD_MS && coarseVoiceOverlap(x.hz, lsb, g) >= COARSE_MIN_OVERLAP_HZ);
    // ★★ A HELD reading only while its voice is still in the passband (2026-10-18, the sweep test on real voices): the
    //    hold is for the pause between overs, not for a voice the knob has left behind — mid-sweep it kept leaning
    //    toward a voice that was no longer in your ears, for up to COARSE_HOLD_MS after the dial had passed it.
    const okFirst = Number.isFinite(this.coarseHz) && now - this.coarseAt <= BinauralTracker.COARSE_HOLD_MS
      && (r.ok || coarseVoiceOverlap(this.coarseHz, lsb, g) >= COARSE_MIN_OVERLAP_HZ);
    /* ★★★ THE LEAN ONLY AT FINE BINS; THE SECOND VOICE AT COARSE ONES TOO. Over COARSE_MAX_BIN_HZ the first reading cannot
     *     tell on tune from a bin or two off, so it is never sent (native would lean on it) — but it still finds the second
     *     voice, and two voices in the passband lean nothing: native places each by its own frequency
     *     (BinauralSpread::crowd). So up to COARSE_CROWD_MAX_BIN_HZ the second is sent ALONE, and native takes a second
     *     voice with no first cue as two voices (binaural.cpp, the `two` judgement). The second is stated only while the
     *     first reading holds, at every zoom — at fine bins that is what native's own check (hint and hint2 both valid,
     *     ≥ kCrowdSidesApartHz apart) always required, so nothing changes there. */
    const binHz = bins.length > 0 ? g.bwHz / bins.length : Infinity;
    const fine = binHz <= COARSE_MAX_BIN_HZ;
    this.firstHz = okFirst ? this.coarseHz : NaN;
    {
      let other: { hz: number; at: number; n: number } | undefined;
      if (Number.isFinite(this.coarseHz))
        for (const x of this.seen)
          if (x.n >= BinauralTracker.SECOND_MIN_FRAMES && Math.abs(x.hz - this.coarseHz) >= COARSE_SECOND_APART_HZ
              && coarseVoiceOverlap(x.hz, lsb, g) >= BinauralTracker.SECOND_MIN_OVERLAP_HZ && (!other || x.at > other.at)
              && this.coarse.separate(this.coarseHz, x.hz, lsb)) other = x;   // ★★★ really two (SsbCoarse.separate)
      const ok2 = other !== undefined && okFirst;
      const hz2 = ok2 ? other!.hz : 0;
      this.secondHz = ok2 ? hz2 : NaN;
      const changed2 = ok2 !== this.secondSentOk || (ok2 && !(Math.abs(hz2 - this.secondSentHz) < 10));
      if (changed2 && now - this.secondSentAt >= BinauralTracker.PUSH_MS) {
        this.sink.setBinauralSsbHint2?.(hz2, ok2);
        this.secondSentHz = hz2; this.secondSentOk = ok2; this.secondSentAt = now;
      }
    }
    const ok = okFirst && fine;
    const hz = ok ? this.coarseHz : 0;
    // 10 Hz is well inside the cue's own error: finer changes are not worth a bridge message.
    const changed = ok !== this.coarseSentOk || (ok && !(Math.abs(hz - this.coarseSentHz) < 10));
    if (changed && now - this.coarseSentAt >= BinauralTracker.PUSH_MS) {
      this.sink.setBinauralSsbHint?.(hz, ok);
      this.coarseSentHz = hz; this.coarseSentOk = ok; this.coarseSentAt = now;
    }
  }
}
