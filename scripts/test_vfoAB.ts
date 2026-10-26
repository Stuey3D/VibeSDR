/**
 * V12 VFO A/B — the app's half (src/services/vfoAB.ts): the wire, the A/B slots, and the L/R tags.
 *
 * Proves:
 *   • parseWatch: the server's state sanitised (NaN, junk channels, control characters, long notes), refusals kept;
 *   • watchOffered: hwinfo `watch` → the toggle exists only when the server says so (absent / 0 = none);
 *   • the messages: on (with stereo + monoOut), off, aim (integers, a passband only when it is one), squelch (the app's
 *     -999 = off rule), stereo, monoOut;
 *   • THE SLOTS: on → B starts at A's tuning, A stays selected; select B → the status IS B's (so every control drives
 *     B), A waits in the stash and the server's config for A lands there (aSlot); B's tune / mode / passband / squelch
 *     go out as `watch`, A's squelch never does; select A → A back exactly; each VFO keeps its OWN squelch;
 *   • the server's word: a refusal reverts B to where the server kept it and its note is shown; B gone without us
 *     (yielded to a listener) → back to one VFO, A selected, with the server's note; our own drag is not dragged back
 *     by an older echo; the meter follows B while B is tuned;
 *   • off with B tuned → A restored; the offer withdrawn (hwinfo watch 0) → reset; reset() for a fresh session;
 *   • THE EARS: lower L / higher R whichever is A or B; 100 Hz of hysteresis (no flutter); within 5 Hz both; stereo off
 *     or a mono output → no tags — the server's PanMixer rule exactly.
 *
 * Run: node --no-warnings scripts/test_vfoAB.ts   (silent: no audio anywhere)
 */
import {
  parseWatch, watchOffered, vfoB, msgOn, msgOff, msgAim, msgSquelch, msgStereo, msgMonoOut, EarOrder, VfoABCore,
  SWAP_HZ, SAME_HZ, otherVfo, type TuneStatus,
} from '../src/services/vfoAB.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

console.log('── the wire ──');
{
  ok(parseWatch({ type: 'sig' }) === null && parseWatch(null) === null, 'not a watch message → null');
  const st = parseWatch({ type: 'watch', max: 1, lo: 5.8e6, hi: 8.2e6, stereo: true, monoOut: false,
    chans: [{ id: 1, freq: 7.03e6, mode: 'usb', bw: 2700, sqlOn: true, sql: -80, open: false, since: 1761500000000, lvl: -92.5, parked: false },
            { id: 0, freq: 7e6 }, { id: 2, freq: NaN }, 'junk', null] });
  ok(!!st && st.chans.length === 1 && st.chans[0].mode === 'usb' && st.chans[0].open === false && st.chans[0].since === 1761500000000,
     'a channel parsed, junk channels dropped (id < 1, NaN freq, not an object)');
  ok(!!st && st.max === 1 && st.lo === 5.8e6 && st.stereo === true, 'the window, the offer and stereo');
  const r = parseWatch({ type: 'watch', max: 1, chans: [], refused: 'outside', note: 'VFO B must stay inside\u0007 the window' + 'x'.repeat(400) });
  ok(!!r && r.refused === 'outside' && !!r.note && !r.note.includes('\u0007') && r.note.length <= 200, 'a refusal: kept, its note cleaned and capped');
  ok(parseWatch({ type: 'watch', chans: [{ id: 1, freq: 7e6, lvl: 'x', bw: -5 }] })!.chans[0].lvl === -200, 'a non-number level → the floor, never NaN');
  ok(watchOffered({ type: 'hwinfo', watch: 1 }) === 1 && watchOffered({ type: 'hwinfo' }) === 0 && watchOffered({ type: 'hwinfo', watch: 0 }) === 0,
     'hwinfo watch: 1 → offered; absent or 0 → no toggle');
  const on = msgOn(false, true);
  ok(on.type === 'watch' && on.id === 1 && on.on === true && on.stereo === false && on.monoOut === true, 'on carries stereo and monoOut');
  ok(msgOff().off === true && msgOff().id === 1, 'off');
  const aim = msgAim({ frequency: 7030000.4, low: -1500, high: 1500 });
  ok(aim.frequency === 7030000 && aim.bandwidthLow === -1500 && aim.bandwidthHigh === 1500 && !('mode' in aim), 'aim: whole Hz, the passband edges');
  ok(!('bandwidthLow' in msgAim({ low: 5, high: 5 })), 'aim: an empty passband is not sent');
  ok(msgSquelch(-85.37).sql === -85.4 && msgSquelch(-85).sqlOn === true && msgSquelch(-999).sqlOn === false, 'squelch: -999 = off');
  ok(msgStereo(false).stereo === false && msgMonoOut(true).monoOut === true && otherVfo('A') === 'B', 'stereo, monoOut, other');
}

console.log('── the slots ──');
{
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  const sent: Record<string, unknown>[] = [];
  let t = 1000;
  const adopt = (mode: string, s: TuneStatus) => { if (s.mode !== mode) { s.mode = mode; s.bandwidthLow = mode === 'am' ? -5000 : 100; s.bandwidthHigh = mode === 'am' ? 5000 : 2800; } };
  const ab = new VfoABCore(() => status, (m) => sent.push(m), adopt, () => t);
  ok(!ab.setOn(true) && !ab.on && sent.length === 0, 'not offered: the toggle does nothing and sends nothing');
  ok(ab.setOffered(1) && !ab.setOffered(1), 'hwinfo watch 1: offered (a repeat is not a change)');
  ab.squelch(-90);
  ok(ab.setOn(true) && ab.on && ab.active === 'A' && sent.at(-1)!.on === true, 'on: B is created, A stays selected');
  ok(ab.info().other!.frequency === 7074000 && ab.info().other!.squelchDb === -999, 'B starts at A\'s tuning, its squelch off');
  t += 100;
  ab.onWatch({ type: 'watch', max: 1, stereo: true, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95 }] });
  ok(ab.on && ab.state!.chans.length === 1, 'the server confirms B');
  ok(ab.select('B') && ab.active === 'B' && status.frequency === 7074000, 'select B: status is B\'s now');
  sent.length = 0;
  ab.tuneB(7080000);
  ok(status.frequency === 7080000 && sent.length === 1 && sent[0].type === 'watch' && sent[0].frequency === 7080000, 'tuning B: `watch`, never `tune`');
  ab.modeB('am');
  ok(status.mode === 'am' && status.bandwidthLow === -5000 && sent.at(-1)!.mode === 'am' && sent.at(-1)!.bandwidthHigh === 5000, 'B\'s mode carries its passband');
  ab.passbandB(-3000, 3000);
  ok(sent.at(-1)!.bandwidthLow === -3000 && status.bandwidthHigh === 3000, 'B\'s passband');
  ok(ab.squelch(-70) && sent.at(-1)!.sql === -70, 'B\'s squelch goes as `watch`');
  // the server's config for VFO A lands in A's slot, not on the screen
  const a = ab.aSlot();
  a.frequency = 7076000;
  ok(status.frequency === 7080000 && ab.info().other!.frequency === 7076000, 'while B is tuned, A\'s server dial lands in A\'s slot (aSlot)');
  // an older echo of B does not drag our drag back…
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7079000, mode: 'am', open: true, lvl: -60 }] });
  ok(status.frequency === 7080000, 'an echo within 1.5 s of our own move is not adopted');
  // …but a refusal is, with its note
  const o = ab.onWatch({ type: 'watch', max: 1, refused: 'outside', note: 'VFO B must stay inside the window',
                         chans: [{ id: 1, freq: 7079000, mode: 'am', open: true, lvl: -61 }] });
  ok(status.frequency === 7079000 && o.statusChanged && o.notes[0] === 'VFO B must stay inside the window', 'a refusal: B back where the server kept it, the note shown');
  ok(o.meterDb === -61, 'the meter reads B while B is tuned');
  ok(ab.select('A') && status.frequency === 7076000 && status.mode === 'usb' && ab.info().activeSquelchDb === -90, 'select A: A back exactly, with ITS squelch');
  ok(ab.info().other!.squelchDb === -70 && ab.info().other!.frequency === 7079000, 'B kept its own squelch and tuning');
  ok(!ab.squelch(-88) && ab.info().activeSquelchDb === -88, 'A\'s squelch is A\'s (not sent as `watch`)');
  // B yields to a listener
  ab.select('B');
  t += 5000;
  const y = ab.onWatch({ type: 'watch', max: 1, chans: [], note: 'VFO B closed to make room for a listener' });
  ok(y.reset && !ab.on && ab.active === 'A' && status.frequency === 7076000 && y.notes[0].includes('room for a listener'),
     'B gone without us: one VFO again, A selected and restored, the server\'s note shown');
  // off with B tuned
  ab.setOn(true); t += 100;
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7076000, mode: 'usb', open: true }] });
  ab.select('B'); ab.tuneB(7100000);
  sent.length = 0;
  ok(ab.setOn(false) && !ab.on && status.frequency === 7076000 && sent[0].off === true, 'off with B tuned: A restored, `off` sent');
  // a just-sent `on` is not undone by the empty state that might precede the server's answer
  ab.setOn(true);
  const g = ab.onWatch({ type: 'watch', max: 1, chans: [] });
  ok(ab.on && !g.reset, 'an empty state inside the grace after our own `on` does not reset');
  t += 2000;
  ab.setOffered(0);
  ok(!ab.on && ab.active === 'A', 'the offer withdrawn (owner switch / a mode with no room): reset');
  ab.setOffered(1); ab.setOn(true); ab.select('B');
  ok(ab.reset() && !ab.on && status.frequency === 7076000, 'reset() for a fresh session: A restored');
  ab.setStereo(false);
  sent.length = 0; ab.setStereo(true);
  ok(sent.length === 0, 'stereo is remembered, sent only while B is on');
}

console.log('── the ears ──');
{
  const e = new EarOrder();
  let r = e.ears(7.000e6, 7.010e6, true);
  ok(r.A === 'L' && r.B === 'R', 'A lower: A left, B right');
  e.reset(); r = e.ears(7.020e6, 7.010e6, true);
  ok(r.A === 'R' && r.B === 'L', 'B lower: B left — the ears follow frequency, not the letter');
  e.reset(); e.ears(7.000e6, 7.010e6, true);
  r = e.ears(7.010e6 + SWAP_HZ / 2, 7.010e6, true);
  ok(r.A === 'L', `A ${SWAP_HZ / 2} Hz past B: inside the hysteresis, no swap`);
  r = e.ears(7.010e6 + SWAP_HZ + 20, 7.010e6, true);
  ok(r.A === 'R' && r.B === 'L', `A ${SWAP_HZ + 20} Hz past B: swapped`);
  let flips = 0, prev = r.A;
  for (let k = 0; k < 20; k++) { const x = e.ears(7.010e6 + ((k & 1) ? 60 : -60), 7.010e6, true); if (x.A !== prev) flips++; prev = x.A; }
  ok(flips === 0, 'a needle wobbling ±60 Hz over the other never flutters the tags');
  r = e.ears(7.010e6 + SAME_HZ - 1, 7.010e6, true);
  ok(r.A === 'LR' && r.B === 'LR', `within ${SAME_HZ} Hz: both in both ears`);
  ok(e.ears(7e6, 7.01e6, false).A === null && e.ears(7e6, 7.01e6, true, true).B === null, 'stereo off, or a mono output: no tags');
  ok(e.ears(0, 7e6, true).A === null, 'no frequency: no tag');
}
ok(vfoB(parseWatch({ type: 'watch', chans: [{ id: 1, freq: 1e6 }] }))?.freq === 1e6, 'vfoB() finds channel 1');

// ★★ The scanner holds VFO A: engaging it selects A, and the A/B swap is ignored while it runs (source checks).
{
  const scr = require('node:fs').readFileSync(require('node:path').join(__dirname, '../src/screens/SDRScreen.tsx'), 'utf8');
  ok(/if \(scanEngagedRef\.current\) return;/.test(scr), 'scanner: the A/B swap is ignored while scanning');
  ok(/scanner\.engaged\)[\s\S]{0,240}info\.active === 'B'\) c\?\.abSelect\?\.\('A'\)/.test(scr), 'scanner: starting with B selected goes back to A');
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
