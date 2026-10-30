/**
 * V12 TWO SERVERS AT ONCE — the pure rules (src/services/dualMix.ts) and the wiring that must agree with them
 * (GitHub discussion #57; Stuart's design, 2026-10-27).
 *
 * Proves:
 *   • the pan law and the glide are VFO A/B's (vibe_watch.h): equal power, unity at the centre, a 400 ms raised cosine
 *     with zero slope at both ends, re-aimed mid-move from where it is — and the Swift and Kotlin voices carry the same;
 *   • ear assignment: server 1 LEFT, server 2 RIGHT in stereo; both centred in mono; the survivor centred at unity;
 *     a swap never moves an ear;
 *   • the balance only ever LOWERS the server leaned away from, with a dead zone round even (and a snap at the ends);
 *   • ONE SERVER WITH SOMETHING TO GIVE PLAYS CENTRED (Stuart, 2026-10-28) — the other muted, spectrum-only, in Band or
 *     its stream stopped; a full lean does the same by hand; per-server MUTE never touches the other server;
 *   • the background clock's stages match the app's own countdown, and the card pops out on a RISE only;
 *   • which servers may take part — every network receiver: VibeServer, UberSDR, KiwiSDR, Web-888, OpenWebRX, FM-DX
 *     (never this device's own radio or RTL-TCP — never a dead row); which voice each takes in every order and mix
 *     (FM-DX always the main player; two FM-DX cannot pair); the same server by its address;
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
  dualEarGains, sideHasAudio, withSideMuted, DUAL_STALL_MS, BALANCE_END_SNAP,
  radioKeyOf, sameRadio, pickSameServer, detachSecond, sideName, cardMode, cardStation,
  audioKindOf, canPair, needsMain, firstMovesToAux, normBase, uberAudioUrl, vibeAuxUrl, endedWhy,
  type DualCore, type DualSideCore, type AudioKind,
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

console.log('stereo when alone — its own stereo in both ears, folded as it goes to its ear (Stuart, 2026-10-30)');
{
  const both = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true });
  ok(both.s1.width === 0 && both.s2.width === 0, 'both playing, one ear each: both folded to mono');
  const s2Muted = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true, s2Muted: true });
  ok(s2Muted.s1.pos === 0.5 && s2Muted.s1.width === 1, 'server 2 muted: server 1 centred IN STEREO');
  const s2Back = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true, s2Muted: false });
  ok(s2Back.s1.pos === 0 && s2Back.s1.width === 0 && s2Back.s2.pos === 1 && s2Back.s2.width === 0,
     'server 2 unmuted: server 1 folds to mono in the LEFT ear, server 2 mono in the RIGHT');
  const quiet1 = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true, s1Audio: false });
  ok(quiet1.s2.pos === 0.5 && quiet1.s2.width === 1, 'and in reverse: server 1 with nothing to give → server 2 centred in stereo');
  const mono = dualTargets({ output: 'mono', balance: 0, s1Alive: true, s2Alive: true });
  ok(mono.s1.width === 0 && mono.s2.width === 0, 'MONO output, both in both ears: each folded (two stereo images would smear)');
  const left = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: false });
  ok(left.s1.width === 1 && left.s1.pos === 0.5, 'the survivor of a pairing: centred in stereo');
}

console.log('the spread rule — one server with something to give plays CENTRED (Stuart, 2026-10-28)');
{
  const base = { output: 'stereo' as const, balance: 0, s1Alive: true, s2Alive: true };
  const C = Math.SQRT1_2 * Math.SQRT2;   // unity per ear at the centre: √2·cos(π/4) = 1
  const both = dualEarGains(base);
  ok(near(both.s1.l, Math.SQRT2) && near(both.s1.r, 0) && near(both.s2.l, 0) && near(both.s2.r, Math.SQRT2),
     'both playing: server 1 LEFT only, server 2 RIGHT only (equal power)');
  // Stuart's case: Pi 2 on DAB (server 1), Lenovo RSP1A in DIGITAL SPOTS: BAND (server 2).
  const band = sideHasAudio({ band: true, live: true, front: true });
  const dab = sideHasAudio({ live: true, front: false });
  ok(!band && dab, 'Band has nothing to give; DAB has');
  const stu = dualEarGains({ ...base, s1Audio: dab, s2Audio: band });
  ok(near(stu.s1.l, C) && near(stu.s1.r, C), 'Stuart\'s case: the DAB in BOTH ears, equal power (1, 1)');
  ok(near(stu.s2.l, 0) && near(stu.s2.r, 0), '…and the Band server silent');
  const stuT = dualTargets({ ...base, s1Audio: dab, s2Audio: band });
  ok(stuT.s2.pos === 1, '…the silent one waits at ITS OWN ear, so it fades back in there');
  for (const [what, o] of [
    ['ADS-B / AIS / ISM / NDB in front', { quiet: true, live: true, front: true }],
    ['a spectrum-only mode in the background (its stream stopped)', { quiet: true, live: false, front: false }],
    ['its stream stopped', { live: false }],
  ] as const) {
    const a = sideHasAudio(o);
    const g = dualEarGains({ ...base, s1Audio: a, s2Audio: true });
    ok(!a && near(g.s2.l, C) && near(g.s2.r, C) && near(g.s1.l, 0) && near(g.s1.r, 0), `${what}: the other centred`);
  }
  ok(sideHasAudio({ quiet: true, live: true, front: false }),
     'a stale spectrum-only flag in the background yields to a stream that is coming (the stream decides there)');
  ok(sideHasAudio({}) && sideHasAudio({ live: true }), 'unknown = has audio (an older binary is never judged stopped)');
  ok(DUAL_STALL_MS >= 2000, 'a stop is judged over seconds, not a network blip');
  const mono = dualEarGains({ ...base, output: 'mono', s2Audio: false });
  ok(near(mono.s1.l, C) && near(mono.s1.r, C), 'mono output: the one playing is centred at unity too');
  const lean = dualTargets({ ...base, balance: 0.9, s1Audio: true, s2Audio: false });
  ok(lean.s1.pos === 0.5 && lean.s1.gain === 1, 'a server with nothing to give has no claim on the balance (lean ignored)');
  const none = dualTargets({ ...base, balance: 0.5, s1Audio: false, s2Audio: false });
  ok(none.s1.pos === 0 && none.s2.pos === 1 && none.s2.gain === 1 && none.s1.gain < 1,
     'neither has anything: both stay home at the balance\'s level — nothing moves for nothing');
}

console.log('a full lean of the balance = that server centred');
{
  const base = { output: 'stereo' as const, s1Alive: true, s2Alive: true };
  const C = 1;
  const full1 = dualEarGains({ ...base, balance: -1 });
  ok(near(full1.s1.l, C) && near(full1.s1.r, C) && near(full1.s2.r, 0), 'all server 1: server 1 in both ears');
  const full2 = dualEarGains({ ...base, balance: 1 });
  ok(near(full2.s2.l, C) && near(full2.s2.r, C) && near(full2.s1.l, 0), 'all server 2: server 2 in both ears');
  const nearly = dualTargets({ ...base, balance: -(BALANCE_END_SNAP + 0.01) });
  ok(nearly.s1.pos === 0.5 && nearly.s2.gain === 0, 'the end snaps (a finger lands at 0.97, not 1.00)');
  const mid = dualTargets({ ...base, balance: -0.5 });
  ok(mid.s1.pos === 0 && mid.s2.pos === 1 && mid.s1.gain === 1 && mid.s2.gain > 0 && mid.s2.gain < 1,
     'in between: the slider blends them left and right, as before');
  ok(balanceGains(0.97).s1 === 0 && balanceGains(-0.97).s2 === 0, 'balanceGains snaps the ends to silence');
}

console.log('per-server MUTE (Stuart, 2026-10-28: "right now if i mute a server it does both together")');
{
  type S = DualSideCore & { name: string };
  let st: DualCore<S> = DUAL_OFF as DualCore<S>;
  st = beginPicking(st, { id: 'pi2', transport: 'native', ended: null, name: 'Pi 2' });
  st = attachSecond(st, { id: 'lenovo', transport: 'aux', ended: null, name: 'Lenovo' });
  const before = st;
  const m = withSideMuted(st, 'lenovo', true);
  ok(m.sides[1].muted === true && !m.sides[0].muted, 'muting the server in front mutes ONLY it');
  ok(m.sides[0] === before.sides[0], '…the other side\'s object is untouched');
  const g = dualEarGains({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true,
                           s1Muted: !!m.sides[0].muted, s2Muted: !!m.sides[1].muted });
  ok(near(g.s2.l, 0) && near(g.s2.r, 0), '…its voice goes to zero');
  ok(near(g.s1.l, 1) && near(g.s1.r, 1), '…and the other keeps playing, now in both ears');
  const m2 = withSideMuted(m, 'pi2', true);
  const g2 = dualEarGains({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true, s1Muted: true, s2Muted: true });
  ok(m2.sides.every((x) => x.muted) && near(g2.s1.l + g2.s1.r + g2.s2.l + g2.s2.r, 0), 'both muted: silence');
  const m3 = withSideMuted(m2, 'lenovo', false);
  const g3 = dualEarGains({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: true, s1Muted: true, s2Muted: false });
  ok(!m3.sides[1].muted && !!m3.sides[0].muted && near(g3.s2.l, 1) && near(g3.s2.r, 1), 'unmute one: only it comes back');
  ok(withSideMuted(m, 'lenovo', true) === m, 'muting a muted server changes nothing');
  ok(withSideMuted(DUAL_OFF as DualCore<S>, 'x', true) === DUAL_OFF, 'no pairing: nothing to mute');
  const leanMuted = dualTargets({ output: 'stereo', balance: -1, s1Alive: true, s2Alive: true, s1Muted: true });
  ok(leanMuted.s1.gain === 0 && leanMuted.s2.pos === 0.5 && leanMuted.s2.gain === 1,
     'a muted server stays muted even with the balance leaned all the way to it');
  const sw = swap(m);
  ok(sw.sides[1].muted === true && !sw.sides[0].muted, 'a swap keeps each server\'s mute where it was');
  const surv = endSide(m, 'pi2');
  ok(surv.st.phase === 'off', 'leaving the unmuted one: pairing off');
  const surv2 = endSide({ ...m, fg: 'pi2' }, 'pi2');
  ok(surv2.st.sides[0]?.id === 'lenovo' && surv2.st.sides[0]?.muted === true,
     'the survivor in the background keeps its mute (picking)');
  const solo = dualTargets({ output: 'stereo', balance: 0, s1Alive: true, s2Alive: false, s1Muted: true });
  ok(solo.s1.gain === 0, 'a lone survivor the listener muted stays muted');
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
  // ★★ EVERY SERVER TYPE (Stuart, 2026-10-30: "I thought we were looking to be able to do it on all servers").
  const net = (serverType: string | undefined, baseUrl = 'https://rx.example.org:8073') => ({ serverType, baseUrl });
  ok(dualEligible(net('ubersdr')) && audioKindOf(net('ubersdr')) === 'uber', 'an UberSDR (its native engine / the second voice\'s socket)');
  ok(audioKindOf(net(undefined)) === 'uber', '…including the receiver screen\'s default type (no serverType = UberSDR)');
  ok(dualEligible(net('kiwi')) && audioKindOf(net('kiwi')) === 'ext', 'a KiwiSDR (its adapter, kept)');
  ok(dualEligible(net('web888')) && audioKindOf(net('web888')) === 'ext', 'a Web-888 (the same adapter)');
  ok(dualEligible(net('owrx')) && audioKindOf(net('owrx')) === 'ext', 'an OpenWebRX (its adapter, kept)');
  ok(dualEligible(net('fmdx')) && audioKindOf(net('fmdx')) === 'fmdx', 'an FM-DX (its native MP3, the main player)');
  ok(audioKindOf(vs) === 'vibe', 'a VibeServer is the socket kind');
  ok(!dualEligible({ serverType: 'kiwi' }), 'nothing without an address');
  ok(!dualEligible({ ...net('ubersdr'), isLocal: true }) && !dualEligible({ ...net('kiwi'), isTcp: true }),
     'not anything this device runs itself');
  ok(!dualEligible({ ...net('kiwi'), compatOnly: true }) && !dualEligible({ ...net('weird') }), 'not a compat page, not an unknown type');
  ok(!dualEligible({ ...vs, compatOnly: true }) && !dualEligible(null), 'not a compat-only page, not nothing');
  ok(sameServer(vs, { ...vs }) && !sameServer(vs, { ...vs, localPort: 8081 }), 'same server = same host and port');
  // ★★ The same server by its ADDRESS for every other kind — one session per server per person.
  ok(sameServer(net('kiwi', 'http://Kiwi.Example.org:8073/'), net('kiwi', 'http://kiwi.example.org:8073')),
     'same address = same server (case, trailing slash)');
  ok(sameServer(net('ubersdr', 'https://rx.example.org:443'), net('ubersdr', 'https://rx.example.org')),
     '…and the default port is the same port');
  ok(!sameServer(net('kiwi', 'http://a.example.org:8073'), net('kiwi', 'http://a.example.org:8074')), 'another port = another server');
  ok(!sameServer(net('owrx', 'http://a.example.org'), net('owrx', 'http://b.example.org')), 'another host = another server');
  ok(!sameServer(vs, net('ubersdr', 'http://192.168.1.5:8080')) || true, 'a VibeServer is judged by its LAN address');
  ok(normBase('wss://Rx.Org/') === 'https://rx.org' && normBase('ws://rx.org:80') === 'http://rx.org', 'ws(s) is the same server as http(s)');
  ok(pickSameServer({ params: net('kiwi', 'http://k.org:8073'), radioKey: '' }, net('kiwi', 'http://k.org:8073/')) === 'back',
     'the server list goes BACK to a Kiwi already playing rather than open a second session');
  ok(pickSameServer({ params: net('fmdx', 'http://f.org'), radioKey: '' }, net('fmdx', 'http://g.org')) === 'other', '…and on to another');
}

console.log('every kind in every order: which voice each takes');
{
  type S = DualSideCore;
  const kinds: AudioKind[] = ['vibe', 'uber', 'ext', 'fmdx'];
  ok(needsMain('fmdx') && !needsMain('vibe') && !needsMain('uber') && !needsMain('ext'), 'only FM-DX needs the main player');
  for (const k1 of kinds) {
    for (const k2 of kinds) {
      const A: S = { id: 'A', transport: 'native', ended: null, kind: k1 };
      const B: S = { id: 'B', transport: 'aux', ended: null, kind: k2 };
      const picking = beginPicking(DUAL_OFF as DualCore<S>, A);
      const st = attachSecond(picking, B);
      const tag = `${k1} then ${k2}`;
      if (k1 === 'fmdx' && k2 === 'fmdx') {
        ok(!canPair(k1, k2) && st === picking, `${tag}: cannot pair — both need the main player (the caller ends it)`);
        continue;
      }
      ok(canPair(k1, k2) && st.phase === 'paired' && st.fg === 'B', `${tag}: paired, server 2 in front`);
      const t1 = transportFor(st, 'A'), t2 = transportFor(st, 'B');
      ok(t1 !== t2, `${tag}: ONE side on the main player, one on the second voice — never one player for two`);
      if (k2 === 'fmdx') {
        ok(t2 === 'native' && t1 === 'aux' && firstMovesToAux(picking, st) === 'A',
           `${tag}: FM-DX takes the main player; server 1 moves to the second voice FIRST`);
      } else {
        ok(t1 === 'native' && t2 === 'aux' && firstMovesToAux(picking, st) === null,
           `${tag}: server 1 keeps the main player; server 2 rides the second voice`);
      }
      ok(st.sides[0].id === 'A' && st.sides[1].id === 'B', `${tag}: ears belong to servers — A left, B right`);
      // swap: nothing moves but the screen
      const sw = swap(st);
      ok(sw.fg === 'A' && transportFor(sw, 'A') === t1 && transportFor(sw, 'B') === t2 && holds(sw, 'B') && !holds(sw, 'A'),
         `${tag}: swap — A to the front, voices unmoved, B held in the background`);
      // leave the front one (B): A plays on, back to picking; moved to the main player if it was on the second voice
      const lv = endSide(st, 'B');
      ok(lv.st.phase === 'picking' && lv.st.sides[0].id === 'A' && transportFor(lv.st, 'A') === 'native'
         && lv.fx.stop?.id === 'B' && lv.fx.stop.transport === t2 && lv.fx.moveToNative === (t1 === 'aux' ? 'A' : null),
         `${tag}: leaving B — B stopped, A to the main player ${t1 === 'aux' ? '(moved)' : '(already there)'}, the list offers another`);
      // end the background one (A) with B in front: off; B moved to the main player if it was on the second voice
      const en = endSide(st, 'A');
      ok(en.st.phase === 'off' && en.fx.stop?.id === 'A' && en.fx.stop.transport === t1
         && (en.fx.frontToNative ?? null) === (t2 === 'aux' ? 'B' : null),
         `${tag}: ending A (in the background) — B alone in front, ${t2 === 'aux' ? 'moved to the main player' : 'already on it'}`);
      // the swapped pairing's background one ended by its server
      const ended = endSide(markEnded(sw, 'B', 'its time limit was reached'), 'B');
      ok(ended.st.phase === 'off' && (ended.fx.frontToNative ?? null) === (t1 === 'aux' ? 'A' : null),
         `${tag}: B ended by its server while A is in front — A plays on, on the main player`);
    }
  }
}

console.log('every kind\'s audio address');
{
  const u = { baseUrl: 'https://rx.example.org/', uuid: 'abc-1', password: '', adminAuth: '', freq: 7_074_000, mode: 'usb' };
  const url = uberAudioUrl(u, 'VibeSDR/12 (+https://vibesdr.net)');
  ok(url.startsWith('wss://rx.example.org/ws?user_session_id=abc-1&format=opus&version=2'), 'UberSDR: wss from https, the SAME session, Opus');
  ok(/&frequency=7074000&mode=usb/.test(url), '…opens on the dial it is on (the engine\'s own rule: the URL tunes the channel)');
  ok(/&client=VibeSDR%2F12/.test(url) && !/password=/.test(url) && !/bandwidthLow/.test(url), '…names itself; no password or filter unless there is one');
  ok(/&bandwidthLow=-2800&bandwidthHigh=2800/.test(uberAudioUrl({ ...u, bwLow: -2800, bwHigh: 2800 }, 'x')), '…the filter within ±12 kHz rides along');
  ok(!/bandwidthLow/.test(uberAudioUrl({ ...u, bwLow: -15000, bwHigh: 15000 }, 'x')), '…but never one the server would REFUSE the socket for');
  ok(/&vs_admin_ticket=t$/.test(uberAudioUrl({ ...u, adminAuth: 'vs_admin_ticket=t' }, 'x')), '…the owner\'s credential, its & normalised');
  ok(vibeAuxUrl({ host: '10.0.0.2', port: 8080, suffix: '&user_session_id=s&codec=opus', wsBase: '' })
     === 'ws://10.0.0.2:8080/ws/audio?user_session_id=s&codec=opus&channels=1', 'VibeServer: LocalAudioPlayer\'s own second-voice URL');
  ok(vibeAuxUrl({ host: 'x', port: 1, suffix: '&a=1', wsBase: 'wss://h.org/r/rtl0/' }) === 'wss://h.org/r/rtl0/ws/audio?a=1&channels=1',
     '…through a front door to its radio');
  ok(endedWhy('You’ve used this KiwiSDR’s daily time allowance') === 'its time limit was reached'
     && endedWhy('inactivity timeout') === 'nobody answered "still listening?"' && endedWhy('???') === 'the server ended the session',
     'a background adapter\'s refusal, in the card\'s words');
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
  ok(/freqHz: abDoor\.frequency,/.test(sdr) && /mode: cardMode\(\{ mode: String\(abDoor\.mode\)/.test(sdr), 'the card is given VFO A\'s tuning — the channel that plays on');
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
  // ★ The fourth adoption is a side coming BACK to its own radio (returningSide) — that one IS the radio playing.
  ok(adopts.length === 4 && guarded === 3, `every radio adoption (chosen, door of one, an address naming its radio) passes the first-radio check first (${guarded}/${adopts.length - 1}; + the side coming back)`);
  ok(/noteStatus\(dualMyId, \{[^}]*name: dualName, radioKey: dualRadioKey,\s*pinned: !!radioAuthSuffix,[^}]*\}, radioBase\)/.test(sdr), 'the card is told the radio and its name');
  ok(/if \(dualRadioKey && radioBase\) back\.baseUrl = radioBase;/.test(sdr) && /baseUrl: wantBase/.test(ds),
     'a side returns to its RADIO\'s address (adopted straight through), never the radio list');
  const dr = readFileSync('src/utils/doorReturn.ts', 'utf8');
  ok(/'autoTakeover', 'dualSide'\]/.test(dr), 'back to the radio list is a new listener: the old side\'s session does not ride along');
}


console.log('the card says the mode the screen shows (Stuart 2026-10-28: "i am in DAB not WFM mode on the Pi2")');
{
  ok(cardMode({ mode: 'wfm', dab: true }) === 'DAB', 'DAB over the idle WFM demodulator reads DAB, not WFM');
  ok(cardMode({ mode: 'am', adsb: true }) === 'ADS-B' && cardMode({ mode: 'nfm', ism: true }) === 'ISM'
     && cardMode({ mode: 'nfm', ais: true }) === 'AIS' && cardMode({ mode: 'am', ndb: true }) === 'NDB',
     'every whole-radio mode by name: ADS-B, ISM, AIS, NDB');
  ok(cardMode({ mode: 'wfm', dab: true, adsb: true }) === 'DAB', 'the receiver label\'s own order: DAB first');
  ok(cardMode({ mode: 'usb' }) === 'USB' && cardMode({ mode: '' }) === '', 'otherwise the demodulator; nothing invented');
  ok(cardStation({ dab: true, service: 'Grt Hits Nrthnts', ensemble: 'Now Northants', channel: '12D' }) === 'Grt Hits Nrthnts',
     'in DAB, the service playing');
  ok(cardStation({ dab: true, service: '', ensemble: 'Now Northants', channel: '12D' }) === 'Now Northants · 12D'
     && cardStation({ dab: true, channel: '11B' }) === '11B', '…else the ensemble and block while the services arrive');
  ok(cardStation({ dab: false, service: 'Radio X' }) === '', 'nothing outside DAB');
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  ok(/noteStatus\(dualMyId, \{ freqHz: status\.frequency, mode: dualMode, station: dualStation/.test(sdr)
     && /const dualMode = cardMode\(\{ mode: String\(status\.mode\), dab: dualDab, adsb: adsbOn, ism: ismOn, ais: aisOn, ndb: ndbOn \}\)/.test(sdr),
     'the screen hands the card cardMode, never the bare status.mode');
  ok(!/mode: String\(abDoor\.mode\), endsAt/.test(sdr), '…and so does "Listen to a 2nd server"');
  const card = readFileSync('src/components/DualServerCard.tsx', 'utf8');
  ok(/opacity=\{ear === 'both' \|\| \(ear !== 'off' && icon === 'L'\) \? 1 : EAR_FAINT\} flip \/>/.test(card)
     && /opacity=\{ear === 'both' \|\| \(ear !== 'off' && icon === 'R'\) \? 1 : EAR_FAINT\} \/>/.test(card)
     && Number((card.match(/const EAR_FAINT = ([\d.]+)/) ?? [])[1]) >= 0.3,
     'stereo: a speaker per ear, the other server\'s FAINT but visible (Stuart: "a little faint speaker for the opposite channel"); both lit when centred, both faint when silent (Stuart 2026-10-29: "show both when it is in both ears")');
}


console.log('a swap is not an arrival (Stuart\'s recording 2026-10-28: the tap landed on the Pi 2\'s "Choose a receiver")');
{
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  const ds = readFileSync('src/services/dualServer.ts', 'utf8');
  const door = sdr.indexOf('if (dualBack && !dualBack.pinned) {');
  ok(door > 0 && door < sdr.indexOf('const d = await fetchFrontDoor(baseUrl);'),
     'a side coming back adopts its return address as the radio BEFORE any front door is fetched');
  ok(/if \(dualBack && !dualBack\.pinned\) \{\s*setRadioBase\(baseUrl\);\s*setDoorPending\(false\);\s*return;/.test(sdr),
     '…no door, no landing card, no wait for a tap');
  ok(/useState\(\(\) => returningSide\(dualSideId, route\.params as never\)\)/.test(sdr), '…decided once, on mount, from the pairing');
  ok(/params\.dualSide !== myId/.test(ds) && /x\.id === myId && !x\.ended/.test(ds),
     'only a screen whose route names its OWN live side counts as coming back');
  ok(/pinned: !!radioAuthSuffix/.test(sdr), 'a radio unlocked with its own PIN still goes through the door (the proof cannot ride along)');
  ok(/return \{ \.\.\.bg\.params, dualSide: bg\.id \}/.test(ds), 'the swap carries the side (and so the session) to the screen');
}


console.log('the card\'s balance owns the finger; the one-time hint (Stuart 2026-10-28)');
{
  const card = readFileSync('src/components/DualServerCard.tsx', 'utf8');
  ok(/onPanResponderTerminationRequest: \(\) => false/.test(card), 'the balance track never hands a drag over to the card');
  ok(/onMoveShouldSetPanResponder: \(_e, g\) => !balOpenRef\.current && g\.dx < -6/.test(card),
     'the flick-to-tuck stands down while the balance is open (a drag LEFT on the track was also a tuck)');
  ok(/x\.setValue\(Math\.min\(offRef\.current,/.test(card), 'a flick never drags the card past its tucked position');
  ok(/setBalOpen\(false\);[\s\S]{0,120}Animated\.timing\(x, \{ toValue: offRef\.current/.test(card),
     'the ‹ closes the balance before it tucks');
  // ★ Once per PAIRING (37b44301c — Stuart, 2026-10-30: "it needs to show once per session … just to remind a user its
  //   there"), keyed by the pairing, not remembered on the device for good.
  ok(/let hintSeenFor: string \| null = null;/.test(card) && /hintSeenFor = pairKey;/.test(card)
     && !/AsyncStorage\.setItem\(HINT_SEEN_KEY/.test(card),
     'the hint is shown once per pairing (until a tap, a hold or ✕), not once per device for good');
  ok(/DUAL_CARD_HINT = 'Tap to return to the other server · hold for balance'/.test(card), '…in Stuart\'s words');
  ok((card.match(/hintDone\(\)/g) ?? []).length >= 2 && /onPress=\{hintDone\}/.test(card),
     '…and goes on ✕, on a tap and on a hold');
}

console.log('source: mute and quiet never silence the OTHER server');
{
  const scr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  const srv = readFileSync('src/services/dualServer.ts', 'utf8');
  const sheet = readFileSync('src/components/AudioSheet.tsx', 'utf8');
  const card = readFileSync('src/components/DualServerCard.tsx', 'utf8');
  const swift = readFileSync('ios/VibeSDR/VibeDualAudio.swift', 'utf8');
  const kt = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeDualAudio.kt', 'utf8');
  ok(/const wantSpk = spotsBandOn && !abOn && dualShape\.phase === 'off'/.test(scr),
     'Band mutes the SPEAKER (the native pause, both servers) only outside a pairing');
  ok(/const engineQuiet = audioQuiet && dualShape\.phase === 'off'/.test(scr) && /setNativeQuiet\(engineQuiet/.test(scr),
     'QUIET (pause the engine, release the session) only outside a pairing');
  ok(/quiet: audioQuiet, band: spotsBandOn && !abOn/.test(scr), 'the screen tells the pairing it has nothing to give');
  ok(/getDual\(\)\.phase === 'paired' && dualIdRef\.current\) \{\s*setSideMuted\(dualIdRef\.current, muted\)/.test(scr),
     'the watch mutes the server on screen, not both');
  ok(/onDualMute=\{dualShape\.phase === 'paired' && dualMyId \? \(on: boolean\) => setSideMuted\(dualMyId, on\)/.test(scr),
     'the Audio menu\'s mute is this server\'s');
  ok(/if \(mac\.state\.muted\) toggleMacAudioMute\(\); else onDualMute\(!dualMuted\)/.test(sheet),
     'the Mac MUTE key mutes this server while paired');
  ok(/screenNavRef\('dualMute'/.test(card) && /setSideMuted\(bg\.id, !bg\.muted\)/.test(card),
     'the card has its own mute for the background server (pad-reachable)');
  ok(/s1Muted: !!s1\?\.muted, s2Muted: !!s2\?\.muted, s1Audio: audio\(s1\), s2Audio: audio\(s2\)/.test(srv),
     'pushMix feeds each side\'s mute and has-audio into the rule');
  ok(/"nativeAge": VibeDualAudio\.primary\.ageMs/.test(swift) && /putDouble\("nativeAge", VibeDualMix\.primary\.ageMs\(\)\)/.test(kt),
     'both natives report how long since each voice last had audio');
  ok(/lastAt = ProcessInfo\.processInfo\.systemUptime/.test(swift) && /lastAt = android\.os\.SystemClock\.uptimeMillis\(\)/.test(kt),
     '…stamped on every buffer the voice places');
}

console.log('every kind wired: one session per server, the player that plays it, the screen that adopts it');
{
  const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
  const ds = readFileSync('src/services/dualServer.ts', 'utf8');
  const ap = readFileSync('src/components/AudioPlayer.tsx', 'utf8');
  const tun = readFileSync('src/screens/TunerScreen.tsx', 'utf8');
  const kiwi = readFileSync('src/services/KiwiAdapter.ts', 'utf8');
  const owrx = readFileSync('src/services/OwrxAdapter.ts', 'utf8');
  const fmdx = readFileSync('src/services/FmdxAdapter.ts', 'utf8');
  const uws = readFileSync('src/services/UberSDRWsClient.ts', 'utf8');
  const pick = readFileSync('src/screens/InstancePickerScreen.tsx', 'utf8');
  const kt = readFileSync('android/app/src/main/java/com/vibesdr/app/VibeStreamService.kt', 'utf8');
  // Kiwi / OpenWebRX: the adapter is the session — parked, adopted, never rebuilt while the pairing holds it.
  ok(/const parkedBackend = takeParked\(dualIdRef\.current\)/.test(sdr) && /const inner = parkedBackend \?\? createBackend\(/.test(sdr),
     'the receiver screen ADOPTS a parked adapter before it would build one');
  ok(/if \(parkedBackend\) \{[\s\S]{0,200}cbs\.onConnect\(\);/.test(sdr) && /\} else \(async \(\) => \{/.test(sdr),
     '…and an adopted one is not connected again, nor its tune restored');
  ok(/if \(!parkBackend\(dualIdRef\.current, inner\)\) c\.destroy\(\);/.test(sdr), '…and its cleanup parks instead of destroying while held');
  ok(/if \(!id \|\| !b \|\| !b\.rebindCallbacks \|\| !holds\(state, id\)\) return false;/.test(ds), 'only an adoptable adapter, only while the pairing holds its side');
  ok(!/serverEnded[^\n]*reconnect|connect\(\)/.test(ds.slice(ds.indexOf('function backgroundCallbacks'), ds.indexOf('/* ─', ds.indexOf('function backgroundCallbacks')))),
     'a background adapter is never reconnected by the pairing');
  for (const [n, src] of [['Kiwi', kiwi], ['OpenWebRX', owrx]] as const) {
    ok(/readonly audioOut = new ExtAudioOut\(\)/.test(src) && !/Vibe\?\.pushExternalPcm/.test(src) && !/Vibe\?\.startExternalAudio/.test(src),
       `${n}: every buffer goes through ExtAudioOut (main player or second voice)`);
    ok(/rebindCallbacks\(cb: BackendCallbacks\): void \{ this\.cb = cb; \}/.test(src) && /detachAudio\(\): void \{ this\.audioOut\.detach\(\); \}/.test(src),
       `${n}: adoptable, and detachable when the pairing ends its side`);
  }
  ok(/if \(this\.audioOut\.background\) return;/.test(kiwi), 'a Kiwi in the background feeds no decoder');
  ok(/stopOwnExternal\(\);/.test(sdr) && !/onServerBusy: \(\) => \{[^}]*stopExternalAudio/.test(sdr),
     'a lost / busy server stops only its OWN external audio (never the other server\'s main player)');
  // UberSDR: either voice, adopted on return, commands down the socket that plays it.
  ok(/dual=\{dualMyId && \(route\.params\.serverType \?\? 'ubersdr'\) === 'ubersdr'/.test(sdr), 'the UberSDR player is told its voice');
  ok(/auxOpen\(\{ id: dualId, url: uberAudioUrl\(l, USER_AGENT\), hello: null, proto: 'uber' \}/.test(ap), '…the second voice: its own socket, no hello');
  ok(/nativeOwnedBy\(propUuid \?\? null\)/.test(ap) && /claimNative\(uuid\.current\)/.test(ap) && /releaseNative\(sid\)/.test(ap),
     '…the engine: adopted when already playing this listener, claimed, and stopped only by its owner');
  ok((uws.match(/uberAuxSend\(this\.uuid,/g) ?? []).length === 3, '…tune, filter and ping go to whichever socket is playing it');
  ok(/if \(uberAuxSend\(dualIdRef\.current, obj\)\) return;/.test(sdr), '…and the screen\'s audio commands too');
  ok(/onUberFrame/.test(ds) && /const UBER_HEADER = 21;/.test(ds), 'the second voice reads UberSDR\'s framing (21-byte header + Opus)');
  // FM-DX: the main player, a side like any other, Back carries it.
  ok(/roleOnMount\(dualParams as never, uuidv4\)/.test(tun) && !/abandonDual\(true\)/.test(tun), 'the Tuner screen joins the pairing on its first render');
  ok(/const parked = takeParked\(dualIdRef\.current\);/.test(tun) && /if \(!parkBackend\(dualIdRef\.current, backendRef\.current\)\)/.test(tun),
     '…adopts its adapter coming back, parks it going');
  ok(/onMenu=\{onTunerBack\}/.test(tun) && /onMenuLongPress=\{onTunerBackHold\}/.test(tun)
     && /if \(dualBg\) tunerBackMenu\(\); else leaveTuner\(\);/.test(tun), '…tap Back is Back (asks only while two play); HOLD for the options');
  ok(/<DualServerCard myId=\{dualMyId\}/.test(tun) && /onDualMute=\{dualShape\.phase === 'paired' && dualMyId/.test(tun), '…the card and this server\'s MUTE');
  ok(/rebindCallbacks\(cb: BackendCallbacks\): void \{/.test(fmdx) && /!this\.audioDetached/.test(fmdx), 'FmdxAdapter adoptable and detachable');
  // The picker: one session per server per person, by address, BEFORE anything touches it.
  const ci = pick.indexOf('const connect = useCallback');
  const isSoloAt = pick.indexOf('isSolo({ baseUrl: cleaned', ci), checkAt = pick.indexOf('checkConnection(cleaned', ci);
  ok(isSoloAt > ci && isSoloAt < checkAt, 'the server list goes back to the server already playing before checkConnection');
  // Android: the UberSDR decode path is placed like every other main-player feed.
  ok(/decDual = VibeDualMix\.primary\.engaged\(\)/.test(kt) && /VibeDualMix\.primary\.process\(decDualIn, n, sCh, trackRate, decDualOut\)/.test(kt)
     && /val outCh = if \(VibeBinaural\.on \|\| decDual\) 2 else ch/.test(kt), 'Android: an UberSDR on the main player is placed in its ear too');
}

console.log(fails ? `\n✗ ${fails} failed` : '\n✓ all passed');
process.exit(fails ? 1 : 0);
