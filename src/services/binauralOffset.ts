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
 *                   either way, and the carrier is ~300 Hz below that start (above it on LSB) — good to ±~150 Hz.
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
 * estimator answer must pass it to START a lean; a dial move leans at once. Full scale stays 300 Hz — still well over
 * three dead zones of ramp.
 */
export const BIN_DEAD_SSB_HZ = 50;
/** ★ Full lean at 300 Hz off for SSB and CW: the SSB estimator searches ±300 Hz, and 300 Hz is the edge of a
 *  typical CW filter — past it you are hearing a different signal, not a mistuned one. */
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
    case 'usb': return plan('ssb', BIN_SRC_SSB_USB, BIN_DEAD_SSB_HZ, BIN_FULL_NARROW_HZ);
    case 'lsb': return plan('ssb', BIN_SRC_SSB_LSB, BIN_DEAD_SSB_HZ, BIN_FULL_NARROW_HZ);
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

/** Bins coarser than this cannot place a voice's edge well enough to be worth a lean: leave it to the estimator. */
export const COARSE_MAX_BIN_HZ = 100;
/** ★ Where the voice's band starts, above the suppressed carrier. Transmit filters cut their low end at ~100-400 Hz;
 *  300 Hz is the common figure (the usual 300-2700 Hz SSB filter), and the spread either side is the cue's error. */
export const COARSE_VOICE_LOW_HZ = 300;
/** The window: the passband, plus this either side — enough to see the start of a voice the dial is well off. */
export const COARSE_MARGIN_HZ = 3000;
/** A voice's averaged band stands this far over the floor (dB). */
export const COARSE_OVER_DB = 6;
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
}

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
  private gridHz = 0;
  private startHz = 0;           // the absolute frequency of cell 0 (a multiple of gridHz)
  private pow = new Float64Array(0);
  private cnt = new Uint16Array(0);

  reset(): void { this.gridHz = 0; this.pow = new Float64Array(0); this.cnt = new Uint16Array(0); }

  /** One frame. `lsb` = the voice is below its carrier. */
  onFrame(bins: ArrayLike<number>, g: SpecGeom, lsb: boolean): CoarseReading {
    const none: CoarseReading = { ok: false, carrierHz: 0, bound: false };
    const n = bins.length;
    if (n < 8 || !(g.bwHz > 0) || !Number.isFinite(g.tuneHz)) return none;
    const binHz = g.bwHz / n;
    if (binHz > COARSE_MAX_BIN_HZ) { this.reset(); return none; }
    if (!(Math.abs(binHz - this.gridHz) <= 0.01 * binHz)) {
      this.gridHz = binHz; this.pow = new Float64Array(0); this.cnt = new Uint16Array(0);
    }
    const lo = Math.min(g.lo, g.hi), hi = Math.max(g.lo, g.hi);
    const grid = this.gridHz;
    const start = Math.floor((g.tuneHz + lo - COARSE_MARGIN_HZ) / grid) * grid;
    const cells = Math.ceil((hi - lo + 2 * COARSE_MARGIN_HZ) / grid) + 1;
    // Slide the average to the window's new place (a retune): cells still in view keep what they have learnt.
    if (this.pow.length !== cells || start !== this.startHz) {
      const np = new Float64Array(cells), nc = new Uint16Array(cells);
      const shift = Math.round((start - this.startHz) / grid);
      for (let i = 0; i < cells; i++) {
        const j = i + shift;
        if (j >= 0 && j < this.pow.length) { np[i] = this.pow[j]; nc[i] = this.cnt[j]; }
      }
      this.pow = np; this.cnt = nc; this.startHz = start;
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
    // The floor: the lower fifth of the matured cells.
    const ripe: number[] = [];
    for (let i = 0; i < cells; i++) if (this.cnt[i] >= COARSE_MIN_FRAMES) ripe.push(this.pow[i]);
    if (ripe.length < cells / 2) return none;
    ripe.sort((a, b) => a - b);
    const floor = ripe[Math.floor(ripe.length / 5)];
    if (!(floor > 0)) return none;
    const thr = floor * Math.pow(10, COARSE_OVER_DB / 10);
    const on = (i: number) => this.cnt[i] >= COARSE_MIN_FRAMES && this.pow[i] > thr;
    // Runs of occupied cells, bridging gaps of up to 150 Hz (a weak formant valley, a quiet harmonic).
    const gap = Math.max(1, Math.round(150 / grid));
    const pbLo = g.tuneHz + lo, pbHi = g.tuneHz + hi;
    let best = -1, bestA = 0, bestZ = 0, i = 0;
    while (i < cells) {
      if (!on(i)) { i++; continue; }
      const a = i;
      let z = i, j = i + 1;
      while (j < cells && j - z <= gap + 1) { if (on(j)) z = j; j++; }
      i = z + 1;
      const fa = start + (a - 0.5) * grid, fz = start + (z + 0.5) * grid;
      const width = fz - fa;
      const overlap = Math.min(fz, pbHi) - Math.max(fa, pbLo);
      if (width < 500 || width > 4000 || overlap < 300) continue;
      if (overlap > best) { best = overlap; bestA = a; bestZ = z; }
    }
    if (best < 0) return none;
    const db = (x: number) => 10 * Math.log10(Math.max(x, 1e-30));
    const tDb = db(thr);
    // The carrier-side edge, between the last cell under the threshold and the first over it (linear in dB).
    const edgeAt = (inside: number, outside: number) => {
      const di = db(this.pow[inside]), dout = db(this.pow[outside]);
      const t = di > dout ? (di - tDb) / (di - dout) : 0.5;
      return start + (inside + (outside - inside) * Math.max(0, Math.min(1, t))) * grid;
    };
    if (!lsb) {
      if (bestA === 0) return { ok: true, carrierHz: start - COARSE_VOICE_LOW_HZ, bound: true };
      return { ok: true, carrierHz: edgeAt(bestA, bestA - 1) - COARSE_VOICE_LOW_HZ, bound: false };
    }
    if (bestZ === cells - 1) return { ok: true, carrierHz: start + (cells - 1) * grid + COARSE_VOICE_LOW_HZ, bound: true };
    return { ok: true, carrierHz: edgeAt(bestZ, bestZ + 1) + COARSE_VOICE_LOW_HZ, bound: false };
  }
}

/** What the tracker drives — VibePowerModule's binaural methods on both platforms (optional: older natives). */
export interface BinauralSink {
  setBinauralSource?: (source: number, deadHz: number, fullHz: number) => void;
  setBinauralOffset?: (hz: number, valid: boolean) => void;
  setBinauralStereo?: (hint: number) => void;
  setBinauralDial?: (hz: number) => void;
  /** The coarse SSB cue: the voice's carrier, ABSOLUTE Hz (absolute, so it cannot race the dial). */
  setBinauralSsbHint?: (carrierHz: number, valid: boolean) => void;
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
  static readonly PUSH_MS = 100;
  /** ★ A coarse SSB reading survives this long without a fresh one: the pause between overs. (The average itself
   *  fades below the threshold within a second or two of silence.) */
  static readonly COARSE_HOLD_MS = 4000;
  private sink: BinauralSink;
  private spreadSent = -1;
  private coarse = new SsbCoarse();
  private coarseKey = '';
  private coarseHz = NaN;
  private coarseAt = -Infinity;
  private coarseSentHz = NaN;
  private coarseSentOk = false;
  private coarseSentAt = -Infinity;
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
    this.lastOkAt = -Infinity; this.sentHz = NaN; this.sentOk = false; this.sentAt = -Infinity;
    this.spreadSent = -1; this.coarse.reset(); this.coarseKey = ''; this.coarseHz = NaN; this.coarseAt = -Infinity;
    this.coarseSentHz = NaN; this.coarseSentOk = false; this.coarseSentAt = -Infinity;
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
      }
    }
    // ★★ A MOVE IS 2 Hz OR MORE. radiod stamps each frame's frequency with ±1 Hz of jitter (see the setStatus
    //    gate in SDRScreen); passed on, every frame would read as a retune and native would restart the SSB
    //    estimator twenty times a second — it would never finish one.
    if (Number.isFinite(g.tuneHz) && !(Math.abs(g.tuneHz - this.dial) < BIN_DIAL_MOVE_HZ)) {
      // The dial moved: a smoothed spectrum reading is of the OLD position — start the smoothing again.
      // (Native carries the SSB answer across the move itself.)
      if (Number.isFinite(this.dial)) this.smooth = NaN;
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
    if (r.ok) {
      // A peak is already sharp (light smoothing); a centroid wobbles with the modulation (heavier).
      const a = p.kind === 'peak' ? 0.5 : 0.25;
      this.smooth = Number.isFinite(this.smooth) ? this.smooth + a * (r.hz - this.smooth) : r.hz;
      this.lastOkAt = now;
    }
    const ok = Number.isFinite(this.smooth) && now - this.lastOkAt <= BinauralTracker.HOLD_MS;
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
    if (key !== this.coarseKey) { this.coarseKey = key; this.coarse.reset(); this.coarseHz = NaN; this.coarseAt = -Infinity; }
    const r = this.coarse.onFrame(bins, g, lsb);
    if (r.ok) { this.coarseHz = r.carrierHz; this.coarseAt = now; }
    const ok = Number.isFinite(this.coarseHz) && now - this.coarseAt <= BinauralTracker.COARSE_HOLD_MS;
    const hz = ok ? this.coarseHz : 0;
    // 10 Hz is well inside the cue's own error: finer changes are not worth a bridge message.
    const changed = ok !== this.coarseSentOk || (ok && !(Math.abs(hz - this.coarseSentHz) < 10));
    if (changed && now - this.coarseSentAt >= BinauralTracker.PUSH_MS) {
      this.sink.setBinauralSsbHint?.(hz, ok);
      this.coarseSentHz = hz; this.coarseSentOk = ok; this.coarseSentAt = now;
    }
  }
}
