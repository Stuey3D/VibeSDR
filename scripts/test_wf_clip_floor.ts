/**
 * AN ICOM'S CLIPPED SCOPE FLOOR IS NOT A NOISE FLOOR (src/assets/signalProcessor.ts clipFloorDb; Stuart 2026-11-04, IC-7300 on
 * 41 m: "the auto contrast isn't working" — the floor flat on the scale's bottom, the waterfall black). The radio sends levels
 * only inside its REF window; below REF a point is 0, which the server writes as exactly −130 dB (icom_scope.h kScopeFloorDb).
 *   • clipFloorDb null (every other radio): the output is BYTE-IDENTICAL to a processor that never heard of the setting;
 *   • clipFloorDb −130 on a frame that is mostly clip: the display floor rises off the clip to the real levels above it
 *     (the colour range spans what the radio actually measured, not the clip);
 *   • a frame with no clip at all: the setting changes nothing.
 * Run: node --no-warnings scripts/test_wf_clip_floor.ts
 */
// ★ A fake monotonic clock, 250 ms a row (the IC-7300's ~4 sweeps/s): the processor's EMAs and windows read
//   performance.now(), so two runs over the same rows are only comparable on the same clock.
let T = 1000;
Object.defineProperty(globalThis, 'performance', { configurable: true, value: { now: () => T } });
const { SignalProcessor } = await import('../src/assets/signalProcessor.ts');

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const N = 1024;
let seed = 7;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 2 ** 32; };
/** A scope row: `clipFrac` of the bins at the clip (−130), the rest a floor near −100 with carriers. */
function row(clipFrac: number): Float32Array {
  const b = new Float32Array(N);
  for (let i = 0; i < N; i++) b[i] = rnd() < clipFrac ? -130 : -100 + rnd() * 6 + (i % 97 === 0 ? 30 : 0);
  return b;
}
type SP = InstanceType<typeof SignalProcessor>;
const run = (p: SP, rows: Float32Array[]) => { T = 1000; return rows.map((r) => { T += 250; const f = p.process(r, 7.25e6, 50e3); return { row: Array.from(f.row), min: f.dbMin, max: f.dbMax }; }); };

seed = 7; const rowsA = Array.from({ length: 40 }, () => row(0.6));
seed = 7; const rowsB = Array.from({ length: 40 }, () => row(0.6));
const plain = new SignalProcessor();
const nulled = new SignalProcessor(); nulled.applySettings({ clipFloorDb: null });
const a = run(plain, rowsA), b = run(nulled, rowsB);
ok(JSON.stringify(a) === JSON.stringify(b), 'clipFloorDb null: every row and range byte-identical to the default processor (the SDR path)');

seed = 7; const rowsC = Array.from({ length: 40 }, () => row(0.6));
const icom = new SignalProcessor(); icom.applySettings({ clipFloorDb: -130 });
const c = run(icom, rowsC);
const lastA = a[a.length - 1], lastC = c[c.length - 1];
ok(lastA.min <= -129.5, `without it, 60 % clip drags the floor onto the clip (${lastA.min.toFixed(1)} dB)`);
ok(lastC.min > -115 && lastC.min < -95, `with it, the floor sits on the real levels above the clip (${lastC.min.toFixed(1)} dB)`);
ok(lastC.max - lastC.min >= 30 - 0.01, `and the window keeps its minimum 30 dB (${(lastC.max - lastC.min).toFixed(1)} dB)`);

seed = 11; const rowsD = Array.from({ length: 20 }, () => row(0));
seed = 11; const rowsE = Array.from({ length: 20 }, () => row(0));
const d0 = run(new SignalProcessor(), rowsD);
const e = new SignalProcessor(); e.applySettings({ clipFloorDb: -130 });
const e0 = run(e, rowsE);
ok(JSON.stringify(d0) === JSON.stringify(e0), 'no clip in the frame: the setting changes nothing');

console.log(`clip floor: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
