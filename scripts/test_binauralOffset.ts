/**
 * BINAURAL TUNING — the app's half (src/services/binauralOffset.ts): which offset source per mode, and the
 * spectrum readings (AM carrier, CW peak, FM centroid) on synthetic frames with KNOWN offsets.
 *
 * Proves:
 *   • the plan per mode: SSB → the native estimator, AM/CW → peak, FM → centroid, DAB → nothing;
 *   • a dead zone of at least ±15 Hz, and never finer than the spectrum can resolve;
 *   • the SIGN: a signal ABOVE where it should be reads + (lean right = tune up), below reads −;
 *   • an AM carrier against the dial, a CW peak against the PASSBAND CENTRE (a Kiwi's 300-700 Hz), an NFM
 *     signal's centroid — each within a fraction of a bin;
 *   • noise alone is never a reading;
 *   • the tracker holds through a CW space, lets go after it, sends the source/dial/offset only on change and
 *     at most every 100 ms.
 *
 * Run: node --no-warnings scripts/test_binauralOffset.ts   (silent: no audio anywhere)
 */
import {
  binauralPlan, binauralModeOk, spectrumOffset, BinauralTracker,
  BIN_SRC_SSB_USB, BIN_SRC_SSB_LSB, BIN_SRC_GIVEN, BIN_SRC_NONE,
} from '../src/services/binauralOffset.ts';

let fails = 0;
const ok = (c: boolean, m: string) => { if (c) console.log('  ✓ ' + m); else { fails++; console.log('  ✗ ' + m); } };

// A deterministic noise source, so the test means the same thing every run.
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };

const N = 2048, BW = 48000, CENTRE = 7_100_000, BIN = BW / N;   // 23.4 Hz bins
const freqOf = (k: number) => CENTRE + (k - N / 2) * BIN;
function noise(): Float32Array { const b = new Float32Array(N); for (let k = 0; k < N; k++) b[k] = -100 + 3 * (rnd() - 0.5); return b; }
/** A narrow line (carrier / CW mark) at `hz`, drawn as a window's main lobe across the neighbouring bins. */
function line(b: Float32Array, hz: number, db: number) {
  const x = (hz - CENTRE) / BIN + N / 2;
  for (let k = Math.floor(x) - 3; k <= Math.ceil(x) + 3; k++) {
    const d = k - x;
    const p = db - 6 * d * d;           // a parabola in dB — what a Hann main lobe looks like near its top
    b[k] = Math.max(b[k], p);
  }
}
/** A band of signal (FM): flat-topped from f1 to f2 at `db`. */
function band(b: Float32Array, f1: number, f2: number, db: number) {
  for (let k = 0; k < N; k++) { const f = freqOf(k); if (f >= f1 && f <= f2) b[k] = db + 2 * (rnd() - 0.5); }
}

console.log('── the plan per mode ──');
{
  const u = binauralPlan('usb', 50, 2700, BIN), l = binauralPlan('lsb', -2700, -50, BIN);
  ok(u.kind === 'ssb' && u.source === BIN_SRC_SSB_USB && l.source === BIN_SRC_SSB_LSB, 'USB/LSB → the native voice estimator, by sideband');
  ok(u.deadHz === 15 && u.fullHz === 300, 'SSB: dead zone ±15 Hz, full lean at 300 Hz off');
  const am = binauralPlan('am', -5000, 5000, BIN), cw = binauralPlan('cwu', 300, 700, BIN);
  ok(am.kind === 'peak' && am.source === BIN_SRC_GIVEN && am.refHz === 0, 'AM → the carrier peak, against the dial');
  ok(cw.kind === 'peak' && cw.refHz === 500, 'CW → the peak, against the PASSBAND CENTRE (Kiwi 300-700 → 500 Hz)');
  ok(binauralPlan('cwu', -200, 200, BIN).refHz === 0, 'CW centred on the dial (VibeServer/UberSDR ±200) → 0');
  const nfm = binauralPlan('nfm', -5000, 5000, BIN), wfm = binauralPlan('wfm', -100000, 100000, 1000);
  ok(nfm.kind === 'centroid' && nfm.deadHz === 200 && nfm.fullHz === 2500, 'NFM → centroid, dead ±200 Hz, full at 2.5 kHz');
  ok(wfm.kind === 'centroid' && wfm.deadHz === 4000 && wfm.fullHz === 50000, 'WFM → centroid, dead ±4 kHz, full at 50 kHz');
  ok(binauralPlan('dab', 0, 0, BIN).source === BIN_SRC_NONE && !binauralModeOk('dab') && !binauralModeOk('adsb'),
     'DAB and other whole-profile modes → none (the row hides)');
  ok(binauralModeOk('usb') && binauralModeOk('am') && binauralModeOk('cwl') && binauralModeOk('wfm'), 'every listening mode is offered');
  const coarse = binauralPlan('am', -5000, 5000, 1200);
  ok(coarse.deadHz >= 0.6 * 1200 && coarse.fullHz >= 3 * coarse.deadHz,
     'a coarse spectrum (1.2 kHz bins) widens the dead zone — a quantised peak must never lean');
}

console.log('── spectrum readings with known offsets ──');
{
  const g = { centerHz: CENTRE, bwHz: BW, tuneHz: CENTRE + 1000, lo: -5000, hi: 5000 };
  for (const off of [+180, -260, +37]) {
    const b = noise(); line(b, g.tuneHz + off, -50);
    const r = spectrumOffset(b, g, 'peak', 0);
    ok(r.ok && Math.abs(r.hz - off) < BIN * 0.3, `AM carrier ${off > 0 ? '+' : ''}${off} Hz → read ${r.hz.toFixed(1)} Hz (sign right, < 0.3 bin)`);
  }
  {
    const gk = { centerHz: CENTRE, bwHz: BW, tuneHz: CENTRE - 3000, lo: 300, hi: 700 };   // a Kiwi CWU passband
    const b = noise(); line(b, gk.tuneHz + 500 - 120, -70);
    const r = spectrumOffset(b, gk, 'peak', 500);
    ok(r.ok && Math.abs(r.hz + 120) < BIN * 0.3, `CW mark 120 Hz below the passband centre → ${r.hz.toFixed(1)} Hz (lean LEFT)`);
  }
  {
    const b = noise(); band(b, g.tuneHz - 1500 - 3000, g.tuneHz - 1500 + 3000, -70);
    const r = spectrumOffset(b, { ...g, lo: -6000, hi: 6000 }, 'centroid', 0);
    // ★ The passband clips the signal's lower edge (−4.5…+1.5 kHz of a ±6 kHz window keeps it all, so exact).
    ok(r.ok && Math.abs(r.hz + 1500) < 150, `NFM signal 1.5 kHz below the dial → centroid ${r.hz.toFixed(0)} Hz`);
    const b2 = noise(); band(b2, g.tuneHz + 3500 - 3000, g.tuneHz + 3500 + 3000, -70);
    const r2 = spectrumOffset(b2, { ...g, lo: -5000, hi: 5000 }, 'centroid', 0);
    ok(r2.ok && r2.hz > 1500 && r2.hz < 3500,
       `NFM signal 3.5 kHz above, its top 1.5 kHz outside the passband → ${r2.hz.toFixed(0)} Hz (pulled in, sign right)`);
    const b3 = noise(); band(b3, g.tuneHz - 3000, g.tuneHz + 3000, -70);
    const r3 = spectrumOffset(b3, { ...g, lo: -6000, hi: 6000 }, 'centroid', 0);
    ok(r3.ok && Math.abs(r3.hz) < 200, `NFM on tune → ${r3.hz.toFixed(0)} Hz (inside the ±200 Hz dead zone)`);
  }
  {
    const b = noise();
    ok(!spectrumOffset(b, g, 'peak', 0).ok && !spectrumOffset(b, g, 'centroid', 0).ok, 'noise alone is never a reading');
    const nan = noise(); nan[1500] = NaN;
    const r = spectrumOffset(nan, g, 'centroid', 0);
    ok(!r.ok || Number.isFinite(r.hz), 'a NaN bin cannot make a NaN offset');
  }
}

console.log('── the tracker ──');
{
  const calls: string[] = [];
  const sink = {
    setBinauralSource: (s: number, d: number, f: number) => calls.push(`src ${s} ${Math.round(d)} ${Math.round(f)}`),
    setBinauralOffset: (hz: number, v: boolean) => calls.push(`off ${v ? Math.round(hz) : 'x'}`),
    setBinauralDial: (hz: number) => calls.push(`dial ${hz}`),
    setBinauralStereo: (h: number) => calls.push(`st ${h}`),
  };
  const t = new BinauralTracker(sink);
  const g = { centerHz: CENTRE, bwHz: BW, tuneHz: CENTRE, lo: -200, hi: 200 };
  let now = 1000;
  const mark = () => { const b = noise(); line(b, CENTRE - 150, -60); return b; };
  t.onFrame(mark(), g, 'cwu', now);
  ok(calls[0] === 'src 1 15 300' && calls[1] === `dial ${CENTRE}` && calls[2] === 'off -150',
     'first frame: source, dial, then the offset (−150 Hz: lean left)');
  const n0 = calls.length;
  for (let i = 0; i < 5; i++) { now += 100; t.onFrame(mark(), g, 'cwu', now); }
  ok(calls.length === n0, 'a steady signal sends NOTHING more (only changes cross the bridge)');
  // A CW space: no peak for 1 s → held; after 1.5 s → let go.
  for (let i = 0; i < 10; i++) { now += 100; t.onFrame(noise(), g, 'cwu', now); }
  ok(calls.length === n0, 'a 1 s CW space: the reading is HELD (no flick back to centre)');
  for (let i = 0; i < 6; i++) { now += 100; t.onFrame(noise(), g, 'cwu', now); }
  ok(calls[calls.length - 1] === 'off x', 'past the 1.5 s hold: let go (back to centre)');
  // Throttle: readings that change every 30 ms reach native at most every 100 ms.
  const before = calls.length;
  for (let i = 0; i < 30; i++) { now += 30; const b = noise(); line(b, CENTRE + 40 + i * 5, -60); t.onFrame(b, g, 'cwu', now); }
  const sent = calls.slice(before).filter((c) => c.startsWith('off')).length;
  ok(sent >= 6 && sent <= 10, `30 changing frames in 0.9 s → ${sent} offset messages (≤ one per 100 ms)`);
  // Mode change to USB: the native estimator takes over; no spectrum offsets are sent.
  const b4 = calls.length;
  now += 200; t.onFrame(noise(), { ...g, lo: 50, hi: 2700 }, 'usb', now);
  ok(calls[b4] === 'src 2 15 300' && !calls.slice(b4).some((c) => c.startsWith('off')), 'switch to USB → native source 2, no spectrum offsets');
  now += 100; t.onFrame(noise(), { ...g, tuneHz: CENTRE + 50, lo: 50, hi: 2700 }, 'usb', now);
  ok(calls[calls.length - 1] === `dial ${CENTRE + 50}`, 'the dial is passed on (native carries its SSB answer across the move)');
  t.setStereo(1); t.setStereo(1);
  ok(calls.filter((c) => c === 'st 1').length === 1, 'the stereo hint is sent once per change');
}

console.log(fails ? `\nFAILED: ${fails}` : '\nall passed');
process.exit(fails ? 1 : 0);
