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
 *     at most every 100 ms;
 *   • the COARSE SSB cue from the waterfall: a USB voice +500 / −800 / +1500 Hz off (and an LSB one) → the right
 *     sign and size within 150 Hz; coarse bins, noise → nothing; the average survives a retune; the tracker sends
 *     the ABSOLUTE carrier, holds it through a pause and lets go after;
 *   • THE SPREAD MAP: which map per mode (none for AM/FM/DAB), the fixed scale = the protocol's default USB
 *     passband, and — ONE behaviour since 476, no GUIDE / SPREAD style — the tracker sends each mode's map BY
 *     ITSELF, only on change, with no style message, and keeps working out the SSB coarse cue with a map set.
 *
 * Run: node --no-warnings scripts/test_binauralOffset.ts   (silent: no audio anywhere)
 */
import {
  binauralPlan, binauralModeOk, spectrumOffset, BinauralTracker, SsbCoarse, spreadKindFor,
  BIN_SRC_SSB_USB, BIN_SRC_SSB_LSB, BIN_SRC_GIVEN, BIN_SRC_NONE, BIN_SPREAD_EDGES,
  BIN_SPREAD_OFF, BIN_SPREAD_USB, BIN_SPREAD_LSB, BIN_SPREAD_CW_UPPER, BIN_SPREAD_CW_LOWER,
} from '../src/services/binauralOffset.ts';
import { MODE_BANDWIDTHS } from '../src/services/sdrProtocol.ts';

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
  // ★ ±50 Hz since 477 (was AM's ±15): a voice cannot be measured that finely, and QSO stations sit tens of Hz apart.
  ok(u.deadHz === 50 && u.fullHz === 1000 && l.deadHz === 50 && l.fullHz === 1000, 'SSB: dead zone ±50 Hz, full lean at 1 kHz off (both sidebands)');
  ok(binauralPlan('am', -5000, 5000, 0).deadHz === 15 && binauralPlan('cwu', 300, 700, 0).deadHz === 15,
     'AM and CW keep the ±15 Hz dead zone (the SSB change is SSB only)');
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
  ok(calls[b4] === 'src 2 50 1000' && !calls.slice(b4).some((c) => c.startsWith('off')), 'switch to USB → native source 2 (dead ±50 Hz), no spectrum offsets');
  now += 100; t.onFrame(noise(), { ...g, tuneHz: CENTRE + 50, lo: 50, hi: 2700 }, 'usb', now);
  ok(calls[calls.length - 1] === `dial ${CENTRE + 50}`, 'the dial is passed on (native carries its SSB answer across the move)');
  const b5 = calls.length;
  now += 100; t.onFrame(noise(), { ...g, tuneHz: CENTRE + 51, lo: 50, hi: 2700 }, 'usb', now);
  now += 100; t.onFrame(noise(), { ...g, tuneHz: CENTRE + 49, lo: 50, hi: 2700 }, 'usb', now);
  ok(calls.length === b5, '±1 Hz of frequency-stamp jitter is NOT a move (the SSB estimator is left to finish)');
  t.setStereo(1); t.setStereo(1);
  ok(calls.filter((c) => c === 'st 1').length === 1, 'the stereo hint is sent once per change');
}

console.log('── SSB: the coarse cue from the waterfall ──');
/** A speech-like SSB voice on one frame: the band from carrier+lowCut to carrier+2700 (mirrored for LSB), louder
 *  low down (−6 dB/octave above 500 Hz), ragged bin to bin, and silent on ~30 % of frames (syllables, breaths). */
function ssbFrame(dial: number, carrier: number, lowCut: number, lsb: boolean, n = N, bw = BW, centre = CENTRE): Float32Array {
  const bin = bw / n;
  const b = new Float32Array(n);
  for (let k = 0; k < n; k++) b[k] = -100 + 3 * (rnd() - 0.5);
  if (rnd() < 0.3) return b;
  for (let k = 0; k < n; k++) {
    const f = centre + (k - n / 2) * bin;
    const a = lsb ? carrier - f : f - carrier;          // audio frequency of this bin
    if (a < lowCut || a > 2700) continue;
    const tilt = a > 500 ? -6 * Math.log2(a / 500) : 0;
    b[k] = Math.max(b[k], -78 + tilt + 10 * (rnd() - 0.5));
  }
  return b;
}
{
  const dial = CENTRE + 2000;
  const cases: [number, number, boolean][] = [[+500, 300, false], [-800, 250, false], [+1500, 350, false], [+700, 300, true], [-1200, 300, true]];
  for (const [off, cut, lsb] of cases) {
    const c = new SsbCoarse();
    const g = { centerHz: CENTRE, bwHz: BW, tuneHz: dial, lo: lsb ? -2700 : 50, hi: lsb ? -50 : 2700 };
    let r = { ok: false, carrierHz: 0, bound: false };
    for (let i = 0; i < 25; i++) r = c.onFrame(ssbFrame(dial, dial + off, cut, lsb), g, lsb);
    const got = r.carrierHz - dial;
    ok(r.ok && Math.abs(got - off) < 150,
       `${lsb ? 'LSB' : 'USB'} voice ${off > 0 ? '+' : ''}${off} Hz off (its low cut ${cut} Hz) → ${r.ok ? (got > 0 ? '+' : '') + got.toFixed(0) + ' Hz' : 'nothing'}${r.bound ? ' (bound)' : ''}`);
  }
  {
    // The average is in ABSOLUTE cells: a retune keeps it, and the answer is relative to the new dial at once.
    const c = new SsbCoarse();
    const g = { centerHz: CENTRE, bwHz: BW, tuneHz: dial, lo: 50, hi: 2700 };
    for (let i = 0; i < 25; i++) c.onFrame(ssbFrame(dial, dial + 600, 300, false), g, false);
    const r = c.onFrame(ssbFrame(dial + 200, dial + 600, 300, false), { ...g, tuneHz: dial + 200 }, false);
    ok(r.ok && Math.abs(r.carrierHz - (dial + 200) - 400) < 150, `a 200 Hz retune keeps the average: +600 → ${(r.carrierHz - dial - 200).toFixed(0)} Hz against the new dial`);
  }
  {
    // Bins too coarse to place an edge → nothing (left to the estimator).
    const c = new SsbCoarse();
    const n = 256, g = { centerHz: CENTRE, bwHz: BW, tuneHz: dial, lo: 50, hi: 2700 };   // 187.5 Hz bins
    let any = false;
    for (let i = 0; i < 25; i++) any = c.onFrame(ssbFrame(dial, dial + 500, 300, false, n), g, false).ok || any;
    ok(!any, 'coarse bins (187 Hz) → no reading');
    const c2 = new SsbCoarse();
    let any2 = false;
    for (let i = 0; i < 40; i++) any2 = c2.onFrame(noise(), { ...g }, false).ok || any2;
    ok(!any2, 'noise only → no reading');
    const c3 = new SsbCoarse();
    let any3 = false;
    for (let i = 0; i < 25; i++) { const b = noise(); line(b, dial + 900, -60); any3 = c3.onFrame(b, { ...g }, false).ok || any3; }
    ok(!any3, 'a lone carrier (too narrow for a voice) → no reading');
  }
  {
    // The tracker: the ABSOLUTE carrier to native, held through a pause between overs, let go after the hold.
    const hints: string[] = [];
    const t = new BinauralTracker({ setBinauralSsbHint: (hz: number, v: boolean) => hints.push(v ? `hint ${Math.round(hz)}` : 'hint x') });
    const g = { centerHz: CENTRE, bwHz: BW, tuneHz: dial, lo: 50, hi: 2700 };
    let now = 0;
    for (let i = 0; i < 25; i++) { now += 100; t.onFrame(ssbFrame(dial, dial + 500, 300, false), g, 'usb', now); }
    const last = hints.filter((h) => h !== 'hint x').pop() ?? '';
    const hz = Number(last.split(' ')[1]);
    ok(Math.abs(hz - (dial + 500)) < 150 && hints.length <= 6, `the tracker sends the carrier as an absolute frequency (${last}), a trickle (${hints.length} messages)`);
    const n0 = hints.length;
    for (let i = 0; i < 30; i++) { now += 100; t.onFrame(noise(), g, 'usb', now); }
    ok(hints.slice(n0).every((h) => h !== 'hint x'), 'a 3 s pause between overs: the cue is HELD');
    for (let i = 0; i < 40; i++) { now += 100; t.onFrame(noise(), g, 'usb', now); }
    ok(hints[hints.length - 1] === 'hint x', 'long silence: the cue is let go');
  }
}

console.log('── THE SPREAD MAP (one behaviour: no style) ──');
{
  ok(BIN_SPREAD_EDGES[0] === MODE_BANDWIDTHS.usb[0] && BIN_SPREAD_EDGES[1] === MODE_BANDWIDTHS.usb[1] && BIN_SPREAD_EDGES[1] === 2700,
     'the fixed scale IS the protocol\'s default USB passband (50-2700 Hz)');
  ok(spreadKindFor('usb', 50, 2700) === BIN_SPREAD_USB && spreadKindFor('lsb', -2700, -50) === BIN_SPREAD_LSB, 'USB/LSB → their maps');
  ok(spreadKindFor('cwu', -200, 200) === BIN_SPREAD_CW_UPPER && spreadKindFor('cwl', -200, 200) === BIN_SPREAD_CW_LOWER
     && spreadKindFor('cwl', 300, 700) === BIN_SPREAD_CW_UPPER && spreadKindFor('cw', -700, -300) === BIN_SPREAD_CW_LOWER,
     'CW → upper or lower by where the passband sits, by name when it straddles the dial');
  ok(['am', 'sam', 'nfm', 'fm', 'wfm', 'dab'].every((m) => spreadKindFor(m, -5000, 5000) === BIN_SPREAD_OFF),
     'no map for AM, SAM, NFM, FM, WFM or DAB — the lean alone there');
  // ★ Record EVERY call the tracker makes, by name: a style message (or any unknown one) would show up here.
  const calls: string[] = [];
  const names = new Set<string>();
  const sink = new Proxy({}, {
    get: (_t, name: string) => (...a: unknown[]) => {
      names.add(name);
      if (name === 'setBinauralSpread') calls.push(`spread ${a[0]} ${a[1]} ${a[2]}`);
    },
  });
  const t = new BinauralTracker(sink);
  ok(!('setStyle' in t), 'the tracker has no style to set any more');
  const g = { centerHz: CENTRE, bwHz: BW, tuneHz: CENTRE, lo: 50, hi: 2700 };
  let now = 0;
  t.onFrame(noise(), g, 'usb', (now += 100)); t.onFrame(noise(), g, 'usb', (now += 100));
  ok(calls.length === 1 && calls[0] === `spread ${BIN_SPREAD_USB} 50 2700`, 'USB: the USB map is sent BY ITSELF, once');
  t.onFrame(noise(), { ...g, lo: -2700, hi: -50 }, 'lsb', (now += 100));
  ok(calls[1] === `spread ${BIN_SPREAD_LSB} 50 2700`, 'to LSB → the mirrored map (the same scale)');
  t.onFrame(noise(), { ...g, lo: 300, hi: 700 }, 'cw', (now += 100));
  ok(calls[2] === `spread ${BIN_SPREAD_CW_UPPER} 50 2700`, 'to CW → the CW map');
  t.onFrame(noise(), { ...g, lo: -5000, hi: 5000 }, 'am', (now += 100));
  ok(calls[3] === `spread ${BIN_SPREAD_OFF} 50 2700`, 'to AM → no map (there is no spectrum in AM audio to spread)');
  t.onFrame(noise(), { ...g, lo: -6000, hi: 6000 }, 'nfm', (now += 100)); t.onFrame(noise(), { ...g, lo: -6000, hi: 6000 }, 'nfm', (now += 100));
  ok(calls.length === 4, 'to NFM → still no map, and nothing re-sent');
  t.reset();
  t.onFrame(noise(), g, 'usb', (now += 100));
  ok(calls[4] === `spread ${BIN_SPREAD_USB} 50 2700`, 'after a reset (switched back on) the map is stated afresh');
  ok([...names].every((n) => n.startsWith('setBinaural')) && ![...names].some((n) => /style/i.test(n)),
     `no style message — only ${[...names].sort().join(', ')}`);
}
{
  // ★ The SSB coarse cue is worked out WITH the map set (it used to stop under the SPREAD style): it drives the
  //   off-tune weight, and so the spread as well as the lean.
  const hints: string[] = [], maps: number[] = [];
  const t = new BinauralTracker({
    setBinauralSsbHint: (hz: number, v: boolean) => hints.push(v ? `hint ${Math.round(hz)}` : 'hint x'),
    setBinauralSpread: (k: number) => maps.push(k),
  });
  const dial = CENTRE + 2000;
  const g = { centerHz: CENTRE, bwHz: BW, tuneHz: dial, lo: 50, hi: 2700 };
  let now = 0;
  for (let i = 0; i < 25; i++) t.onFrame(ssbFrame(dial, dial + 500, 300, false), g, 'usb', (now += 100));
  const last = hints.filter((h) => h !== 'hint x').pop();
  ok(maps[0] === BIN_SPREAD_USB && last !== undefined && Math.abs(Number(last.split(' ')[1]) - (dial + 500)) < 150,
     `USB with its map set: the coarse cue is still sent (${last ?? 'none'})`);
}

console.log(fails ? `\nFAILED: ${fails}` : '\nall passed');
process.exit(fails ? 1 : 0);
