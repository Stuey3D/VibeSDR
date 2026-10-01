/**
 * Nixie tube rules (src/constants/nixie.ts) — the fixed tube layout, leading zeros, the lit bulb per
 * unit, the cathode stack, and "the tube shrinks, the window doesn't" (smallest window first).
 *
 * Brief: docs/BRIEF-faceplates.md §7 (Nixie tubes), acceptance §13.11.
 *
 * Run: node --no-warnings scripts/test_faceplate_nixie.ts   (run-tests.sh does)
 */
import {
  CATHODE_STACK, cathodeDepth, cathodeNeighbours, nixieSpec, nixieReadout, nixieGeometry,
  stackHeight, mhzDigitsFor, TUBE_DESIGN, PIP_H, COLLAR_H, CLEAR, MIN_GLASS,
} from '../src/constants/nixie.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const show = (r: { tubes: (string | null)[]; bulbs: boolean[] }, bulbAt: number[]) =>
  r.tubes.map((d, k) => (bulbAt.includes(k) ? (r.bulbs[bulbAt.indexOf(k)] ? '•' : '_') : '') + (d ?? ' ')).join('');

// ── ★★★ THE SMALLEST WINDOW FIRST: analogue + shared banner = 35 pt vs a 37 pt stack ──
{
  const d = TUBE_DESIGN.meterShared;
  const hf = nixieSpec('hf');
  const g = nixieGeometry(400, 35, hf, d, { bar: false });
  ok('35 pt window: the glass shrank below its design height', g.glassH < d.th + 2 && g.glassH <= 35 - 10);
  ok('35 pt window: pip + glass + collar + clearance fits', stackHeight(g.glassH) <= 35);
  ok('35 pt window: the pip is not clipped', g.collarY - g.glassH - PIP_H >= 0);
  // A design stack of 37 (glass 27) in a 35 window must lose exactly the 2 pt.
  const g37 = nixieGeometry(400, 35, hf, { tw: 18, th: 27, nf: 26 }, { bar: false });
  eq('37 pt design stack in a 35 pt window loses 2 pt of glass', g37.glassH, 25);
}
// Every window height from the bar's shared pill (≈ 20 pt) up: the stack never overflows.
for (let h = 18; h <= 64; h++) {
  for (const [name, d] of Object.entries(TUBE_DESIGN)) {
    for (const layout of ['hf', 'wide', 'fm'] as const) {
      const g = nixieGeometry(220, h, nixieSpec(layout), d, { bar: name.startsWith('bar') });
      ok(`h=${h} ${name} ${layout}: stack fits`, stackHeight(g.glassH) <= h + 1e-9);
      ok(`h=${h} ${name} ${layout}: glass capped at design`, g.glassH <= d.th + 1e-9);
      ok(`h=${h} ${name} ${layout}: digit fits the glass`, g.nf <= g.glassH * 0.8 + 1e-9);
      ok(`h=${h} ${name} ${layout}: bulb shorter than a tube`, g.bulbH < g.glassH);
    }
  }
}
// ★★★ The tubes STAND ON THE LINE below the window (Stuart, 2026-10-01): collar foot = h, at every
//     window height, every design, every layout, every scale — the clearance is all above the pip.
{
  const g = nixieGeometry(300, 48, nixieSpec('hf'), TUBE_DESIGN.meter, { bar: false });
  eq('collar stands on the window floor', g.collarY + COLLAR_H, 48);
  for (const sc of [0.58, 0.82, 1, 1.13, 1.45]) for (let h = 18; h <= 72; h++)
    for (const [name, d] of Object.entries(TUBE_DESIGN)) for (const layout of ['hf', 'wide', 'fm'] as const) {
      const gg = nixieGeometry(260, h, nixieSpec(layout), d, { bar: name.startsWith('bar'), scale: sc });
      ok(`h=${h} ${name} ${layout} @${sc}: collar foot on the floor`, Math.abs(gg.collarY + COLLAR_H * sc - h) < 1e-9);
      // ★★ `drop` is exactly how far the row moved from the brief's foot (h − CLEAR), so the wall's
      //    neon spill that NixieStatic moves by it stays where it was RELATIVE TO THE TUBES.
      ok(`h=${h} ${name} ${layout} @${sc}: drop = the row's move`,
         Math.abs((gg.collarY + COLLAR_H * sc) - gg.drop - (h - CLEAR * sc)) < 1e-9);
      // …and the clearance went to the top, not into a clipped dome.
      ok(`h=${h} ${name} ${layout} @${sc}: ≥ ${2 * CLEAR} pt over the pip or the glass is at its cap`,
         gg.collarY - gg.glassH - PIP_H * sc >= 2 * CLEAR * sc - 1e-9 || gg.glassH <= MIN_GLASS * sc + 1e-9);
    }
}

// ── Width: the group narrows to fit, the window never grows ──────────────────
{
  const d = TUBE_DESIGN.bar;
  const g = nixieGeometry(1000, 40, nixieSpec('hf'), d, { bar: true });
  eq('bar window: 16 pt digit tubes (§7 TRAP)', g.tubes[0].w, 16);
  eq('bar window: 1 pt gaps', g.gap, 1);
  const narrow = nixieGeometry(120, 40, nixieSpec('hf'), d, { bar: true });
  ok('a narrow window narrows the tubes', narrow.fit < 1 && narrow.tubes[0].w < 16);
  const last = narrow.tubes[narrow.tubes.length - 1];
  ok('…and the whole group fits inside it', last.x + last.w <= 120 + 1e-6 && narrow.tubes[0].x >= 0);
  const wide = nixieGeometry(1000, 60, nixieSpec('wide'), TUBE_DESIGN.meter, { bar: false });
  eq('10-tube radio: 8/10 of the width', +(wide.tubes[0].w).toFixed(2), +(22 * 0.8).toFixed(2));
  eq('10-tube radio: digits 0.9×', +(wide.nf).toFixed(2), +(30 * 0.9).toFixed(2));
  const w10b = nixieGeometry(1000, 60, nixieSpec('wide'), TUBE_DESIGN.bar, { bar: true });
  eq('10 tubes in the bar window narrow further by 8/10', +(w10b.tubes[0].w).toFixed(2), +(16 * 0.8).toFixed(2));
  const bulbW = nixieGeometry(1000, 60, nixieSpec('hf'), TUBE_DESIGN.meter, { bar: false });
  eq('bulb ≈ 0.45 × a digit tube', bulbW.bulbs[0].w, Math.round(22 * 0.45));
  eq('bulb glass ≈ 0.62 × a digit tube', +(bulbW.bulbH).toFixed(2), +(36 * 0.62).toFixed(2));
}

// ── ★★★ Fixed tube layout: units change only the lit bulb (§13.11) ──────────
{
  const hf = nixieSpec('hf');
  eq('hf: 8 digit tubes, 2 bulbs', [hf.mhzTubes + hf.fracTubes, hf.bulbAt.length], [8, 2]);
  const wide = nixieSpec('wide');
  eq('wide: 10 digit tubes', wide.mhzTubes + wide.fracTubes, 10);
  eq('kHz  → 14 230•000', show(nixieReadout(14_230_000, hf, 'khz'), hf.bulbAt), '14_230•000');
  eq('MHz  → 14•230 000', show(nixieReadout(14_230_000, hf, 'mhz'), hf.bulbAt), '14•230_000');
  eq('Hz   → 14 230 000, both bulbs off', show(nixieReadout(14_230_000, hf, 'hz'), hf.bulbAt), '14_230_000');
  // Geometry does not depend on the unit or the frequency at all — it takes neither.
  const g1 = nixieGeometry(260, 40, hf, TUBE_DESIGN.bar, { bar: true });
  const g2 = nixieGeometry(260, 40, hf, TUBE_DESIGN.bar, { bar: true });
  eq('geometry is a function of the window and the radio only', g1, g2);
  // Tuning 30 MHz → 10 kHz never changes the tube count.
  for (const hz of [30_000_000, 14_230_000, 7_074_000, 648_000, 10_000]) {
    for (const u of ['hz', 'khz', 'mhz'] as const) {
      eq(`${hz} ${u}: still 8 tubes`, nixieReadout(hz, hf, u).tubes.length, 8);
    }
  }
}

// ── Leading zeros are switched OFF; the digit left of a lit point lights ─────
{
  const hf = nixieSpec('hf');
  eq('648 kHz: leading zeros off', show(nixieReadout(648_000, hf, 'khz'), hf.bulbAt), '  _648•000');
  eq('0•648 000 MHz lights its 0', show(nixieReadout(648_000, hf, 'mhz'), hf.bulbAt), ' 0•648_000');
  eq('10 kHz', show(nixieReadout(10_000, hf, 'khz'), hf.bulbAt), '  _ 10•000');
  eq('10 kHz in MHz', show(nixieReadout(10_000, hf, 'mhz'), hf.bulbAt), ' 0•010_000');
  eq('648 kHz in Hz: no point, zeros off', show(nixieReadout(648_000, hf, 'hz'), hf.bulbAt), '  _648_000');
  eq('0 Hz shows one 0', show(nixieReadout(0, hf, 'hz'), hf.bulbAt), '  _   _  0');
  const fm = nixieSpec('fm');
  eq('FM tuner: " 96•600"', show(nixieReadout(96_600_000, fm, 'mhz'), fm.bulbAt), ' 96•600');
  eq('FM tuner: 105.4', show(nixieReadout(105_400_000, fm, 'khz'), fm.bulbAt), '105•400');
  const wide = nixieSpec('wide');
  eq('wide: 1090 MHz', show(nixieReadout(1_090_000_000, wide, 'mhz'), wide.bulbAt), '1090•000_000');
  eq('wide: 145.2625 MHz in kHz', show(nixieReadout(145_262_500, wide, 'khz'), wide.bulbAt), ' 145_262•500');
}
eq('mhzDigitsFor(145 MHz)', mhzDigitsFor(145_000_000), 3);
eq('mhzDigitsFor(648 kHz)', mhzDigitsFor(648_000), 1);

// ── Cathode stack ────────────────────────────────────────────────────────────
eq('stack order', CATHODE_STACK.join(' '), '1 6 2 7 5 0 4 9 8 3');
eq('a 1 sits at the front', cathodeDepth('1'), { scale: 1, dy: 0, opacity: 1 });
{
  const d3 = cathodeDepth('3');
  ok('a 3 sits smaller, lower and dimmer than a 1', d3.scale < 1 && d3.dy > 0 && d3.opacity < 1);
  eq('a 3 (index 9)', [+(d3.scale).toFixed(3), d3.dy, +(d3.opacity).toFixed(2)], [0.748, 2.25, 0.73]);
}
eq('neighbours of 5: 3 behind, 2 in front', cathodeNeighbours('5'), { back: ['0', '4', '9'], front: ['6', '2', '7'].slice(1) });
eq('neighbours of 1: nothing in front', cathodeNeighbours('1').front, []);
eq('neighbours of 3: nothing behind', cathodeNeighbours('3').back, []);
eq('a switched-off tube shows four bare cathodes', cathodeNeighbours(null), { back: ['1', '6', '2', '7'], front: [] });

console.log(`faceplate nixie: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
