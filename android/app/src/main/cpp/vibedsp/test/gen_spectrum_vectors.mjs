// gen_spectrum_vectors.mjs — drive the REAL src/assets/signalProcessor.ts through a scripted session and record
// every input and every output, for test_spectrum_processor.cpp to replay against the C++ port.
//   node gen_spectrum_vectors.mjs <out-dir>     (Node ≥ 23: runs the .ts directly)
// ★ Deterministic: a seeded PRNG and an injected clock (Date.now is replaced). Nothing here touches a network.
import { writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { SignalProcessor } = await import(join(here, '../../../../../../../src/assets/signalProcessor.ts'));
const outDir = process.argv[2] ?? here;

let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; };
const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return s - 3; };

let clock = 1_700_000_000_000;
Date.now = () => clock;

const inBuf = [], outBuf = [];
const push = (arr, b) => arr.push(Buffer.from(b));
const f64 = (v) => { const b = Buffer.alloc(8); b.writeDoubleLE(v); return b; };
const i32 = (v) => { const b = Buffer.alloc(4); b.writeInt32LE(v); return b; };
const u8 = (v) => Buffer.from([v ? 1 : 0]);

const p = new SignalProcessor();
// The C++ settings struct, field for field (manualRange flattened).
let cur = { autoContrast: 5, minRangeDb: 30, manual: false, manualMinDb: -120, manualMaxDb: -20, specFloor: 0,
            specPeakScale: 10, smoothingFrames: 5, avgFrames: 0, spatialSmooth: true, peakHold: true,
            wfBrightness: 0, wfContrast: 0, wfSharpness: 0 };
let manualObj = null;
function settings(patch) {
  const next = { ...cur, ...patch };
  const tsPatch = { ...patch };
  if ('manual' in patch || 'manualMinDb' in patch || 'manualMaxDb' in patch) {
    delete tsPatch.manual; delete tsPatch.manualMinDb; delete tsPatch.manualMaxDb;
    manualObj = next.manual ? { minDb: next.manualMinDb, maxDb: next.manualMaxDb } : null;
    tsPatch.manualRange = manualObj;
  }
  p.applySettings(tsPatch);
  cur = next;
  push(inBuf, [2]);
  for (const k of ['autoContrast', 'minRangeDb']) push(inBuf, f64(cur[k]));
  push(inBuf, u8(cur.manual)); push(inBuf, f64(cur.manualMinDb)); push(inBuf, f64(cur.manualMaxDb));
  for (const k of ['specFloor', 'specPeakScale', 'smoothingFrames', 'avgFrames']) push(inBuf, f64(cur[k]));
  push(inBuf, u8(cur.spatialSmooth)); push(inBuf, u8(cur.peakHold));
  for (const k of ['wfBrightness', 'wfContrast', 'wfSharpness']) push(inBuf, f64(cur[k]));
}

function frame(n, centerHz, bwHz, { interacting = false, dtMs = 50, carriers = [], floor = -110, edgeNoData = 0,
                                    spike = false, nan = false } = {}) {
  clock += dtMs;
  const bins = new Float32Array(n);
  for (let i = 0; i < n; i++) bins[i] = floor + gauss() * 3;
  for (const [pos, lvl, w] of carriers) {
    const c = Math.floor(pos * n);
    for (let k = -w; k <= w; k++) if (c + k >= 0 && c + k < n) bins[c + k] = Math.max(bins[c + k], lvl - Math.abs(k) * 2 + gauss());
  }
  for (let i = 0; i < edgeNoData; i++) { bins[i] = -256; bins[n - 1 - i] = -256; }
  if (spike) { bins[n >> 1] = -5; bins[(n >> 1) + 1] = -8; }
  if (nan) bins[7] = NaN;
  push(inBuf, [1]); push(inBuf, i32(n)); push(inBuf, f64(centerHz)); push(inBuf, f64(bwHz)); push(inBuf, u8(interacting));
  push(inBuf, f64(clock)); push(inBuf, Buffer.from(bins.buffer));
  const r = p.process(bins, centerHz, bwHz, interacting);
  push(outBuf, i32(n)); push(outBuf, f64(r.dbMin)); push(outBuf, f64(r.dbMax));
  push(outBuf, Buffer.from(r.row.buffer, r.row.byteOffset, n));
  push(outBuf, Buffer.from(r.spec.buffer, r.spec.byteOffset, n * 4));
  push(outBuf, Buffer.from(r.peak.buffer, r.peak.byteOffset, n * 4));
}

const fm = [[0.2, -60, 20], [0.5, -45, 25], [0.8, -70, 15]];
// 1. Settle on FM: steady frames, jittered timing.
for (let i = 0; i < 120; i++) frame(1024, 98.5e6, 2.048e6, { dtMs: 40 + rnd() * 30, carriers: fm });
// 2. Retune spike + small pan + big pan (band flush) + pans while interacting.
frame(1024, 98.7e6, 2.048e6, { carriers: fm, spike: true });
for (let i = 0; i < 20; i++) frame(1024, 98.7e6 + i * 20e3, 2.048e6, { interacting: true, carriers: fm });
for (let i = 0; i < 30; i++) frame(1024, 100.5e6, 2.048e6, { carriers: [[0.4, -50, 30]] });
// 3. Zoom in (bw change, then under the floor-freeze span), with a bin-count change.
for (let i = 0; i < 30; i++) frame(2048, 100.5e6, 512e3, { carriers: [[0.4, -50, 60]] });
for (let i = 0; i < 40; i++) frame(2048, 100.5e6, 20e3, { carriers: [[0.5, -50, 400]], floor: -100 });
for (let i = 0; i < 20; i++) frame(2048, 100.5e6, 12e3, { carriers: [[0.5, -48, 900]], floor: -95 });
// 4. Zoom back out; out-of-band markers at the edges; a NaN bin; a long gap.
for (let i = 0; i < 30; i++) frame(1024, 100.5e6, 2.048e6, { edgeNoData: 60, carriers: fm });
frame(1024, 100.5e6, 2.048e6, { nan: true, carriers: fm });
frame(1024, 100.5e6, 2.048e6, { dtMs: 2500, carriers: fm });
// 5. Settings: every knob, including ones that flush and peak hold off/on.
settings({ avgFrames: 0.6 });                 for (let i = 0; i < 25; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ smoothingFrames: 1 });             for (let i = 0; i < 10; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ smoothingFrames: 9, spatialSmooth: false }); for (let i = 0; i < 15; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ autoContrast: 12, wfBrightness: 6 }); for (let i = 0; i < 20; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ peakHold: false });                for (let i = 0; i < 10; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ peakHold: true, specFloor: 5, specPeakScale: 15 }); for (let i = 0; i < 15; i++) frame(1024, 100.5e6, 2.048e6, { carriers: fm });
settings({ minRangeDb: 15 });                 for (let i = 0; i < 20; i++) frame(1024, 225.648e6, 2.048e6, { floor: -95, carriers: [[0.5, -88, 300]] });
settings({ manual: true, manualMinDb: -100, manualMaxDb: -40 }); for (let i = 0; i < 10; i++) frame(1024, 225.648e6, 2.048e6, { carriers: fm });
settings({ manual: false });                  for (let i = 0; i < 20; i++) frame(1024, 98.5e6, 2.048e6, { carriers: fm });
// 6. Peak-hold reset (view moving) and a full reset (reconnect).
push(inBuf, [3]); p.resetPeakHold();          for (let i = 0; i < 10; i++) frame(1024, 98.5e6, 2.048e6, { carriers: fm });
push(inBuf, [4]); p.reset();                  for (let i = 0; i < 30; i++) frame(1024, 98.5e6, 2.048e6, { carriers: fm, dtMs: 30 + rnd() * 80 });
// 7. Tiny frames (narrow-view guards) and odd sizes.
for (let i = 0; i < 10; i++) frame(17, 98.5e6, 2.048e6, { carriers: [[0.5, -50, 2]] });
for (let i = 0; i < 10; i++) frame(4, 98.5e6, 2.048e6);
for (let i = 0; i < 10; i++) frame(1000, 98.5e6, 2.048e6, { carriers: fm });

writeFileSync(join(outDir, 'spectrum_in.bin'), Buffer.concat(inBuf));
writeFileSync(join(outDir, 'spectrum_out.bin'), Buffer.concat(outBuf));
console.log(`wrote ${inBuf.length} input pieces, ${outBuf.length} output pieces to ${outDir}`);
