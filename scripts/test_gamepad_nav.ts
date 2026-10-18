// The gamepad layer's pure parts (src/utils/gamepadNav.ts) — Stuart's mapping, 2026-10-10.
import { pickNext, Jog, jogDegPerStep, padProfile, type Box } from '../src/utils/gamepadNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const b = (x: number, y: number, w = 40, h = 20): Box => ({ x, y, w, h });

console.log('test_gamepad_nav');
// A row of three keys, and one below the middle.
const row = [b(0, 0), b(60, 0), b(120, 0), b(60, 50)];
ok(pickNext(null, row, 'right') === 0, 'nothing highlighted yet: the top-left control');
ok(pickNext(row[0], row, 'right') === 1 && pickNext(row[1], row, 'right') === 2, 'right walks along the row');
ok(pickNext(row[2], row, 'right') === -1, 'nothing further right: stays put');
ok(pickNext(row[1], row, 'down') === 3, 'down goes to the control below');
ok(pickNext(row[3], row, 'up') === 1, 'up comes back to the one above it, not a corner');
ok(pickNext(row[0], row, 'left') === -1, 'nothing to the left of the first');
// Straight ahead beats nearer-but-off-to-the-side.
const fan = [b(0, 100), b(300, 105), b(90, 190)];
ok(pickNext(fan[0], fan, 'right') === 1, 'right prefers the control in line over a nearer one well below');

// The jog wheel: clockwise = +, 30° a step, only while pushed out.
const j = new Jog(30);
const at = (deg: number, m = 1) => [Math.cos(deg * Math.PI / 180) * m, Math.sin(deg * Math.PI / 180) * m] as const;
let sum = 0;
for (let d = 0; d <= 90; d += 5) sum += j.feed(...at(d));
ok(sum === 3, 'a quarter turn clockwise = 3 steps forward (30° each)');
sum = 0;
for (let d = 90; d >= 0; d -= 5) sum += j.feed(...at(d));
ok(sum === -3, 'back the other way = 3 steps back');
const k = new Jog(30); sum = 0;
for (let d = 150; d <= 210; d += 5) sum += k.feed(...at(d));
ok(sum === 2, 'turning through west is smooth, not a 360° jump');
const r = new Jog(30); sum = 0;
for (let d = 0; d <= 180; d += 5) sum += r.feed(...at(d, 0.2));
ok(sum === 0, 'a stick barely pushed (resting drift) never turns the dial');

// ── speed decides the rate (Stuart, 2026-10-18: overshooting signals) ──
// Turn the stick from `from` to `to` degrees at `dps` °/s in 16 ms frames (a 60 Hz poll), out at magnitude `m`.
let clock = 0;
const spin = (jg: Jog, from: number, to: number, dps: number, m = 1) => {
  let n = 0; const dir = to > from ? 1 : -1; const per = dps * 0.016; let d = from;
  for (;;) { d = dir > 0 ? Math.min(to, d + per) : Math.max(to, d - per); clock += 16; n += jg.feed(...at(d, m), clock); if (d === to) break; }
  return n;
};
const slow = new Jog(30); slow.feed(...at(0), (clock += 16));
ok(spin(slow, 0, 90, 60) === 2, 'slow and deliberate (60 °/s): a quarter turn is 2 steps — 45° each, easy to land');
const norm = new Jog(30); norm.feed(...at(0), (clock += 16));
ok(spin(norm, 0, 90, 360) === 3, 'normal speed (360 °/s): a quarter turn is 3 steps — 30° each, as before');
const fast = new Jog(30); fast.feed(...at(0), (clock += 16));
const nf = spin(fast, 0, 720, 1800);
ok(nf >= 60, `a fast spin (5 turns a second): two turns cover ${nf} steps — not 24 — to cross a band`);
const ease = new Jog(30); ease.feed(...at(0), (clock += 16));
spin(ease, 0, 720, 1800);
const landing = spin(ease, 720, 810, 60);
ok(landing === 2, `easing off to land: the last slow quarter turn after a spin is ${landing} steps (2), not a spin's worth`);
const spring = new Jog(30); spring.feed(...at(0), (clock += 16));
spin(spring, 0, 60, 360);
let extra = 0;
// A real release: the stick is back at the centre within a few frames, its angle wobbling more the nearer it gets.
for (const [dd, m] of [[2, 0.9], [-15, 0.65], [25, 0.45], [-30, 0.38], [40, 0.36], [0, 0.1]]) { clock += 16; extra += spring.feed(...at(60 + dd, m), clock); }
ok(extra === 0, 'letting go: the spring back to the centre (wobbling as it goes) adds no steps');
const curve = [0, 120, 200, 300, 450, 900, 5000].map((v) => jogDegPerStep(30, v));
ok(curve.every((d, i) => i === 0 || d <= curve[i - 1]) && curve[0] === 45 && curve[3] === 30 && curve.at(-1) === 10,
   `the curve only ever gets faster with speed (${curve.map((d) => d.toFixed(1)).join(' ')} °/step)`);
const s = new Jog(30); sum = 0;
for (let d = 0; d <= 50; d += 5) sum += s.feed(...at(d));
s.feed(0, 0);                              // released
for (let d = 200; d <= 215; d += 5) sum += s.feed(...at(d));
ok(sum === 1, 'letting go resets: picking the stick up elsewhere does not count the jump');

ok(padProfile(4) === 'sticks' && padProfile(2) === 'buttons' && padProfile(0) === 'buttons', 'two sticks → stick profile; none → by position');
ok(padProfile(4, 'buttons') === 'buttons', 'an explicit choice wins (test the no-stick mapping on a PS pad)');

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
