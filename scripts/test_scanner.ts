/**
 * THE SCANNER ENGINE (src/services/scanner.ts, docs/v12/SCANNER.md) against fake backends of every family the app
 * supports — UberSDR, KiwiSDR, OpenWebRX (incl. a profile hijack and its revert), VibeServer (range-locked, single-user,
 * shared dial), local hardware — and FM-DX's permanent exclusion. A fake clock; a frame every 100 ms; signals are
 * functions of frequency and time. Silent: nothing here makes a sound.
 *
 * Run: node --no-warnings scripts/test_scanner.ts
 */
import {
  ScannerController, normaliseConfig, setTrigger, scanPolicy, scanTiming, bandChoices, snapSMeter, sMeterDetents,
  ImpulseDetector, nameAt, freqText, validateCandidates, clipBand, bandPassband, presetFrom, serialisePresets, parsePresets,
  BUNDLED_PRESETS, clonePreset, bookmarkTag, DEFAULT_CONFIG, PRIORITY_MS, SOFT_SKIP_MS,
  type ScanHost, type ScanReach, type ScanTarget, type MeterMode, type ScanTag, type ScanTiming, type ScanSample,
} from '../src/services/scanner.ts';
import type { SDRMode } from '../src/services/sdrProtocol.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  ok(what, g === w, `got  ${g}\n   want ${w}`);
}

// ── A fake radio ─────────────────────────────────────────────────────────────────────────────────────────────────
type Sig = (hz: number, t: number) => number;   // dB above the floor at hz (0 = nothing), at time t
interface FakeOpts { meter: MeterMode; reach: ScanReach; floor?: number; timing: ScanTiming; frameMs?: number }

class Fake implements ScanHost {
  t = 0;
  meter: MeterMode;
  r: ScanReach;
  floor: number;
  frameMs: number;
  tunedTo: ScanTarget | null = null;
  tunes: { t: ScanTarget; at: number; reach: ScanReach }[] = [];
  gate = true;
  gateAtTune: boolean[] = [];
  recStarts: ScanTarget[] = [];
  recStops = 0;
  said: string[] = [];
  sigs: Sig[] = [];
  /** per-frame overrides: impulse flag, user dial */
  impulseAt: (t: number) => boolean = () => false;
  userDial: number | null = null;
  ctl: ScannerController;
  names: { name: string; frequency: number; rank: number }[] = [];
  constructor(o: FakeOpts) {
    this.meter = o.meter; this.r = o.reach; this.floor = o.floor ?? -110; this.frameMs = o.frameMs ?? 100;
    this.ctl = new ScannerController(this, o.timing);
  }
  now() { return this.t; }
  reach() { return this.r; }
  tune(t: ScanTarget) { this.tunes.push({ t, at: this.t, reach: this.r }); this.gateAtTune.push(this.gate); this.tunedTo = t; }
  setGate(o: boolean) { this.gate = o; }
  meterMode() { return this.meter; }
  startRecording(t: ScanTarget) { this.recStarts.push(t); }
  stopRecording() { this.recStops++; }
  announce(s: string) { this.said.push(s); }
  nameFor(hz: number, tol: number) { return nameAt(hz, tol, this.names); }
  above(hz: number): number {
    let v = 0;
    for (const s of this.sigs) v = Math.max(v, s(hz, this.t));
    return v;
  }
  sample(): ScanSample {
    const hz = this.userDial ?? this.tunedTo?.frequency ?? 0;
    const a = this.above(hz);
    const imp = this.impulseAt(this.t);
    return { snr: a + (imp ? 25 : 0), level: this.floor + a + (imp ? 25 : 0), floor: this.floor, impulse: imp, tunedHz: hz };
  }
  run(ms: number) {
    const end = this.t + ms;
    while (this.t < end) {
      this.t += this.frameMs;
      this.ctl.feed(this.sample());
      this.ctl.tick();
    }
  }
  tunedHzList(): number[] { return this.tunes.map((x) => x.t.frequency); }
}

const between = (lo: number, hi: number, db: number, hz: number): Sig =>
  (f, t) => (f === hz && t >= lo && t < hi ? db : 0);
const always = (hz: number, db: number): Sig => (f) => (f === hz ? db : 0);

const HF: ScanReach = { allowed: true, loHz: 0, hiHz: 30_000_000, key: 'hf' };
const UBER = (meter: MeterMode = 'snr') => new Fake({ meter, reach: HF, timing: scanTiming('ubersdr', false) });
const KIWI = () => new Fake({ meter: 'smeter', reach: HF, floor: -120, timing: scanTiming('kiwi', false) });
const OWRX_AIR: ScanReach = { allowed: true, loHz: 117_000_000, hiHz: 119_400_000, key: 'p:air' };
const OWRX = () => new Fake({ meter: 'smeter', reach: OWRX_AIR, floor: -105, timing: scanTiming('owrx', false), frameMs: 200 });

let idn = 0;
const tag = (hz: number, mode: SDRMode, extra: Partial<ScanTag> = {}): ScanTag => {
  const bw: Record<string, [number, number]> = { usb: [50, 2700], lsb: [-2700, -50], am: [-5000, 5000], nfm: [-5000, 5000] };
  return { id: `t${++idn}`, frequency: hz, mode, bwLow: bw[mode][0], bwHigh: bw[mode][1], ...extra };
};

// ═══ Policy: which backends offer a scanner at all ═══════════════════════════════════════════════════════════════
{
  eq('FM-DX never', scanPolicy({ kind: 'fmdx', isLocal: false }).offered, false);
  eq('UberSDR yes, no hardware move', scanPolicy({ kind: 'ubersdr', isLocal: false }), { offered: true, movesHardware: false });
  eq('Kiwi yes', scanPolicy({ kind: 'kiwi', isLocal: false }).offered, true);
  eq('Web-888 yes', scanPolicy({ kind: 'web888', isLocal: false }).offered, true);
  eq('OWRX yes, never the hardware', scanPolicy({ kind: 'owrx', isLocal: false }), { offered: true, movesHardware: false });
  eq('VibeServer shared dial: absent', scanPolicy({ kind: 'vibeserver', isLocal: false, sharedDial: true }).offered, false);
  eq('VibeServer range-locked: in the window', scanPolicy({ kind: 'vibeserver', isLocal: false, sharedDial: false, lockedCentreHz: 145e6 }),
     { offered: true, movesHardware: false });
  eq('VibeServer single-user: may retune the radio', scanPolicy({ kind: 'vibeserver', isLocal: false, sharedDial: false }),
     { offered: true, movesHardware: true });
  eq('VibeServer Icom: not in M1', scanPolicy({ kind: 'vibeserver', isLocal: false, icom: true }).offered, false);
  eq('local hardware: retunes the dongle', scanPolicy({ kind: 'ubersdr', isLocal: true }), { offered: true, movesHardware: true });
  eq('OWRX settles slowest', scanTiming('owrx', false).settleMs, 500);
  eq('local settles fastest', scanTiming('vibeserver', true).settleMs, 100);
}

// ═══ Config: the two triggers, migration from the brief's enum ═══════════════════════════════════════════════════
{
  const m1 = normaliseConfig({ advance: 'timed', timedDwellMs: 12000 });
  eq('timed → time limit only', [m1.untilStops, m1.timeLimit, m1.timeLimitMs], [false, true, 12000]);
  const m2 = normaliseConfig({ advance: 'activity', maxDwellMs: 60000 });
  eq('activity + maxDwell → both', [m2.untilStops, m2.timeLimit, m2.timeLimitMs], [true, true, 60000]);
  const m3 = normaliseConfig({ advance: 'activity', maxDwellMs: 0 });
  eq('activity, unlimited → until stops only', [m3.untilStops, m3.timeLimit], [true, false]);
  const m4 = normaliseConfig({ untilStops: false, timeLimit: false });
  eq('never neither', m4.untilStops || m4.timeLimit, true);
  const a = setTrigger(DEFAULT_CONFIG, 'untilStops', false);
  eq('untick the last → the other comes on', [a.untilStops, a.timeLimit], [false, true]);
  const b = setTrigger(a, 'timeLimit', false);
  eq('and back', [b.untilStops, b.timeLimit], [true, false]);
  eq('defaults: until stops ON, time limit OFF, Auto, 5 s hang', [DEFAULT_CONFIG.untilStops, DEFAULT_CONFIG.timeLimit, DEFAULT_CONFIG.thresholdMode, DEFAULT_CONFIG.hangMs],
     [true, false, 'auto', 5000]);
  eq('hang clamped 1–15 s', normaliseConfig({ hangMs: 60000 }).hangMs, 15000);
}

// ═══ Acceptance 9: the band picker ═══════════════════════════════════════════════════════════════════════════════
{
  const r1 = bandChoices(1, { loHz: 0, hiHz: 30e6 });
  const r2 = bandChoices(2, { loHz: 0, hiHz: 30e6 });
  const cb1 = r1.find((b) => b.name === '11m CB Band'), cb2 = r2.find((b) => b.name === '11m CB Band');
  eq('CB R1 = NFM 10 kHz 26.965–27.405', cb1?.spec, { loHz: 26_965_000, hiHz: 27_405_000, stepHz: 10_000, mode: 'nfm' });
  eq('CB R2 = AM 10 kHz', cb2 && [cb2.spec.mode, cb2.spec.stepHz], ['am', 10_000]);
  ok('no 2 m on a 0–30 MHz receiver', !r1.some((b) => b.name === '2m Ham Band' || b.spec.loHz > 30e6));
  const vhf = bandChoices(1, { loHz: 24e6, hiHz: 1.7e9 });
  const air = vhf.find((b) => b.name === 'Airband (VHF Air)');
  ok('airband offered on a VHF receiver', !!air);
  eq('airband R1 = AM', air?.spec.mode, 'am');
  const ssb = r1.find((b) => b.name === '40m Ham Band');
  eq('40 m scans a passband apart, LSB', ssb && [ssb.spec.mode, ssb.spec.stepHz], ['lsb', 2500]);
  const sw = r1.find((b) => b.name === '31m Broadcast Band');
  eq('SW broadcast on the 5 kHz raster', sw?.spec.stepHz, 5000);
  ok('one entry per band name', new Set(r1.map((b) => b.name)).size === r1.length);
  eq('AM passband never wider than an 8.33 raster', bandPassband('am', 8333).map(Math.round), [-3750, 3750]);
  eq('USB passband stays one-sided', bandPassband('usb', 2500)[0] >= 0, true);
}

// ═══ Acceptance 8: threshold UX ══════════════════════════════════════════════════════════════════════════════════
{
  eq('S-meter detents S1…S9 then +10…+40', sMeterDetents(), [-121, -115, -109, -103, -97, -91, -85, -79, -73, -63, -53, -43, -33]);
  eq('snap −80 → S8', snapSMeter(-80), -79);
  eq('snap −60 → S9+10', snapSMeter(-60), -63);
  // Auto works untouched on all four families (UberSDR SNR, Kiwi dBm, OWRX dBm, a VibeServer dBFS):
  for (const [nm, mk] of [['UberSDR', () => UBER()], ['Kiwi', KIWI], ['VibeServer', () => new Fake({ meter: 'dbfs', reach: HF, timing: scanTiming('vibeserver', false) })]] as [string, () => Fake][]) {
    const f = mk();
    f.ctl.setSource('list');
    f.ctl.setTags([tag(7_100_000, 'lsb'), tag(14_200_000, 'usb')]);
    f.sigs.push(between(3000, 60000, 15, 14_200_000));
    f.ctl.start();
    f.run(6000);
    eq(`Auto, untouched, stops (${nm})`, [f.ctl.state, f.ctl.current?.frequency], ['STOPPED', 14_200_000]);
  }
  {
    const f = OWRX();
    f.ctl.setSource('band');
    f.ctl.setBand({ loHz: 118_000_000, hiHz: 118_500_000, stepHz: 25_000, mode: 'am' });
    f.sigs.push(always(118_300_000, 12));
    f.ctl.start(); f.run(30000);
    eq('Auto, untouched, stops (OWRX)', [f.ctl.state, f.ctl.current?.frequency], ['STOPPED', 118_300_000]);
  }
  // Meter-mode change: per-family manual thresholds, never reinterpreted.
  const f = UBER();
  f.ctl.setConfig({ thresholdMode: 'manual', threshold: { snr: 12, dbfs: -95 } });
  f.ctl.setTags([tag(7_100_000, 'lsb')]);
  f.sigs.push(always(7_100_000, 10));     // SNR 10, level −100
  f.ctl.start(); f.run(2000);
  eq('SNR 12 manual: a 10 dB signal does not stop', f.ctl.state === 'STOPPED', false);
  f.ctl.stop(); f.meter = 'dbfs';
  f.ctl.setConfig({ threshold: { snr: 12, dbfs: -105 } });
  f.ctl.start(); f.run(2000);
  eq('dBFS manual −105: the −100 signal stops it (its own number, not 12)', f.ctl.state, 'STOPPED');
  eq('the SNR number was kept untouched', f.ctl.config.threshold.snr, 12);
}

// ═══ Acceptance 2: the QSO hopper (UberSDR) ══════════════════════════════════════════════════════════════════════
{
  const f = UBER();
  const a = tag(7_074_000, 'lsb'), b = tag(14_205_000, 'usb'), c = tag(3_620_000, 'am');
  f.ctl.setTags([a, b, c]);
  // b: an SSB QSO with a 3 s over-change gap; a, c quiet.
  f.sigs.push((hz, t) => (hz === b.frequency && t >= 2000 && t < 30000 && !(t >= 10000 && t < 13000) ? 18 : 0));
  f.ctl.start();
  f.run(5000);
  eq('stopped on the live QSO', [f.ctl.state, f.ctl.current?.frequency], ['STOPPED', b.frequency]);
  const hopsBefore = f.tunes.length;
  f.run(9000);                         // through the 3 s gap (hang 5 s)
  eq('a 3 s over-change does not advance', [f.ctl.state, f.ctl.current?.frequency, f.tunes.length], ['STOPPED', b.frequency, hopsBefore]);
  f.run(30000);
  ok('after the QSO ends it moves on', f.ctl.current?.frequency !== b.frequency || f.tunes.length > hopsBefore);
  // per-tag mode and passband on every tune
  const tb = f.tunes.find((x) => x.t.frequency === b.frequency)!.t;
  eq('tag tuned with its own mode + passband', [tb.mode, tb.bwLow, tb.bwHigh], ['usb', 50, 2700]);
  const tc = f.tunes.find((x) => x.t.frequency === c.frequency)!.t;
  eq('cross-mode list: AM tag in AM', tc.mode, 'am');
  ok('the gate is SHUT on every hop (no audio artefacts on retune)', f.gateAtTune.every((g) => g === false));
}
{
  // noiseRef-relative: a tag made during the strong station's over (−80) holds for the weak one (−112) — the threshold
  // comes from the floor captured at tag time (−120), never from the level at tag time.
  const f = KIWI();
  const t1 = tag(7_150_000, 'lsb', { noiseRefDb: -120 });
  f.ctl.setTags([t1]);
  f.sigs.push((hz, t) => (hz !== t1.frequency ? 0 : t < 4000 ? 40 : t < 30000 ? 8 : 0));   // strong, then weak at +8
  f.ctl.start(); f.run(3000);
  eq('stopped on the strong station', f.ctl.state, 'STOPPED');
  f.run(10000);
  eq('weak station (+8 over its floor) still holds the stop', [f.ctl.state, f.tunes.length], ['STOPPED', 1]);
  // and a floor-less level reading with a stale strong reference would NOT: show the threshold is the noiseRef's
  ok('threshold = noiseRef + margin', Math.abs(f.ctl.effectiveThreshold() - (-120 + 6)) < 1e-9, String(f.ctl.effectiveThreshold()));
}

// ═══ Triggers: until-stops only / time limit only / both — and Hold freezes both ═══════════════════════════════
{
  const mk = (cfg: Parameters<ScannerController['setConfig']>[0]) => {
    const f = UBER();
    f.ctl.setConfig(cfg);
    f.ctl.setTags([tag(7_100_000, 'lsb'), tag(7_200_000, 'lsb')]);
    return f;
  };
  // until stops only: a continuous carrier holds it for ever
  let f = mk({ untilStops: true, timeLimit: false });
  f.sigs.push(always(7_100_000, 20));
  f.ctl.start(); f.run(120000);
  eq('until-stops only: a carrier holds it', [f.ctl.current?.frequency, f.tunes.length], [7_100_000, 1]);
  // time limit only: moves on after the limit even though the signal is still there
  f = mk({ untilStops: false, timeLimit: true, timeLimitMs: 10000 });
  f.sigs.push(always(7_100_000, 20));
  f.ctl.start(); f.run(10500);
  ok('time-limit only: still there before the limit', f.ctl.current?.frequency === 7_100_000);
  f.run(1500);
  ok('time-limit only: gone after it', f.tunes.length >= 2, `tunes ${f.tunes.length}`);
  // time limit only ignores a signal that drops (no hang)
  f = mk({ untilStops: false, timeLimit: true, timeLimitMs: 10000 });
  f.sigs.push(between(0, 2500, 20, 7_100_000));
  f.ctl.start(); f.run(9000);
  eq('time-limit only: the signal stopping does not move it', [f.ctl.current?.frequency, f.tunes.length], [7_100_000, 1]);
  // both, carrier → the limit wins
  f = mk({ untilStops: true, timeLimit: true, timeLimitMs: 15000 });
  f.sigs.push(always(7_100_000, 20));
  f.ctl.start(); f.run(16500);
  ok('both: a carrier escapes on the limit', f.tunes.length >= 2);
  // both, short over → the hang wins
  f = mk({ untilStops: true, timeLimit: true, timeLimitMs: 60000, hangMs: 2000 });
  f.sigs.push(between(0, 3000, 20, 7_100_000));
  f.ctl.start(); f.run(6000);
  ok('both: a short over escapes on the hang', f.tunes.length >= 2);
  // Hold freezes both
  f = mk({ untilStops: true, timeLimit: true, timeLimitMs: 8000, hangMs: 2000 });
  f.sigs.push(between(0, 3000, 20, 7_100_000));
  f.ctl.start(); f.run(1500);
  f.ctl.hold();
  eq('HOLD', f.ctl.state, 'HOLD');
  ok('the gate is open on hold', f.gate);
  f.run(30000);
  eq('Hold freezes the hang AND the limit', [f.ctl.state, f.tunes.length], ['HOLD', 1]);
  f.ctl.hold();
  eq('Hold again resumes the stop', f.ctl.state, 'STOPPED');
  f.run(3000);
  ok('…and the hang then runs', f.tunes.length >= 2);
}

// ═══ Acceptance 5 (probe): priority ══════════════════════════════════════════════════════════════════════════════
{
  const f = UBER();
  const a = tag(7_100_000, 'lsb'), star = tag(14_300_000, 'usb', { priority: true });
  f.ctl.setTags([a, star]);
  f.sigs.push(always(a.frequency, 20));
  f.sigs.push(between(20000, 90000, 20, star.frequency));
  f.ctl.start(); f.run(15000);
  eq('dwelling on A', f.ctl.current?.frequency, a.frequency);
  const probesQuiet = f.tunes.filter((x) => x.t.frequency === star.frequency).length;
  ok('the quiet star was probed while stopped on A', probesQuiet >= 2, `probes ${probesQuiet}`);
  ok('every probe came back to A', f.tunes[f.tunes.length - 1].t.frequency === a.frequency);
  f.run(PRIORITY_MS + 3000 + 2000);
  eq('the star going live interrupts the stop', [f.ctl.state, f.ctl.current?.frequency], ['STOPPED', star.frequency]);
  ok('announced as a priority', f.said.some((s) => s.startsWith('Priority')));
}
{
  // time-limit-only mode: priority still cuts in
  const f = UBER();
  f.ctl.setConfig({ untilStops: false, timeLimit: true, timeLimitMs: 120000 });
  const a = tag(7_100_000, 'lsb'), star = tag(14_300_000, 'usb', { priority: true });
  f.ctl.setTags([a, star]);
  f.sigs.push(always(a.frequency, 20), between(6000, 90000, 20, star.frequency));
  f.ctl.start(); f.run(20000);
  eq('priority cuts in under time-limit only', f.ctl.current?.frequency, star.frequency);
}
{
  // HOLD suppresses it
  const f = UBER();
  const a = tag(7_100_000, 'lsb'), star = tag(14_300_000, 'usb', { priority: true });
  f.ctl.setTags([a, star]);
  f.sigs.push(always(a.frequency, 20), always(star.frequency, 20));
  f.ctl.start(); f.run(1500);
  eq('stopped on A first', f.ctl.current?.frequency, a.frequency);
  f.ctl.hold();
  const n = f.tunes.length;
  f.run(30000);
  eq('HOLD suppresses priority (no probe, still A)', [f.ctl.current?.frequency, f.tunes.length], [a.frequency, n]);
}

// ═══ Acceptance 6: record-on-stop ════════════════════════════════════════════════════════════════════════════════
{
  const f = UBER();
  f.ctl.setConfig({ recordOnStop: true, hangMs: 1000 });
  const a = tag(7_100_000, 'lsb'), star = tag(14_300_000, 'usb', { priority: true });
  f.ctl.setTags([a, star]);
  f.sigs.push(between(0, 20000, 20, a.frequency));
  f.ctl.start(); f.run(1500);
  eq('a stop starts the recorder with its target', f.recStarts.map((t) => t.frequency), [a.frequency]);
  const n = f.tunes.length;
  f.run(15000);
  eq('no priority probe tunes while recording (a clean file)', f.tunes.length, n);
  f.run(6000);
  eq('leaving the stop ends the recording', f.recStops, 1);
  const g = UBER();
  g.ctl.setConfig({ recordOnStop: true });
  g.ctl.setTags([a]); g.sigs.push(always(a.frequency, 20));
  (g as any).manualRecording = () => true;
  g.ctl.start(); g.run(2000);
  eq('a manual recording in progress is left alone', g.recStarts.length, 0);
}

// ═══ Acceptance 7: OWRX safety — hijack → PAUSED, revert → resume, never out of span ═════════════════════════════
{
  const f = OWRX();
  const tags = [tag(118_100_000, 'am'), tag(118_700_000, 'am'), tag(119_100_000, 'am')];
  f.ctl.setTags(tags);
  f.sigs.push(between(0, 4000, 15, 118_700_000));
  f.ctl.start(); f.run(3000);
  ok('scanning on the airband profile', f.ctl.running());
  // another listener switches the SDR to a 2 m profile
  f.r = { allowed: true, loHz: 144_000_000, hiHz: 146_400_000, key: 'p:2m' };
  f.run(200);   // ONE tick
  eq('PAUSED within one config message', f.ctl.state, 'PAUSED');
  ok('banner lists the unreachable channels', f.ctl.unreachable.length === 3, JSON.stringify(f.ctl.unreachable));
  ok('the audio is open while paused', f.gate);
  const n = f.tunes.length;
  f.run(10000);
  eq('nothing is tuned while paused', f.tunes.length, n);
  f.r = OWRX_AIR;
  f.run(200);
  ok('auto-resumes on revert', f.ctl.state !== 'PAUSED' && f.ctl.running());
  ok('"Scan resumed" said', f.said.includes('Scan resumed'));
  // a profile switch whose span still covers the list keeps scanning
  f.r = { ...OWRX_AIR, key: 'p:air-wide', loHz: 110_000_000, hiHz: 125_000_000 };
  f.run(400);
  ok('a covering profile keeps scanning', f.ctl.state !== 'PAUSED');
  // the user's OWN profile switch to one that does not cover → refused like start, not paused
  f.ctl.noteLocalSwitch();
  f.r = { allowed: true, loHz: 144_000_000, hiHz: 146_400_000, key: 'p:2m' };
  f.run(200);
  eq('own switch to a non-covering profile stops with a refusal', [f.ctl.state, !!f.ctl.refusal], ['IDLE', true]);
  ok('…and the gate is open', f.gate);
}
{
  // Fuzz: the span jumps about at random; EVERY tune ever emitted lies inside the span current at that moment.
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const f = OWRX();
  f.ctl.setSource('band');
  f.ctl.setBand({ loHz: 118_000_000, hiHz: 119_000_000, stepHz: 25_000, mode: 'am' });
  f.sigs.push((hz, t) => ((hz / 25000 + Math.floor(t / 3000)) % 7 === 0 ? 14 : 0));
  f.ctl.start();
  for (let i = 0; i < 400; i++) {
    if (rnd() < 0.15) {
      const lo = 117e6 + Math.floor(rnd() * 3) * 0.6e6;
      f.r = { allowed: rnd() > 0.1, loHz: lo, hiHz: lo + 1.2e6, key: `k${i}` };
    }
    f.run(200 + Math.floor(rnd() * 800));
    if (rnd() < 0.05) f.ctl.skip();
    if (rnd() < 0.03) f.ctl.hold();
    if (f.ctl.state === 'IDLE' && rnd() < 0.3) f.ctl.start({ reachableOnly: true });
  }
  const bad = f.tunes.filter((x) => !(x.reach.allowed && x.t.frequency >= x.reach.loHz && x.t.frequency <= x.reach.hiHz));
  eq(`fuzz: ${f.tunes.length} tunes, none outside the span`, bad.length, 0);
}
{
  // VibeServer range-locked: the owner moves the locked window → PAUSED; back → resumes. The radio becoming a shared
  // dial (allowed false) → PAUSED with the reason.
  const W: ScanReach = { allowed: true, loHz: 144_550_000, hiHz: 146_350_000, key: 'lock:145.45' };
  const f = new Fake({ meter: 'dbfs', reach: W, timing: scanTiming('vibeserver', false) });
  f.ctl.setTags([tag(145_500_000, 'nfm'), tag(144_800_000, 'nfm')]);
  f.ctl.start(); f.run(2000);
  f.r = { allowed: true, loHz: 432_100_000, hiHz: 433_900_000, key: 'lock:433' };
  f.run(100);
  eq('VibeServer: the owner moves the window → PAUSED', f.ctl.state, 'PAUSED');
  f.r = W; f.run(100);
  ok('…and back → resumes', f.ctl.state !== 'PAUSED');
  f.r = { ...W, allowed: false, why: 'This radio has one shared dial — scanning would retune every listener' };
  f.run(100);
  eq('becomes a shared dial → PAUSED with the reason', [f.ctl.state, f.ctl.pauseWhy], ['PAUSED', 'This radio has one shared dial — scanning would retune every listener']);
}
{
  // Refusal of out-of-span candidates at start; "Start with N reachable" is an explicit action; the out one is never tuned.
  const f = OWRX();
  const inA = tag(118_100_000, 'am'), outB = tag(145_500_000, 'nfm'), inC = tag(118_900_000, 'am');
  f.ctl.setTags([inA, outB, inC]);
  eq('refuses to start with anything out of span', f.ctl.start(), false);
  eq('says how many it can reach', f.ctl.refusal?.okCount, 2);
  ok('names the unreachable one', !!f.ctl.refusal?.out[0]?.startsWith('145.500 MHz'));
  eq('nothing tuned on a refusal', f.tunes.length, 0);
  eq('Start with 2 reachable', f.ctl.start({ reachableOnly: true }), true);
  f.run(20000);
  ok('the out-of-span tag is never sent', !f.tunedHzList().includes(outB.frequency));
  // a band partly outside the span
  const g = OWRX();
  g.ctl.setSource('band');
  g.ctl.setBand({ loHz: 118_000_000, hiHz: 121_000_000, stepHz: 25_000, mode: 'am' });
  eq('a band past the profile edge refuses', g.ctl.start(), false);
  eq('scan the reachable part', g.ctl.start({ reachableOnly: true }), true);
  g.run(60000);
  ok('…never past the edge', g.tunedHzList().every((hz) => hz <= OWRX_AIR.hiHz));
  eq('validateCandidates', validateCandidates([{ frequency: 1 }, { frequency: 2e8 }], OWRX_AIR).out.length, 2);
  eq('clipBand', clipBand({ loHz: 118e6, hiHz: 121e6, stepHz: 25000, mode: 'am' }, OWRX_AIR)?.hiHz, 119_400_000);
}

// ═══ Noise spikes never stop it; VOLMET never traps it (Stuart's band-scan rules) ══════════════════════════════════
{
  // a broadband crash every 700 ms (flagged impulse by the detector) → no stop, no found entry
  const f = UBER();
  f.ctl.setSource('band');
  f.ctl.setBand({ loHz: 7_000_000, hiHz: 7_100_000, stepHz: 2500, mode: 'lsb' });
  f.impulseAt = (t) => t % 700 < 200;
  f.ctl.start(); f.run(60000);
  eq('static crashes: never a stop', f.said.filter((s) => s.startsWith('Stopped')).length, 0);
  eq('static crashes: the found list stays empty', f.ctl.found.length, 0);
  // narrow single-frame blips on the channel itself (not broadband): the sustain rule rejects them
  const g = UBER();
  g.ctl.setSource('band');
  g.ctl.setBand({ loHz: 7_000_000, hiHz: 7_100_000, stepHz: 2500, mode: 'lsb' });
  g.sigs.push((hz, t) => (t % 300 === 0 ? 30 : 0));   // one frame in three, everywhere it lands
  g.ctl.start(); g.run(60000);
  eq('one-frame spikes: never a stop', g.ctl.found.length, 0);
  // a real intermittent signal still stops reliably — five overs of 2 s, each caught
  const h = UBER();
  h.ctl.setSource('band');
  h.ctl.setConfig({ hangMs: 1000 });
  // ★ ATC-like: a 5 s over every 15 s on one of nine channels (a pass of the nine takes ~5.4 s on UberSDR).
  h.ctl.setBand({ loHz: 7_000_000, hiHz: 7_020_000, stepHz: 2500, mode: 'lsb' });
  h.sigs.push((hz, t) => (hz === 7_012_500 && (t % 15000) < 5000 ? 15 : 0));
  h.impulseAt = (t) => t % 1300 < 100;      // with crashes going on
  h.ctl.start(); h.run(75000);
  const hits = h.ctl.found.find((x) => x.frequency === 7_012_500)?.hits ?? 0;
  ok(`intermittent signal caught on most overs (${hits} of 5)`, hits >= 4);
  ok('and nothing else found', h.ctl.found.length === 1, JSON.stringify(h.ctl.found.map((x) => x.frequency)));
}
{
  // VOLMET: a continuous carrier, time limit on → escaped every time, then offered for Ignore (never auto-ignored);
  // one tap ignores it for good, and the ignore persists in the preset.
  const f = UBER();
  f.ctl.setSource('band');
  f.ctl.setConfig({ timeLimit: true, timeLimitMs: 15000 });
  f.ctl.setBand({ loHz: 5_440_000, hiHz: 5_460_000, stepHz: 2500, mode: 'usb' });
  f.sigs.push(always(5_450_000, 20));
  f.ctl.start(); f.run(80000);
  const v = f.ctl.found.find((x) => x.frequency === 5_450_000)!;
  ok('the carrier was escaped by the time limit repeatedly', v.capped >= 2, `capped ${v?.capped}`);
  eq('…and is offered as "Ignore?"', f.ctl.ignoreHint(v), true);
  ok('…but NOT ignored without a tap', f.ctl.config.lockouts.length === 0);
  f.ctl.ignoreTarget({ frequency: v.frequency });
  const n = f.tunes.length;
  f.run(60000);
  ok('once ignored, never tuned again', f.tunes.slice(n).every((x) => x.t.frequency !== 5_450_000));
  eq('shown in the Ignored section', f.ctl.foundRows().ignored.map((x) => x.frequency), [5_450_000]);
  const p = parsePresets(serialisePresets([presetFrom(f.ctl, 'p1', 'RAF')]))[0];
  eq('the ignore persists in the preset', p.config.lockouts, [{ frequency: 5_450_000, halfWidthHz: 1250 }]);
  f.ctl.unignore(5_450_000);
  eq('Un-ignore', f.ctl.config.lockouts.length, 0);
}
{
  // Ignore while stopped (the live control) + its undo; Skip is a soft ignore that expires.
  const f = UBER();
  f.ctl.setConfig({ timeLimit: true, timeLimitMs: 10000 });
  f.ctl.setTags([tag(7_100_000, 'lsb'), tag(7_150_000, 'lsb'), tag(7_190_000, 'lsb')]);
  f.sigs.push(always(7_100_000, 20), always(7_150_000, 20));
  f.ctl.start(); f.run(1500);
  eq('stopped on the first', f.ctl.current?.frequency, 7_100_000);
  const undo = f.ctl.ignoreCurrent();
  f.run(1500);
  eq('Ignore moves on to the next live one', f.ctl.current?.frequency, 7_150_000);
  ok('the tag is marked ignored', f.ctl.tags[0].lockedOut === true);
  undo?.();
  ok('Undo restores it', f.ctl.tags[0].lockedOut === false);
  f.ctl.skip();
  f.run(SOFT_SKIP_MS - 5000);
  ok('Skip: passed over for a few minutes', f.tunes.filter((x) => x.t.frequency === 7_150_000).length === 1);
  f.run(30000);
  ok('…then back in the scan', f.tunes.filter((x) => x.t.frequency === 7_150_000).length >= 2);
}

// ═══ The person at the dial; the gate; the found list ════════════════════════════════════════════════════════════
{
  const f = UBER();
  f.ctl.setTags([tag(7_100_000, 'lsb'), tag(7_200_000, 'lsb')]);
  f.sigs.push(always(7_100_000, 20));
  f.ctl.start(); f.run(1500);
  f.userDial = 7_123_000;
  f.run(500);
  eq('the user tuning away holds the scan', f.ctl.state, 'HOLD');
  const g = UBER();
  g.ctl.setTags([tag(7_100_000, 'lsb'), tag(7_200_000, 'lsb')]);
  g.ctl.start(); g.run(150);
  eq('mid-hop the gate is shut', g.gate, false);
  g.ctl.stop();
  eq('stop always leaves the gate open', g.gate, true);
  // found list: newest-active first
  const h = UBER();
  h.ctl.setSource('band'); h.ctl.setConfig({ hangMs: 1000 });
  h.ctl.setBand({ loHz: 7_000_000, hiHz: 7_020_000, stepHz: 2500, mode: 'lsb' });
  h.sigs.push(between(0, 3000, 20, 7_005_000), between(8000, 15000, 20, 7_015_000));
  h.ctl.start(); h.run(20000);
  eq('found list newest first', h.ctl.foundRows().live.map((x) => x.frequency), [7_015_000, 7_005_000]);
}

// ═══ Impulse detector on synthetic rows ══════════════════════════════════════════════════════════════════════════
{
  const d = new ImpulseDetector();
  const row = (lvl: number, peak = -1) => Float32Array.from({ length: 2048 }, (_, i) => lvl + ((i * 7919) % 11) * 0.3 + (i === peak ? 40 : 0));
  let flagged = 0;
  for (let i = 0; i < 50; i++) if (d.feed(row(-110, 700), 'v', i * 100)) flagged++;
  eq('ordinary rows with a strong carrier: never an impulse', flagged, 0);
  eq('a whole-span crash is one', d.feed(row(-90), 'v', 5100), true);
  eq('back to normal', d.feed(row(-110), 'v', 5200), false);
  let t = 6000, lastFlag = true;
  for (let i = 0; i < 40; i++) { lastFlag = d.feed(row(-95), 'v', t); t += 100; }
  eq('a rise that stays becomes the new normal', lastFlag, false);
  eq('a view change resets it', d.feed(row(-60), 'other', t), false);
}

// ═══ Names beside frequencies ════════════════════════════════════════════════════════════════════════════════════
{
  const list = [
    { name: 'London South Information', frequency: 128_590_000, rank: 1 },
    { name: 'Wrong neighbour', frequency: 128_600_000, rank: 1 },
    { name: 'My own name', frequency: 118_100_000, rank: 0 },
    { name: 'Server name', frequency: 118_101_000, rank: 1 },
  ];
  eq('a stop on a bookmarked frequency shows its name', nameAt(128_590_000, 12_500, list), 'London South Information');
  eq('the nearest of two within tolerance wins', nameAt(128_598_000, 12_500, list), 'Wrong neighbour');
  eq('none → frequency only', nameAt(130_000_000, 12_500, list), undefined);
  eq("the user's own bookmark wins over the server's", nameAt(118_101_000, 4000, list), 'My own name');
  eq('format', freqText(128_590_000), '128.590 MHz');
  eq('8.33 channel precision', freqText(118_008_333), '118.00833 MHz');
  const f = UBER();
  f.names = [{ name: 'Shannon VOLMET', frequency: 5_505_000, rank: 0 }];
  f.ctl.setTags([tag(5_505_000, 'usb')]); f.sigs.push(always(5_505_000, 20));
  f.ctl.start(); f.run(1500);
  ok('VoiceOver: "Stopped on 5.505 megahertz USB, Shannon VOLMET"', f.said.includes('Stopped on 5.505 megahertz USB, Shannon VOLMET'), JSON.stringify(f.said));
}

// ═══ Presets ═════════════════════════════════════════════════════════════════════════════════════════════════════
{
  ok('three bundled starters', BUNDLED_PRESETS.length === 3 && BUNDLED_PRESETS.every((p) => p.bundled && p.source === 'list'));
  const c = clonePreset(BUNDLED_PRESETS[0], 'mine');
  ok('a clone is editable and keeps the channels', !c.bundled && c.channels!.length === BUNDLED_PRESETS[0].channels!.length);
  ok('bundled are never serialised', !serialisePresets([BUNDLED_PRESETS[0], c]).includes('bundled-volmet'));
  const f = UBER();
  f.ctl.setTags([tag(7_100_000, 'lsb', { priority: true }), tag(14_200_000, 'usb', { lockedOut: true })]);
  const p = parsePresets(serialisePresets([presetFrom(f.ctl, 'p2', '40/20')]))[0];
  eq('stars and ignores persist in preset channels', p.channels!.map((x) => [x.priority, x.lockedOut]), [[true, undefined], [undefined, true]]);
  eq('garbage in → nothing, no throw', parsePresets('{nope'), []);
  const bt = bookmarkTag({ name: 'X', frequency: 1e6, mode: 'USB' }, 'b1');
  eq('a bookmark without a stored passband takes the mode default', [bt.mode, bt.bwLow, bt.bwHigh], ['usb', 50, 2700]);
  const bt2 = bookmarkTag({ name: 'Y', frequency: 1e6, mode: 'am', bandwidth_low: -3000, bandwidth_high: 3000 }, 'b2');
  eq('…and its own when it has one', [bt2.bwLow, bt2.bwHigh], [-3000, 3000]);
}

// ═══ Local hardware: retunes anywhere in range; KiwiSDR without a floor: the sweep finds its own quiet ═══════════
{
  const f = new Fake({ meter: 'dbfs', reach: { allowed: true, loHz: 24e6, hiHz: 1.7e9, key: 'local' }, timing: scanTiming('ubersdr', true) });
  f.ctl.setTags([tag(145_500_000, 'nfm'), tag(433_500_000, 'nfm')]);
  f.sigs.push(always(433_500_000, 15));
  f.ctl.start(); f.run(3000);
  eq('local: hops across bands (the dongle follows)', f.ctl.current?.frequency, 433_500_000);
  const k = KIWI();
  (k as any).sample = function () {
    const s = Fake.prototype.sample.call(this); return { ...s, floor: NaN };     // a zoomed-out Kiwi: no neighbour floor
  };
  k.ctl.setSource('band');
  k.ctl.setBand({ loHz: 7_000_000, hiHz: 7_100_000, stepHz: 2500, mode: 'lsb' });
  k.sigs.push(between(40000, 120000, 15, 7_050_000));
  k.ctl.start(); k.run(80000);
  ok('Kiwi, no floor from the view: Auto still finds the signal', k.ctl.found.some((x) => x.frequency === 7_050_000), JSON.stringify(k.ctl.found));
  eq('Kiwi, no floor: nothing false', k.ctl.found.length, 1);
}

console.log(`test_scanner: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
