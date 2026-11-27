// test_sat_tune.ts — ★★★ beta97 TUNING INSIDE A SATELLITE PASS, the clients' rules (src/services/satTune.ts, web + app):
// the satellite view's 500 Hz step (the listener's own step back on the way out, a step chosen meanwhile left alone), and
// where a waterfall tap puts the dial while the Doppler lock is live (the shift taken off, the snap kept inside the
// passband). The model end to end: tune +12.3 kHz into RS-44's passband mid-pass, the Doppler moves −3 kHz, the channel
// (dial + shift) stays on the signal. The server's half (a mode change keeps the lock) is scripts/test-server-sat.mjs.
// Silent. Run: npx tsx scripts/test_sat_tune.ts
import { SAT_STEP_HZ, SatStepKeeper, satAnchorCentreHz, satAnchorStale, satAnchorX, satClickDialHz, satPassbandOf, parseSpotVia, spotViaText, spotBandCell } from '../src/services/satTune';
import { satMarkerShiftHz, type SatReadout } from '../src/services/satellites';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

console.log('── the satellite view\'s step ──');
{
  const k = new SatStepKeeper();
  ok(SAT_STEP_HZ === 500, 'the satellite step is 500 Hz');
  ok(k.enter(100000) === 500, 'in from WFM at 100 kHz → 500 Hz');
  ok(k.held === 100000, '…the listener\'s 100 kHz held');
  ok(k.enter(500) === null, 'a second enter (another pick) changes nothing');
  ok(k.leave(500) === 100000, 'out → 100 kHz back');
  ok(k.held === null && k.leave(100000) === null, 'out again: nothing to do');
}
{
  const k = new SatStepKeeper();
  k.enter(100000);
  // the listener picks 1 kHz in the step menu while in the view: the keeper is not asked again, so it stands
  ok(k.enter(1000) === null, 'a step the listener chose in the view stands (no re-imposed 500 Hz)');
  ok(k.leave(1000) === 100000, '…and leaving still puts their own 100 kHz back');
}
{
  const k = new SatStepKeeper();
  ok(k.enter(500) === null && k.held === 500, 'already on 500 Hz: nothing to set, 500 held');
  ok(k.leave(500) === null, '…and nothing to put back');
}

console.log('── a tap on the waterfall with the lock live ──');
const rs44 = { hz: 435610000, hiHz: 435670000 };
const pb = satPassbandOf(rs44);
ok(!!pb && pb.lo === 435610000 && pb.hi === 435670000, 'RS-44\'s transponder: a passband');
ok(satPassbandOf({ hz: 437800000 }) === null && satPassbandOf({ hz: 435605000, hiHz: 0 }) === null, 'an FM bird / a beacon: no passband');
{
  // mid-pass: the lock's shift −5 kHz (no follow) — the signal seen on the waterfall at 435.617300 RF
  const ro: SatReadout = { state: 'TRACK', offset: -5000 } as SatReadout;
  const shift = satMarkerShiftHz(ro);
  ok(shift === -5000, 'the marker shift is the lock\'s offset');
  const sigRf = 435617300;                     // = satellite frame 435.622300, 12.3 kHz into the passband
  const dial = satClickDialHz(sigRf, shift, 500, true, pb);
  ok(Math.abs(dial + shift - sigRf) <= 250, `the channel lands within 250 Hz of the tap (dial ${dial}, channel ${dial + shift})`);
  ok(dial - pb!.lo >= 12000 && dial - pb!.lo <= 12500, `…12.3 kHz into the passband in the satellite frame (+${dial - pb!.lo} Hz)`);
  ok(dial >= pb!.lo && dial <= pb!.hi, '…inside the passband: the server\'s nudge keeps the lock');
  // the Doppler moves −3 kHz: the dial (nominal) stays, the server's channel = dial + the new shift — the signal's RF moved with it
  const shift2 = shift - 3000, sigRf2 = sigRf - 3000;
  ok(Math.abs(dial + shift2 - sigRf2) <= 250, 'the Doppler moves −3 kHz: channel = centre + offset + Doppler, still on the signal');
  // before the fix: the RF sent as the dial → the channel a whole shift away
  ok(Math.abs(sigRf + shift - sigRf) === 5000, '(the old arithmetic put the channel 5 kHz off)');
  // the old step: 100 kHz snaps out of the passband — the cause Stuart found
  const old = satClickDialHz(sigRf, shift, 100000, true, null);
  ok(old < pb!.lo || old > pb!.hi, `(a 100 kHz step snapped the tap to ${old}, outside the passband)`);
}
{
  // the snap is clamped to the passband: a tap just inside an edge whose nearest 500 Hz grid point is outside
  const pbOdd = { lo: 435610200, hi: 435669800 };
  const d = satClickDialHz(435669780, 0, 500, true, pbOdd);   // nearest grid point 435.670000 — outside
  ok(d === pbOdd.hi, `a tap inside the top edge that would round out stays in (${d} = ${pbOdd.hi})`);
  const d2 = satClickDialHz(435610220, 0, 500, true, pbOdd);  // nearest grid point 435.610000 — outside
  ok(d2 === pbOdd.lo, `…and the bottom edge (${d2} = ${pbOdd.lo})`);
  // a 100 kHz step can never throw a tap inside out of it either
  const d3 = satClickDialHz(435640000, 0, 100000, true, pbOdd);
  ok(d3 >= pbOdd.lo && d3 <= pbOdd.hi, `…even on a 100 kHz step (${d3})`);
  // a tap OUTSIDE is the listener's own choice: not pulled in (the server ends the lock)
  ok(satClickDialHz(435700000, 0, 500, true, pbOdd) === 435700000, 'a tap outside the passband goes where it was asked');
}
{
  ok(satClickDialHz(96123456, 0, 100000, true, null) === 96100000, 'no lock (shift 0): the ordinary snapped tap');
  ok(satClickDialHz(96123456, 0, 100000, false, null) === 96123456, 'snap switched off: exactly where it lands');
  ok(satClickDialHz(435617300, NaN, 500, true, null) === 435617500, 'a junk shift is no shift');
  // the finder follows a station (+400 Hz): the dial already carries it, so the marker shift is offset − follow
  const ro: SatReadout = { state: 'TRACK', offset: -4600, follow: 400 } as SatReadout;
  ok(satMarkerShiftHz(ro) === -5000, 'with a follow the shift taken off is offset − follow');
  ok(satMarkerShiftHz({ state: 'OFF', offset: -5000 } as SatReadout) === 0, 'no live lock: no shift');
}

console.log('── the dial beside the satellite box ──');
{
  ok(satAnchorX(1440, 560) === 1000, '1440 px with the box to x 560: the line belongs at 1000 (the uncovered middle)');
  ok(satAnchorX(390, 380) === null, 'a phone: no anchor');
  ok(satAnchorX(1440, 0) === null && satAnchorX(1440, 1200) === null, 'no box, or a box over most of the view: no anchor');
  // 100 kHz across 1440 px; line at 145.8615 MHz to sit at x 1000 → centre = line − (1000 − 720) · 100000/1440
  const c = satAnchorCentreHz(145861500, 100000, 1440, 1000);
  ok(c === Math.round(145861500 - 280 * 100000 / 1440), `the view centre that puts it there (${c})`);
  ok(!satAnchorStale(c, c + 400, 100000, 1440), 'a 400 Hz drift (≈6 px) leaves the view alone');
  ok(satAnchorStale(c, c + 700, 100000, 1440), 'a 700 Hz drift (≈10 px) moves it');
}

console.log('── spots via a satellite ──');
{
  ok(parseSpotVia('JO-97') === 'JO-97' && parseSpotVia(undefined) === '' && parseSpotVia(42) === '', 'via parsed: a name, else nothing');
  ok(parseSpotVia('JO-97"<script>') === 'JO-97script', 'off the wire: no quotes or angle brackets');
  ok(parseSpotVia('X'.repeat(80)).length === 24, '…and bounded');
  ok(spotViaText('JO-97') === 'via JO-97' && spotViaText('') === '' && spotViaText(undefined) === '', '"via JO-97", else nothing');
  ok(spotBandCell('2m', 'JO-97') === 'JO-97' && spotBandCell('2m', undefined) === '2m', 'the band column shows the bird, not "2m"');
}

console.log(`\n  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
