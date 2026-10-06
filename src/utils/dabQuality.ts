/**
 * dabQuality — how well a DAB multiplex is RECEIVED, as three bars and a sentence (app + web).
 *
 * ★★★ WHY (Stuart, 2026-10-06): a listener in the US spent an hour on the Pi 2 tuned to the Coventry
 *     multiplex. The station list loaded, the errors before Viterbi were far too high, and no audio ever
 *     started — a station that LOOKS tuned and is silent, which reads as a broken app. The ordinary signal
 *     bar said S9 the whole time: in DAB it measures power in the passband, not whether the multiplex can be
 *     decoded. His brief: "upon switching to DAB mode the signal bar changes to a 2nd text bar … No Signal /
 *     Multiplex Weak: No or heavily broken audio / Multiplex moderate: Expect occasional audio breakups /
 *     Multiplex strong — That way a user doesn't see a multiplex that should work and wonder why it is silent."
 *
 * ★★ ONE RULE, ONE COPY: the app (SDRScreen → ControlsBar) and the web client (web/client/src/main.ts) both
 *    import this file. scripts/test_dab_quality.ts holds it down.
 *
 * ★★ NOTHING HERE IS INVENTED. Every input is a field the server already sends in its `dab` report
 *    (src/services/dabTypes.ts) and the ADVANCED rows of DabPanel already show; the thresholds are DabPanel's
 *    own ok / warn / bad lines, except where a measurement on air said otherwise (noted at each one).
 *
 * ★★ THE COUNTERS ARE CUMULATIVE (super frames, Layer II frames, erased frames), so a session average would
 *    describe the last ten minutes, not now. The meter keeps ~5 s of reports and judges the DIFFERENCE across
 *    them. The server zeroes the audio counters on a service change (vibe_dab_service.h resetAudioCounters) —
 *    a counter that goes BACKWARDS starts the window again, as does a new multiplex.
 *
 * ★★ AUDIO EVIDENCE OUTRANKS PREDICTION. Before a service is decoding, the level is predicted from lock, the
 *    FIC, MER and the raw bit error rate. Once super frames / Layer II frames are arriving, what happened to
 *    THEM is the answer — measured 2026-09-15 on 10D: two radios at the same MER 9 and ~9 % raw BER, one with
 *    0 erased frames and one with 13–18 %. The averages agreed and the audio did not, because the errors were
 *    bursty. The listener hears the audio channel, so the audio channel decides.
 */

/** The report fields the meter reads — a subset of DabState, so either client can pass its own copy. */
export interface DabQualityReport {
  channel?: string;
  locked?: boolean;
  fibOk?: number; fibTotal?: number; fibRate?: number;
  mer?: number;
  /** Raw MSC bit error rate before Viterbi, 0..1 — meaningful only while a service is selected (`sid`). */
  mscBer?: number; sid?: number;
  frames?: number; erased?: number;
  mp2In?: number; mp2Bad?: number;
  sfTried?: number; sfOk?: number;
  rsFixed?: number; rsLost?: number;
}

export type DabLevel = 0 | 1 | 2 | 3;

export interface DabQuality {
  level: DabLevel;
  /** "No signal" / "Multiplex weak" / "Multiplex moderate" / "Multiplex strong". */
  label: string;
  /** One word for a narrow reading box: "No signal" / "Weak" / "Moderate" / "Strong". */
  short: string;
  /** What the listener should expect to HEAR — the half of the sentence that explains a silence. */
  advice: string;
  /** The error figure behind the verdict, when there is one worth showing: "14 % frames lost", "BER 4.1 %". */
  detail?: string;
}

const LABEL: Record<DabLevel, string> = {
  0: 'No signal', 1: 'Multiplex weak', 2: 'Multiplex moderate', 3: 'Multiplex strong',
};
const SHORT: Record<DabLevel, string> = { 0: 'No signal', 1: 'Weak', 2: 'Moderate', 3: 'Strong' };
const ADVICE: Record<DabLevel, string> = {
  0: 'Searching for the multiplex',
  1: 'No or heavily broken audio',
  2: 'Expect occasional audio break-ups',
  3: 'Clear audio',
};

/** ★ Shown between entering DAB (or changing multiplex) and the first report — never the last mux's verdict. */
export const DAB_SEARCHING: DabQuality = {
  level: 0, label: LABEL[0], short: SHORT[0], advice: ADVICE[0],
};

/** How much history the meter judges. ~5 reports at the server's one a second. */
export const DAB_WINDOW_MS = 5000;

/* ── Thresholds ─────────────────────────────────────────────────────────────────────────────────────────
 * ★ DabPanel.tsx's ADVANCED tones, as the brief asks, with the on-air evidence that checks each one.
 *   All measurements are from the memory notes of the DAB repair sessions (2026-09-07 → 09-21); no new
 *   capture was replayed for this (none is on the Mac — they live on the Pi, which this work must not touch).
 */
/** DAB+ super frames OK: DabPanel ok > 98 %, bad ≤ 90 %. ✓ 7D at 2.7 % erased frames lost ~20 % → weak;
 *  the Pi 2's random wire at 95.7 % (1063/1111) is audibly imperfect but listenable → moderate. */
export const SF_OK_STRONG = 0.98, SF_OK_WEAK = 0.90;
/** Layer II frames bad: DabPanel ok < 1 %, bad ≥ 10 %. ✓ 9A at MER 8.5: 19 % bad, "bubbling mud" → weak;
 *  12B on the Pi V4L: 15–30 bad in ~10 000 (0.15–0.3 %) → strong. */
export const MP2_BAD_STRONG = 0.01, MP2_BAD_WEAK = 0.10;
/** Raw MSC BER before Viterbi. DabPanel bad ≥ 3 % — ✓ every on-air case at 8.7–9.6 % (MER 8.5–9.6) broke up.
 *  ★ The MODERATE line is 1.5 %, NOT DabPanel's 0.5 %: 12B at 0.5 % and 11D at 0.8 % played 0 bad frames in
 *    eight minutes (2026-09-16), so 0.5 % would have called a perfect multiplex "moderate". */
export const BER_MODERATE = 0.015, BER_WEAK = 0.03;
/** MER (dB): DabPanel ok ≥ 16, bad < 10. ✓ MER 8.5–9.6 broke up every time; 12B at 16.1 played clean. */
export const MER_STRONG = 16, MER_WEAK = 10;
/** FIC pass rate: DabPanel ok ≥ 99 %, bad < 90 %. ✓ 10C at 23–55 % never decoded; FIB dipped to 88–95 %
 *  exactly when Layer II went bad (2026-09-07). ★ The server's own no-FIC watchdog line is 5 %. */
export const FIB_STRONG = 0.99, FIB_WEAK = 0.90, FIB_NONE = 0.05;
/* ★ NULL DEPTH IS NOT JUDGED, deliberately. DabPanel colours it (bad < 8 dB), but 9A on the Pi decoded its
 *  whole station list at 4.5 dB (2026-09-08 — "below the old 6 dB gate, the whole story"): it says how deep
 *  the null symbol dips, not whether the audio survives. MER and the FIC already carry the same news. */
/** Frames erased (whole CIFs lost before the audio decoder): DabPanel ok < 2 %, bad ≥ 10 %. Used only while
 *  no audio counters are moving — once they are, the frames they lost already include these. */
export const ERASED_MODERATE = 0.02, ERASED_WEAK = 0.10;
/** Enough audio to judge on: ~0.5 s of DAB+ super frames (120 ms each), ~0.5 s of Layer II (24 ms each). */
const MIN_SF = 4, MIN_MP2 = 20;
/** ★ Hysteresis: a new level must be seen on this many consecutive reports before the bars move. Down is
 *  quicker than up — a multiplex that has started to break up should say so promptly, one that has
 *  recovered should prove it. At one report a second: 2 s down, 3 s up. */
export const HOLD_DOWN = 2, HOLD_UP = 3;

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const pct = (f: number) => {
  const p = f * 100;
  return p >= 10 ? `${Math.round(p)}` : p >= 1 ? p.toFixed(1).replace(/\.0$/, '') : p.toFixed(1);
};

interface Sample {
  t: number;
  frames?: number; erased?: number; mp2In?: number; mp2Bad?: number; sfTried?: number; sfOk?: number;
  fibRate?: number; mer?: number; mscBer?: number;
}

/** The window's evidence, from its oldest sample to its newest. Exported for the tests. */
export interface DabWindow {
  locked: boolean;
  /** Mean FIC pass rate over the window, and the latest. */
  fibRate?: number; fibNow?: number;
  mer?: number; mscBer?: number;
  sfTried: number; sfOk: number; mp2In: number; mp2Bad: number; frames: number; erased: number;
}

/** ★ The verdict for one window, with no memory — the pure core the meter wraps in hysteresis. */
export function classifyDabWindow(w: DabWindow): { level: DabLevel; detail?: string } {
  if (!w.locked) return { level: 0, detail: 'searching' };
  // ★ Locked but no FIC is the server's own "false lock" (vibe_dab_service.h noFib): nothing will decode.
  if (w.fibNow !== undefined && w.fibNow < FIB_NONE) return { level: 0, detail: 'no station list' };

  // ── Audio evidence: what happened to the frames the listener would hear ──
  if (w.sfTried >= MIN_SF) {
    const lost = 1 - Math.min(w.sfOk, w.sfTried) / w.sfTried;
    const level: DabLevel = 1 - lost <= SF_OK_WEAK ? 1 : 1 - lost <= SF_OK_STRONG ? 2 : 3;
    return { level, detail: lost > 0 ? `${pct(lost)} % frames lost` : undefined };
  }
  if (w.mp2In >= MIN_MP2) {
    const bad = Math.min(w.mp2Bad, w.mp2In) / w.mp2In;
    const level: DabLevel = bad >= MP2_BAD_WEAK ? 1 : bad >= MP2_BAD_STRONG ? 2 : 3;
    return { level, detail: bad > 0 ? `${pct(bad)} % frames bad` : undefined };
  }

  // ── Prediction: no audio decoding yet (no service, or one just picked) ──
  // The worst of what we can see, each with the figure that put it there.
  let level: DabLevel = 3; let detail: string | undefined;
  const worse = (l: DabLevel, d: string) => { if (l < level) { level = l; detail = d; } };
  if (w.fibRate !== undefined) {
    const f = w.fibRate;
    if (f < FIB_WEAK) worse(1, `FIC ${pct(1 - f)} % errors`);
    else if (f < FIB_STRONG) worse(2, `FIC ${pct(1 - f)} % errors`);
  }
  if (w.mer !== undefined && w.mer > 0) {
    if (w.mer < MER_WEAK) worse(1, `MER ${w.mer.toFixed(1)} dB`);
    else if (w.mer < MER_STRONG) worse(2, `MER ${w.mer.toFixed(1)} dB`);
  }
  if (w.mscBer !== undefined) {
    if (w.mscBer >= BER_WEAK) worse(1, `BER ${pct(w.mscBer)} %`);
    else if (w.mscBer >= BER_MODERATE) worse(2, `BER ${pct(w.mscBer)} %`);
  }
  if (w.frames >= 10) {
    const e = Math.min(w.erased, w.frames) / w.frames;
    if (e >= ERASED_WEAK) worse(1, `${pct(e)} % frames erased`);
    else if (e >= ERASED_MODERATE) worse(2, `${pct(e)} % frames erased`);
  }
  return { level, detail };
}

/**
 * ★ The meter: feed it every `dab` report; it keeps the window and the hysteresis. One per client.
 *   `reset()` on leaving DAB — no verdict may outlive the mode (the RC15 Exit DAB lesson).
 */
export class DabQualityMeter {
  private samples: Sample[] = [];
  private channel: string | undefined;
  private shown: DabLevel | null = null;
  private pending: { level: DabLevel; n: number } | null = null;
  private last: DabQuality | null = null;

  reset(): void {
    this.samples = []; this.channel = undefined; this.shown = null; this.pending = null; this.last = null;
  }

  /** The current verdict without pushing anything (null before the first report). */
  current(): DabQuality | null { return this.last; }

  push(r: DabQualityReport, nowMs: number): DabQuality {
    const ch = typeof r.channel === 'string' ? r.channel : undefined;
    // ★ A new multiplex is a new receiver as far as this meter is concerned: nothing carries over.
    if (ch !== this.channel) { this.reset(); this.channel = ch; }

    const sid = num(r.sid) ?? 0;
    const s: Sample = {
      t: nowMs,
      frames: num(r.frames), erased: num(r.erased), mp2In: num(r.mp2In), mp2Bad: num(r.mp2Bad),
      sfTried: num(r.sfTried), sfOk: num(r.sfOk),
      fibRate: num(r.fibRate), mer: num(r.mer),
      // ★ DabPanel shows the BER only with a service selected — the server's figure is the MSC's.
      mscBer: sid ? num(r.mscBer) : undefined,
    };
    // ★ A counter that went BACKWARDS was reset by the server (a service change): the older samples
    //   describe another station, so the window starts again from this one.
    const prev = this.samples[this.samples.length - 1];
    if (prev && (['frames', 'erased', 'mp2In', 'mp2Bad', 'sfTried', 'sfOk'] as const)
        .some(k => prev[k] !== undefined && s[k] !== undefined && (s[k] as number) < (prev[k] as number))) {
      this.samples = [];
    }
    this.samples.push(s);
    // Keep the newest sample at or beyond the window's far edge, so a full window is always spanned.
    while (this.samples.length > 2 && nowMs - this.samples[1].t >= DAB_WINDOW_MS) this.samples.shift();

    const a = this.samples[0], b = s;
    const d = (k: 'frames' | 'erased' | 'mp2In' | 'mp2Bad' | 'sfTried' | 'sfOk') => {
      const x = a[k], y = b[k];
      if (y === undefined) return 0;
      // ★ A single sample (window just started): its own count is all there is — the server zeroed
      //   these at the service change, so it IS the recent history.
      if (this.samples.length === 1 || x === undefined) return y;
      return Math.max(0, y - x);
    };
    const mean = (k: 'fibRate' | 'mer' | 'mscBer') => {
      const v = this.samples.map(x => x[k]).filter((x): x is number => x !== undefined);
      return v.length ? v.reduce((p, q) => p + q, 0) / v.length : undefined;
    };
    const win: DabWindow = {
      locked: !!r.locked,
      fibRate: mean('fibRate'), fibNow: s.fibRate,
      mer: mean('mer'), mscBer: mean('mscBer'),
      sfTried: d('sfTried'), sfOk: d('sfOk'), mp2In: d('mp2In'), mp2Bad: d('mp2Bad'),
      // ★ frames/erased are the multiplex's, never zeroed with a service — but a fresh window has no
      //   history for them, and a session total is exactly what this meter must not judge on.
      frames: this.samples.length > 1 ? d('frames') : 0, erased: this.samples.length > 1 ? d('erased') : 0,
    };
    const v = classifyDabWindow(win);

    // ── Hysteresis ──
    if (this.shown === null) this.shown = v.level;
    else if (v.level !== this.shown) {
      this.pending = this.pending && this.pending.level === v.level
        ? { level: v.level, n: this.pending.n + 1 } : { level: v.level, n: 1 };
      if (this.pending.n >= (v.level < this.shown ? HOLD_DOWN : HOLD_UP)) {
        this.shown = v.level; this.pending = null;
      }
    } else this.pending = null;

    const lvl = this.shown;
    // ★ The figure goes with the verdict ON SCREEN: while a change is being held, the detail of the new
    //   level would contradict the bars, so the held level shows its own kind of detail or none.
    const detail = lvl === v.level ? v.detail : undefined;
    this.last = {
      level: lvl, label: LABEL[lvl], short: SHORT[lvl],
      advice: lvl === 0 && v.level === 0 && v.detail === 'no station list' ? 'Locked, but nothing decodes' : ADVICE[lvl],
      ...(detail && lvl !== 0 ? { detail } : {}),
    };
    return this.last;
  }
}

/** The one-line text the bar shows: "Multiplex weak · No or heavily broken audio · 14 % frames lost". */
export function dabQualityLine(q: DabQuality, withDetail = true): string {
  return [q.label, q.advice, withDetail ? q.detail : undefined].filter(Boolean).join(' · ');
}
