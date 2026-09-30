/**
 * Signal-meter rules (src/constants/meters.ts) — one deck height, the LED table and the squelch ring,
 * the edge LED's partial brightness (Φ, the σ window, the eye filter, steady-LED hysteresis) and the
 * edgewise needles' ballistics.
 *
 * Brief: docs/BRIEF-faceplates.md §4.1–§4.5, acceptance §13.5 / §13.7 / §13.11.
 *
 * Run: node --no-warnings scripts/test_faceplate_meters.ts   (run-tests.sh does)
 */
import {
  portraitDeck, compactKeyHitSlop, sqlClosedOf, type MeterKind,
  VU_SEGMENTS, VU_LABELS, VU_THRESHOLDS, LED_SPEC, RING_OPEN, RING_CLOSED, ledColourOf, ringSegment, vuPos, peakStep,
  phi, edgeBrightness, segmentTarget, makeWindow, pushSample, eyeStep, steadyLit,
  scalePointX, needleX, needleSpring, peakNeedleStep,
} from '../src/constants/meters.ts';
import { nixieGeometry, nixieSpec, stackHeight, TUBE_DESIGN, PIP_H } from '../src/constants/nixie.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (what: string, got: number, want: number, tol: number) =>
  ok(`${what} (got ${got.toFixed(4)}, want ${want} ± ${tol})`, Math.abs(got - want) <= tol);

// ── §4.1 ONE DECK HEIGHT ─────────────────────────────────────────────────────
const METERS: MeterKind[] = ['bar', 'vu', 'edge'];
for (const scale of [0.82, 1, 1.1, 1.45]) {
  const r = (n: number) => Math.round(n * scale);
  const rowGap = r(7);
  for (const tablet of [false, true]) {
    // ★★★ Silver / black: every meter × shared is ONE height.
    const metal = new Set<number>();
    for (const meter of METERS) for (const shared of [false, true]) {
      const d = portraitDeck({ cap: true, meter, shared, tablet, rowGap, r });
      metal.add(d.blockH);
      eq(`metal ${meter} shared=${shared} @${scale}: block = display + gap + keys`, d.displayH + rowGap + d.keySlot, d.blockH);
      if (d.compact) {
        eq(`metal ${meter} shared=${shared} @${scale}: the column adds up`,
           d.bannerH + d.bannerGap + d.freqH + d.meterGap + d.housingH, d.displayH);
        ok(`metal ${meter} shared=${shared} @${scale}: the window is never squeezed away`, d.freqH >= r(30));
      }
    }
    eq(`metal @${scale} tablet=${tablet}: one deck height across meter × shared`, metal.size, 1);
    // Default chassis: LED / analogue one height; the bar is TODAY's (§4.2 / §13.1).
    const def = new Set<number>();
    for (const meter of ['vu', 'edge'] as const) for (const shared of [false, true])
      def.add(portraitDeck({ cap: false, meter, shared, tablet, rowGap, r }).blockH);
    eq(`default @${scale} tablet=${tablet}: LED / analogue one height across meter × shared`, def.size, 1);
    const bar = portraitDeck({ cap: false, meter: 'bar', shared: false, tablet, rowGap, r });
    eq(`default bar @${scale}: today's bar`, bar.displayH, r(tablet ? 62 : 40));
    eq(`default bar @${scale}: today's key`, bar.keySlot, r(44));
    eq(`default bar shared @${scale}: the banner is inside the bar, no change`,
       portraitDeck({ cap: false, meter: 'bar', shared: true, tablet, rowGap, r }).blockH, bar.blockH);
  }
}
// The §4.1 table falls OUT of the fixed block at scale 1 — it is not typed in anywhere.
{
  const r = (n: number) => n, rowGap = 7;
  const d = (meter: MeterKind, shared: boolean) => portraitDeck({ cap: true, meter, shared, tablet: false, rowGap, r });
  eq('§4.1 LED VU window 48', d('vu', false).freqH, 48);
  eq('§4.1 LED VU shared window 38', d('vu', true).freqH, 38);
  eq('§4.1 analogue window 46', d('edge', false).freqH, 46);
  eq('§4.1 analogue shared window 35', d('edge', true).freqH, 35);
  eq('§4.1 keys 44 / shared 34', [d('vu', false).keySlot, d('vu', true).keySlot, d('edge', true).keySlot], [44, 34, 34]);
  eq('§4.1 bar keys 58 on metal', d('bar', false).keySlot, 58);
  eq('metal bar frame = the mockup\'s 72', d('bar', true).displayH, 72);
  eq('§4.5 analogue housing 34 (28 window)', d('edge', false).housingH, 34);
  eq('legends 88 % / shared 80 %', [d('vu', false).legendScale, d('vu', true).legendScale], [0.88, 0.8]);
  eq('the bar\'s legends are untouched', d('bar', false).legendScale, 1);
}
// ★ §4.1 TRAP: 34 pt keys reach a 44 pt target through the gaps, without overlapping a neighbour.
{
  const h = compactKeyHitSlop(34, 7, 8);
  ok('34 pt key + slop reaches 44 pt', h.reach >= 44);
  ok('side slop stays inside half the column gap', h.left <= 4 && h.right <= 4);
  ok('downward slop stays inside half the row gap (the drums are below)', h.bottom <= 3.5);
}
// The one SQL rule the bar, the mode box and the meters share.
eq('squelch off (−1): never closed', sqlClosedOf(-1, true, 0), false);
eq('gate verdict wins over geometry', sqlClosedOf(0.5, false, 0.1), false);
eq('no verdict: below the line = closed', sqlClosedOf(0.5, undefined, 0.4), true);
eq('no verdict: above the line = open', sqlClosedOf(0.5, undefined, 0.6), false);

// ── §4.3 THE LED TABLE ───────────────────────────────────────────────────────
eq('5 green / 3 orange / 2 red', Array.from({ length: VU_SEGMENTS }, (_, i) => ledColourOf(i)).join(','),
   'green,green,green,green,green,orange,orange,orange,red,red');
eq('labels, S9 top of the green', [VU_LABELS.length, VU_LABELS[4], ledColourOf(4), ledColourOf(5)], [10, 'S9', 'green', 'orange']);
eq('LED colours are the brief\'s, verbatim (green)', Object.values(LED_SPEC.green).slice(0, 6),
   ['#e9ffe9', '#7dff9c', '#22d24e', '#0c7a26', '#1d3a23', '#0b170e']);
eq('LED colours (orange)', Object.values(LED_SPEC.orange).slice(0, 6),
   ['#fff3dc', '#ffc36b', '#ff8a12', '#a34a05', '#3d2811', '#170f06']);
eq('LED colours (red)', Object.values(LED_SPEC.red).slice(0, 6),
   ['#fff0ee', '#ff8a80', '#f2231a', '#8d0c07', '#3e1613', '#180807']);
eq('ring green open / red closed', [RING_OPEN, RING_CLOSED], ['#3dff72', '#ff3a2e']);
// ★★ The ring and the segments read ONE table: the mockup's bar line for a ring on `sq` is (sq + .5)×10 %.
for (let sq = 0; sq < 10; sq++) eq(`mockup: bar line at ${(sq + 0.5) * 10}% → ring on segment ${sq}`, ringSegment((sq + 0.5) / 10), sq);
eq('squelch off (−1): no ring', ringSegment(-1), -1);
eq('squelch absent: no ring', ringSegment(undefined), -1);
eq('squelch at the very top: the last LED', ringSegment(1), 9);
eq('squelch at the very bottom: the first LED', ringSegment(0), 0);
// ★★★ §4.3 TRAP, as a property: AT the level where the gate opens (level == sql), the ringed LED is the
//     one on the edge — every LED below it is past its threshold, none above it is. Any table.
for (const table of [VU_THRESHOLDS, [0.2, 1, 2.1, 3.5, 4.4, 5.9, 6.5, 7.2, 8.8, 9.6]]) {
  for (let k = 0; k <= 1000; k++) {
    const sql = k / 1000, p = vuPos(sql), ring = ringSegment(sql, table);
    const below = table.slice(0, ring).every(T => T <= p + 1e-9);
    const above = table.slice(ring + 1).every(T => T >= p - 1e-9);
    if (!below || !above) { eq(`ring on the edge LED at sql ${sql} (table ${table === VU_THRESHOLDS ? 'uniform' : 'uneven'})`, ring, 'the edge'); break; }
  }
  passes++;
}
// Peak hold: one segment above the level, ~1 s, then back to the level.
{
  const p = { idx: -1, at: 0 };
  eq('peak rises with the level', peakStep(p, 6, 0), -1);
  eq('level falls: the peak holds one segment above', peakStep(p, 3, 500), 6);
  eq('…still held at 1 s', peakStep(p, 3, 1000), 6);
  eq('…drops after ~1 s', peakStep(p, 3, 1001), -1);
  eq('a new high is caught at once', peakStep(p, 8, 1100), -1);
}

// ── §4.4 THE EDGE LED ────────────────────────────────────────────────────────
// Φ against known values.
near('Φ(0) = 0.5', phi(0), 0.5, 1e-7);
near('Φ(1) = 0.8413', phi(1), 0.841345, 1e-6);
near('Φ(−1.96) = 0.025', phi(-1.96), 0.024998, 1e-6);
near('Φ(3) = 0.99865', phi(3), 0.998650, 1e-6);
ok('Φ is monotone', Array.from({ length: 200 }, (_, k) => phi(-5 + k * 0.05)).every((v, k, a) => k === 0 || v >= a[k - 1]));
// σ floor: a steady carrier is crisp, never a hard step.
near('σ floor: 1.5 dB above T is Φ(1)', edgeBrightness(11.5, 10, 0), phi(1), 1e-9);
near('σ = 6 dB: 6 dB above T is Φ(1)', edgeBrightness(16, 10, 6), phi(1), 1e-9);
// ★ §4.4 TRAP: no linear ramp — a STEADY carrier halfway between two thresholds lights the lower
//   one fully and the upper one not at all (a ramp would sit both half-lit for ever).
{
  const mu = 4.0;                                     // halfway between T3 = 3.5 and T4 = 4.5 (segments)
  ok('steady carrier: the lower LED is fully lit', segmentTarget(3, mu, 0, false, false, false) > 0.99);
  ok('steady carrier: the upper LED is dark', segmentTarget(4, mu, 0, false, false, false) < 0.01);
  // fading HF (σ ≈ 6 dB ≈ 0.67 segment): a soft, wide edge
  const b3 = segmentTarget(3, mu, 0.67, false, false, false), b4 = segmentTarget(4, mu, 0.67, false, false, false);
  ok(`fading HF: both edge LEDs partly lit (${b3.toFixed(2)}, ${b4.toFixed(2)})`, b3 > 0.6 && b3 < 0.95 && b4 > 0.05 && b4 < 0.4);
  near('at the threshold, exactly half', segmentTarget(4, 4.5, 0.5, false, false, false), 0.5, 1e-9);
}
// While the squelch mutes: the plain threshold — no σ shimmer under the red ring.
eq('muting: no partial brightness', [segmentTarget(4, 4.4, 1, false, true, false), segmentTarget(4, 4.6, 1, false, true, false)], [0, 1]);
// The σ window: ~0.5 s, population std-dev.
{
  const w = makeWindow();
  pushSample(w, 0, 0);
  const sd1 = pushSample(w, 100, 2);
  near('σ of {0, 2} = 1', sd1, 1, 1e-9);
  pushSample(w, 200, 0); pushSample(w, 300, 2);
  const sd2 = pushSample(w, 900, 5);                   // everything older than 500 ms is gone
  eq('the window forgets after ~0.5 s', w.v, [5]);
  eq('one sample: σ 0 (the floor takes over)', sd2, 0);
  const w2 = makeWindow();
  for (let t = 0; t <= 2000; t += 200) pushSample(w2, t, 3);
  eq('steady level: σ 0', pushSample(w2, 2200, 3), 0);
}
// The eye filter: τ ≈ 100 ms, ≤ 0.35 per frame, the ONLY easing.
{
  near('one τ closes 63 %', eyeStep(0, 0.1, 100), 0.1 * (1 - Math.exp(-1)), 1e-9);
  eq('a big step is limited to 0.35 per frame', eyeStep(0, 1, 100), 0.35);
  eq('…and downward', eyeStep(1, 0, 100), 0.65);
  // ★★★ NO FLICKER at 5 fps on fading HF: targets that jump every 200 ms must GLIDE — at 60 fps the
  //     brightness never changes by more than the limit in a frame, and never reverses direction
  //     between two updates.
  let b = 0, worst = 0, reversals = 0;
  const targets = [0.9, 0.2, 0.8, 0.1, 0.95, 0.3, 0.7];
  for (const tgt of targets) {
    let dirSeen = 0;
    for (let f = 0; f < 12; f++) {                   // 12 frames at 60 fps = one 5 fps update
      const n = eyeStep(b, tgt, 1000 / 60);
      worst = Math.max(worst, Math.abs(n - b));
      const dir = Math.sign(n - b);
      if (dirSeen && dir && dir !== dirSeen) reversals++;
      if (dir) dirSeen = dir;
      b = n;
    }
  }
  ok(`60 fps: never more than 0.35 in a frame (worst ${worst.toFixed(3)})`, worst <= 0.35 + 1e-12);
  eq('no reversal within an update (a steady glide, never a pulse)', reversals, 0);
  ok('a stalled frame is not one giant jump', eyeStep(0, 1, 5000) <= 0.35);
}
// Steady LEDs: solid on / off with ~1 dB of hysteresis.
{
  eq('off → needs T + ½ dB', [steadyLit(false, 10.4, 10), steadyLit(false, 10.6, 10)], [false, true]);
  eq('on → holds to T − ½ dB', [steadyLit(true, 9.6, 10), steadyLit(true, 9.4, 10)], [true, false]);
  // A level dithering ±0.3 dB round T never toggles the LED.
  let lit = false, toggles = 0;
  for (let k = 0; k < 200; k++) {
    const n = steadyLit(lit, 10 + 0.3 * Math.sin(k), 10);
    if (n !== lit) toggles++;
    lit = n;
  }
  eq('±0.3 dB dither round T: no toggling', toggles, 0);
  eq('steady target is 0 / 1 only', [segmentTarget(4, 4.53, 2, true, false, false), segmentTarget(4, 4.6, 2, true, false, false)], [0, 1]);
}

// ── §4.5 THE EDGEWISE NEEDLES ────────────────────────────────────────────────
// Scale points at 8 + (i + 0.5) × (w − 16) / 10 — the mockup's portrait card (328 wide).
eq('scale points (mockup 328 pt card)', Array.from({ length: 10 }, (_, i) => +scalePointX(i, 328).toFixed(1)),
   [23.6, 54.8, 86, 117.2, 148.4, 179.6, 210.8, 242, 273.2, 304.4]);
// ★ The needle, the ring and the LEDs share ONE table: position T_i lands exactly on scale point i.
for (let i = 0; i < 10; i++) near(`needle at threshold ${i} sits on its label`, needleX(VU_THRESHOLDS[i], 328), scalePointX(i, 328), 1e-9);
eq('needle pinned inside the scale', [needleX(-3, 328), needleX(14, 328)], [8, 320]);
// Ballistics: simulate the spring the way Reanimated integrates it (semi-implicit, 1 ms steps).
function step(sp: { mass: number; stiffness: number; damping: number }) {
  let x = 0, v = 0, t99 = -1, maxX = 0;
  for (let t = 1; t <= 1500; t++) {
    const a = (-sp.stiffness * (x - 1) - sp.damping * v) / sp.mass;
    v += a / 1000; x += v / 1000;
    maxX = Math.max(maxX, x);
    if (t99 < 0 && Math.abs(x - 1) <= 0.01) {
      // "99 %" = inside 1 % and staying there
      let stays = true, xx = x, vv = v;
      for (let u = t + 1; u <= 1500 && stays; u++) {
        const aa = (-sp.stiffness * (xx - 1) - sp.damping * vv) / sp.mass;
        vv += aa / 1000; xx += vv / 1000;
        if (Math.abs(xx - 1) > 0.01) stays = false;
      }
      if (stays) t99 = t;
    }
  }
  return { t99, over: maxX - 1 };
}
{
  const vu = step(needleSpring(false));
  ok(`VU ballistics: 99 % within 300 ms (${vu.t99} ms)`, vu.t99 > 0 && vu.t99 <= 300);
  ok(`VU ballistics: ~1 % overshoot (${(vu.over * 100).toFixed(2)} %)`, vu.over > 0.005 && vu.over < 0.02);
  const rm = step(needleSpring(true));
  ok(`Reduce Motion: still moving, 99 % within 300 ms (${rm.t99} ms)`, rm.t99 > 0 && rm.t99 <= 310);
  ok(`Reduce Motion: no overshoot (${(rm.over * 100).toFixed(3)} %)`, rm.over < 0.001);
}
// The peak needle: pushed by the needle's on-screen position, holds ~1 s, drifts ~6 dB/s easing in.
{
  const p = { pos: 0, heldMs: 0 };
  eq('pushed up by the needle', peakNeedleStep(p, 6, 16), 6);
  eq('never ahead of the needle it follows', peakNeedleStep(p, 6.5, 16), 6.5);
  let t = 0;
  for (; t < 990; t += 10) peakNeedleStep(p, 2, 10);
  eq('holds ~1 s after the needle falls away', p.pos, 6.5);
  const at = (ms: number) => { for (let k = 0; k < ms; k += 10) peakNeedleStep(p, 2, 10); return p.pos; };
  const a1 = at(100), a2 = at(100);
  ok('then drifts down, easing in (slow first)', 6.5 - a1 < a1 - a2);
  at(200);                                            // past the 300 ms ease-in
  const before = p.pos; at(1000);
  near('~6 dB/s once it has eased in (in segments: 6 / 9 per s)', before - p.pos, 6 / 9, 0.02);
  for (let k = 0; k < 3000; k++) peakNeedleStep(p, 2, 10);
  eq('…until it is caught by the needle again', p.pos, 2);
  eq('and it can never fall below the needle', peakNeedleStep(p, 3, 10), 3);
}

// ── §4.1 × §7: the LED / analogue windows on REAL TUBES — smallest case first ──
// ★★★ Analogue + shared = the 35 pt window against a 37 pt design stack: the tube shrinks, the window
//     does not, the domes are never clipped — at every portrait scale from the SE in Display Zoom
//     (320 pt) up, for every radio's fixed tube row, in the width the window really has (the deck
//     less its padding, the 70 pt mode box and the unit label's reserve).
for (const W of [320, 375, 390, 430]) {
  const scale = Math.max(0.75, Math.min(1.45, W / 390));
  const r = (n: number) => Math.round(n * scale);
  for (const meter of ['edge', 'vu'] as const) for (const shared of [true, false]) {
    const d = portraitDeck({ cap: true, meter, shared, tablet: false, rowGap: r(7), r });
    const winW = W - 2 * r(14) - r(70) - Math.round(r(11) * 2.6);
    const design = shared ? TUBE_DESIGN.meterShared : TUBE_DESIGN.meter;
    for (const layout of ['hf', 'wide', 'fm'] as const) {
      const g = nixieGeometry(winW, d.freqH, nixieSpec(layout), design, { bar: false, scale });
      const tag = `${W} pt ${meter}${shared ? '+shared' : ''} ${layout} (window ${d.freqH} pt)`;
      ok(`${tag}: the stack fits the window`, stackHeight(g.glassH, scale) <= d.freqH + 1e-9);
      ok(`${tag}: the dome's pip is inside the window`, g.collarY - g.glassH - PIP_H * scale >= -1e-9);
      ok(`${tag}: the glass never exceeds its design height`, g.glassH <= design.th * scale + 1e-9);
      const last = g.tubes[g.tubes.length - 1];
      ok(`${tag}: the tube row fits the width`, last.x + last.w <= winW + 1e-6);
    }
  }
}
{
  // The named smallest case at scale 1: 35 pt, and the glass really did give way.
  const d = portraitDeck({ cap: true, meter: 'edge', shared: true, tablet: false, rowGap: 7, r: (n: number) => n });
  const g = nixieGeometry(300, d.freqH, nixieSpec('hf'), TUBE_DESIGN.meterShared, { bar: false });
  eq('analogue + shared: 35 pt window', d.freqH, 35);
  ok(`analogue + shared: the glass shrank (${g.glassH} < ${TUBE_DESIGN.meterShared.th})`, g.glassH < TUBE_DESIGN.meterShared.th);
}

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate meters: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
