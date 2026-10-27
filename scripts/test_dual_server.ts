/**
 * V12 TWO SERVERS AT ONCE — the pure rules (src/services/dualMix.ts) and the wiring that must agree with them
 * (GitHub discussion #57; Stuart's design, 2026-10-27).
 *
 * Proves:
 *   • the pan law and the glide are VFO A/B's (vibe_watch.h): equal power, unity at the centre, a 400 ms raised cosine
 *     with zero slope at both ends, re-aimed mid-move from where it is — and the Swift and Kotlin voices carry the same;
 *   • ear assignment: server 1 LEFT, server 2 RIGHT in stereo; both centred in mono; the survivor centred at unity;
 *     a swap never moves an ear;
 *   • the balance only ever LOWERS the server leaned away from, with a dead zone round even;
 *   • the background clock's stages match the app's own countdown, and the card pops out on a RISE only;
 *   • which servers may take part (VibeServer only — never a dead row);
 *   • the pairing's state machine: begin → attach → swap → end, in every configuration, including the lone survivor on
 *     the second voice being moved to the main pump;
 *   • "may a leaving screen keep its audio?" (holds) for swaps, unannounced exits and endings;
 *   • the terminal reasons are exactly the native pumps' terminal set (ONE RULE, THREE READERS);
 *   • source checks: the player stops only what it owns, the role is decided before any child effect, the recorder
 *     taps BEFORE the pan on both platforms.
 *
 * Run: node --no-warnings scripts/test_dual_server.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  Ramp, earGains, balanceGains, dualTargets, earIcon, countdownStage, shouldPopOut, fmtLeft, dualEligible, sameServer,
  DUAL_OFF, beginPicking, attachSecond, swap, endSide, markEnded, holds, transportFor, bgSide, terminalWhy,
  TERMINAL_WHY, DUAL_RAMP_SEC, clampBalance, pairingStart, secondServerHint, VFO_B_CLOSED_NOTE,
  radioKeyOf, sameRadio, pickSameServer, detachSecond, sideName,
  type DualCore, type DualSideCore,
} from '../src/services/dualMix.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}
const near = (a: number, b: number, e = 1e-6) => Math.abs(a - b) <= e;

console.log('pan law (vibe_watch.h PanMixer)');
{
  const c = earGains(0.5, 1);
  ok(near(c.l, 1) && near(c.r, 1), 'centre: unity in each ear');
  const l = earGains(0, 1);
  ok(near(l.l, Math.SQRT2) && near(l.r, 0), 'hard left: +3 dB left, silent right (equal power)');
  const r = earGains(1, 1);
  ok(near(r.l, 0, 1e-9) && near(r.r, Math.SQRT2), 'hard right: silent left, +3 dB right');
  for (const p of [0, 0.2, 0.5, 0.77, 1]) {
    const g = earGains(p, 1);
    ok(near(g.l * g.l + g.r * g.r, 2), `equal power at pos ${p}`);
  }
  ok(near(earGains(0, 0.5).l, Math.SQRT2 / 2), 'gain scales both ears');
}

console.log('the glide (vibe_watch.h Ramp)');
{
  const rate = 48_000, steps = Math.round(DUAL_RAMP_SEC * rate);
  ok(steps === 19_200, '400 ms at 48 kHz = 19 200 steps');
  const rp = new Ramp(0.5);
  rp.aim(0, steps);
  const first = rp.step(), second = rp.step();
  ok(first < 0.5 && 0.5 - first < 1e-8, 'starts with (near) zero slope — no click');
  ok(second < first, 'moves the right way');
  for (let i = 2; i < steps / 2; i++) rp.step();
  ok(near(rp.cur, 0.25, 1e-4), 'half-way in time = half-way in value (symmetric raised cosine)');
  const mid = rp.cur;
  rp.aim(1, steps);   // re-aimed mid-glide
  ok(near(rp.from, mid) && rp.moving(), 're-aimed mid-move: starts FROM WHERE IT IS');
  for (let i = 0; i < steps; i++) rp.step();
  ok(rp.cur === 1 && !rp.moving(), 'lands exactly on the target and rests');
  const before = rp.cur; rp.aim(1, steps);
  ok(!rp.moving() && rp.cur === before, 're-aiming at the same target does nothing');
}

console.log('balance');
{
  ok(balanceGains(0).s1 === 1 && balanceGains(0).s2 === 1, 'even: both at their own level');
  ok(balanceGains(0.02).s1 === 1 && balanceGains(-0.03).s2 === 1, 'dead zone round even');
  const b = balanceGains(0.5);
  ok(b.s2 === 1 && b.s1 < 1 && b.s1 > 0, 'leaning to server 2 lowers server 1 only — never raises anyone');
  ok(near(balanceGains(1).s1, 0) && balanceGains(1).s2 === 1, 'full lean silences the other (without disconnecting)');
  ok(near(balanceGains(-1).s2, 0) && balanceGains(-1).s1 === 1, 'and the other way');
  ok(clampBalance(NaN) === 0 && clampBalance(7) === 1 && clampBalance(-9) === -1, 'clamped, NaN = even');
}

console.log('ears');
{
  const st = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true });
  ok(st.s1.pos === 0 && st.s2.pos === 1 && st.s1.gain === 1 && st.s2.gain === 1, 'stereo: server 1 LEFT, server 2 RIGHT');
  const mo = dualTargets({ output: 'mono', balance: 0, s1Alive: true, s2Alive: true });
  ok(mo.s1.pos === 0.5 && mo.s2.pos === 0.5, 'mono: both in both ears');
  const one = dualTargets({ output: 'stereo', balance: 0.8, s1Alive: false, s2Alive: true });
  ok(one.s2.pos === 0.5 && one.s2.gain === 1, 'survivor: centred at unity, the balance forgotten');
  ok(one.s1.gain === 0 && one.s1.pos === 0, 'the one leaving fades OUT where it is — it does not swing across the head');
  ok(earIcon('stereo', 0) === 'L' && earIcon('stereo', 1) === 'R' && earIcon('mono', 0) === 'mono', 'card icon');
}

console.log('the background clock');
{
  ok(countdownStage(null, false) === 0, 'no limit: normal');
  ok(countdownStage(301_000, false) === 0 && countdownStage(299_000, false) === 1, 'yellow under five minutes');
  ok(countdownStage(119_000, false) === 2, 'red under two (= SDRScreen useCountdownStage)');
  ok(countdownStage(10_000, true) === 0, 'a soft limit never warns');
  const z = { stage: 0, asking: false, ended: false };
  ok(shouldPopOut(z, { ...z, stage: 1 }) && shouldPopOut({ ...z, stage: 1 }, { ...z, stage: 2 }), 'pops out on each RISE');
  ok(!shouldPopOut({ ...z, stage: 1 }, { ...z, stage: 1 }), 'not again at the same stage (a tuck is respected)');
  ok(shouldPopOut(z, { ...z, asking: true }) && shouldPopOut(z, { ...z, ended: true }), 'STILL LISTENING? and an ending pop it');
  ok(fmtLeft(61_500) === '1:01' && fmtLeft(-5) === '0:00', 'm:ss, never negative');
}

console.log('who can take part');
{
  const vs = { serverType: 'vibeserver', isLocal: true, localHost: '192.168.1.5', localPort: 8080 };
  ok(dualEligible(vs), 'a VibeServer');
  ok(!dualEligible({ ...vs, localHost: undefined }), 'not this device\'s own radio (connectLocal sets no host)');
  ok(!dualEligible({ ...vs, isTcp: true }), 'not RTL-TCP / SpyServer (an in-app shim)');
  ok(!dualEligible({ serverType: 'ubersdr' }) && !dualEligible({ serverType: 'kiwi' }) && !dualEligible({ serverType: 'owrx' }),
     'not UberSDR / Kiwi / OpenWebRX');
  ok(!dualEligible({ ...vs, compatOnly: true }) && !dualEligible(null), 'not a compat-only page, not nothing');
  ok(sameServer(vs, { ...vs }) && !sameServer(vs, { ...vs, localPort: 8081 }), 'same server = same host and port');
}

console.log('the pairing');
{
  type S = DualSideCore;
  const A: S = { id: 'A', transport: 'native', ended: null };
  const B: S = { id: 'B', transport: 'aux', ended: null };
  let st: DualCore<S> = DUAL_OFF;
  ok(!holds(st, 'A') && transportFor(st, 'A') === 'native', 'off: nothing held, the main pump');
  st = beginPicking(st, A);
  ok(st.phase === 'picking' && st.fg === null, 'Listen to a 2nd server → picking, the list in front');
  ok(holds(st, 'A'), '…and A\'s leaving screen KEEPS its audio');
  ok(beginPicking(st, B) === st, 'cannot begin twice');
  st = attachSecond(st, { ...B, transport: 'native' });
  ok(st.phase === 'paired' && st.sides[0].id === 'A' && st.sides[1].id === 'B' && st.fg === 'B',
     'second server attaches as server 2, in front');
  ok(transportFor(st, 'B') === 'aux' && transportFor(st, 'A') === 'native', 'server 2 on the second voice whatever it asked');
  ok(holds(st, 'A') && !holds(st, 'B'), 'the background one is held; the one in front is not');
  ok(bgSide(st)?.id === 'A', 'the card shows A');
  const sw = swap(st);
  ok(sw.fg === 'A' && sw.sides === st.sides, 'TAP: A to the front — sides (ears, voices) untouched');
  ok(holds(sw, 'B') && !holds(sw, 'A'), '…B\'s screen leaves and B keeps playing');
  ok(swap(swap(st)).fg === 'B', 'and back');

  // stop the background one (A, native) while B (aux) is in front → off; B's screen moves itself to the main pump
  let r = endSide(st, 'A');
  ok(r.st.phase === 'off' && r.fx.stop?.id === 'A' && r.fx.stop.transport === 'native' && r.fx.moveToNative === null,
     'stop A with B in front: pairing off, A stopped, B moves itself (transportFor → native)');
  ok(transportFor(r.st, 'B') === 'native', '…B now asks for the main pump');

  // stop the background one (B, aux) while A is in front
  r = endSide(sw, 'B');
  ok(r.st.phase === 'off' && r.fx.stop?.transport === 'aux', 'stop B with A in front: B\'s voice closed, A plays on');

  // leave B (in front) → A survives in the background → picking again with A
  r = endSide(st, 'B');
  ok(r.st.phase === 'picking' && r.st.sides[0].id === 'A' && r.fx.moveToNative === null,
     'leave B: back to the list, A still playing — ready for a new second server');

  // leave A (in front, after a swap) → B survives on the SECOND voice with no screen → moved to the main pump
  r = endSide(sw, 'A');
  ok(r.st.phase === 'picking' && r.st.sides[0].id === 'B' && r.st.sides[0].transport === 'native' && r.fx.moveToNative === 'B',
     'leave A: B, alone in the background on the second voice, is moved to the main pump');

  // the server ended the background one
  const ended = markEnded(st, 'A', 'its time limit was reached');
  ok(ended.sides[0].ended !== null && !holds(ended, 'A'), 'ended: no longer held');
  ok(swap({ ...ended, fg: 'B' }).fg === 'B', 'an ended server cannot be swapped to');
  r = endSide(ended, 'A');
  ok(r.st.phase === 'off', 'and when it goes, the one in front plays on alone');

  // picking: the solo ends
  r = endSide(beginPicking(DUAL_OFF, A), 'A');
  ok(r.st.phase === 'off' && r.fx.stop?.id === 'A', 'picking: the one playing ends → off');
  ok(endSide(DUAL_OFF, 'A').fx.stop === null, 'nothing to end when off');
}

console.log('terminal reasons — ONE RULE, THREE READERS');
{
  const swift = readFileSync('ios/VibeSDR/VibePowerModule.swift', 'utf8');
  const kt = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeStreamService.kt', 'utf8');
  const grab = (src: string, re: RegExp) => [...(src.match(re)?.[1] ?? '').matchAll(/"([a-z_]+)"/g)].map((m) => m[1]).sort();
  const sw = grab(swift, /laTerminalTypes: Set<String> = \[([^\]]+)\]/);
  const kk = grab(kt, /LA_TERMINAL_TYPES = setOf\(([^)]+)\)/);
  const js = Object.keys(TERMINAL_WHY).sort();
  ok(sw.length > 0 && JSON.stringify(sw) === JSON.stringify(kk), 'Swift and Kotlin agree');
  ok(JSON.stringify(js) === JSON.stringify(sw), 'dualMix.TERMINAL_WHY names exactly that set');
  ok(terminalWhy('idle_check') === null && terminalWhy('session_warning') === null, 'a question or a warning is not an ending');
}

console.log('source checks');
{
  const lap = readFileSync('src/components/LocalAudioPlayer.tsx', 'utf8');
  ok(/!dualHolds\(dualId\) && releaseNative\(sessionId\)\) Vibe\?\.stopLocalAudio/.test(lap),
     'the player stops the main pump only if it still owns it and the pairing does not hold it');
  ok(/claimNative\(sessionId\)/.test(lap), '…and claims it on every start');
  ok(/if \(!dualHolds\(dualId\)\) auxClose\(dualId\)/.test(lap), 'the second voice is closed only when not held');
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  ok(/useState<string \| null>\(\(\) => roleOnMount\(/.test(sdr), 'the role is decided in a useState initialiser (first render)');
  const swift = readFileSync('ios/VibeSDR/VibePowerModule.swift', 'utf8');
  const iRec = swift.indexOf('if recArmed { writeRecording(buf) }');
  const iPan = swift.indexOf('VibeDualAudio.primary.process(buf)');
  ok(iRec > 0 && iPan > iRec && iPan < swift.indexOf('if !isMuted { scheduleOut(buf) }', iRec),
     'iOS: the recorder taps BEFORE the pan, the pan before the speaker');
  const kt = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeStreamService.kt', 'utf8');
  ok(/playN = VibeDualMix\.primary\.process\(pcm, pcmN/.test(kt) && /recTapShorts\(pcm, pcmN, pcmCh/.test(kt),
     'Android: the track gets the placed copy, the recorder the server as it is');
  const vSwift = readFileSync('ios/VibeSDR/VibeDualAudio.swift', 'utf8');
  const vKt = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeDualAudio.kt', 'utf8');
  for (const [name, src] of [['Swift', vSwift], ['Kotlin', vKt]] as const) {
    ok(/0\.5f? - 0\.5f? \* cos\((Float\.pi|PI\.toFloat\(\)) \* u\)/.test(src), `${name}: the raised cosine`);
    ok(/0\.4 \* (buf\.format\.sampleRate|rate)/.test(src), `${name}: 400 ms`);
    ok(/squareRoot\(\)|sqrt\(2f\)/.test(src) && /sin\(/.test(src), `${name}: √2·cos / √2·sin`);
    ok(/0\.5f? \* \((L\[i\] \+ R\[i\]|src\[2 \* i\] \+ src\[2 \* i \+ 1\])\)/.test(src), `${name}: folded to mono first`);
  }
  const pbx = readFileSync('ios/VibeSDR.xcodeproj/project.pbxproj', 'utf8');
  ok((pbx.match(/VibeDualAudio\.swift in Sources/g) ?? []).length === 2 && (pbx.match(/VibeDualAudio\.m in Sources/g) ?? []).length === 2,
     'iOS: both files are in the app target');
  const pkg = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeStreamPackage.kt', 'utf8');
  ok(/VibeDualAudioModule\(ctx\)/.test(pkg), 'Android: the module is registered');
  const jni = readFileSync('android/app/src/main/cpp/vibe_localsdr_jni.cpp', 'utf8');
  ok((jni.match(/nativeOpusDecodeAux/g) ?? []).length === 2, 'Android: a second Opus decoder (and its no-Opus stub)');
}

console.log('two servers OR VFO A/B, never both (Stuart, #57: "There will only be 2 channels at a time")');
{
  const off = pairingStart(false), on = pairingStart(true);
  ok(!off.closeB && off.note === null, 'VFO B off: the pairing starts as before, nothing said');
  ok(on.closeB && on.note === VFO_B_CLOSED_NOTE, 'VFO B on: B is closed first, and the server list says so');
  ok(/VFO B closed/.test(VFO_B_CLOSED_NOTE) && /both ears/.test(VFO_B_CLOSED_NOTE), 'the note: "VFO B closed — two servers use both ears"');
  ok(!/VFO B/.test(secondServerHint(false)), 'the row\'s hint does not mention B when B is off');
  ok(/^VFO B will close/.test(secondServerHint(true)) && secondServerHint(true).endsWith(secondServerHint(false)),
     'with B on the row stays, and its hint says first that it will close B');
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  ok(/closesVfoB=\{abOn\}/.test(sdr), 'the Servers menu is told when B is on');
  ok(/onListenSecond=\{dualMyId && !refusal && canListenSecond\(route\.params\) && dualShape\.phase === 'off'\s*\? onListenSecond : undefined\}/.test(sdr),
     '"Listen to a 2nd server" is NOT hidden by VFO B (the row stays available)');
  ok(/freqHz: abDoor\.frequency, mode: String\(abDoor\.mode\)/.test(sdr), 'the card is given VFO A\'s tuning — the channel that plays on');
  const chip = readFileSync('src/components/ServersChip.tsx', 'utf8');
  ok(/secondServerHint\(closesVfoB\)/.test(chip) && !/SECOND_HINT/.test(chip), 'the chip draws the one hint rule');
  const ds = readFileSync('src/services/dualServer.ts', 'utf8');
  ok(/beginPicking\(state, [^)]*\), note \}\)/.test(ds), 'the note rides the picking state');
  const pick = readFileSync('src/screens/InstancePickerScreen.tsx', 'utf8');
  ok(/dual\.note \?/.test(pick), 'the server list\'s banner shows it');
  const mix = readFileSync('src/services/dualMix.ts', 'utf8');
  ok(!/a VFO A\/B pair the\s*\*\s*server has already panned/.test(mix), 'the old "fold a VFO A/B pair to mono" rule is gone from the notes');
}

console.log('pairing is per RADIO (Gesiima\'s two dongles on one VibeServer, #57 — Stuart: "treated as separate instances")');
{
  const vs = { serverType: 'vibeserver', isLocal: true, localHost: '192.168.1.5', localPort: 8073 };
  ok(radioKeyOf('http://192.168.1.5:8073/r/rtl0') === '/r/rtl0' && radioKeyOf('http://192.168.1.5:8073/r/rtl0/') === '/r/rtl0',
     'the radio key is the /r/<id> part of the address');
  ok(radioKeyOf('http://192.168.1.5:8073') === '' && radioKeyOf(null) === '', 'an address that IS the radio has none');
  const r0 = { params: vs, radioKey: '/r/rtl0' }, r1 = { params: vs, radioKey: '/r/rtl1' };
  ok(sameRadio(r0, { params: { ...vs }, radioKey: '/r/rtl0' }), 'same server + same radio = the same radio');
  ok(!sameRadio(r0, r1), 'two dongles on ONE server are two radios');
  ok(!sameRadio(r0, { params: { ...vs, localPort: 8074 }, radioKey: '/r/rtl0' }), 'the same id on another server is another radio');
  // The server list, the server already playing picked again:
  ok(pickSameServer(r0, vs) === 'radios', 'a radio behind a front door → open the server: its radio list offers the other radio');
  ok(pickSameServer({ params: vs, radioKey: '' }, vs) === 'back', 'a single-radio server → back to it (nothing else on it to choose)');
  ok(pickSameServer(r0, { ...vs, localHost: '10.0.0.2' }) === 'other' && pickSameServer(null, vs) === 'other', 'another server → a second side as ever');
  // The radio list on the joining screen chose the radio ALREADY playing: not a second side.
  type S = DualSideCore;
  const A: S = { id: 'A', transport: 'native', ended: null };
  const paired: DualCore<S> = attachSecond(beginPicking(DUAL_OFF as DualCore<S>, A), { id: 'B', transport: 'aux', ended: null });
  const back = detachSecond(paired, 'B');
  ok(back.phase === 'picking' && back.sides.length === 1 && back.sides[0].id === 'A' && back.fg === null,
     'detachSecond: back to picking with the first radio alone (its screen then adopts the audio still playing)');
  ok(detachSecond(paired, 'A') === paired && detachSecond(back, 'B') === back, '…only the JOINING side, only while paired');
  ok(sideName('Gesiima Pi', 'RTL 2') === 'Gesiima Pi · RTL 2' && sideName('Gesiima Pi', null) === 'Gesiima Pi'
     && sideName('Pi', 'Pi') === 'Pi', 'the card says "server · radio" when there is a radio to name');
  // ★ The two sessions never collide: each side IS its own session id (a uuid per radio screen), and the singleton guards
  //   (claimNative / releaseNative / dualHolds / the aux socket) are keyed by that id, never by host.
  const ds = readFileSync('src/services/dualServer.ts', 'utf8');
  ok(/export function releaseNative\(id: string\)/.test(ds) && /export function dualHolds\(id: string \| null\)/.test(ds)
     && /aux\.id === id/.test(ds) && !/releaseNative\([^)]*[Hh]ost/.test(ds), 'the pump and the second voice are owned by SESSION, not host');
  ok(/sameServer\(s\.params as DualRouteLike, params\) && !s\.radioKey/.test(ds),
     'a screen on the same server is turned away only when the side playing there is the server\'s only radio');
  ok(/pickSameServer\(\{ params: s\.params as DualRouteLike, radioKey: s\.radioKey \?\? '' \}, params\) === 'back'/.test(ds),
     'the server list goes back only to a single-radio server');
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  const adopts = sdr.match(/setRadioBase\((?!null)/g) ?? [];
  const guarded = (sdr.match(/bounceIfFirstRadio\((baseUrl|radioBaseUrl\(baseUrl, (id|d\.radios\[0\]\.id)\))\)\) return/g) ?? []).length;
  ok(adopts.length === 3 && guarded === 3, `every radio adoption (chosen, door of one, an address naming its radio) passes the first-radio check first (${guarded}/${adopts.length})`);
  ok(/noteStatus\(dualMyId, \{[^}]*name: dualName, radioKey: dualRadioKey \}, radioBase\)/.test(sdr), 'the card is told the radio and its name');
  ok(/if \(dualRadioKey && radioBase\) back\.baseUrl = radioBase;/.test(sdr) && /baseUrl: wantBase/.test(ds),
     'a side returns to its RADIO\'s address (adopted straight through), never the radio list');
  const dr = readFileSync('src/utils/doorReturn.ts', 'utf8');
  ok(/'autoTakeover', 'dualSide'\]/.test(dr), 'back to the radio list is a new listener: the old side\'s session does not ride along');
}

console.log(fails ? `\n✗ ${fails} failed` : '\n✓ all passed');
process.exit(fails ? 1 : 0);
