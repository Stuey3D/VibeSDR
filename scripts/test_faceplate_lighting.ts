/**
 * Faceplate LIGHTING and VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md) — the pure rules:
 *   §1 filament wires: count by HEIGHT, even spacing, snapped to the device-pixel grid;
 *   §2 the one light angle: 104° (LEFT) reproduces today's numbers exactly — sheen, gloss, hot-spot, screws;
 *   §3 MOTION EFFECTS: follows the OS until picked, then the pick wins both ways;
 *   §4 LIGHT ANGLE: the five keys, and the row shown only where it is the light in use.
 *
 * Run: node --no-warnings scripts/test_faceplate_lighting.ts   (run-tests.sh does)
 */
import { filamentCount, filamentYs, devicePixel } from '../src/constants/vfdGlass.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (a: number, b: number, tol = 1e-9) => Math.abs(a - b) <= tol;

// ── §1 Filament wires ────────────────────────────────────────────────────────
eq('today\'s windows get 2 wires (40 pt)', filamentCount(40), 2);
eq('a 20 pt strip still gets 2', filamentCount(20), 2);
eq('by HEIGHT: 66 pt → 3', filamentCount(66), 3);
eq('nothing measured → no wires', filamentCount(0), 0);
eq('NaN height → no wires', filamentYs(NaN, 3), []);
for (const pr of [1, 2, 3, 2.625]) {
  for (const h of [19, 20, 29, 35, 38, 40, 46, 48, 66]) {
    const ys = filamentYs(h, pr);
    ok(`h ${h} @${pr}x: count ${ys.length} = filamentCount`, ys.length === filamentCount(h));
    ok(`h ${h} @${pr}x: every wire on a device pixel (no sub-pixel smear)`,
       ys.every(y => near(y * pr, Math.round(y * pr), 1e-6)));
    ok(`h ${h} @${pr}x: inside the glass, ascending`, ys.every((y, i) => y > 0 && y < h && (i === 0 || y > ys[i - 1])));
    ok(`h ${h} @${pr}x: within half a device pixel of i/(n+1)`,
       ys.every((y, i) => Math.abs(y - ((i + 1) * h) / (ys.length + 1)) <= 0.5 / pr + 1e-9));
  }
}
eq('40 pt @3x: ⅓ and ⅔, snapped', filamentYs(40, 3), [13.333333333333334, 26.666666666666668]);
eq('one device pixel @3x', devicePixel(3), 1 / 3);
eq('one device pixel, bad ratio → 1 pt', devicePixel(0), 1);

console.log(`faceplate lighting: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
