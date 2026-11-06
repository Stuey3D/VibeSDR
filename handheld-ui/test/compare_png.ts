/** Pixel comparison of the port's render and the app's (raw RGBA, same size).
 *  RAW: pixels identical / beyond ±tol in any channel. TIE-TOLERANT: a pixel also matches if it equals (±6 levels — one
 *  step of the LUT) the other render at x−1 or x+1 on the same line. Nearest-texel sampling at a coordinate that lands
 *  exactly on a texel boundary (every 5th column at 640 px over 1024 bins; whole rows during a move whose quantised h
 *  puts every tap on a boundary) is a TIE that two float implementations break either way — the app's own GPU paths
 *  (Metal / Vulkan / Skia raster) differ from each other the same way. What is left after that is a real difference.
 *  Run: node --no-warnings compare_png.ts <app.rgba> <port.rgba> <W> <H> [tol] */
import { readFileSync } from 'node:fs';
const [a, b, Ws, Hs, tols] = process.argv.slice(2);
const A = readFileSync(a), B = readFileSync(b), W = +Ws, H = +Hs, tol = +(tols ?? 2);
const dAt = (i: number, j: number) => { let m = 0; for (let c = 0; c < 3; c++) m = Math.max(m, Math.abs(A[i * 4 + c] - B[j * 4 + c])); return m; };
let diff = 0, exact = 0, maxd = 0, real = 0, realMax = 0; const rows = new Set<number>();
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
  const i = y * W + x, m = dAt(i, i);
  if (m === 0) exact++;
  if (m > tol) { diff++; rows.add(y); }
  maxd = Math.max(maxd, m);
  let best = Math.min(m, x > 0 ? dAt(i, i - 1) : 999, x < W - 1 ? dAt(i, i + 1) : 999);
  if (best > 6) { real++; realMax = Math.max(realMax, best); }
}
console.log(`  ${b.split('/').pop()}: ${(100 * exact / (W * H)).toFixed(2)} % identical; ${(100 * diff / (W * H)).toFixed(3)} % beyond ±${tol} (max ${maxd}); ` +
            `tie-tolerant: ${real} px (${(100 * real / (W * H)).toFixed(4)} %) differ by more than one LUT step${real ? ` (max ${realMax})` : ''}`);
