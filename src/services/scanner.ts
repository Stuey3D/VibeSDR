/**
 * ★★★ THE SCANNER ENGINE (docs/v12/SCANNER.md — Stuart's BRIEF-scanner.md brought up to date for V12).
 *
 * Three ways to scan, ONE engine:
 *   · BAND      — a traditional search: Lo / Hi / step / mode, Start, leave it. Stops on a signal, resumes when it
 *                 drops, and every stop lands in the FOUND list with a plain IGNORE beside it (Stuart, 2026-10-27:
 *                 "it builds a list of found frequencies with a simple ignore button, so that permanent noise spikes
 *                 or VOLMET cannot trap it").
 *   · BOOKMARKS — hop a bookmark group.
 *   · MANUAL    — THE QSO HOPPER, the headline: tag a conversation (a full VFO snapshot — frequency, mode, passband —
 *                 not a channel number), tag another, across bands and demodulators; the scanner dwells on whichever
 *                 is active.
 *
 * ★★★ COMPLEXITY LIVES IN THE ENGINE, NEVER IN THE SETTINGS (brief §1.1). The threshold defaults to AUTO (noise floor +
 *     margin); it is expressed in whatever units the user's own meter shows; exactly THREE live controls while scanning
 *     — Hold, Skip, Ignore; priority is one star; recording is one tick.
 *
 * ★★ PURE AND CLOCK-DRIVEN. Nothing here touches React, a socket or a timer: the screen (hooks/useScanner) hands it a
 *    host — tune, the audio gate, where it may tune, the clock — and calls `feed()` with every meter sample and `tick()`
 *    on a short interval. So every behaviour below is tested with a fake host and a fake clock
 *    (scripts/test_scanner.ts), against fake backends of every family.
 *
 * ★★ M1 = PROBE COVERAGE ONLY (brief §14). Every activity decision is made ON TUNE: hop, settle, sample, decide. M2
 *    (passive — from the spectrum the client already draws) and M3 (local IQ / the server's watch channels) only ever
 *    UPGRADE a tag's coverage; this path remains the permanent fallback, so it has to be right, not provisional.
 */

import type { SDRMode } from './sdrProtocol.ts';
import { MODE_BANDWIDTHS } from './sdrProtocol.ts';
import { BAND_PLAN, type Band, type BandType } from '../constants/bandPlan.ts';
import { S9_DB, DB_PER_S_UNIT } from '../constants/meters.ts';
import { STEP_833, stepExactHz, stepIndex, stepHz as rasterHz } from '../utils/airband.ts';

// ═══ Data model (brief §2) ═════════════════════════════════════════════════════════════════════════════════════════

/** One scan-list entry. A full VFO snapshot, NOT just a frequency. */
export interface ScanTag {
  id:          string;
  frequency:   number;        // Hz
  mode:        SDRMode;
  bwLow:       number;        // passband edges at tag time (Hz, signed as elsewhere)
  bwHigh:      number;
  label?:      string;
  /** ★★ The noise floor where this tag lives, captured AT TAG TIME from a reference the signal cannot be in (the
   *  neighbour windows — squelchNeighbours.ts). In the meter's LEVEL units (dBFS / dBm). The Auto threshold for the tag
   *  is this + margin, so a tag made during the strong station's over still holds the stop when the weak one speaks. */
  noiseRefDb?: number;
  priority?:   boolean;       // ★ starred (§6.4)
  /** ★ Ignored — passed over by the scan (the brief's "lockout"; one word in the UI, Stuart 2026-10-27: "Ignore"). */
  lockedOut?:  boolean;
}

/** ★★ WHAT MOVES THE SCAN ON (Stuart, 2026-10-27: "the scanners need triggers for moving to the next point, so the dwell
 *  on the signal until it stops, and/or a time limit per signal"). TWO INDEPENDENT TICKS, not a mode picker:
 *    ☑ Until the signal stops  (+ hang)       — the brief's `activity`
 *    ☐ Time limit per signal  (+ seconds)     — alone: the brief's `timed`; with the first: whichever comes first
 *                                                (the brief's activity capped by maxDwell — the VOLMET escape)
 *  At least one is always on (normaliseConfig). */
export interface ScannerConfig {
  untilStops:     boolean;
  /** ★★★ THE CRITICAL KNOB FOR SSB. Speech is bursty and an over-change takes seconds: the signal falling below the
   *  threshold only STARTS this timer, and the signal coming back before it expires CANCELS it. That is the whole
   *  difference between a usable SSB scanner and a useless one. Default 5 s, 1–15 s. */
  hangMs:         number;
  timeLimit:      boolean;
  timeLimitMs:    number;
  thresholdMode:  'auto' | 'manual';
  /** ★ Manual threshold, stored PER METER UNIT FAMILY (§5.2). SNR does not convert to dBFS, so a number is never
   *  reinterpreted across units: changing the meter re-seeds from that family's own last value. */
  threshold:      { snr: number; dbfs: number };
  marginDb:       number;
  recordOnStop:   boolean;
  /** ★ Band-scan ignores (the brief's lockouts): `|f − frequency| ≤ halfWidthHz` is passed over. */
  lockouts:       { frequency: number; halfWidthHz: number }[];
}

export type ScanSource = 'band' | 'bookmarks' | 'list';

export interface BandSpec { loHz: number; hiHz: number; stepHz: number; mode: SDRMode }

/** Persistent named scan (§10). */
export interface ScanPreset {
  id:       string;
  name:     string;
  source:   ScanSource;
  config:   ScannerConfig;
  band?:    BandSpec;
  bookmarkGroup?: string;
  channels?: ScanTag[];
  /** ★ Bundled starter (read-only — clone, then edit). */
  bundled?: boolean;
}

/** Band-scan discovery log entry (§6.1). Session-scoped. */
export interface FoundSignal {
  frequency:  number;
  mode:       SDRMode;
  firstHeard: number;
  lastHeard:  number;
  hits:       number;     // times stopped here
  peakMetric: number;     // in the meter's units at the time
  dwellMs:    number;     // total time stopped here
  /** How many stops here ended on the TIME LIMIT rather than the signal stopping — a continuous carrier's signature. */
  capped:     number;
}

export const DEFAULT_CONFIG: ScannerConfig = {
  untilStops: true, hangMs: 5000,
  timeLimit: false, timeLimitMs: 30_000,
  thresholdMode: 'auto', threshold: { snr: 10, dbfs: -100 }, marginDb: 6,
  recordOnStop: false, lockouts: [],
};

export const HANG_MIN_MS = 1000, HANG_MAX_MS = 15_000;
export const LIMIT_MIN_MS = 2000, LIMIT_MAX_MS = 600_000;

/** ★ The brief's July config (`advance` + `maxDwellMs` + `timedDwellMs`) → the two ticks. Presets saved by an early
 *  build, and anything else carrying the old shape, read correctly; a missing field takes its default. */
export function normaliseConfig(raw: unknown): ScannerConfig {
  const r = (raw && typeof raw === 'object' ? raw : {}) as Record<string, any>;
  const c: ScannerConfig = { ...DEFAULT_CONFIG, threshold: { ...DEFAULT_CONFIG.threshold }, lockouts: [] };
  if (r.advance === 'timed') {
    c.untilStops = false; c.timeLimit = true;
    if (Number.isFinite(r.timedDwellMs)) c.timeLimitMs = r.timedDwellMs;
  } else if (r.advance === 'activity') {
    c.untilStops = true;
    c.timeLimit = Number(r.maxDwellMs) > 0;
    if (c.timeLimit) c.timeLimitMs = Number(r.maxDwellMs);
  }
  if (typeof r.untilStops === 'boolean') c.untilStops = r.untilStops;
  if (typeof r.timeLimit === 'boolean') c.timeLimit = r.timeLimit;
  if (Number.isFinite(r.timeLimitMs)) c.timeLimitMs = r.timeLimitMs;
  if (Number.isFinite(r.hangMs)) c.hangMs = r.hangMs;
  if (r.thresholdMode === 'manual' || r.thresholdMode === 'auto') c.thresholdMode = r.thresholdMode;
  if (r.threshold && typeof r.threshold === 'object') {
    if (Number.isFinite(r.threshold.snr)) c.threshold.snr = r.threshold.snr;
    if (Number.isFinite(r.threshold.dbfs)) c.threshold.dbfs = r.threshold.dbfs;
  } else if (Number.isFinite(r.thresholdValue)) {
    // ★ The brief's single number had no unit of its own: it can only be trusted as the family it was SAVED in, which
    //   the old shape never recorded. Dropped, rather than guessed — Auto is the default anyway.
  }
  if (Number.isFinite(r.marginDb)) c.marginDb = r.marginDb;
  if (typeof r.recordOnStop === 'boolean') c.recordOnStop = r.recordOnStop;
  if (Array.isArray(r.lockouts)) {
    c.lockouts = r.lockouts
      .filter((l: any) => l && Number.isFinite(l.frequency) && Number.isFinite(l.halfWidthHz))
      .map((l: any) => ({ frequency: l.frequency, halfWidthHz: Math.max(0, l.halfWidthHz) }));
  }
  c.hangMs = clamp(c.hangMs, HANG_MIN_MS, HANG_MAX_MS);
  c.timeLimitMs = clamp(c.timeLimitMs, LIMIT_MIN_MS, LIMIT_MAX_MS);
  c.marginDb = clamp(c.marginDb, 1, 30);
  // ★ At least one trigger: a scan with neither would stop on the first signal and never move again.
  if (!c.untilStops && !c.timeLimit) c.untilStops = true;
  return c;
}

/** ★ Untick the last trigger → the other turns on (Stuart's rule for the two ticks). */
export function setTrigger(c: ScannerConfig, which: 'untilStops' | 'timeLimit', on: boolean): ScannerConfig {
  const n = { ...c, [which]: on };
  if (!n.untilStops && !n.timeLimit) {
    if (which === 'untilStops') n.timeLimit = true; else n.untilStops = true;
  }
  return n;
}

function clamp(v: number, lo: number, hi: number): number { return Math.max(lo, Math.min(hi, v)); }

// ═══ Constants the engine tunes itself with (from the tests, never settings) ═════════════════════════════════════

/** ★★ SUSTAINED ACTIVITY BEFORE A STOP (Stuart: noise spikes must never stop it). A stop needs this many consecutive
 *  above-threshold samples spanning at least SUSTAIN_MS. A static crash or a single-frame blip is one or two frames;
 *  a voice, a carrier or an ATC call is many. */
export const SUSTAIN_N = 3;
export const SUSTAIN_MS = 200;
/** ★ A run that is still building when the sample window ends gets this much longer to prove itself, so a slow frame
 *  rate (a Pi 2 at 5 fps, OpenWebRX) cannot miss a real signal simply because three frames did not fit. */
export const SAMPLE_EXTEND_MS = 450;
/** ★ Once stopped, the signal must fall this far below the threshold to count as gone — no chatter at the edge. */
export const STOP_HYST_DB = 2;
/** ★ A starred tag gets a priority probe this often while the scanner sits on something else (§6.4). */
export const PRIORITY_MS = 5000;
/** ★ Skip passes the current frequency over for this long (Stuart: a SOFT ignore — "only annoying now"). */
export const SOFT_SKIP_MS = 180_000;
/** ★ The user turning the dial this far off a stop (for USER_TUNE_N samples) holds the scan where they are. */
export const USER_TUNE_TOL_HZ = 2000;
export const USER_TUNE_N = 3;
/** ★ A frequency becomes an "Ignore?" hint after this many stops, this much dwell, or this many time-limit escapes. */
export const HINT_HITS = 4;
export const HINT_DWELL_MS = 120_000;
export const HINT_CAPPED = 2;

// ═══ Per-backend timing and policy ═══════════════════════════════════════════════════════════════════════════════

export type ScanBackendKind = 'ubersdr' | 'vibeserver' | 'kiwi' | 'web888' | 'owrx' | 'fmdx';

export interface ScanTiming { settleMs: number; sampleMs: number }

/** ★ Brief §5.3, with VibeServer added: its per-listener channel reports the level its own gate reads (`sig.chan`)
 *  every spectrum frame, so it settles like UberSDR's pushed SNR. Initial values — tuned in testing. */
export function scanTiming(kind: ScanBackendKind, isLocal: boolean): ScanTiming {
  if (isLocal) return { settleMs: 100, sampleMs: 500 };
  switch (kind) {
    case 'ubersdr':    return { settleMs: 250, sampleMs: 300 };
    case 'vibeserver': return { settleMs: 250, sampleMs: 400 };
    case 'kiwi': case 'web888': return { settleMs: 400, sampleMs: 500 };
    case 'owrx':       return { settleMs: 500, sampleMs: 500 };
    default:           return { settleMs: 500, sampleMs: 500 };
  }
}

export interface ScanPolicyInput {
  kind: ScanBackendKind;
  isLocal: boolean;
  /** VibeServer: the server's `dial` says the one tuner is shared (mode "open"). */
  sharedDial?: boolean;
  /** VibeServer: the owner has pinned the centre — every listener has their own VFO inside the window. */
  lockedCentreHz?: number;
  /** VibeServer: the radio is an Icom (one physical receiver, the rig's own audio). */
  icom?: boolean;
}

export interface ScanPolicy {
  /** ★ No dead control (AGENTS.md): when false the SCANNER key is ABSENT, not greyed. */
  offered: boolean;
  why?: string;
  /** True when a scan retunes the hardware itself (only ever on a radio that is this listener's alone). */
  movesHardware: boolean;
}

/**
 * ★★★ WHERE A SCANNER MAY RUN AT ALL — one place, so the key and the engine cannot disagree.
 *
 *  · FM-DX — NEVER (brief §3, by the platform's nature): one physical tuner tuned for every listener at once.
 *  · OpenWebRX — yes, inside the active profile only; the scanner can never switch a profile (§4.4).
 *  · UberSDR, KiwiSDR — yes: every listener has their own channel on a wideband receiver; nothing moves for anyone else.
 *  · VibeServer, range-locked (the owner pinned the centre) — yes, inside the window: every listener has their own VFO
 *    there, the hardware never moves (the shared-dial contract's clamp — the same refuse/PAUSED path as OWRX's profile).
 *  · VibeServer, ONE shared dial (dial mode "open") — NOT OFFERED: a hop would retune every listener. Stuart's DAB-scan
 *    rule (2026-10-27): "on shared user servers I won't enable scanning as that would be antisocial". Absent, not greyed.
 *    (SCANNER.md proposal B2: once VFO A/B's watch channels land, a scan could run on this listener's WATCH channel
 *    and leave the shared dial alone.)
 *  · VibeServer single-user, or local hardware — yes, and here it may retune the hardware: the radio is this listener's.
 *  · An Icom — NOT in M1 (proposal B3): a CI-V retune per hop is untested and the rig may be in its owner's hands.
 */
export function scanPolicy(p: ScanPolicyInput): ScanPolicy {
  if (p.kind === 'fmdx') return { offered: false, why: 'An FM-DX receiver is one tuner shared by every listener', movesHardware: false };
  if (p.isLocal) return { offered: true, movesHardware: true };
  if (p.kind === 'vibeserver') {
    if (p.icom) return { offered: false, why: 'Scanning an Icom is not supported yet', movesHardware: false };
    if ((p.lockedCentreHz ?? 0) > 0) return { offered: true, movesHardware: false };
    if (p.sharedDial) return { offered: false, why: 'This radio has one shared dial — scanning would retune every listener', movesHardware: false };
    return { offered: true, movesHardware: true };
  }
  return { offered: true, movesHardware: false };
}

// ═══ Reach — where the scanner may tune, right now ═══════════════════════════════════════════════════════════════

/**
 * ★★★ ONE CLAMP, TWO RULES (OWRX's profile, VibeServer's shared-dial contract), ONE MECHANISM. The host answers this
 * from the backend on EVERY tick — the scanner never caches where it may go:
 *  · OWRX: the active profile's span (panSpan()); `key` carries the profile id, so another user switching profile is
 *    seen on the very next tick even when the new span happens to cover the same Hz.
 *  · VibeServer range-locked: the owner's locked window (utils/tuneWindow tuneBounds) — moving the pin moves the key.
 *  · Local hardware (movable): the whole tunable range (the dongle follows), keyed on the range, not on the moving view.
 *  · Everything else: caps.freqRange ∩ tuneBounds.
 * `allowed` false = the policy no longer allows a scan (e.g. the radio turned into a shared dial) → PAUSED with `why`.
 */
export interface ScanReach { allowed: boolean; why?: string; loHz: number; hiHz: number; key: string }

export function inReach(hz: number, r: ScanReach): boolean {
  return r.allowed && Number.isFinite(hz) && hz >= r.loHz && hz <= r.hiHz;
}

// ═══ Candidates ══════════════════════════════════════════════════════════════════════════════════════════════════

export interface ScanTarget {
  frequency: number;
  mode: SDRMode;
  bwLow: number;
  bwHigh: number;
  /** The tag's id (list modes); absent in a band scan. */
  tagId?: string;
  noiseRefDb?: number;
  priority?: boolean;
}

/** ★ The passband a band scan tunes with: the mode's default, never wider than the channel raster (an AM airband scan
 *  at 8.33 kHz must not hear its neighbours). */
export function bandPassband(mode: SDRMode, stepHz: number): [number, number] {
  const [lo, hi] = MODE_BANDWIDTHS[mode] ?? [-5000, 5000];
  const half = stepHz * 0.45;
  if (hi - lo <= stepHz * 0.9 || !(stepHz > 0)) return [lo, hi];
  if (lo >= 0) return [lo, Math.min(hi, lo + 2 * half)];       // USB / CW-U: one-sided
  if (hi <= 0) return [Math.max(lo, hi - 2 * half), hi];       // LSB: one-sided
  return [Math.max(lo, -half), Math.min(hi, half)];
}

/* ★★ THE 8.33 kHz RASTER IS 25 kHz / 3, NOT 8333 Hz. The step is carried as the integer sentinel STEP_833 (8333 —
 *  utils/airband.ts), and stepping a band by the sentinel itself drifts a third of a hertz a channel: 2400 channels
 *  across 110–130 MHz ended 800 Hz off the channel. Every band sum goes through the EXACT step, each channel computed
 *  from its index (never accumulated), and an 8.33 band starts ON the raster — the channels are absolute (118.00833 is
 *  a channel wherever the scan starts), so a Lo typed between two of them begins at the next one up. */
const exStep = (b: BandSpec) => stepExactHz(b.stepHz);

/** The band as it is scanned: an 8.33 kHz band moved onto the airband raster (first channel at or above Lo); any other
 *  step scans from Lo exactly as typed. */
export function rasterBand(b: BandSpec): BandSpec {
  if (b.stepHz !== STEP_833 || !(b.hiHz >= b.loHz)) return b;
  return { ...b, loHz: rasterHz(stepIndex(b.loHz, STEP_833, 'ceil'), STEP_833) };
}

export function bandCount(b: BandSpec): number {
  if (!(b.stepHz > 0) || !(b.hiHz >= b.loHz)) return 0;
  return Math.floor((b.hiHz - b.loHz) / exStep(b) + 1e-6) + 1;
}

export function bandTarget(b: BandSpec, n: number): ScanTarget {
  const [bwLow, bwHigh] = bandPassband(b.mode, b.stepHz);
  return { frequency: Math.round(b.loHz + n * exStep(b)), mode: b.mode, bwLow, bwHigh };
}

/**
 * ★★★ THE LO / HI FIELDS → A RANGE, in one pure place (Stuart, 2026-10-28, 110–130 MHz AM 8.33 airband: the panel
 * answered "This receiver reaches 0.100 MHz–1766.000 MHz from here: 130.000 MHz–110000.000 MHz" while its fields read
 * 110 and 130). Two faults met there:
 *  · The fields only reached the engine on END EDITING. On the Mac a click on START does not end a text field's editing,
 *    so START scanned whatever band was last committed — not what the fields said. The panel now commits the fields
 *    through this function on START, so what is scanned is what is on screen.
 *  · The fields are MEGAHERTZ, but nothing beside them said so once a number was in (the unit was only the empty
 *    placeholder): "110000", meant as kHz, became 110 GHz. The fields now carry the unit, and the controller keyboard's
 *    own kHz / Hz unit is honoured here instead of being read as MHz.
 * Either order is fine (Lo > Hi is swapped). Returns the range in Hz, or `why` in plain words.
 */
export type RangeUnit = 'mhz' | 'khz' | 'hz';
export function parseScanFreq(t: string, unit: RangeUnit = 'mhz'): number {
  const v = parseFloat(String(t ?? '').trim().replace(',', '.'));
  if (!Number.isFinite(v) || !(v > 0)) return NaN;
  return Math.round(v * (unit === 'khz' ? 1e3 : unit === 'hz' ? 1 : 1e6));
}
export function parseScanRange(loT: string, hiT: string, units: { lo?: RangeUnit; hi?: RangeUnit } = {}):
  { loHz: number; hiHz: number } | { why: string } {
  const lo = parseScanFreq(loT, units.lo), hi = parseScanFreq(hiT, units.hi);
  if (!Number.isFinite(lo) && !Number.isFinite(hi)) return { why: 'Enter the range to scan — Lo and Hi in MHz' };
  if (!Number.isFinite(lo)) return { why: 'Enter where to scan from — Lo, in MHz' };
  if (!Number.isFinite(hi)) return { why: 'Enter where to scan to — Hi, in MHz' };
  return { loHz: Math.min(lo, hi), hiHz: Math.max(lo, hi) };
}

export function tagTarget(t: ScanTag): ScanTarget {
  return { frequency: t.frequency, mode: t.mode, bwLow: t.bwLow, bwHigh: t.bwHigh, tagId: t.id,
           noiseRefDb: t.noiseRefDb, priority: !!t.priority };
}

export function isLockedOut(hz: number, lockouts: ScannerConfig['lockouts']): boolean {
  for (const l of lockouts) if (Math.abs(hz - l.frequency) <= l.halfWidthHz) return true;
  return false;
}

/** ★★ §4.3 — one function, three call sites (start, every reach change, preset load). */
export function validateCandidates<T extends { frequency: number }>(c: T[], r: ScanReach): { ok: T[]; out: T[] } {
  const ok: T[] = [], out: T[] = [];
  for (const x of c) (inReach(x.frequency, r) ? ok : out).push(x);
  return { ok, out };
}

/** A band against the reach: the part inside, or null when none of it is. */
export function clipBand(b: BandSpec, r: ScanReach): BandSpec | null {
  if (!r.allowed || bandCount(b) === 0) return null;
  const ex = exStep(b);
  const firstN = Math.max(0, Math.ceil((r.loHz - b.loHz) / ex - 1e-6));
  const lastN = Math.min(bandCount(b) - 1, Math.floor((r.hiHz - b.loHz) / ex + 1e-6));
  if (lastN < firstN) return null;
  return { ...b, loHz: Math.round(b.loHz + firstN * ex), hiHz: Math.round(b.loHz + lastN * ex) };
}

// ═══ The band picker (§6.1) — a shortcut that fills Lo / Hi / step / mode ════════════════════════════════════════

export interface BandChoice { name: string; type: BandType; spec: BandSpec }

/** ★ The SCAN step for a band — not its tuning step. A 40 m tuning step is 500 Hz; scanning SSB at 500 Hz would take six
 *  minutes a pass and stop five times on every voice. One passband apart is the right raster for SSB, 500 Hz for CW,
 *  and SW broadcast lives on a 5 kHz raster. Channelised bands (CB, airband, MW) keep their own step. */
export function scanStepFor(mode: SDRMode | undefined, bandStep: number | undefined): number {
  const s = bandStep && bandStep > 0 ? bandStep : 5000;
  if (mode === 'usb' || mode === 'lsb') return Math.max(s, 2500);
  if (mode === 'cwu' || mode === 'cwl') return Math.max(s, 500);
  if ((mode === 'am' || mode === 'sam') && s < 5000) return 5000;
  return s;
}

/** ★★ The name a band goes by IN THE SCANNER'S LIST. bandPlan names 108–118 and 118–137 MHz both "Airband (VHF Air)"
 *  (one label on the dial), and the list kept the first of a name — so "Airband" filled 108–118 MHz, the VOR / ILS
 *  navaids (Morse idents, no voice), and the voice band was never offered at all. The two are told apart here, and the
 *  voice band says its raster: "Airband (118–137 MHz, AM 8.33)" in Region 1, "AM 25" where 25 kHz is the plan. */
function choiceName(b: Band): string {
  if (b.name !== 'Airband (VHF Air)') return b.name;
  if (b.hi <= 118e6) return 'Air navaids (108–118 MHz, VOR / ILS)';
  return `Airband (118–137 MHz, AM ${b.step === STEP_833 ? '8.33' : '25'})`;
}

/** Mode for a band that names none: SSB by the amateur convention (LSB below 10 MHz), AM on VHF/UHF voice. */
function modeFor(b: Band): SDRMode {
  if (b.mode) return b.mode;
  const mid = (b.lo + b.hi) / 2;
  if (mid >= 30e6) return 'nfm';
  return mid < 10e6 ? 'lsb' : 'usb';
}

/**
 * ★★ THE BAND LIST FOR THIS REGION AND THIS RECEIVER: the regional entry wins (11m CB is NFM in Regions 1/3 and AM in
 * Region 2 — bandPlan.ts), and a band the receiver cannot reach is ABSENT ("no offering 2 m on a 0–30 MHz UberSDR").
 * A band partly out of reach is offered clipped. Grouped by type, in frequency order.
 */
export function bandChoices(region: number, reach: { loHz: number; hiHz: number }): BandChoice[] {
  const seen = new Set<string>();
  const out: BandChoice[] = [];
  for (const b of BAND_PLAN) {
    if (b.regions && b.regions.length && region && !b.regions.includes(region)) continue;
    const name = choiceName(b);
    if (seen.has(name)) continue;
    const lo = Math.max(b.lo, reach.loHz), hi = Math.min(b.hi, reach.hiHz);
    if (!(hi > lo)) continue;
    seen.add(name);
    const mode = modeFor(b);
    const stepHz = scanStepFor(mode, b.step);
    // ★ Start on the raster the band uses, so a CB scan lands ON 26.965, not 26.9653 (and an 8.33 one on 25 kHz / 3).
    const ex = stepExactHz(stepHz);
    const loHz = Math.round(Math.ceil((lo - b.lo) / ex - 1e-6) * ex + b.lo);
    out.push({ name, type: b.type, spec: { loHz, hiHz: hi, stepHz, mode } });
  }
  const order: Record<BandType, number> = { ham: 0, broadcast: 1, utility: 2 };
  return out.sort((a, b) => (order[a.type] - order[b.type]) || (a.spec.loHz - b.spec.loHz));
}

// ═══ The meter: comparator, Auto threshold, the S-meter detents (§5) ═════════════════════════════════════════════

export type MeterMode = 'snr' | 'smeter' | 'dbfs';
export const meterFamily = (m: MeterMode): 'snr' | 'dbfs' => (m === 'snr' ? 'snr' : 'dbfs');

/** One meter sample, in the units the user's meter shows (the screen's own numbers — the same ones feeding the bar). */
export interface ScanSample {
  /** SNR dB (UberSDR/VibeServer: the server's channel SNR; local: derived). NaN/absent where there is none (OWRX, Kiwi). */
  snr?: number;
  /** The absolute channel level the meter shows in dBFS / S-meter mode (dBFS, or a Kiwi's / OWRX's dBm). */
  level?: number;
  /** ★ The noise floor IN THE SAME UNITS as `level`, from a reference the signal cannot be in (the screen's neighbour
   *  windows, else its tracked floor). NaN/absent when it cannot answer — the engine then holds its last one. */
  floor?: number;
  /** ★★ A BROADBAND IMPULSE this frame (ImpulseDetector): a static crash or ignition noise lights the whole span at
   *  once. Such a sample is not evidence about THIS channel either way, so the engine ignores it. */
  impulse?: boolean;
  /** Where the backend says the dial is (display Hz) — how a person turning the dial during a stop is noticed. */
  tunedHz?: number;
}

/** The S-meter slider's detents: S1…S9 at 6 dB, then S9+10/20/30/40. Same scale as the meter (constants/meters.ts). */
export function sMeterDetents(): number[] {
  const d: number[] = [];
  for (let s = 1; s <= 9; s++) d.push(S9_DB - (9 - s) * DB_PER_S_UNIT);
  for (const over of [10, 20, 30, 40]) d.push(S9_DB + over);
  return d;
}
export function snapSMeter(db: number): number {
  let best = S9_DB, bd = Infinity;
  for (const v of sMeterDetents()) { const e = Math.abs(v - db); if (e < bd) { bd = e; best = v; } }
  return best;
}

/**
 * ★★ BROADBAND IMPULSE DETECTOR (Stuart: "give it the ability to ignore signals such as noise spikes"). A signal on a
 * channel raises a handful of bins; a static crash, ignition or switching noise raises the WHOLE SPAN together. So the
 * row's median — which a signal cannot move — is compared with its own slow baseline: a jump of IMPULSE_DB or more is
 * an impulse. The baseline is learnt only from ordinary rows, and a rise that persists past IMPULSE_REBASE_MS is taken
 * as the new normal (a band opening, a gain change) rather than flagged for ever.
 * ★ Cost: 64 strided bins sorted per row — nothing, whatever the bin count. Reset when the view changes.
 */
export const IMPULSE_DB = 6;
export const IMPULSE_REBASE_MS = 2000;
export class ImpulseDetector {
  private base = NaN;
  private key = '';
  private since = -1;
  private scratch: number[] = [];
  feed(bins: ArrayLike<number>, viewKey: string, now: number): boolean {
    const n = bins.length;
    if (n < 16) return false;
    if (viewKey !== this.key) { this.key = viewKey; this.base = NaN; this.since = -1; }
    const m = Math.min(64, n), s = this.scratch;
    s.length = m;
    for (let i = 0; i < m; i++) s[i] = bins[Math.floor((i + 0.5) * n / m)];
    s.sort((a, b) => a - b);
    const med = s[m >> 1];
    if (!Number.isFinite(med)) return false;
    if (!Number.isFinite(this.base)) { this.base = med; return false; }
    if (med - this.base >= IMPULSE_DB) {
      if (this.since < 0) this.since = now;
      if (now - this.since < IMPULSE_REBASE_MS) return true;
      this.base = med; this.since = -1;          // ★ it has stayed — the new normal
      return false;
    }
    this.since = -1;
    this.base += 0.1 * (med - this.base);
    return false;
  }
}

// ═══ Names beside frequencies (Stuart, 2026-10-27) ═══════════════════════════════════════════════════════════════

/** One named frequency. `rank`: 0 = the user's own bookmark, 1 = the server's (incl. learned / scanned), 2 = built-in. */
export interface NamedFreq { name: string; frequency: number; rank: number }

/**
 * ★★ "128.590 MHz – London South Information Broadcast". The best SOURCE wins (the user's own name over the server's
 * over a built-in one), and within it the NEAREST — inside the channel only (`tolHz`: half the scan step, or the tag's
 * passband half-width). No match → undefined → the frequency alone.
 */
export function nameAt(hz: number, tolHz: number, list: readonly NamedFreq[]): string | undefined {
  let best: NamedFreq | undefined, bd = Infinity;
  for (const b of list) {
    const d = Math.abs(b.frequency - hz);
    if (d > tolHz || !b.name) continue;
    if (!best || b.rank < best.rank || (b.rank === best.rank && d < bd)) { best = b; bd = d; }
  }
  return best?.name;
}

/** The channel half-width a name may sit within. */
export function nameTolerance(t: { bwLow: number; bwHigh: number }, stepHz?: number): number {
  if (stepHz && stepHz > 0) return stepHz / 2;
  return Math.max(500, (t.bwHigh - t.bwLow) / 2);
}

/** "128.590 MHz" — kHz precision, more only when the frequency has it (an 8.33 kHz airband channel, an SSB tag). */
export function freqText(hz: number): string {
  const mhz = hz / 1e6;
  const dp = Math.round(hz) % 1000 === 0 ? 3 : Math.round(hz) % 100 === 0 ? 4 : 5;
  return `${mhz.toFixed(dp)} MHz`;
}
/** ★ "128.590 MHz – London South Information Broadcast" — the row text exactly as Stuart wrote it (2026-10-27); the
 *  frequency alone when nothing matches. */
export function scanRowText(hz: number, name?: string): string {
  return name ? `${freqText(hz)} – ${name}` : freqText(hz);
}
/** The same, as VoiceOver should say it. */
export function freqSpoken(hz: number): string {
  return `${(hz / 1e6).toFixed(Math.round(hz) % 1000 === 0 ? 3 : 4)} megahertz`;
}
export function modeText(m: string): string { return m.toUpperCase(); }

/** "14.205 USB" — a tag's default label (§2). */
export function defaultTagLabel(t: { frequency: number; mode: string }): string {
  return `${(t.frequency / 1e6).toFixed(3)} ${modeText(t.mode)}`;
}

// ═══ The controller (§4) ═════════════════════════════════════════════════════════════════════════════════════════

export type ScanState = 'IDLE' | 'SETTLING' | 'SAMPLING' | 'STOPPED' | 'HOLD' | 'PAUSED';

export interface ScanHost {
  now(): number;
  reach(): ScanReach;
  /** Tune with recenter:false and the target's passband. ★ Only ever called with a target inReach() — the controller
   *  checks the reach afresh immediately before every call. */
  tune(t: ScanTarget): void;
  /** ★ The persistent native audio gate (VibePowerModule.setSquelchOpen) — closed while hopping, open on a stop. */
  setGate(open: boolean): void;
  meterMode(): MeterMode;
  startRecording?(t: ScanTarget): void;
  stopRecording?(): void;
  /** Is the user recording by hand? Record-on-stop then leaves the recorder alone. */
  manualRecording?(): boolean;
  announce?(text: string): void;
  /** The best name for a frequency (nameAt over the screen's merged bookmarks), or undefined. */
  nameFor?(hz: number, tolHz: number): string | undefined;
  /** Something the panel shows changed. */
  changed?(): void;
}

export interface ScanRefusal {
  /** What is out of reach, readable ("118.000–121.500 MHz", "14.205 USB"). */
  out: string[];
  okCount: number;
  why: string;
}

type Probe = { kind: 'priority'; returnTo: ScanTarget; stoppedAt: number; belowSince: number } | null;

/**
 * ★★★ THE STATE MACHINE (brief §4):
 *
 *   IDLE → (hop: gate shut, tune) → SETTLING → SAMPLING → STOPPED (gate open) → … hang / time limit / Skip / priority → hop
 *                                                ↓ nothing sustained → hop
 *   HOLD   = STOPPED with every timer frozen (the user pressed Hold); priority interrupts suppressed.
 *   PAUSED = candidates out of reach (an OWRX profile change by someone else, a VibeServer owner moving the locked
 *            window, the radio becoming a shared dial) — keeps its place and resumes by itself when they are back.
 *
 * The brief's ADVANCING is the instant between a decision and the next tune, so it is a transition here, not a state.
 */
export class ScannerController {
  readonly host: ScanHost;
  timing: ScanTiming;
  config: ScannerConfig = normaliseConfig(DEFAULT_CONFIG);
  source: ScanSource = 'list';
  band: BandSpec | null = null;
  /** The tag list (manual) or the bookmark group's tags (bookmarks). */
  tags: ScanTag[] = [];
  /** ★ Session-scoped (§10.2): survives backgrounding (the host keeps this object in a ref), cleared on app kill. */
  found: FoundSignal[] = [];

  state: ScanState = 'IDLE';
  current: ScanTarget | null = null;
  refusal: ScanRefusal | null = null;
  pauseWhy = '';
  unreachable: string[] = [];
  /** "measuring the noise floor…" — Auto in a level unit with no floor known yet. */
  needsFloor = false;
  lastMetric = NaN;
  lastThreshold = NaN;

  private index = -1;
  /** The run's band (clipped when the user chose "scan the reachable part"). */
  private runBand: BandSpec | null = null;
  /** The run's tags (only the reachable ones when the user chose "Start with N reachable"). */
  private runTagIds: Set<string> | null = null;
  private latchedKey = '';
  private localSwitchPending = false;
  private phaseEndsAt = 0;
  private runStart = -1;
  private runN = 0;
  private runPeak = -Infinity;
  private stoppedAt = 0;
  private belowSince = -1;
  private holdAt = 0;
  private holdFromStop = false;
  private userTuneN = 0;
  private probe: Probe = null;
  private lastPriorityAt = 0;
  private priorityRot = 0;
  private softSkip = new Map<string, number>();
  private heldFloor = NaN;
  private sweepLevels: number[] = [];
  private firstSampleTaken = false;
  private recording = false;
  private stopEndedByLimit = false;

  constructor(host: ScanHost, timing: ScanTiming) { this.host = host; this.timing = timing; }

  // ── Setup ──────────────────────────────────────────────────────────────────────────────────────────────────────

  setConfig(patch: Partial<ScannerConfig>): void {
    this.config = normaliseConfig({ ...this.config, ...patch });
    this.host.changed?.();
  }
  setSource(s: ScanSource): void {
    if (this.running()) this.stop();
    this.source = s; this.refusal = null; this.host.changed?.();
  }
  setBand(b: BandSpec | null): void { this.band = b; this.refusal = null; this.host.changed?.(); }
  /** ★★ The Lo / Hi fields, committed (parseScanRange) — what the panel's START scans is what its fields say. The band
   *  keeps its step and mode. A range that does not parse: `quiet` (a field losing the cursor mid-entry) changes
   *  nothing; otherwise (START) it says why on the panel and returns false. */
  commitRange(loT: string, hiT: string, opts: { lo?: RangeUnit; hi?: RangeUnit; quiet?: boolean } = {}): boolean {
    const r = parseScanRange(loT, hiT, opts);
    if ('why' in r) {
      if (!opts.quiet) { this.refusal = { out: [], okCount: 0, why: r.why }; this.host.changed?.(); }
      return false;
    }
    const b = this.band ?? { loHz: 0, hiHz: 0, stepHz: 25000, mode: 'am' as SDRMode };
    if (b.loHz !== r.loHz || b.hiHz !== r.hiHz || !this.band) this.setBand({ ...b, loHz: r.loHz, hiHz: r.hiHz });
    return true;
  }
  setTags(t: ScanTag[]): void {
    this.tags = t;
    // ★ A tag removed while scanning must never be tuned again; the run's index is re-found by id on the next hop.
    this.host.changed?.();
  }

  running(): boolean { return this.state !== 'IDLE'; }
  isRecording(): boolean { return this.recording; }
  /** ★ The user pressed REC by hand: the scanner's own recording (if any) ends first, so the two never fight over the
   *  one recorder; record-on-stop then stands aside while the manual one runs (host.manualRecording). */
  releaseRecorder(): void { this.stopRecordingIfAny(); }

  /** ★ The user picked a profile IN THE APP: the next reach change is theirs — re-latch, and if the list no longer
   *  fits, behave like start-time validation (refuse) rather than an external hijack (pause and wait). */
  noteLocalSwitch(): void { this.localSwitchPending = true; }

  // ── Candidates ─────────────────────────────────────────────────────────────────────────────────────────────────

  private runTags(): ScanTag[] {
    return this.runTagIds ? this.tags.filter((t) => this.runTagIds!.has(t.id)) : this.tags;
  }
  private count(): number {
    if (this.source === 'band') return this.runBand ? bandCount(this.runBand) : 0;
    return this.runTags().length;
  }
  private targetAt(n: number): ScanTarget | null {
    if (this.source === 'band') return this.runBand ? bandTarget(this.runBand, n) : null;
    const t = this.runTags()[n];
    return t ? tagTarget(t) : null;
  }
  private keyOf(t: ScanTarget): string { return t.tagId ?? String(t.frequency); }
  private ignored(t: ScanTarget): boolean {
    if (t.tagId) return !!this.tags.find((x) => x.id === t.tagId)?.lockedOut;
    return isLockedOut(t.frequency, this.config.lockouts);
  }
  private softSkipped(t: ScanTarget, now: number): boolean {
    const until = this.softSkip.get(this.keyOf(t));
    return until !== undefined && until > now;
  }
  /** Every target of this run, readable — for refusals and the PAUSED banner. */
  private describeOut(out: ScanTarget[]): string[] {
    return out.slice(0, 12).map((t) => `${freqText(t.frequency)} ${modeText(t.mode)}`);
  }

  /** ★★ The next candidate to visit: a starred tag that is due a visit first, else the next in order that is neither
   *  ignored, soft-skipped nor out of reach. null when there is nothing left to scan. */
  private nextTarget(now: number): { t: ScanTarget; n: number } | null {
    const r = this.host.reach();
    const due = this.dueStar(now, r);
    if (due) return due;
    const n = this.count();
    if (n === 0) return null;
    let soft: { t: ScanTarget; n: number } | null = null;
    for (let k = 1; k <= n; k++) {
      const i = (this.index + k) % n;
      const t = this.targetAt(i);
      if (!t || this.ignored(t) || !inReach(t.frequency, r)) continue;
      if (this.softSkipped(t, now)) { if (!soft) soft = { t, n: i }; continue; }
      return { t, n: i };
    }
    // ★ Everything left is soft-skipped: a Skip is "not now", never "never", so with nothing else to visit the skips
    //   give way rather than the scan grinding to a halt.
    return soft;
  }

  private stars(r: ScanReach): ScanTarget[] {
    if (this.source === 'band') return [];
    return this.runTags().filter((t) => t.priority && !t.lockedOut && inReach(t.frequency, r)).map(tagTarget);
  }
  private dueStar(now: number, r: ScanReach): { t: ScanTarget; n: number } | null {
    const st = this.stars(r);
    if (!st.length || now - this.lastPriorityAt < PRIORITY_MS) return null;
    const t = st[this.priorityRot++ % st.length];
    if (this.current && t.tagId === this.current.tagId) return null;
    this.lastPriorityAt = now;
    // ★ Visiting a star does not move the round-robin on — n = -1 keeps the index where it was.
    return { t, n: -1 };
  }

  // ── Start / stop ───────────────────────────────────────────────────────────────────────────────────────────────

  /**
   * ★★ §4.3: REFUSE, never silently scan a subset. Anything out of reach → `refusal` lists it and nothing starts; the
   * panel offers "Start with N reachable" as an explicit action (`reachableOnly`).
   */
  start(opts: { reachableOnly?: boolean } = {}): boolean {
    if (this.running()) this.stop();
    if (!this.prepare(opts)) return false;
    const r = this.host.reach();
    this.latchedKey = r.key;
    this.localSwitchPending = false;
    this.index = -1;
    this.unreachable = [];
    this.pauseWhy = '';
    this.lastPriorityAt = this.host.now();
    this.host.announce?.('Scanning');
    return this.hop();
  }

  /** ★ §4.3's third call site — a preset just loaded: validate against this receiver NOW, so a refusal is on the panel
   *  before anything is pressed. Nothing starts. */
  checkReachable(): boolean { return this.running() || this.prepare({}); }

  /** Validate the scan against the reach and fix the run's candidates; sets `refusal` and returns false when it may not
   *  start as asked. */
  private prepare(opts: { reachableOnly?: boolean }): boolean {
    const r = this.host.reach();
    this.refusal = null;
    if (!r.allowed) { this.refusal = { out: [], okCount: 0, why: r.why || 'Scanning is not available here' }; this.host.changed?.(); return false; }
    if (this.source === 'band') {
      const b = this.band ? rasterBand(this.band) : null;
      if (!b || bandCount(b) === 0 || b.hiHz < b.loHz) {
        this.refusal = { out: [], okCount: 0, why: 'Enter the range to scan — Lo, Hi and a step' };
        this.host.changed?.(); return false;
      }
      const clip = clipBand(b, r);
      const whole = !!clip && bandCount(clip) === bandCount(b);
      if (!whole) {
        if (!clip || !opts.reachableOnly) {
          this.refusal = {
            out: [`${freqText(b.loHz)}–${freqText(b.hiHz)}`], okCount: clip ? bandCount(clip) : 0,
            // ★ A range no radio has (110000 MHz) is a unit slip, not a receiver limit — say which unit the fields use.
            why: `This receiver reaches ${freqText(r.loHz)}–${freqText(r.hiHz)} from here${b.hiHz > 10e9 ? ' — Lo and Hi are in MHz' : ''}`,
          };
          this.host.changed?.(); return false;
        }
      }
      this.runBand = whole ? b : clip;
      this.runTagIds = null;
    } else {
      const live = this.tags.filter((t) => !t.lockedOut);
      if (!live.length) { this.refusal = { out: [], okCount: 0, why: this.tags.length ? 'Every channel is ignored' : 'Nothing to scan yet' }; this.host.changed?.(); return false; }
      const { ok, out } = validateCandidates(this.tags.map(tagTarget), r);
      if (out.length) {
        const okLive = ok.filter((t) => !this.ignored(t));
        if (!opts.reachableOnly || !okLive.length) {
          this.refusal = { out: this.describeOut(out), okCount: okLive.length,
                           why: `${out.length} of ${this.tags.length} can't be reached on this receiver right now` };
          this.host.changed?.(); return false;
        }
        this.runTagIds = new Set(ok.map((t) => t.tagId!));
      } else this.runTagIds = null;
      this.runBand = null;
    }
    this.host.changed?.();
    return true;
  }

  /** ★ Stop: the gate is ALWAYS left open (brief §7.4 / acceptance 11) — a scanner must never leave the audio shut. */
  stop(): void {
    const was = this.state;
    this.endStop(false);
    this.state = 'IDLE';
    this.current = null;
    this.probe = null;
    this.host.setGate(true);
    if (was !== 'IDLE') this.host.announce?.('Scan stopped');
    this.host.changed?.();
  }

  // ── The three live controls ────────────────────────────────────────────────────────────────────────────────────

  /** HOLD toggle (also stops a scan cold mid-sample). Hold again resumes: a hold made on a stop goes back to that stop
   *  with its timers where they were; one made mid-sample samples this channel afresh. */
  hold(): void {
    const now = this.host.now();
    if (this.state === 'HOLD') {
      const held = now - this.holdAt;
      if (this.holdFromStop) {
        this.stoppedAt += held;
        if (this.belowSince >= 0) this.belowSince += held;
        this.state = 'STOPPED';
        this.host.announce?.('Scan resumed');
      } else {
        this.host.setGate(false);
        this.enterSettle(now);
      }
      this.lastPriorityAt = now;
      this.host.changed?.();
      return;
    }
    if (this.state === 'PAUSED') { this.resumeIfReachable(); return; }
    if (this.state === 'IDLE' || !this.current) return;
    if (this.probe) {
      // ★ Mid-priority-probe: Hold means the thing you were LISTENING to.
      const back = this.probe.returnTo;
      this.stoppedAt = this.probe.stoppedAt; this.belowSince = this.probe.belowSince;
      this.probe = null;
      if (!this.sendTune(back)) return;
      this.current = back;
      this.holdFromStop = true;
    } else {
      this.holdFromStop = this.state === 'STOPPED';
    }
    this.state = 'HOLD';
    this.holdAt = now;
    this.host.setGate(true);
    this.host.announce?.(`Held on ${this.spoken(this.current!)}`);
    this.host.changed?.();
  }

  /** SKIP: move on now, and pass this frequency over for a few minutes (a soft ignore — "only annoying now"). In
   *  PAUSED, try to resume; in HOLD, resume by moving on. */
  skip(): void {
    const now = this.host.now();
    if (this.state === 'IDLE') return;
    if (this.state === 'PAUSED') { this.resumeIfReachable(); return; }
    const cur = this.probe ? this.probe.returnTo : this.current;
    this.probe = null;
    if (cur) this.softSkip.set(this.keyOf(cur), now + SOFT_SKIP_MS);
    this.endStop(false);
    this.hop();
  }

  /**
   * IGNORE the current frequency (the brief's Lockout): a band scan adds it ± half a step to the ignores; a list scan
   * marks the tag. Moves on at once. Returns an UNDO.
   */
  ignoreCurrent(): (() => void) | null {
    const cur = this.probe ? this.probe.returnTo : this.current;
    if (!cur || this.state === 'IDLE') return null;
    const undo = this.ignoreTarget(cur);
    this.probe = null;
    this.endStop(false);
    this.host.announce?.(`Ignoring ${this.spoken(cur)}`);
    this.hop();
    return undo;
  }

  /** Ignore a frequency from the found list (or a tag). Returns an undo. */
  ignoreTarget(t: { frequency: number; tagId?: string }): () => void {
    if (t.tagId) {
      const id = t.tagId;
      this.tags = this.tags.map((x) => (x.id === id ? { ...x, lockedOut: true } : x));
      this.host.changed?.();
      return () => { this.tags = this.tags.map((x) => (x.id === id ? { ...x, lockedOut: false } : x)); this.host.changed?.(); };
    }
    const half = Math.max(1, (this.runBand?.stepHz ?? this.band?.stepHz ?? 5000) / 2);
    const l = { frequency: t.frequency, halfWidthHz: half };
    this.config = { ...this.config, lockouts: [...this.config.lockouts, l] };
    this.host.changed?.();
    return () => this.unignore(t.frequency);
  }

  /** Un-ignore every ignore covering this frequency. */
  unignore(hz: number): void {
    this.config = { ...this.config, lockouts: this.config.lockouts.filter((l) => Math.abs(hz - l.frequency) > l.halfWidthHz) };
    this.host.changed?.();
  }

  // ── Tags (manual mode) ─────────────────────────────────────────────────────────────────────────────────────────

  addTag(t: ScanTag): void { this.tags = [...this.tags, t]; this.host.changed?.(); }
  removeTag(id: string): void {
    this.tags = this.tags.filter((t) => t.id !== id);
    if (this.current?.tagId === id && this.running()) { this.endStop(false); this.hop(); }
    this.host.changed?.();
  }
  toggleStar(id: string): void { this.tags = this.tags.map((t) => (t.id === id ? { ...t, priority: !t.priority } : t)); this.host.changed?.(); }
  toggleTagIgnored(id: string): void { this.tags = this.tags.map((t) => (t.id === id ? { ...t, lockedOut: !t.lockedOut } : t)); this.host.changed?.(); }

  // ── The clock ──────────────────────────────────────────────────────────────────────────────────────────────────

  /** Call every ~100 ms and on every frame. Settling, sample windows, hang, the time limit, priority probes, and the
   *  reach check all run from here. */
  tick(): void {
    if (this.state === 'IDLE') return;
    const now = this.host.now();
    if (!this.checkReach()) return;
    switch (this.state) {
      case 'SETTLING':
        if (now >= this.phaseEndsAt) {
          if (this.probe && this.current === this.probe.returnTo) {
            // ★ Back from a priority probe that found nothing: carry on the stop exactly where it was.
            this.stoppedAt = this.probe.stoppedAt; this.belowSince = this.probe.belowSince;
            this.probe = null;
            this.state = 'STOPPED';
            this.host.setGate(true);
            this.host.changed?.();
          } else this.enterSample(now);
        }
        break;
      case 'SAMPLING':
        if (now >= this.phaseEndsAt) {
          const building = this.runN > 0 && now < this.phaseEndsAt + SAMPLE_EXTEND_MS;
          if (!building) this.sampleFailed();
        }
        break;
      case 'STOPPED': {
        const c = this.config;
        if (c.timeLimit && now - this.stoppedAt >= c.timeLimitMs) { this.stopEndedByLimit = true; this.leaveStop(); break; }
        if (c.untilStops && this.belowSince >= 0 && now - this.belowSince >= c.hangMs) { this.leaveStop(); break; }
        // ★ Priority probe (§6.4): HOLD is exempt (Hold means hold) and so is a recording (probes would click in it).
        if (!this.recording) {
          const r = this.host.reach();
          const st = this.stars(r).filter((t) => t.tagId !== this.current?.tagId);
          if (st.length && now - this.lastPriorityAt >= PRIORITY_MS) {
            const t = st[this.priorityRot++ % st.length];
            this.lastPriorityAt = now;
            this.probe = { kind: 'priority', returnTo: this.current!, stoppedAt: this.stoppedAt, belowSince: this.belowSince };
            this.host.setGate(false);
            if (this.sendTune(t)) { this.current = t; this.enterSettle(now); }
            else this.probe = null;
          }
        }
        break;
      }
      default: break;
    }
  }

  /** One meter sample (call with every frame's numbers). */
  feed(s: ScanSample): void {
    if (this.state === 'IDLE' || this.state === 'PAUSED') return;
    const now = this.host.now();
    if (Number.isFinite(s.floor)) this.heldFloor = s.floor!;
    if (s.impulse) return;                         // ★ a broadband crash says nothing about this channel
    const cur = this.current;
    if (!cur) return;
    if (this.state === 'SAMPLING') {
      if (!this.firstSampleTaken) {
        this.firstSampleTaken = true;
        if (Number.isFinite(s.level)) this.noteSweepLevel(s.level!);
      }
      const above = this.isActive(s, cur, false);
      if (above) {
        if (this.runStart < 0) this.runStart = now;
        this.runN++;
        this.runPeak = Math.max(this.runPeak, this.lastMetric);
        if (this.runN >= SUSTAIN_N && now - this.runStart >= SUSTAIN_MS) this.sampleSucceeded(now);
      } else { this.runStart = -1; this.runN = 0; this.runPeak = -Infinity; }
      return;
    }
    if (this.state === 'STOPPED' || this.state === 'HOLD') {
      // ★ The user turned the dial: hold the scan where THEY are (Hold again / Skip carries on).
      if (Number.isFinite(s.tunedHz) && Math.abs(s.tunedHz! - cur.frequency) > USER_TUNE_TOL_HZ) {
        if (++this.userTuneN >= USER_TUNE_N && this.state === 'STOPPED') {
          this.userTuneN = 0;
          this.holdFromStop = true; this.state = 'HOLD'; this.holdAt = now;
          this.host.announce?.('Scan held — you tuned away');
          this.host.changed?.();
          return;
        }
      } else this.userTuneN = 0;
      if (this.state === 'HOLD') return;
      const above = this.isActive(s, cur, true);
      if (Number.isFinite(this.lastMetric)) this.noteFoundPeak(cur, this.lastMetric);
      if (!this.config.untilStops) return;          // ★ time limit alone: the metric is ignored once stopped
      if (above) this.belowSince = -1;              // ★★★ the over-change: signal back → the hang is CANCELLED
      else if (this.belowSince < 0) this.belowSince = now;
    }
  }

  // ── Internals ──────────────────────────────────────────────────────────────────────────────────────────────────

  /** ★★ The threshold, in the meter's own units. SNR: Auto = margin (SNR is already floor-relative). Level (dBFS /
   *  S-meter / a Kiwi's or OWRX's dBm): Auto = the floor here + margin — the TAG'S own floor when it has one. */
  private isActive(s: ScanSample, t: ScanTarget, stopped: boolean): boolean {
    const fam = meterFamily(this.host.meterMode());
    const c = this.config;
    let v: number, thr: number;
    if (fam === 'snr') {
      v = Number(s.snr);
      thr = c.thresholdMode === 'auto' ? c.marginDb : c.threshold.snr;
      this.needsFloor = false;
    } else {
      v = Number(s.level);
      if (c.thresholdMode === 'auto') {
        const floor = this.floorFor(t, s);
        this.needsFloor = !Number.isFinite(floor);
        thr = floor + c.marginDb;
      } else { thr = c.threshold.dbfs; this.needsFloor = false; }
    }
    this.lastMetric = v; this.lastThreshold = thr;
    if (!Number.isFinite(v) || !Number.isFinite(thr)) return false;
    return v >= thr - (stopped ? STOP_HYST_DB : 0);
  }

  /** The floor for a target: the tag's own (captured at tag time), else this frame's, else the last one known, else
   *  the quiet end of what the sweep has been reading (band scans on a backend whose view cannot answer). */
  private floorFor(t: ScanTarget, s: ScanSample): number {
    if (Number.isFinite(t.noiseRefDb)) return t.noiseRefDb!;
    if (Number.isFinite(s.floor)) return s.floor!;
    if (Number.isFinite(this.heldFloor)) return this.heldFloor;
    return this.sweepFloor();
  }
  private noteSweepLevel(v: number): void {
    this.sweepLevels.push(v);
    if (this.sweepLevels.length > 32) this.sweepLevels.shift();
  }
  /** ★ The 25th percentile of the last 32 channels' first readings — most channels in a sweep are empty. Needs 8. */
  private sweepFloor(): number {
    if (this.sweepLevels.length < 8) return NaN;
    const a = [...this.sweepLevels].sort((x, y) => x - y);
    return a[Math.floor((a.length - 1) * 0.25)];
  }

  /** ★★★ THE ONLY PLACE A TUNE LEAVES THE SCANNER — and the reach is asked afresh right here, so no path, however it
   *  got here, can emit a frequency outside the span (§4.4 — a correctness requirement, not a preference). */
  private sendTune(t: ScanTarget): boolean {
    const r = this.host.reach();
    if (!inReach(t.frequency, r)) { this.pause(r.allowed ? 'Scan paused — that channel is out of reach' : (r.why || 'Scan paused'), [t]); return false; }
    this.host.tune(t);
    return true;
  }

  private hop(): boolean {
    const now = this.host.now();
    this.host.setGate(false);
    const nx = this.nextTarget(now);
    if (!nx) {
      this.state = 'IDLE'; this.current = null; this.host.setGate(true);
      this.refusal = { out: [], okCount: 0, why: 'Everything in the scan is ignored or out of reach' };
      this.host.announce?.('Scan stopped — nothing left to scan');
      this.host.changed?.();
      return false;
    }
    if (nx.n >= 0) this.index = nx.n;
    if (!this.sendTune(nx.t)) return false;
    this.current = nx.t;
    this.enterSettle(now);
    return true;
  }

  private enterSettle(now: number): void {
    this.state = 'SETTLING';
    this.phaseEndsAt = now + this.timing.settleMs;
    this.host.changed?.();
  }
  private enterSample(now: number): void {
    this.state = 'SAMPLING';
    this.phaseEndsAt = now + this.timing.sampleMs;
    this.runStart = -1; this.runN = 0; this.runPeak = -Infinity;
    this.firstSampleTaken = false;
    this.host.changed?.();
  }

  private sampleFailed(): void {
    if (this.probe) {
      // ★ The star was quiet: back to what we were listening to (gate stays shut until it has settled).
      const back = this.probe.returnTo;
      if (!this.sendTune(back)) return;
      this.current = back;
      this.enterSettle(this.host.now());
      return;
    }
    this.hop();
  }

  private sampleSucceeded(now: number): void {
    const t = this.current!;
    const wasProbe = !!this.probe;
    if (wasProbe) {
      // ★★ PRIORITY INTERRUPT — whatever the stop was doing, the star matters more. The abandoned stop's recording ends.
      this.probe = null;
      this.stopRecordingIfAny();
      const n = this.runTags().findIndex((x) => x.id === t.tagId);
      if (n >= 0) this.index = n;
    }
    this.state = 'STOPPED';
    this.stoppedAt = now;
    this.belowSince = -1;
    this.userTuneN = 0;
    this.stopEndedByLimit = false;
    this.lastPriorityAt = now;
    this.host.setGate(true);
    if (this.source === 'band') this.noteFound(t, now);
    if (this.config.recordOnStop && !this.host.manualRecording?.()) { this.host.startRecording?.(t); this.recording = !!this.host.startRecording; }
    this.host.announce?.(`${wasProbe ? 'Priority — ' : 'Stopped on '}${this.spoken(t)}`);
    this.host.changed?.();
  }

  private leaveStop(): void {
    this.endStop(this.stopEndedByLimit);
    this.hop();
  }

  /** Close a stop's books: its dwell in the found list, its recording. */
  private endStop(byLimit: boolean): void {
    if ((this.state === 'STOPPED' || this.state === 'HOLD') && this.current && this.source === 'band') {
      const f = this.found.find((x) => x.frequency === this.current!.frequency);
      if (f) { f.dwellMs += Math.max(0, this.host.now() - this.stoppedAt); if (byLimit) f.capped++; }
    }
    this.stopEndedByLimit = false;
    this.stopRecordingIfAny();
  }
  private stopRecordingIfAny(): void {
    if (this.recording) { this.recording = false; this.host.stopRecording?.(); }
  }

  private noteFound(t: ScanTarget, now: number): void {
    let f = this.found.find((x) => x.frequency === t.frequency);
    if (!f) {
      f = { frequency: t.frequency, mode: t.mode, firstHeard: now, lastHeard: now, hits: 0, peakMetric: -Infinity, dwellMs: 0, capped: 0 };
      this.found.push(f);
    }
    f.hits++; f.lastHeard = now;
    if (Number.isFinite(this.runPeak)) f.peakMetric = Math.max(f.peakMetric, this.runPeak);
  }
  private noteFoundPeak(t: ScanTarget, v: number): void {
    if (this.source !== 'band') return;
    const f = this.found.find((x) => x.frequency === t.frequency);
    if (f) { f.peakMetric = Math.max(f.peakMetric, v); f.lastHeard = this.host.now(); }
  }

  /** ★★ The reach, every tick. A change of key re-validates the run; anything now out of reach PAUSES (someone else's
   *  doing) or REFUSES (the user's own profile switch). Returns false when the tick should go no further. */
  private checkReach(): boolean {
    const r = this.host.reach();
    if (this.state === 'PAUSED') { this.resumeIfReachable(); return false; }
    if (!r.allowed) { this.pause(r.why || 'Scan paused', []); return false; }
    if (r.key === this.latchedKey) return true;
    const out = this.outOfReach(r);
    this.latchedKey = r.key;
    if (!out.length) { this.localSwitchPending = false; return true; }
    if (this.localSwitchPending) {
      this.localSwitchPending = false;
      const n = this.count();
      this.stop();
      this.refusal = { out: this.describeOut(out), okCount: Math.max(0, n - out.length),
                       why: 'The profile you picked does not cover the whole scan' };
      this.host.changed?.();
      return false;
    }
    this.pause('Scan paused — receiver profile changed', out);
    return false;
  }

  private outOfReach(r: ScanReach): ScanTarget[] {
    if (this.source === 'band') {
      const b = this.runBand;
      if (!b) return [];
      const ends = [bandTarget(b, 0), bandTarget(b, bandCount(b) - 1)];
      return ends.filter((t) => !inReach(t.frequency, r));
    }
    return validateCandidates(this.runTags().filter((t) => !t.lockedOut).map(tagTarget), r).out;
  }

  private pause(why: string, out: ScanTarget[]): void {
    if (this.state === 'PAUSED') return;
    this.endStop(false);
    this.probe = null;
    this.state = 'PAUSED';
    this.pauseWhy = why;
    this.unreachable = this.source === 'band' && this.runBand
      ? [`${freqText(this.runBand.loHz)}–${freqText(this.runBand.hiHz)}`] : this.describeOut(out);
    // ★ The audio is opened while paused: the receiver changed under us, and silence would read as a fault.
    this.host.setGate(true);
    this.host.announce?.(why);
    this.host.changed?.();
  }

  private resumeIfReachable(): void {
    const r = this.host.reach();
    if (!r.allowed || this.outOfReach(r).length) return;
    this.latchedKey = r.key;
    this.state = 'SETTLING';
    this.pauseWhy = ''; this.unreachable = [];
    this.host.announce?.('Scan resumed');
    // ★ From the same place: the index was kept, and the hop moves on from it.
    this.hop();
  }

  private spoken(t: ScanTarget): string {
    const nm = this.host.nameFor?.(t.frequency, nameTolerance(t, this.source === 'band' ? this.runBand?.stepHz : undefined));
    // ★ Stuart's sentence: "Stopped on 128.590 megahertz, London South Information Broadcast" — the name when there
    //   is one, else the mode (which is what tells two unnamed SSB tags apart).
    return nm ? `${freqSpoken(t.frequency)}, ${nm}` : `${freqSpoken(t.frequency)} ${modeText(t.mode)}`;
  }

  /** ★ "Ignore?" — a frequency that keeps trapping the scan (many stops, long dwell, or repeated time-limit escapes).
   *  Only ever a HINT: nothing is ignored without the user's tap. */
  ignoreHint(f: FoundSignal): boolean {
    if (isLockedOut(f.frequency, this.config.lockouts)) return false;
    return f.hits >= HINT_HITS || f.dwellMs >= HINT_DWELL_MS || f.capped >= HINT_CAPPED;
  }

  /** The found list, newest-active first, ignored ones separated (the panel's "Ignored" section). */
  foundRows(): { live: FoundSignal[]; ignored: FoundSignal[] } {
    const rows = [...this.found].sort((a, b) => b.lastHeard - a.lastHeard);
    return {
      live: rows.filter((f) => !isLockedOut(f.frequency, this.config.lockouts)),
      ignored: rows.filter((f) => isLockedOut(f.frequency, this.config.lockouts)),
    };
  }

  /** The effective threshold the panel shows next to the Auto toggle, in the meter's units. */
  effectiveThreshold(): number { return this.lastThreshold; }
}

// ═══ Bundled starter presets (§10.1) ═════════════════════════════════════════════════════════════════════════════

/**
 * ★ Discrete-frequency sets only — anything that is a BAND is one tap in the band picker and is not duplicated here.
 * Frequencies from the published schedules: VOLMET from the ICAO EUR HF VOLMET allocations (Shannon, RAF), the time
 * stations from their operators (NIST WWV/WWVH, NRC CHU, VNIIFTRI RWM), marine distress and safety voice from the ITU
 * Radio Regulations Appendix 15. Read-only: Clone, then edit.
 */
function tagsOf(prefix: string, list: [number, SDRMode, string][]): ScanTag[] {
  return list.map(([hz, mode, label], i) => {
    const [bwLow, bwHigh] = MODE_BANDWIDTHS[mode];
    return { id: `${prefix}-${i}`, frequency: hz, mode, bwLow, bwHigh, label };
  });
}
export const BUNDLED_PRESETS: ScanPreset[] = [
  {
    id: 'bundled-volmet-eu', name: 'VOLMET (Europe)', source: 'list', bundled: true, config: { ...DEFAULT_CONFIG },
    channels: tagsOf('volmet', [
      [3_413_000, 'usb', 'Shannon VOLMET'], [5_450_000, 'usb', 'RAF VOLMET'], [5_505_000, 'usb', 'Shannon VOLMET'],
      [8_957_000, 'usb', 'Shannon VOLMET'], [11_253_000, 'usb', 'RAF VOLMET'], [13_264_000, 'usb', 'Shannon VOLMET'],
    ]),
  },
  {
    id: 'bundled-time', name: 'Time Signals', source: 'list', bundled: true, config: { ...DEFAULT_CONFIG },
    channels: tagsOf('time', [
      [2_500_000, 'am', 'WWV / WWVH'], [3_330_000, 'am', 'CHU'], [4_996_000, 'am', 'RWM'], [5_000_000, 'am', 'WWV / WWVH'],
      [7_850_000, 'am', 'CHU'], [9_996_000, 'am', 'RWM'], [10_000_000, 'am', 'WWV / WWVH'], [14_670_000, 'am', 'CHU'],
      [14_996_000, 'am', 'RWM'], [15_000_000, 'am', 'WWV / WWVH'], [20_000_000, 'am', 'WWV'],
    ]),
  },
  {
    id: 'bundled-marine-hf', name: 'Marine HF distress & safety', source: 'list', bundled: true, config: { ...DEFAULT_CONFIG },
    channels: tagsOf('marine', [
      [2_182_000, 'usb', '2182 kHz distress'], [4_125_000, 'usb', '4125 kHz'], [6_215_000, 'usb', '6215 kHz'],
      [8_291_000, 'usb', '8291 kHz'], [12_290_000, 'usb', '12290 kHz'], [16_420_000, 'usb', '16420 kHz'],
    ]),
  },
];

/** A bundled preset is cloned before it can be edited — never edited in place. */
export function clonePreset(p: ScanPreset, newId: string, name?: string): ScanPreset {
  return {
    ...p, id: newId, name: name ?? `${p.name} (copy)`, bundled: false,
    config: normaliseConfig(p.config),
    channels: p.channels?.map((c, i) => ({ ...c, id: `${newId}-${i}` })),
    band: p.band ? { ...p.band } : undefined,
  };
}

/** ★ Freeze the current scan as a preset: the tagging workflow IS the preset editor (§10.1). Stars and ignores persist. */
export function presetFrom(ctl: ScannerController, id: string, name: string, bookmarkGroup?: string): ScanPreset {
  return {
    id, name, source: ctl.source, config: normaliseConfig(ctl.config),
    band: ctl.source === 'band' && ctl.band ? { ...ctl.band } : undefined,
    bookmarkGroup: ctl.source === 'bookmarks' ? bookmarkGroup : undefined,
    channels: ctl.source === 'list' ? ctl.tags.map((t) => ({ ...t })) : undefined,
  };
}

/** Parse the stored preset list (versioned JSON, like userBookmarks). Anything malformed is dropped, never thrown. */
export function parsePresets(json: string | null): ScanPreset[] {
  if (!json) return [];
  try {
    const o = JSON.parse(json);
    const list: unknown[] = Array.isArray(o) ? o : (o && Array.isArray(o.presets) ? o.presets : []);
    const out: ScanPreset[] = [];
    for (const x of list as any[]) {
      if (!x || typeof x.id !== 'string' || typeof x.name !== 'string') continue;
      if (x.source !== 'band' && x.source !== 'bookmarks' && x.source !== 'list') continue;
      out.push({
        id: x.id, name: x.name, source: x.source, config: normaliseConfig(x.config),
        band: x.band && Number.isFinite(x.band.loHz) && Number.isFinite(x.band.hiHz) && Number.isFinite(x.band.stepHz)
          ? { loHz: x.band.loHz, hiHz: x.band.hiHz, stepHz: x.band.stepHz, mode: x.band.mode } : undefined,
        bookmarkGroup: typeof x.bookmarkGroup === 'string' ? x.bookmarkGroup : undefined,
        channels: Array.isArray(x.channels) ? x.channels.filter((c: any) => c && Number.isFinite(c.frequency) && typeof c.id === 'string') : undefined,
      });
    }
    return out;
  } catch { return []; }
}
export function serialisePresets(list: ScanPreset[]): string {
  return JSON.stringify({ v: 1, presets: list.filter((p) => !p.bundled) });
}

/** ★ A bookmark → a tag: its stored passband if it has one, else the mode's default (§2 — no schema change). */
export function bookmarkTag(b: { name: string; frequency: number; mode: string; bandwidth_low?: number | null; bandwidth_high?: number | null },
                            id: string): ScanTag {
  const mode = (b.mode || 'am').toLowerCase() as SDRMode;
  const def = MODE_BANDWIDTHS[mode] ?? [-5000, 5000];
  const lo = b.bandwidth_low, hi = b.bandwidth_high;
  const ok = typeof lo === 'number' && typeof hi === 'number' && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo;
  return { id, frequency: b.frequency, mode: MODE_BANDWIDTHS[mode] ? mode : 'am', bwLow: ok ? lo! : def[0], bwHigh: ok ? hi! : def[1], label: b.name };
}
