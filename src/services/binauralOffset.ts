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
 * ★ PURE apart from the sink it is handed (no React Native import), so scripts/test_binauralOffset.ts runs it
 *   under plain node.
 */

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

/** The smallest dial change that counts as a move (mirrors BinauralTuning::kDialMoveHz). */
export const BIN_DIAL_MOVE_HZ = 2;
/** The base dead zone: on-tune audio must not wander (the brief: about ±15 Hz). */
export const BIN_DEAD_HZ = 15;
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
    case 'usb': return plan('ssb', BIN_SRC_SSB_USB, BIN_DEAD_HZ, BIN_FULL_NARROW_HZ);
    case 'lsb': return plan('ssb', BIN_SRC_SSB_LSB, BIN_DEAD_HZ, BIN_FULL_NARROW_HZ);
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

/** What the tracker drives — VibePowerModule's binaural methods on both platforms (optional: older natives). */
export interface BinauralSink {
  setBinauralSource?: (source: number, deadHz: number, fullHz: number) => void;
  setBinauralOffset?: (hz: number, valid: boolean) => void;
  setBinauralStereo?: (hint: number) => void;
  setBinauralDial?: (hz: number) => void;
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
  private sink: BinauralSink;
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
      if (kindChanged) { this.smooth = NaN; this.lastOkAt = -Infinity; this.sentHz = NaN; this.sentOk = false; }
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
}
