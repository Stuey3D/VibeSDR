// The gamepad layer's pure parts (src/utils/gamepadNav.ts) — Stuart's mapping, 2026-10-10.
import { pickNext, Jog, padProfile, type Box } from '../src/utils/gamepadNav';

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
const s = new Jog(30); sum = 0;
for (let d = 0; d <= 50; d += 5) sum += s.feed(...at(d));
s.feed(0, 0);                              // released
for (let d = 200; d <= 215; d += 5) sum += s.feed(...at(d));
ok(sum === 1, 'letting go resets: picking the stick up elsewhere does not count the jump');

ok(padProfile(4) === 'sticks' && padProfile(2) === 'buttons' && padProfile(0) === 'buttons', 'two sticks → stick profile; none → by position');
ok(padProfile(4, 'buttons') === 'buttons', 'an explicit choice wins (test the no-stick mapping on a PS pad)');

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
