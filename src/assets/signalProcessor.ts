/**
 * signalProcessor.ts — M9PSY signal compression pipeline + UberSDR auto-range
 *
 * Original compression maths by M9PSY (Nathan / MadPsy) from vibeWaterfall.ts v1.5.
 * Auto-range algorithm extracted verbatim from UberSDR's spectrum-display.js
 * (updateAutoRange) — the exact algorithm that keeps the web waterfall calibrated.
 * Ported to TypeScript for VibeSDR v2 by Stuey3D + Claude.
 *
 * This module owns ALL FFT-data→display mapping. It receives raw dBFS
 * Float32Arrays from UberSDRClient (server applies NO smoothing on the
 * user-spectrum WS) and produces:
 *   - row:  Uint8Array of 0–255 LUT indices for the waterfall ring buffer
 *   - spec: Float32Array of normalised [0,1] spectrum trace values
 *   - peak: Float32Array of normalised [0,1] peak-hold values
 *   - dbMin/dbMax: the live auto-ranged window (for the dB axis labels)
 *
 * Pipeline per frame (order matters — matches v1.5 addFftLine):
 *   0. Flush detection: settings change OR band change > 40% of visible BW
 *   1. Resample source bins → processor width (nearest)
 *   2. Auto-range update (UberSDR algorithm) or manual range passthrough
 *   3. Temporal EMA in raw dBFS domain (waterfall: alpha=1, i.e. raw —
 *      server data on this WS is unsmoothed but full-rate; kept as a hook)
 *   4. Spatial 5-tap weighted smooth [1,2,3,2,1]/9 (waterfall path only)
 *   5. Spectrum EMA — time-normalised so it looks identical at all FPS,
 *      rise alpha = 4 × fall alpha (fast attack, slow decay)
 *   6. Peak hold: per-bin max with 10 dB/s linear decay
 *   7. Normalise → 0–255, clip threshold 14.97 → 0, re-stretch remainder
 *   8. Brightness offset + unsharp mask + S-curve contrast (CPU port of the
 *      v1.5 WebGL fragment shader) → final LUT index
 */

// ── Constants (M9PSY / v1.5) ─────────────────────────────────────────────────

const CLIP_THRESHOLD       = 14.97; // 0–255 units; bins below → 0 (noise floor clip)
const PEAK_DECAY_DB_PER_S  = 10;    // peak hold linear decay
const BAND_FLUSH_FRAC      = 0.4;   // recentre > 40% of visible BW → flush history

// ── Constants (UberSDR spectrum-display.js auto-range) ──────────────────────

const RANGE_MARGIN         = 5;     // dB margin added beyond floor/ceiling
const NOISE_PERCENTILE     = 0.10;  // 10th percentile = noise floor estimate
// ★★★ IGNORE THE EDGES OF THE CAPTURE WINDOW WHEN AUTO-RANGING. Every receiver rolls off at the
// edges of its own sample window, and an SDRplay or an Airspy HF+ does so hard enough to raise
// large skirts either side of the span. Those are the FILTER, not the band — but the auto-range
// counted them as bins like any other, so the noise percentile and the peak were both computed
// partly from an artefact, the scale stretched to cover it, and the waterfall blew out to white.
// ★ The tell was decisive: zooming in ONE CLICK made it normal, because zooming excludes the
// skirts (Stuart, 2026-07-27 — he had already seen the same thing on the RSP).
// ★ Applied ALWAYS, not just at full span: the processor is not told the zoom level, and at any
// zoom the outer few per cent contribute nothing the middle does not. Cheap insurance against a
// statistic being dominated by the one part of the spectrum that is guaranteed not to be signal.
// ★ STATISTICS ONLY. Every bin is still DISPLAYED — hiding the roll-off would be a different and
// much worse lie than mis-scaling it.
// ★★★ 0.06 WAS NOT ENOUGH, AND THE AIRSPY PROVED IT. The HF+ still blew out at default zoom after
// the 2026-07-27 fix, because 6% a side lands INSIDE its dead lobe — so the 10th-percentile noise
// floor was still computed partly from the dead region, collapsed, and the live middle of the span
// mapped to the top of the palette.
// ★ The figure comes from SDR++ Brown's "Fill-In bandwidth" (airspyhf_source), which carves the
// unusable edges off an HF+ for real: 80 kHz per side at 912 kHz — 8.8% each, ~752 kHz usable of
// 912. Its author calls them "experimental values", i.e. measured rather than published, which is
// the only kind of figure available for this: no driver reports a usable bandwidth (see
// [[airspy_hf_backend]]). 0.09 covers the HF+ at its top rate and stays well clear of anything
// real — at 1024 bins that is 92 bins a side of guaranteed-not-signal.
// ★ STILL STATISTICS ONLY. Every bin is DISPLAYED, unchanged. Hiding the roll-off is a bigger lie
// than mis-scaling it — and the honest way to remove it is Brown's: FILTER it out and send less
// data, which for us is a bandwidth saving too. That is the "Fill-In bandwidth" button, and it
// belongs server-side in the Airspy backend, not here.
const EDGE_EXCLUDE_FRAC    = 0.09;  // per side, from the auto-range statistics only
/** ★★ "THERE IS NO RADIO HERE." The server sends u8 0 (= -256 dBFS) for display bins that fall
 *  outside the captured band, so panning past the edge draws nothing instead of smearing the
 *  edge bin across the screen. Anything at or below this is a marker, not a measurement, and
 *  must be kept out of the auto-range statistics. The engine's real floor is around -125. */
const NO_DATA_DBFS         = -200;
// ★★ The ceiling ignores the very top of the distribution rather than taking the single
// strongest bin. A retune puts a brief DC/LO spike at the centre bin — one or two bins
// tens of dB above anything real — and a single-bin maximum hands the whole palette to it,
// so the entire waterfall dims for as long as it takes to age out. That is the residual
// "brightness flicker on tune" that clearing the history and slew-limiting could not fix:
// both control how FAST the ceiling moves, neither stops it being WRONG.
// 0.4% of 1024 bins ≈ 4 bins — narrower than any real signal at any zoom (an FM carrier is
// dozens of bins, SSB several), so nothing legitimate is excluded, and a genuinely clipping
// band simply saturates the top few bins to white, which is what it should look like.
const PEAK_EXCLUDE_FRAC    = 0.004;
const MIN_HISTORY_MS       = 2000;  // noise-floor smoothing window
const MAX_HISTORY_MS       = 5000;  // ceiling smoothing window (faster recovery)
// Below this visible span the 10th-percentile noise floor is untrustworthy: zoomed into a
// busy band every bin is signal, so the "floor" climbs into the signal and the waterfall
// rides up to white. Past this we FREEZE the floor at its last wide-view value. Tunable —
// the auto-contrast is admittedly finnicky (m9psy). See the freeze block below.
const FLOOR_FREEZE_SPAN_HZ = 25_000;
// When zoomed in, the display floor follows the LOCAL noise floor (so a quieter sub-band sits at its
// real noise — no dead flat band below it) but may climb at most this far above the remembered
// wide-view floor. That cap is what still stops a busy, all-signal zoom dragging the floor up into
// the signal and blowing the scale to white. Tunable; the auto-contrast is finnicky.
const FLOOR_CLIMB_MAX = 12;
// The DISPLAY WINDOW never shrinks below this many dB. On a dead band the ceiling (strongest bin)
// collapses onto the noise floor and the scale "zooms in" on pure noise — noise fills the screen,
// peaks blow up, waterfall goes bright. A minimum span caps that: the noise keeps its real (small)
// height near the bottom and there's headroom above for a signal. The classic auto-scale fix.
const MIN_RANGE_DB = 30;

// ── Types ────────────────────────────────────────────────────────────────────

export interface SignalProcessorSettings {
  /** 0–20 dB symmetric contrast: floor +N, ceiling −N. UberSDR web uses 10. */
  autoContrast:    number;
  /** Manual range override (wfCoarse='manual'). When set, auto-range is bypassed. */
  manualRange:     { minDb: number; maxDb: number } | null;
  /** −20…+20 dB offset applied to the spectrum trace floor (not waterfall). */
  specFloor:       number;
  /** Spectrum peak amplitude scale ×0.1 (10 = 1.0×). */
  specPeakScale:   number;
  /** 1–10 spectrum trace EMA smoothing frames (1 = instant). */
  smoothingFrames: number;
  /** FFT SMOOTHING (weak-signal averaging). OWRX-style per-frame EMA WEIGHT, 0–0.9 (0 = off/instant,
   *  0.9 = heaviest). Successive frames blend, so random noise averages down and a persistent weak
   *  signal builds up above it; higher = more visibility but more lag. Off by default. */
  avgFrames:       number;
  /** 5-tap spatial waterfall smooth on/off. */
  spatialSmooth:   boolean;
  /** Peak hold on/off. */
  peakHold:        boolean;
  /** −20…+20 dB brightness offset (waterfall only). */
  wfBrightness:    number;
  /** −10…+10 S-curve contrast (waterfall only). 0 = identity. */
  wfContrast:      number;
  /** 0–10 unsharp-mask sharpness (waterfall only). */
  wfSharpness:     number;
}

export const DEFAULT_PROCESSOR_SETTINGS: SignalProcessorSettings = {
  autoContrast:    5,       // 10 (UberSDR's value) crushes the floor — too dark
  manualRange:     null,
  specFloor:       0,
  specPeakScale:   10,
  smoothingFrames: 5,
  avgFrames:       0,        // FFT-smoothing weight OFF by default (0…0.9, OWRX-style)
  spatialSmooth:   true,
  peakHold:        true,
  wfBrightness:    0,
  wfContrast:      0,
  wfSharpness:     0,
};

export interface ProcessedFrame {
  /** Normalised intensity bytes (0–255) for the GPU waterfall texture —
   *  NOT LUT indices (palette/unsharp/contrast are shader-side now). */
  row:   Uint8Array;
  /** Normalised [0,1] spectrum trace, same length. */
  spec:  Float32Array;
  /** Normalised [0,1] peak hold, same length (zeros when peakHold off). */
  peak:  Float32Array;
  /** Live display window after auto-range + contrast. */
  dbMin: number;
  dbMax: number;
}

// ── Processor ────────────────────────────────────────────────────────────────

/** How fast the auto-contrast ceiling may travel, dB/second. Fast enough that a
 *  real level change is followed promptly, slow enough that the step as the
 *  rolling window turns over is a ramp rather than a flash. */
const MAX_SLEW_DB_PER_SEC = 40;

export class SignalProcessor {
  private settings: SignalProcessorSettings = { ...DEFAULT_PROCESSOR_SETTINGS };

  // Working buffers (lazily sized to bin count)
  private dbAvg:      Float32Array | null = null;  // temporal EMA (waterfall)
  private specSmooth: Float32Array | null = null;  // spectrum EMA (dBFS)
  private peakLine:   Float32Array | null = null;  // peak hold (dBFS)
  private tmp:        Float32Array | null = null;  // spatial smooth scratch
  private normRow:    Float32Array | null = null;  // 0–1 scratch for shader port
  private outRow:     Uint8Array   | null = null;
  private outSpec:    Float32Array | null = null;
  private outPeak:    Float32Array | null = null;

  // Auto-range state (UberSDR algorithm)
  private minHistory: Array<{ value: number; ts: number }> = [];
  private lastMaxAt = 0;
  private lastMinAt = 0;
  /** Set by a view change; the next frame adopts its range outright. See below. */
  private adoptRange = false;
  private lastCenterHz = 0;
  private lastBwHz = 0;
  private maxHistory: Array<{ value: number; ts: number }> = [];
  // The last trustworthy (wide-view) noise-floor dB, held while zoomed in past
  // FLOOR_FREEZE_SPAN_HZ. Cleared on every range flush so a band change re-learns.
  private frozenFloorDb: number | null = null;
  private actualMinDb = -120;
  private actualMaxDb = -20;
  // 1dB histogram for the noise percentile — reused every frame. (The original
  // port pushed all bins into a number[] and .sort()ed it with a JS comparator
  // PER FRAME — ~10k interpreted comparator calls + array churn at 10-20Hz was
  // the single biggest JS/GC load in the 2026-06-11 CPU profile.)
  private dbHist = new Uint32Array(300); // bucket b = (db + 280)dB, clamped

  // Frame timing + flush detection
  private lastFrameMs    = 0;
  private prevCenterHz   = 0;
  private settingsVer    = 0;
  private settingsVerApp = 0;

  /** Patch settings. Range-affecting changes flush history (matches v1.5). */
  applySettings(patch: Partial<SignalProcessorSettings>) {
    const rangeKeys: Array<keyof SignalProcessorSettings> =
      ['autoContrast', 'manualRange', 'wfBrightness', 'wfContrast', 'wfSharpness'];
    const rangeChanged = rangeKeys.some(
      k => patch[k] !== undefined && patch[k] !== this.settings[k],
    );
    this.settings = { ...this.settings, ...patch };
    if (patch.peakHold === false) this.peakLine = null;
    if (rangeChanged) this.settingsVer++;
  }

  getSettings(): SignalProcessorSettings { return { ...this.settings }; }

  /** Current auto-ranged window (for dB axis labels between frames). */
  getRange(): { dbMin: number; dbMax: number } {
    return { dbMin: this.actualMinDb, dbMax: this.actualMaxDb };
  }

  /** Rate-limit the floor exactly as the ceiling is limited — see the slew note
   *  there. First frame adopts outright so nothing fades in on connect. */
  private slewFloor(want: number, now: number): number {
    if (!this.lastMinAt || !Number.isFinite(this.actualMinDb) || this.adoptRange) {
      this.lastMinAt = now; return want;
    }
    const dt = Math.min(0.25, (now - this.lastMinAt) / 1000);
    this.lastMinAt = now;
    const step = MAX_SLEW_DB_PER_SEC * dt;
    const d = want - this.actualMinDb;
    return this.actualMinDb + (Math.abs(d) <= step ? d : Math.sign(d) * step);
  }

  /** Process one raw dBFS frame. bins length may change between frames. */
  process(bins: Float32Array, centerHz: number, bwHz: number, interacting = false): ProcessedFrame {
    const n = bins.length;
    const s = this.settings;
    const now = Date.now();
    const dtSec = this.lastFrameMs
      ? Math.min(0.5, Math.max(0.01, (now - this.lastFrameMs) / 1000))
      : 0.1;
    this.lastFrameMs = now;

    // ── 0. View change: drop stale ceiling samples ──────────────────────────
    // ★★ The ceiling averages a 5 SECOND window, so after a tune or zoom it
    // blends pre- and post-tune levels — every intermediate value is WRONG, not
    // merely late, and the waterfall dims and recovers for as long as the window
    // takes to turn over. Discard samples that describe somewhere the user is no
    // longer looking; the new target is then correct immediately and the slew
    // limit below turns the one remaining step into a fade.
    //
    // ★ actualMaxDb is deliberately NOT reset — holding the pre-tune ceiling is
    // what makes this a smooth adjustment rather than a jump to a cold start
    // (Stuart: "preserve the pretune setting and then adjust to the new limit").
    if (centerHz > 0 && bwHz > 0) {
      const moved = Math.abs(centerHz - this.lastCenterHz) > bwHz * 0.25
                 || Math.abs(bwHz - this.lastBwHz) > this.lastBwHz * 0.05;
      // ★ BOTH windows, not just the ceiling. The floor averages its own history
      // (MIN_HISTORY_MS) and steps the same way as it turns over, which moves the
      // bottom of the range and flickers just as visibly. Clearing only the max
      // left a residual flicker (Stuart, 2026-07-26).
      if (moved && this.lastBwHz > 0) {
        this.maxHistory.length = 0; this.minHistory.length = 0;
        // ★★ THE TWO EARLIER FIXES WERE FIGHTING EACH OTHER. Clearing the window
        // makes the new target correct IMMEDIATELY; the slew limit then refuses to
        // go there at more than 40 dB/s. So on every tune AND every zoom the scale
        // spent a few hundred ms travelling to a value it already knew, holding the
        // stale pre-tune ceiling on the way — and a ceiling that is too high maps
        // everything darker. That is the residual flicker: dim, then brighten and
        // settle, exactly as described, and on zoom as well as tune because both
        // clear the window (Stuart, 2026-07-26).
        //
        // ★ The slew limit keeps its real job — smoothing the STEPS as samples age
        // out of the rolling window during normal listening. It was never meant to
        // pace a deliberate, known-good change of view.
        this.adoptRange = true;
      }
      this.lastCenterHz = centerHz;
      this.lastBwHz = bwHz;
    }

    // ── 0a. Resize buffers if bin count changed ─────────────────────────────
    if (!this.dbAvg || this.dbAvg.length !== n) {
      this.dbAvg      = new Float32Array(n);
      this.specSmooth = new Float32Array(n);
      this.peakLine   = null;
      this.tmp        = new Float32Array(n);
      this.normRow    = new Float32Array(n);
      this.outRow     = new Uint8Array(n);
      this.outSpec    = new Float32Array(n);
      this.outPeak    = new Float32Array(n);
      this.dbAvg.set(bins);       // zero settling delay — prime from real data
      this.specSmooth.set(bins);
      this.minHistory = [];
      this.maxHistory = [];
      this.frozenFloorDb = null;
    }

    // ── 0b. Flush on settings change ────────────────────────────────────────
    if (this.settingsVerApp !== this.settingsVer) {
      this.settingsVerApp = this.settingsVer;
      this.dbAvg.set(bins);
      this.minHistory = [];
      this.maxHistory = [];
      this.frozenFloorDb = null;
    }

    // ── 0c. Flush on band change (> 40% of visible bandwidth) ───────────────
    if (centerHz && this.prevCenterHz && bwHz > 0 &&
        Math.abs(centerHz - this.prevCenterHz) > bwHz * BAND_FLUSH_FRAC) {
      this.dbAvg.set(bins);
      this.specSmooth!.set(bins);
      this.peakLine = null;
      this.minHistory = [];
      this.maxHistory = [];
      this.frozenFloorDb = null;   // new band → re-derive the floor (Stuart's broadcast caveat)
    }
    if (centerHz) this.prevCenterHz = centerHz;

    // ── 1a. WEAK-SIGNAL PROCESSING → this.dbAvg ─────────────────────────────
    // Both steps write this.dbAvg, which EVERYTHING downstream (auto-range, trace, waterfall) then
    // reads — so integration and a flat baseline apply consistently across the whole display. With
    // both off this is exactly `dbAvg = bins`, so default behaviour is unchanged.
    //
    // FRAME AVERAGING (integration): time-normalised EMA. Random noise averages down (~√N depth), a
    // persistent weak carrier holds — so it emerges. The flush blocks above re-prime dbAvg = bins on a
    // tune/band change, so integration restarts clean (no smear across a tune).
    // OWRX-style averaging: a per-frame EMA WEIGHT `w` in [0, 0.9] — `w` = the history's share,
    // `(1-w)` = the new frame. w=0 is instant, w=0.9 is OWRX's own heaviest. Deliberately NOT
    // time-normalised: the weight IS the control (exactly like OWRX), so the feel matches theirs
    // instead of blurring to mush above a few frames (Stuart 2026-07-24: >3× was useless).
    // ★ BYPASS while interacting: FFT smoothing (the EMA) inherently LAGS, so during a tune/zoom the
    // averaged spectrum crawls to catch up to the new band — the "spectrum slows on tune, snaps on
    // release" (Stuart 2026-07-24). Force instant (w=0) while the user is moving the view; resume
    // smoothing when settled.
    const w = (interacting || s.avgFrames <= 0) ? 0 : Math.min(0.9, s.avgFrames);
    if (w > 0) {
      const da = this.dbAvg!;
      const nw = 1 - w;
      for (let i = 0; i < n; i++) da[i] = da[i] * w + bins[i] * nw;
    } else {
      this.dbAvg!.set(bins);
    }

    // ── 2. Auto-range (UberSDR updateAutoRange, verbatim port) ──────────────
    if (s.manualRange) {
      this.actualMinDb = s.manualRange.minDb;
      this.actualMaxDb = s.manualRange.maxDb;
    } else {
      // Noise percentile via reusable 1dB histogram — one O(n) pass, zero
      // allocations (sub-dB precision is irrelevant: the result is floored,
      // margined and history-averaged anyway).
      const hist = this.dbHist;
      hist.fill(0);
      let absoluteMax = -Infinity;
      let count = 0;
      const src = this.dbAvg!;   // the PROCESSED signal (averaged / bg-subtracted), so the scale tracks what's shown
      // ★ Skip the roll-off skirts — see EDGE_EXCLUDE_FRAC. Guarded so a very narrow frame
      // cannot exclude itself down to nothing.
      let edge = Math.floor(n * EDGE_EXCLUDE_FRAC);
      if (n - 2 * edge < 16) edge = 0;
      for (let i = edge; i < n - edge; i++) {
        const db = src[i];
        if (!isFinite(db)) continue;
        // ★★★ BINS OUTSIDE THE CAPTURED BAND ARE NOT MEASUREMENTS — EXCLUDE THEM FROM THE SCALE.
        //     The server marks them (u8 0 → NO_DATA_DBFS); they exist so the panned-past region
        //     renders as nothing rather than as a smear of the edge bin. Letting them into the
        //     histogram drags the noise percentile far below anything real, and since that
        //     percentile IS the bottom of the range, the whole display re-contrasts around a
        //     region that has no signal in it at all (Stuart, 2026-08-03: panning past the band
        //     edge "effecting the auto contrast").
        //   ★ Tested against the RAW bin, not `src` — `src` is the EMA-smoothed copy, and a bin
        //     that has just gone out of band is somewhere mid-slide between the two.
        if (bins[i] <= NO_DATA_DBFS) continue;
        count++;
        if (db > absoluteMax) absoluteMax = db;
        let b = (db + 280) | 0; // -280..+19 dB → bucket 0..299
        if (b < 0) b = 0; else if (b > 299) b = 299;
        hist[b]++;
      }
      if (count > 0) {
        const target = Math.floor(count * NOISE_PERCENTILE);
        let acc = 0, floorDb = -120;
        for (let b = 0; b < 300; b++) {
          acc += hist[b];
          if (acc > target) { floorDb = b - 280; break; }
        }
        const targetMin = Math.floor(floorDb - RANGE_MARGIN);
        // Ceiling = the top PEAK_EXCLUDE_FRAC of bins discarded, so a one-bin retune
        // spike cannot set the scale. Falls back to the true maximum when the view is
        // too narrow for the fraction to mean anything (fewer bins than it excludes).
        const drop = Math.floor(count * PEAK_EXCLUDE_FRAC);
        let peakDb = absoluteMax;
        if (drop >= 1) {
          let accTop = 0;
          for (let b = 299; b >= 0; b--) {
            accTop += hist[b];
            if (accTop > drop) { peakDb = b - 280; break; }
          }
        }
        const targetMax = Math.ceil(peakDb + RANGE_MARGIN);

        // Ceiling ALWAYS tracks the strongest bin in view — a signal that appears
        // as you zoom/tune in must still set the top of the scale.
        // In-place history prune (the .filter() pair allocated two arrays per
        // frame); entries are time-ordered so expired ones sit at the front.
        const maxs = this.maxHistory;
        maxs.push({ value: targetMax, ts: now });
        while (maxs.length && now - maxs[0].ts > MAX_HISTORY_MS) maxs.shift();
        let sumMax = 0; for (let i = 0; i < maxs.length; i++) sumMax += maxs[i].value;
        const wantMaxDb = sumMax / maxs.length - s.autoContrast;

        // ★★ SLEW-LIMIT THE TOP OF THE RANGE. The rolling average STEPS as
        // pre-tune samples age out of the window, and because the top of the
        // range sets the palette mapping, the whole waterfall visibly DIMS and
        // then recovers on every tune — worse when landing on a strong carrier,
        // which raises the max sharply (Stuart, 2026-07-26).
        //
        // ★ The settled appearance is UNCHANGED: this only bounds how fast the
        // value may travel, so it ramps over a few hundred ms instead of
        // stepping. The alternative — clearing the history on a view change —
        // trades several small jumps for one large one, which is not better.
        if (!this.lastMaxAt || !Number.isFinite(this.actualMaxDb) || this.adoptRange) {
          // ★ First frame of a session adopts OUTRIGHT — slewing from the initial
          // value would make the waterfall fade in on every connect.
          this.actualMaxDb = wantMaxDb;
        } else {
          const dt = this.lastMaxAt ? Math.min(0.25, (now - this.lastMaxAt) / 1000) : 0.05;
          const maxStep = MAX_SLEW_DB_PER_SEC * dt;        // dB this frame may move
          const delta = wantMaxDb - this.actualMaxDb;
          this.actualMaxDb += Math.abs(delta) <= maxStep ? delta : Math.sign(delta) * maxStep;
        }
        this.lastMaxAt = now;

        // FLOOR FREEZE. The 10th-percentile floor is only a real NOISE floor when the
        // view is wide enough to contain noise. Zoomed into a busy band, every visible
        // bin is signal, the percentile climbs into it, and the whole waterfall rides up
        // to white. Past FLOOR_FREEZE_SPAN_HZ we HOLD the last floor measured while a real
        // floor was in view (frozenFloorDb), so dynamic range is preserved. A band change
        // (0c) clears it, so tuning ONTO a strong broadcast re-derives instead of sitting
        // on a stale low floor — Stuart's caveat. When frozen we also stop pushing the
        // (signal-polluted) mins into history, so zooming back out recovers cleanly.
        const floorTrust = !(bwHz > 0 && bwHz < FLOOR_FREEZE_SPAN_HZ);
        if (floorTrust || this.frozenFloorDb === null) {
          const mins = this.minHistory;
          mins.push({ value: targetMin, ts: now });
          while (mins.length && now - mins[0].ts > MIN_HISTORY_MS) mins.shift();
          let sumMin = 0; for (let i = 0; i < mins.length; i++) sumMin += mins[i].value;
          const avgMin = sumMin / mins.length;
          this.actualMinDb = this.slewFloor(avgMin + s.autoContrast, now);
          if (floorTrust) this.frozenFloorDb = avgMin;   // remember the trustworthy floor
        } else {
          // Zoomed in: hold the LOCAL floor (this frame's targetMin) so a quieter sub-band doesn't
          // leave a dead flat band below the noise — but CAP it at frozenFloorDb + FLOOR_CLIMB_MAX so
          // a busy all-signal zoom (where the 10th percentile has climbed into the signal) can't ride
          // the scale up to white. Previously this hard-held frozenFloorDb, which is what produced the
          // large flat area when zoomed into an active-but-not-full band (Stuart, 2026-07-24).
          const cappedFloor = Math.min(targetMin, this.frozenFloorDb + FLOOR_CLIMB_MAX);
          this.actualMinDb = this.slewFloor(cappedFloor + s.autoContrast, now);
        }
        // Both ends have taken the post-view-change measurement; slew normally again.
        this.adoptRange = false;
      }
    }
    // MINIMUM WINDOW, ANCHORED AT THE FLOOR. On a quiet/no-signal band the ceiling (strongest bin)
    // sits just above the noise, so the window collapses — and the OLD guard CENTRED a 10 dB window,
    // which RAISED THE FLOOR up into the noise: the noise then filled the whole screen, peaks blew up,
    // and the waterfall went bright ("the whole floor climbs", Stuart 2026-07-24). Instead keep the
    // floor at the noise and raise only the CEILING to a minimum span, so noise stays low with headroom
    // above it for a signal to rise into, and the waterfall maps that noise dark again. MIN_RANGE_DB is
    // the one dial — bigger = noise sits lower / more headroom.
    if (this.actualMaxDb - this.actualMinDb < MIN_RANGE_DB) {
      this.actualMaxDb = this.actualMinDb + MIN_RANGE_DB;
    }
    const dbRange = this.actualMaxDb - this.actualMinDb;

    // ── 3. (was: waterfall temporal EMA) — now done in step 1a (avgFrames), so
    //       this.dbAvg already holds the processed signal. Nothing to do here.

    // ── 4. Spatial 5-tap smooth [1,2,3,2,1]/9 (waterfall only) ──────────────
    const tmp = this.tmp!;
    const a = this.dbAvg;
    if (s.spatialSmooth && n >= 5) {
      tmp[0]     = (a[0] * 3 + a[1] * 2) / 5;
      tmp[1]     = (a[0] + a[1] * 2 + a[2] * 2) / 5;
      tmp[n - 1] = (a[n - 2] * 2 + a[n - 1] * 3) / 5;
      tmp[n - 2] = (a[n - 3] + a[n - 2] * 2 + a[n - 1] * 2) / 5;
      for (let k = 2; k < n - 2; k++) {
        tmp[k] = (a[k - 2] + a[k - 1] * 2 + a[k] * 3 + a[k + 1] * 2 + a[k + 2]) / 9;
      }
    } else {
      tmp.set(a);
    }

    // ── 5. Spectrum EMA — time-normalised, rise 4× faster than fall ─────────
    const spec = this.specSmooth!;
    const noSmooth  = s.smoothingFrames <= 1;
    const tcSec     = noSmooth ? 0 : (s.smoothingFrames - 1) / 20;
    const fallAlpha = noSmooth ? 1.0
      : Math.min(0.95, 1.0 - Math.exp(-dtSec / Math.max(0.01, tcSec)));
    const riseAlpha = noSmooth ? 1.0 : Math.min(0.95, fallAlpha * 4);

    // ── 6. Peak hold seed (only once spec has signal) ────────────────────────
    if (s.peakHold && !this.peakLine) {
      let hasSignal = false;
      for (let i = 0; i < n; i++) if (spec[i] !== 0) { hasSignal = true; break; }
      if (hasSignal) this.peakLine = new Float32Array(spec);
    }
    const pk = this.peakLine;

    // ── 5/6/7 combined per-bin loop (matches v1.5 Pass 3) ───────────────────
    const norm = this.normRow!;
    const brightDb = s.wfBrightness; // dB-domain brightness (equivalent to shader u_bright)
    for (let j = 0; j < n; j++) {
      // Spectrum EMA
      const ta = tmp[j] > spec[j] ? riseAlpha : fallAlpha;
      spec[j] += ta * (tmp[j] - spec[j]);
      // Peak hold: rise to current, else 10 dB/s decay
      if (s.peakHold && pk) {
        const cur = spec[j];
        pk[j] = cur > pk[j] ? cur : pk[j] - PEAK_DECAY_DB_PER_S * dtSec;
      }
      // Waterfall: normalise (+brightness), clip floor, re-stretch
      const nrm = Math.max(0, Math.min(1, (tmp[j] + brightDb - this.actualMinDb) / dbRange));
      let mag = nrm * 255;
      mag = mag < CLIP_THRESHOLD ? 0 : ((mag - CLIP_THRESHOLD) / (255 - CLIP_THRESHOLD)) * 255;
      norm[j] = mag / 255;
    }

    // ── 8. Row output = NORMALISED INTENSITY bytes (GPU waterfall, 2026-06-11)
    // Unsharp, S-curve contrast and the LUT lookup moved into the SkSL
    // runtime shader (WaterfallView) — the v1 WebGL architecture. `row` is no
    // longer LUT indices: it feeds a Gray_8 intensity texture, and sharpness/
    // contrast/palette now apply LIVE to the whole history as uniforms.
    const out = this.outRow!;
    for (let j = 0; j < n; j++) {
      out[j] = Math.max(0, Math.min(255, Math.round(norm[j] * 255)));
    }

    // ── Normalised spectrum / peak outputs ───────────────────────────────────
    const oSpec = this.outSpec!;
    const oPeak = this.outPeak!;
    const sf = dbRange > 0 ? s.specFloor / dbRange : 0;
    const sp = s.specPeakScale / 10;
    for (let j = 0; j < n; j++) {
      const ns = Math.max(0, Math.min(1, (spec[j] - this.actualMinDb) / dbRange));
      oSpec[j] = Math.max(0, Math.min(1, (ns + sf) * sp));
      if (s.peakHold && pk) {
        const np = Math.max(0, Math.min(1, (pk[j] - this.actualMinDb) / dbRange));
        oPeak[j] = Math.max(0, Math.min(1, (np + sf) * sp));
      } else {
        oPeak[j] = 0;
      }
    }

    return {
      row:   out,
      spec:  oSpec,
      peak:  oPeak,
      dbMin: this.actualMinDb,
      dbMax: this.actualMaxDb,
    };
  }

  /** Clear ONLY the peak-hold line — used while the view is moving (tune/
   *  pan/zoom): peaks are bin-indexed, so they detach from their signals the
   *  moment the bins' frequencies shift. It re-seeds from live data on the
   *  next settled frame. */
  resetPeakHold() {
    this.peakLine = null;
  }

  /** Full reset (reconnect / instance change). */
  reset() {
    this.dbAvg = null;
    this.specSmooth = null;
    this.peakLine = null;
    this.minHistory = [];
    this.maxHistory = [];
    this.lastFrameMs = 0;
    this.prevCenterHz = 0;
  }
}
