import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
/**
 * V12 VFO A/B — the app's half (src/services/vfoAB.ts): the wire, the A/B slots, and the L/R tags.
 *
 * Proves:
 *   • parseWatch: the server's state sanitised (NaN, junk channels, control characters, long notes), refusals kept;
 *   • watchOffered: hwinfo `watch` → the toggle exists only when the server says so (absent / 0 = none); watchMaxOf:
 *     the watch state's `max` re-states it (leaving DRM gives VFO B back) — junk leaves the offer alone;
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
 *     or a mono output → no tags — the server's PanMixer rule exactly;
 *   • ★★★ THE AUDIO SOCKET IS VFO A'S DOOR (build 498: "they would often snap to the frequency the other was on"): driven
 *     through audioTuneForward's own decision, switching A↔B and tuning B never sends a `tune` — the door is handed
 *     vfoATune(), not the selected VFO's status (which DID send: proven below, so the test cannot pass vacuously); a
 *     userTuneSeq bump made while B is tuned does not reach A's door (DoorSeq);
 *   • ★★★ A FRESH SOCKET IS NOT A FRESH SESSION (build 498, landscape → portrait: one VFO on screen, two in the ears): a
 *     socket reopen keeps VFO B while the server's state carries it; silence for RESYNC_MS → one VFO; a client that does
 *     not know about a B the server holds ADOPTS it (A selected, nothing sent, the mode's passband shape at the
 *     server's width); not the echo of our own OFF, not a refusal; the client's onopen no longer resets;
 *   • the A/B ON/OFF lives on the frequency card (FreqModal + its nav stop), not the tuning-step sheet;
 *   • VFO B's colour: AUTO (the contrast rule B has always drawn in) or the listener's swatch;
 *   • ROUND 3: the mutes on the wire and in the state (muteA, a channel's mute); squelchFor — each VFO's own squelch BY
 *     NAME whichever is tuned (A's never as `watch`, B's never without a B); setMute only while B is on; info carries
 *     both squelches and both mutes; SqlAutoTrack (B's auto squelch) — seeds from the neighbours, rises only while shut,
 *     opens above the margin, holds OFF through the hang, re-judges on a new channel, sends only on a change; and the
 *     screen's wiring (source checks): Band mutes the VFO it runs on with B on;
 *   • ★★★ DECODE ON EITHER VFO (2026-10-27): the watch state's decB; decStartVfo — the selected VFO, or on a server
 *     that cannot decode B (no decB) A with the controls switched to it first; decTagFor — DEC only with two VFOs, on the
 *     VFO a decoder or the spots run on; tuneOtherB — B moved in its slot while A is selected, as `watch`, the controls
 *     left on A, nothing without a B or with B selected; and the screen's wiring: a decoder / spots / Band start on the
 *     selected VFO, the box tunes the decoding VFO (abTuneVfo), Band's arrows are its VFO's, both needles and both
 *     squelch rows carry DEC.
 *
 * Run: node --no-warnings scripts/test_vfoAB.ts   (silent: no audio anywhere)
 */
import {
  parseWatch, watchOffered, watchMaxOf, vfoB, msgOn, msgOff, msgAim, msgSquelch, msgStereo, msgMonoOut, EarOrder, VfoABCore,
  decStartVfo, decTagFor,
  SWAP_HZ, SAME_HZ, otherVfo, vfoATune, DoorSeq, RESYNC_MS, edgesForWidth, autoVfoBColour, vfoBNeedle, VFO_B_AUTO,
  type TuneStatus,
} from '../src/services/vfoAB.ts';
import { shouldForwardTune } from '../src/services/audioTuneForward.ts';
import { msgMute } from '../src/services/vfoAB.ts';
import { SqlAutoTrack, SQL_AUTO_HANG_MS } from '../src/utils/sqlAutoTrack.ts';

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
  // ★★ The watch STATE re-states the offer (watchTellMax: into / out of DRM) — the web reads it, and now the app does.
  ok(watchMaxOf({ type: 'watch', max: 1, chans: [] }) === 1 && watchMaxOf({ type: 'watch', max: 0 }) === 0,
     'watch state max: 1 → offered, 0 → withdrawn');
  ok(watchMaxOf({ type: 'watch' }) === null && watchMaxOf({ type: 'watch', max: 'x' }) === null
     && watchMaxOf({ type: 'watch', max: NaN }) === null && watchMaxOf({ type: 'watch', max: -1 }) === null
     && watchMaxOf({ type: 'hwinfo', max: 1 }) === null && watchMaxOf(null) === null,
     'watch state with no usable max (absent, junk, negative, not a watch) → null: the offer is left as it was');
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

console.log('── the audio socket is VFO A\'s door (the snap, build 498) ──');
{
  // The server's dial (config) is VFO A: 7.074 USB. B starts there too.
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  const serverDial = { frequency: 7074000, mode: 'usb' };
  let t = 10_000;
  const sent: Record<string, unknown>[] = [];
  const adopt = (mode: string, s: TuneStatus) => { if (s.mode !== mode) s.mode = mode; };
  const ab = new VfoABCore(() => status, (m) => sent.push(m), adopt, () => t);
  ab.setOffered(1); ab.setOn(true);
  t += 200;
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true }] });
  // LocalAudioPlayer's forwarding effect, as a function of the props it is handed (unsealed: a person has tuned before).
  const door = (pick: () => TuneStatus) => {
    let last = JSON.stringify(pick());
    const tunes: number[] = [];
    return {
      tunes,
      render() {
        const asked = pick(); const j = JSON.stringify(asked);
        if (j === last) return;   // the effect's deps did not change: it does not run
        last = j;
        const d = shouldForwardTune({ asked, serverDial, seqAdvanced: false, sealed: false, helloTune: '', askedJson: j,
                                      msSinceSend: Infinity });
        if (d.send) tunes.push(asked.frequency);
      },
    };
  };
  const fixed = door(() => vfoATune(status, ab.info()));
  const naive = door(() => ({ ...status }));   // what the screen handed it before: the SELECTED VFO's status
  const step = (f: () => void) => { f(); t += 2000; fixed.render(); naive.render(); };
  step(() => ab.select('B'));
  step(() => ab.tuneB(7080000));
  step(() => ab.tuneB(7085000));
  step(() => ab.select('A'));
  step(() => ab.select('B'));
  step(() => ab.select('A'));
  ok(naive.tunes.includes(7080000),
     `the OLD door (status) sent VFO B's frequency as VFO A's tune: ${naive.tunes.join(', ')} — the snap`);
  ok(fixed.tunes.length === 0, `the door (vfoATune) sends nothing across switches and tunes of B (sent: ${fixed.tunes.join(', ') || 'none'})`);
  ok(status.frequency === 7074000 && ab.info().other!.frequency === 7085000,
     'each VFO stayed put: A on 7.074, B where it was tuned (7.085)');
  ok(vfoATune(status, null).frequency === status.frequency && vfoATune(status, { on: true, active: 'A', other: null }).frequency === 7074000,
     'vfoATune: one VFO, or A selected → the status');
  // A tune of A still goes out (A selected, a real change).
  status.frequency = 7075000; fixed.render();
  ok(fixed.tunes.length === 1 && fixed.tunes[0] === 7075000, 'a tune of VFO A still reaches the audio socket');
  // userTuneSeq as A's door sees it
  const ds = new DoorSeq();
  const s0 = ds.next(5, false);
  ok(ds.next(6, true) === s0 && ds.next(8, true) === s0, 'DoorSeq: bumps made while B is tuned do not advance A\'s counter');
  ok(ds.next(8, false) === s0 && ds.next(9, false) === s0 + 1, 'DoorSeq: switching back to A is not a bump; a tune of A is');
  // the screen hands the door vfoATune / DoorSeq (source check)
  const scr = readFileSync(fileURLToPath(new URL('../src/screens/SDRScreen.tsx', import.meta.url)), 'utf8');
  const lap = scr.slice(scr.indexOf('<LocalAudioPlayer'), scr.indexOf('<LocalAudioPlayer') + 6000);
  ok(/frequency=\{convToHardware\(abDoor\.frequency, converterRef\.current\)\}/.test(lap) && /mode=\{abDoor\.mode/.test(lap)
     && /bandwidthLow=\{abDoor\.bandwidthLow\}/.test(lap) && /userTuneSeq=\{doorSeq\.next\(/.test(lap)
     && !/frequency=\{convToHardware\(status\.frequency/.test(lap),
     'SDRScreen: LocalAudioPlayer is handed VFO A\'s tuning (abDoor) and DoorSeq, never the selected VFO\'s status');
}

console.log('── a fresh socket is not a fresh session (the rotation, build 498) ──');
{
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  let t = 50_000;
  const sent: Record<string, unknown>[] = [];
  const adopt = (mode: string, s: TuneStatus) => {
    if (s.mode === mode) return;
    s.mode = mode;
    const d: Record<string, [number, number]> = { usb: [100, 2800], lsb: [-2800, -100], am: [-5000, 5000], nfm: [-6000, 6000] };
    if (d[mode]) { s.bandwidthLow = d[mode][0]; s.bandwidthHigh = d[mode][1]; }
  };
  const beatB = (freq: number, extra: Record<string, unknown> = {}) =>
    ({ type: 'watch', max: 1, chans: [{ id: 1, freq, mode: 'usb', bw: 2700, open: true, lvl: -90 }], ...extra });
  const ab = new VfoABCore(() => status, (m) => sent.push(m), adopt, () => t);
  ab.setOffered(1); ab.setOn(true); t += 200;
  ab.onWatch(beatB(7074000));
  ab.select('B'); ab.tuneB(7090000); t += 2000;
  ab.onWatch(beatB(7090000));
  // The watchdog reopens the socket (a rotation stalled the JS thread) — same session, the server still mixes B.
  sent.length = 0;
  ab.socketOpened();
  t += 400;
  const o = ab.onWatch(beatB(7090000));
  ok(ab.on && ab.active === 'B' && status.frequency === 7090000 && !o.reset && sent.length === 0,
     'socket reopened, the server\'s beat carries B: still two VFOs, B still selected, nothing sent');
  t += RESYNC_MS * 2;
  ok(!ab.expireIfSilent().expired && ab.on, 'heard on the new socket: no timer can take B away afterwards');
  // A NEW session (the server forgot B): silence on the new socket → one VFO, A restored.
  ab.socketOpened();
  t += RESYNC_MS - 100;
  ok(!ab.expireIfSilent().expired && ab.on, `silence for under ${RESYNC_MS} ms: still waiting`);
  t += 200;
  const ex = ab.expireIfSilent();
  ok(ex.expired && ex.statusChanged && !ab.on && ab.active === 'A' && status.frequency === 7074000,
     `silence for ${RESYNC_MS} ms after a reopen: the session has no B — one VFO, A restored`);
  // A client that does not know about the B the server holds (rebuilt under the same session) ADOPTS it.
  const st2: TuneStatus = { frequency: 145500000, mode: 'nfm', bandwidthLow: -6000, bandwidthHigh: 6000 };
  const sent2: Record<string, unknown>[] = [];
  let t2 = 1_000_000;
  const fresh = new VfoABCore(() => st2, (m) => sent2.push(m), adopt, () => t2);
  fresh.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 145600000, mode: 'nfm', bw: 9000, sqlOn: true, sql: -80, open: false }] });
  ok(!fresh.on, 'not offered (yet): a B is not adopted');
  fresh.setOffered(1);
  const ad = fresh.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 145600000, mode: 'nfm', bw: 9000, sqlOn: true, sql: -80, open: false }] });
  const oth = fresh.info().other!;
  ok(ad.adopted === true && fresh.on && fresh.active === 'A' && st2.frequency === 145500000 && sent2.length === 0,
     'the server holds a B this client did not know: ADOPTED — A stays selected and tuned, nothing sent');
  ok(oth.frequency === 145600000 && oth.mode === 'nfm' && oth.bandwidthLow === -4500 && oth.bandwidthHigh === 4500 && oth.squelchDb === -80,
     'the adopted B: its frequency, mode, the server\'s width in the mode\'s shape, its own squelch');
  // …but not the echo of our own OFF, and not a refusal.
  fresh.setOn(false);
  t2 += 300;
  fresh.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 145600000, mode: 'nfm', bw: 9000, open: true }] });
  ok(!fresh.on, 'a state with B already on its way when we turned B off does not bring it back');
  t2 += 5000;
  fresh.onWatch({ type: 'watch', max: 1, refused: 'outside', note: 'x', chans: [{ id: 1, freq: 145600000, mode: 'nfm', bw: 9000, open: true }] });
  ok(!fresh.on, 'a refusal is not an adoption');
  ok(JSON.stringify(edgesForWidth(100, 2800, 2400)) === '[100,2500]' && JSON.stringify(edgesForWidth(-2800, -100, 2400)) === '[-2500,-100]'
     && JSON.stringify(edgesForWidth(-5000, 5000, 6000)) === '[-3000,3000]' && JSON.stringify(edgesForWidth(100, 2800, 0)) === '[100,2800]',
     'edgesForWidth: a sideband keeps its side, a straddling passband stays centred, no width = the defaults');
  // The client no longer resets on a socket open (source check).
  const ws = readFileSync(fileURLToPath(new URL('../src/services/VibeServerWsClient.ts', import.meta.url)), 'utf8');
  const open = ws.slice(ws.indexOf('ws.onopen = () => {'), ws.indexOf('ws.onopen = () => {') + 2500);
  ok(/this\.ab\.socketOpened\(\)/.test(open) && /this\.ab\.expireIfSilent\(\)/.test(open) && !/this\.ab\.reset\(\)|_abReset/.test(open),
     'the client\'s onopen asks the server\'s state (socketOpened / expireIfSilent) and never resets to one VFO on its own');
}

console.log('── where the A/B switch lives, and VFO B\'s colour ──');
{
  const src = (f: string) => readFileSync(fileURLToPath(new URL(f, import.meta.url)), 'utf8');
  const fm = src('../src/components/FreqModal.tsx'), sp = src('../src/components/StepPicker.tsx'), scr = src('../src/screens/SDRScreen.tsx');
  ok(/case 'vfo:ab': onVfoAB\?\.\(!vfoAB\)/.test(fm) && /focused=\{tuneOn\('vfo:ab'\)\}/.test(fm) && /tuneOn\('vfo:ab'\) && \{ borderColor: focusC\.ring/.test(fm),
     'the frequency card has the VFO A/B key, a highlight stop (pad / keyboard) on both chassis styles');
  ok(!/onVfoAB|VFO A\/B<|abIdx/.test(sp), 'the tuning-step sheet no longer has it');
  const fmTag = scr.slice(scr.indexOf('<FreqModal'), scr.indexOf('<FreqModal') + 6000);
  ok(/vfoAB=\{abOn\}/.test(fmTag) && /onVfoAB=\{abOffered > 0 && !dabOn && !icom && activeDecoder !== 'scanner' \? onVfoAB : undefined\}/.test(fmTag),
     'SDRScreen hands the toggle to the frequency card, under the same gates');
  ok(!/id: 'step', title: 'Step rate, and VFO A\/B'/.test(scr) && /VFO A\/B/.test(scr.slice(scr.indexOf("id: 'freq'"), scr.indexOf("id: 'mode'"))),
     'the tour says VFO A/B is on the frequency card, not under STEP');
  ok(autoVfoBColour('#ffffff') === '#38c8ff' && autoVfoBColour('#4499ff') === '#ffb020' && autoVfoBColour('junk') === '#38c8ff',
     'AUTO: cyan, amber against a blue VFO A — what B has always drawn in');
  ok(vfoBNeedle('#ffffff', VFO_B_AUTO) === '#38c8ff' && vfoBNeedle('#ffffff', '#cc44ff') === '#cc44ff' && vfoBNeedle('#ffffff', 'nonsense') === '#38c8ff',
     'VFO B\'s colour: the listener\'s swatch, AUTO otherwise (and for anything unreadable)');
  const ms = src('../src/components/MenuSheet.tsx');
  ok(/label="VFO B Needle Colour"/.test(ms) && /onVfoNeedleB=\{abOffered > 0 \? setVfoNeedleB : undefined\}/.test(scr)
     && /vfoNeedle, vfoNeedleB, vfoIntensity/.test(scr),
     'the VFO B colour row sits beside VFO A\'s, only where VFO A/B is offered, saved in the display blob');
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

// ★★ Scanner open = VFO B off (Stuart): opening the scanner turns B off and the tuning menu offers no A/B toggle (source checks).
{
  // ★ ESM: no require/__dirname here (the scanner check came in from a CommonJS habit and stopped the whole file loading).
  const scr = readFileSync(fileURLToPath(new URL('../src/screens/SDRScreen.tsx', import.meta.url)), 'utf8');
  ok(/if \(type === 'scanner'\) onVfoAB\(false\);/.test(scr), 'scanner: opening it switches VFO B off');
  ok(/activeDecoder !== 'scanner' \? onVfoAB : undefined/.test(scr), 'scanner: no A/B toggle in the tuning menu while it is open');
}
// ★★★ A BUTTON ON EVERY TUNE CONTROL (Stuart, build 497: "not seeing the button for the B vfo" — iPhone, drums, the
//     default). The keys always had an A/B key; the drum had only a letter and a hidden hold. Source checks.
{
  const src = (f: string) => readFileSync(fileURLToPath(new URL(f, import.meta.url)), 'utf8');
  const drum = src('../src/components/DrumWheel.tsx');
  ok(/const abKey\s*=\s*abOn && !!onTrapHold;/.test(drum) && /\{abKey && \([\s\S]{0,200}<DomeKey[\s\S]{0,200}onPress=\{onTrapHold\}/.test(drum),
     'drum: an A/B KEY in the window while A/B is on (a tap switches)');
  const bar = src('../src/components/ControlsBar.tsx');
  ok((bar.match(/vfoLetter=\{vfoAB\} onTrapHold=\{onVfoSwap\}/g) ?? []).length === 2
     && (bar.match(/abLetter=\{vfoAB\} onAbPress=\{onVfoSwap\}/g) ?? []).length === 2,
     'both bars (portrait + landscape) hand the switch to the drum AND the keys');
  const ws = src('../src/services/VibeServerWsClient.ts');
  ok(/private _onWatch\(msg[^)]*\) \{\s*\{ const m = watchMaxOf\(msg\); if \(m !== null\) this\._abOffer\(m\); \}/.test(ws)
     && /this\._abOffer\(watchOffered\(msg\)\)/.test(ws),
     'the client takes the offer from hwinfo AND the watch state\'s max (as the web does)');
}
console.log('── round 3: mutes, each VFO\'s squelch by name, B\'s auto squelch ──');
{
  const st = parseWatch({ type: 'watch', max: 1, muteA: true, chans: [{ id: 1, freq: 7e6, mute: true }, { id: 2, freq: 7.1e6 }] })!;
  ok(st.muteA === true && st.chans[0].mute === true && st.chans[1].mute === false, 'parseWatch: muteA and each channel\'s mute (absent = false)');
  ok(parseWatch({ type: 'watch', chans: [] })!.muteA === false, 'an older server (no muteA): not muted');
  ok(JSON.stringify(msgMute('A', true)) === '{"type":"watch","muteA":true}' && JSON.stringify(msgMute('B', false)) === '{"type":"watch","id":1,"mute":false}',
     'msgMute: A by muteA, B by its channel id');
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  const sent: Record<string, unknown>[] = [];
  let t = 1000;
  const ab = new VfoABCore(() => status, (m) => sent.push(m), () => {}, () => t);
  ab.setOffered(1);
  ab.setMute('B', true);
  ok(sent.length === 0, 'setMute with no VFO B: nothing sent');
  ok(ab.squelchFor('B', -80) === true && sent.length === 0, 'B\'s squelch with no VFO B: swallowed — never sent as A\'s');
  ab.setOn(true); sent.length = 0;
  ok(ab.squelchFor('A', -85) === false && sent.length === 0, 'A\'s squelch by name: the client sends it as the ordinary squelch, never `watch`');
  ok(ab.squelchFor('B', -77) === true && sent.at(-1)!.type === 'watch' && sent.at(-1)!.sql === -77, 'B\'s squelch by name: `watch`');
  ab.select('B');
  ok(ab.squelchFor('A', -88) === false && ab.info().squelchA === -88 && ab.info().squelchB === -77, 'with B tuned, A\'s squelch by name is still A\'s');
  ab.setMute('A', true);
  ok(sent.at(-1)!.muteA === true, 'setMute A: on the wire');
  t += 2000;
  ab.onWatch({ type: 'watch', max: 1, muteA: true, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, mute: false }] });
  ok(ab.info().muteA === true && ab.info().muteB === false, 'info carries the server\'s mutes');
  ab.onWatch({ type: 'watch', max: 1, muteA: true, chans: [] });
  ok(!ab.on && ab.info().muteA === false, 'B gone: no mute shown (there is no per-VFO mute with one VFO)');
}
{
  // B's auto squelch: a quiet channel at -100 with its neighbours at -100, then a signal at -70, then quiet again.
  const tr = new SqlAutoTrack();
  let now = 0;
  const step = (chan: number, ex: number, key = 'k1') => { now += 50; return tr.tick(chan, key, ex, 12, now); };
  const first = step(-100, 0);
  ok(first === -88, `seeded from the neighbours, threshold = floor + margin (${first})`);
  for (let i = 0; i < 40; i++) step(-100, 0);
  ok(tr.sent === -88 && step(-100, 0) === null, 'steady noise: nothing more sent');
  const open = step(-70, 30);
  ok(open === -999, `a signal above the threshold: the gate is held OPEN (VS_SQL_OFF) (${open})`);
  for (let i = 0; i < 100; i++) step(-70, 30);
  ok(Math.abs(tr.base - -100) < 0.5, `the baseline does not rise during the over (${tr.base.toFixed(2)})`);
  // quiet again: the hang holds, then the threshold comes back
  const t0 = now;
  let back: number | null = null;
  for (let i = 0; i < 40 && back === null; i++) { const r = step(-100, 0); if (r !== null) back = r; }
  ok(back === -88 && now - t0 >= SQL_AUTO_HANG_MS, `after the ${SQL_AUTO_HANG_MS} ms hang the threshold returns (${back} at +${now - t0} ms)`);
  // a new channel re-judges from its own neighbours
  const nk = step(-90, 0, 'k2');
  ok(nk === -78, `a new channel (key): reseeded there (${nk})`);
  ok(new SqlAutoTrack().tick(NaN, 'k', 0, 12, 0) === null, 'no level: nothing sent');
}
console.log('── decode on either VFO ──');
{
  ok(parseWatch({ type: 'watch', max: 1, decB: true, chans: [] })!.decB === true && parseWatch({ type: 'watch', max: 1, chans: [] })!.decB === false
     && parseWatch({ type: 'watch', decB: 'yes', chans: [] })!.decB === false, 'decB: true only when the server says so (a round-3 server: false)');
  let s = decStartVfo(true, 'B', true);
  ok(s.vfo === 'B' && !s.selectA, 'B selected on a server that decodes B: the decoder runs on B, the controls stay');
  s = decStartVfo(true, 'B', false);
  ok(s.vfo === 'A' && s.selectA, 'B selected on a round-3 server: on A, the controls go to A first (its rule)');
  ok(decStartVfo(true, 'A', true).vfo === 'A' && !decStartVfo(false, 'B', true).selectA && decStartVfo(false, 'B', true).vfo === 'A',
     'A selected, or one VFO: A');
  ok(decTagFor('B', true, 'B', null) === 'DEC' && decTagFor('A', true, 'B', null) === '' && decTagFor('A', true, null, 'A') === 'DEC',
     'DEC on the VFO a decoder or the spots run on, not the other');
  ok(decTagFor('A', false, 'A', 'A') === '', 'one VFO: no DEC (nothing to tell apart)');
  // tuneOtherB — B moved while A is selected
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  const sent: Record<string, unknown>[] = [];
  const adopt = (mode: string, t: TuneStatus) => { if (t.mode !== mode) { t.mode = mode; t.bandwidthLow = mode === 'am' ? -5000 : 100; t.bandwidthHigh = mode === 'am' ? 5000 : 2800; } };
  const ab = new VfoABCore(() => status, (m) => sent.push(m), adopt, () => 5000);
  ab.tuneOtherB(7080000);
  ok(sent.length === 0, 'tuneOtherB with no VFO B: nothing sent');
  ab.setOffered(1); ab.setOn(true);
  sent.length = 0;
  ab.tuneOtherB(7040000, 'am');
  ok(status.frequency === 7074000 && status.mode === 'usb' && ab.active === 'A', 'tuneOtherB: the controls stay on A, A untouched');
  ok(ab.info().other!.frequency === 7040000 && ab.info().other!.mode === 'am' && ab.info().other!.bandwidthHigh === 5000, '…B moved in its slot, its mode\'s passband with it');
  ok(sent.length === 1 && sent[0].type === 'watch' && sent[0].frequency === 7040000 && sent[0].mode === 'am', '…sent as B\'s `watch` aim, never a `tune`');
  ab.select('B');
  sent.length = 0;
  ab.tuneOtherB(7050000);
  ok(sent.length === 0 && status.frequency === 7040000, 'with B selected tuneOtherB does nothing (the ordinary tune is B\'s)');
}
{
  const src = (f: string) => readFileSync(fileURLToPath(new URL(f, import.meta.url)), 'utf8');
  const scr = src('../src/screens/SDRScreen.tsx');
  const ws = src('../src/services/VibeServerWsClient.ts'), dc = src('../src/services/DecoderClient.ts');
  ok(/const wantSpk = spotsBandOn && !abOn, wantA = spotsBandOn && abOn;/.test(scr) && /abSetMute\?\.\(spotsVfo, true\)/.test(scr),
     'Band with VFO B on mutes the VFO it runs on in the mix, not the speaker');
  ok(!/abDecodeOnA/.test(scr), 'the forced switch to VFO A is gone');
  ok(/onTuneHz=\{onDecTuneHz\}/.test(scr) && /tuneVfoHz\(decVfoRef\.current, hz, mode\)/.test(scr) && /tuneVfoHz\(spotsVfoRef\.current, hz, 'usb' as SDRMode\)/.test(scr),
     'the decoder box tunes the VFO its decoder runs on; Band tunes the VFO it runs on');
  ok(/decoderClient\.current\.vfo = decVfoRef\.current/.test(scr) && /startSpots\(k, 'single', v\)/.test(scr) && /startSpots\('digi', 'band', v\)/.test(scr),
     'a decoder, Single spots and Band each start on the VFO chosen when started (abStartVfo)');
  ok(/abActiveRef\.current !== spotsVfoRef\.current\) return false/.test(scr), 'Band\'s arrows step bands only while its own VFO is tuned');
  ok(/dec: !!abDecTag\(otherVfo\(abActive\)\)/.test(scr) && /dec: !!abDecTag\(abActive\)/.test(scr) && /decA: !!abDecTag\('A'\), decB: !!abDecTag\('B'\)/.test(scr),
     'both needles and both squelch rows carry DEC');
  ok(/abTuneVfo\(v: VfoId, frequency: number/.test(ws) && /this\.ab\.tuneOtherB\(frequency, mode\)/.test(ws), 'the client tunes a named VFO (abTuneVfo)');
  ok(/\.\.\.\(this\.vfo === 'B' \? \{ vfo: 'B' \} : \{\}\)/.test(dc) && /this\.spotsVfo === 'B' \? \{ vfo: 'B' \} : \{\}/.test(dc),
     'the decoder socket says "vfo":"B" on an attach and a subscribe (and nothing for A — an older server is unchanged)');
  ok(/vfoRows=\{abOn && isLocal && !icom \?/.test(scr), 'the Audio menu gets a row per VFO only while B is on');
  ok(!/setHwSquelch\(info\.activeSquelchDb\)/.test(scr), 'the squelch slider no longer follows a switch (it is VFO A\'s row)');
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
