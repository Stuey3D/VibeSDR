/**
 * THE GROWING WATERFALL RING (src/utils/wfRingSize.ts + src/utils/wfRingGrow.ts) — Stuart, 2026-10-30: "the history only
 * goes when it genuinely goes off screen"; "if a ram figure cannot be detected for whatever reason, assume the worst and
 * default to the low ram mode".
 *
 * Proves:
 *   1. THE TIER: under ~2 GB, isLowRamDevice, or ANY unknown/failed RAM reading → 1024 (today's ring, the 1 GB phones pay
 *      nothing); 2–4 GB → 2048; 4 GB+ → 4096; the marketed sizes the OS under-reports land in their class.
 *   2. THE SIZE: grow-only, never above the cap, never above the view + a small margin, never below 1024.
 *   3. GROWTH PRESERVES EVERY ROW AND ITS GEOMETRY: a 1024-row ring that has wrapped (tunes, a zoom easing, a jump, the
 *      full-capture layer) grown to 2048 holds, for every frame it held, the same stored row, displayed row, placement,
 *      motion (h, g and the encoded planes), fan map, span and layer row — and tap-to-tune's displayMap is the same.
 *   4. GROWN ≡ BORN BIG: the grown ring, carried on, makes the SAME image rows (and motion) for every frame both hold as a
 *      ring that was 2048 from the start; 2048 → 4096 too.
 *   5. THE SHADER (CanvasKit, the real WF_SKSL): a 2048-row ring (motion planes on TWO image rows each) draws a view
 *      pixel-identical to the 1024-row ring (the old one-row layout) over the frames both hold — through a tune and a
 *      zoom, settled and mid-glide; rows older than the ring draw as the floor (never a wrapped newer row); and a view
 *      taller than 1024 rows over a grown ring is identical to one born at 2048 where both hold the frames.
 * Run: node --no-warnings scripts/test_wf_ring_grow.ts
 */
import { existsSync, readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decodeMotion, decodeZoom, displayMap, fanImage, fanRow, motionAt, motionImageRows, MOTION_ROWS, newFanState, newMotionState,
         placeRow, type FanState, type MotionState } from '../src/utils/wfRowPlace.ts';
import { ringCapFromDeviceClass, ringNextRows, ringRowsNeeded, ringTierRows, RING_MIN_ROWS } from '../src/utils/wfRingSize.ts';
import { growRing } from '../src/utils/wfRingGrow.ts';
import { newLayerRing } from '../src/utils/wfWide.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}
const GiB = 1024 ** 3;

// ── 1. The tier ──
ok('1 GB phone (0.9 GiB reported) → 1024', ringTierRows(0.9 * GiB) === 1024);
ok('a "2 GB" phone (1.8 GiB reported) → 2048', ringTierRows(1.8 * GiB) === 2048);
ok('a "3 GB" phone (2.8 GiB) → 2048', ringTierRows(2.8 * GiB) === 2048);
ok('a "4 GB" phone (3.7 GiB reported) → 4096', ringTierRows(3.7 * GiB) === 4096);
ok('an 8 GB iPad / 16 GB Mac → 4096', ringTierRows(8 * GiB) === 4096 && ringTierRows(16 * GiB) === 4096);
ok('isLowRamDevice → 1024 whatever the RAM', ringTierRows(3.7 * GiB, true) === 1024);
// ★★ unknown = the low tier (Stuart: "assume the worst")
for (const [name, v] of [['null', null], ['undefined', undefined], ['0', 0], ['negative', -1], ['NaN', NaN], ['Infinity', Infinity]] as const)
  ok(`UNKNOWN RAM (${name}) → 1024`, ringTierRows(v as number | null | undefined) === 1024);
ok('deviceClass(): an old binary / web / tests (undefined) → 1024', ringCapFromDeviceClass(undefined) === 1024);
ok('deviceClass(): null → 1024', ringCapFromDeviceClass(null) === 1024);
ok('deviceClass(): {} (no totalMemoryBytes) → 1024', ringCapFromDeviceClass({}) === 1024);
ok('deviceClass(): totalMemoryBytes as a string → 1024', ringCapFromDeviceClass({ totalMemoryBytes: '8589934592' }) === 1024);
ok('deviceClass(): totalMemoryBytes 0 (the unreadable read) → 1024', ringCapFromDeviceClass({ totalMemoryBytes: 0 }) === 1024);
ok('deviceClass(): 8 GiB → 4096', ringCapFromDeviceClass({ totalMemoryBytes: 8 * GiB }) === 4096);
ok('deviceClass(): 8 GiB but isLowRamDevice → 1024', ringCapFromDeviceClass({ totalMemoryBytes: 8 * GiB, isLowRamDevice: true }) === 1024);
ok('deviceClass(): isLowRamDevice not a boolean is ignored', ringCapFromDeviceClass({ totalMemoryBytes: 8 * GiB, isLowRamDevice: 'yes' }) === 4096);

// ── 2. The size ──
ok('a phone view (800 rows) needs no growth', ringNextRows(1024, 800, 4096) === 1024);
ok('a view of exactly 1008 rows (1024 − margin) still fits 1024', ringNextRows(1024, 1008, 4096) === 1024);
ok('an iPad Pro portrait (1366 rows) → 1536', ringNextRows(1024, 1366, 4096) === 1536);
ok('a pivoted 4K screen (3840 rows) → 4096', ringNextRows(1024, 3840, 4096) === 4096);
ok('… capped at 2048 on a 2–4 GB device', ringNextRows(1024, 3840, 2048) === 2048);
ok('… and 1024 on the low tier: never grows', ringNextRows(1024, 3840, 1024) === 1024);
ok('never shrinks: a smaller view keeps the grown ring', ringNextRows(2048, 400, 4096) === 2048);
ok('never shrinks below what it is even if the cap is lower', ringNextRows(2048, 3000, 1024) === 2048);
ok('never above the view + margin (rounded to 256)', ringNextRows(1024, 1100, 4096) === 1280 && ringRowsNeeded(1100) === 1280);
ok('never below 1024 (bad input)', ringNextRows(0, NaN, 0) === 1024 && ringRowsNeeded(-5) === RING_MIN_ROWS);
ok('motion planes: ≤ 1024 rows = the old 4 image rows; 2048 → 8; 4096 → 16',
   motionImageRows(1024, 1024) === 4 && motionImageRows(1536, 1024) === 8 && motionImageRows(4096, 1024) === 16);
ok('motionAt at ≤ 1024 rows = the old layout (plane p row, column = slot)', motionAt(1024, 1024, 2, 700) === 2 * 1024 + 700);

// ── 3/4. A wrapped ring with tunes, zooms, a jump and a layer ──
const N = 1024;
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
interface Ring { rows: number; buf: Uint8Array; ms: MotionState; fan: FanState; frames: number }
const newRing = (rows: number): Ring => ({ rows, buf: new Uint8Array(N * (rows + motionImageRows(rows, N))), ms: newMotionState(rows, N), fan: newFanState(rows), frames: 0 });
/** The scripted session: a view centre/span per frame (tunes, a zoom in and back, a jump), the row seeded by frame. */
function viewAt(f: number): [number, number] {
  let c = 7_100_000, s = 200_000;
  if (f >= 300) c += Math.min(40_000, (f - 300) * 500);           // a slow tune (eases)
  if (f >= 700 && f < 1100) s = 100_000;                          // a zoom in …
  if (f >= 1100 && f < 1300) s = 50_000;                          // … further
  if (f >= 1400) c += 900_000;                                    // a jump (a bookmark)
  if (f >= 1700) s = 150_000;                                     // a zoom out while…
  if (f >= 1750) c += (f - 1750) * 300;                           // … tuning
  return [c, s];
}
function rowOf(f: number): Uint8Array {
  const r = new Uint8Array(N); let s = (f * 2654435761) >>> 0;
  for (let i = 0; i < N; i++) { s = (s * 1103515245 + 12345) >>> 0; r[i] = 1 + (s >>> 24) % 200; }
  r[(f * 7) % N] = 255; return r;
}
function push(R: Ring, f: number, visible = 1000) {   // the view's rows (the app: fanVisible) — the same for every ring
  const [c, s] = viewAt(f);
  placeRow(R.buf, R.rows, N, R.frames, rowOf(f), c, s / N, c, s, R.ms, R.fan, Math.min(visible, R.rows));
  // the full-capture layer: a deterministic row per slot (what layerWriteRow would write), once the layer exists
  if (R.fan.layer) {
    const L = R.fan.layer, slot = R.frames % R.rows;
    L.k[slot] = 0.1 + (f % 17) / 100; L.m[slot] = (f % 13) / 50; L.pkN[slot] = f % 5;
    for (let i = 0; i < L.F; i++) L.floor[slot * L.F + i] = (f + i) & 255;
    for (let i = 0; i < L.P; i++) { L.pkCell[slot * L.P + i] = (f * 3 + i) & 0xffff; L.pkIdx[slot * L.P + i] = (f + 2 * i) & 255; L.pkShape[slot * L.P + i] = i & 3; }
    fanRow(R.fan, R.buf, N, slot, R.rows);   // as WaterfallView: an eased row drawn again now its layer row exists
  }
  R.frames++;
}
function run(R: Ring, from: number, to: number) { for (let f = from; f < to; f++) { if (f === 200 && !R.fan.layer) R.fan.layer = newLayerRing(R.rows, 64, 256, 8); push(R, f); } }
function grow(R: Ring, rows: number) {
  const b = growRing(R.buf, R.ms, R.fan, N, R.rows, rows, R.frames);
  if (b) R.buf = b;
  R.rows = rows;
}
const motionBytesOf = (ms: MotionState, rows: number, slot: number) =>
  [0, 1, 2, 3].map((p) => ms.bytes[motionAt(rows, N, p, slot)]);
/** Every per-slot fact about frame f in ring R. */
function slotFacts(R: Ring, f: number) {
  const s = f % R.rows, L = R.fan.layer;
  const img = fanImage(R.fan, R.buf);
  return {
    stored: Array.from(R.buf.subarray(s * N, s * N + N)).join(','),
    shown: Array.from(img.subarray(s * N, s * N + N)).join(','),
    place: [R.ms.startHz[s], R.ms.hzPerCol[s], R.ms.h[s], R.ms.g[s]].join(','),
    planes: motionBytesOf(R.ms, R.rows, s).join(','),
    fan: [R.fan.xfA[s], R.fan.xfB[s], R.fan.span[s]].join(','),
    geo: JSON.stringify(displayMap(R.ms, R.fan.xfA, R.fan.xfB, s, N)),
    layer: L ? [L.k[s], L.m[s], L.pkN[s], Array.from(L.floor.subarray(s * L.F, s * L.F + L.F)).join('.'),
                Array.from(L.pkCell.subarray(s * L.P, s * L.P + L.P)).join('.'), Array.from(L.pkIdx.subarray(s * L.P, s * L.P + L.P)).join('.'),
                Array.from(L.pkShape.subarray(s * L.P, s * L.P + L.P)).join('.')].join('|') : '',
  };
}
function sameFacts(what: string, A: Ring, B: Ring, f0: number, f1: number, keys?: (keyof ReturnType<typeof slotFacts>)[]) {
  let bad = 0, first = '';
  for (let f = f0; f < f1; f++) {
    const a = slotFacts(A, f), b = slotFacts(B, f);
    for (const k of (keys ?? (Object.keys(a) as (keyof typeof a)[]))) if (a[k] !== b[k]) { if (!bad) first = `frame ${f} ${k}`; bad++; }
  }
  ok(what, bad === 0, `${bad} mismatches, first: ${first}`);
}

// 3. Grow a wrapped ring mid-zoom-ease (frame 1302: two rows after the second zoom step — the ease running, disp live)
{
  const A = newRing(1024); run(A, 0, 1302);
  const before = newRing(1024); run(before, 0, 1302);   // the same session, never grown (the reference for "before")
  ok('the fan is mid-ease with a displayed ring at the growth (the hard case)', A.fan.easeLeft > 0 && !!A.fan.disp);
  const lo0 = A.ms.lo, hi0 = A.ms.hi, vf0 = A.fan.validFrom;
  grow(A, 2048);
  ok('the grown ring buffer is 2048 rows + 8 motion rows', A.buf.length === N * (2048 + 8) && A.ms.bytes.length === 8 * N);
  ok('absolute state untouched (motion range, validFrom, frames)', A.ms.lo === lo0 && A.ms.hi === hi0 && A.fan.validFrom === vf0 && A.frames === 1302);
  sameFacts('3. every kept frame (the newest 1024): stored row, displayed row, placement, motion, planes, fan map, span, layer, tap geometry — identical after growth',
            A, before, 1302 - 1024, 1302);
  let decOk = true;
  for (let f = 1302 - 1024; f < 1302; f++) {
    const s = f % 2048, b = motionBytesOf(A.ms, 2048, s);
    const h = decodeMotion(b[0], b[1]), g = decodeZoom(b[2], b[3]);
    if (Math.abs(h - A.ms.h[s]) > 1 / 64 || Math.abs(g - A.ms.g[s]) > 1e-3 * g) { decOk = false; break; }
  }
  ok('the re-encoded planes decode to each slot\'s h and g', decOk);
  let empty = true;
  for (let f = 0; f < 1302 - 1024; f++) { const s = f % 2048; if (!Number.isNaN(A.ms.startHz[s]) || A.buf.subarray(s * N, s * N + N).some((v) => v !== 0)) { empty = false; break; } }
  ok('slots for frames the old ring no longer held are the floor (no stale or wrapped row)', empty);

  // 4. carry on: grown ≡ born at 2048, for every frame both hold
  run(A, 1302, 2600);
  const B = newRing(2048); run(B, 0, 2600);
  sameFacts('4. grown 1024→2048 then carried on ≡ a ring born at 2048 (every frame both hold: image rows, placement, motion, maps, layer)',
            A, B, 1302 - 1024, 2600, ['shown', 'stored', 'place', 'planes', 'fan', 'geo', 'layer']);
  grow(A, 4096); grow(B, 4096);
  run(A, 2600, 3300); run(B, 2600, 3300);
  const C = newRing(4096); run(C, 0, 3300);
  sameFacts('4. … then 2048→4096 (16 motion rows) ≡ born at 4096 over the frames all hold', A, C, 2600 - 2048, 3300, ['shown', 'place', 'planes', 'fan', 'geo', 'layer']);
  sameFacts('4. … and B (born 2048, grown 4096) ≡ C over its kept frames', B, C, 3300 - 2048, 3300, ['shown', 'place', 'planes', 'fan', 'geo', 'layer']);
}
// grow before anything was written (the first render on an iPad): nothing to copy, the state is just deeper
{
  const ms = newMotionState(1024, N), fan = newFanState(1024);
  const b = growRing(null, ms, fan, N, 1024, 1536, 0);
  ok('growth before the first row: no buffer, deeper per-slot state', b === null && ms.startHz.length === 1536 && fan.xfA.length === 1536 && ms.bytes.length === 8 * N && fan.xfA[1500] === 1 && ms.g[1500] === 1);
}

// ── 5. The shader ──
const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(join(ROOT, 'package.json'));
const ckDir = join(ROOT, 'node_modules/canvaskit-wasm/bin');
if (!existsSync(join(ckDir, 'canvaskit.js'))) {
  console.log('  (shader section SKIPPED: canvaskit-wasm not installed)');
} else {
  const CanvasKitInit = require(join(ckDir, 'canvaskit.js'));
  const CK = await CanvasKitInit({ locateFile: (f: string) => join(ckDir, f) });
  const src = readFileSync(join(ROOT, 'src/components/WaterfallView.tsx'), 'utf8');
  const SKSL = src.slice(src.indexOf('const WF_SKSL = `') + 17, src.indexOf('}`;\nconst WF_EFFECT') + 1);
  let err = '';
  const EFFECT = CK.RuntimeEffect.Make(SKSL, (e: string) => { err = e; });
  ok('WF_SKSL compiles', !!EFFECT, err);
  const lut = new Uint8Array(256 * 4); for (let i = 0; i < 256; i++) lut.set([i, i, i, 255], i * 4);
  const W = 600, ROW_PX = 1;
  /** WaterfallView's uniforms; `cont` = a position mid-row (the glide), else settled. */
  function draw(R: Ring, vis: number, cont?: number): Uint8Array {
    const H = vis * ROW_PX, bytes = (R.fan.disp ?? R.buf);
    bytes.set(R.ms.bytes, R.rows * N);
    const img = CK.MakeImage({ width: N, height: R.rows + motionImageRows(R.rows, N), alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.Gray_8, colorSpace: CK.ColorSpace.SRGB }, bytes, N);
    const lu = CK.MakeImage({ width: 256, height: 1, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.RGBA_8888, colorSpace: CK.ColorSpace.SRGB }, lut, 256 * 4);
    const wf = img.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
    const ls = lu.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
    const c = cont != null, P = c ? R.frames - 1 + cont! : R.frames;
    const head = c ? Math.floor(P) + 1 : P, frac = c ? P - Math.floor(P) : 1;
    const u = [head, frac, 1, c ? 0 : 1, R.rows, vis, N, W, H, 0.4, 0, R.ms.lo, R.ms.hi, 1 / ROW_PX, W / N, 1, 0];
    const sh = EFFECT.makeShaderWithChildren(u, [wf, ls]);
    const surf = CK.MakeSurface(W, H), paint = new CK.Paint(); paint.setShader(sh);
    surf.getCanvas().drawRect(CK.LTRBRect(0, 0, W, H), paint);
    const px = surf.getCanvas().readPixels(0, 0, { width: W, height: H, colorType: CK.ColorType.RGBA_8888, alphaType: CK.AlphaType.Unpremul, colorSpace: CK.ColorSpace.SRGB }) as Uint8Array;
    surf.delete(); paint.delete(); sh.delete(); wf.delete(); ls.delete(); img.delete(); lu.delete();
    return px;
  }
  const diff = (a: Uint8Array, b: Uint8Array, y0: number, y1: number) => {
    let d = 0; for (let i = y0 * W * 4; i < y1 * W * 4; i++) if (a[i] !== b[i]) d++; return d;
  };
  // A ring at frame 2000 with the zoom-out + tune (1700..) on screen: the 2048 ring's slots for those frames are ≥ 1024
  // after the wrap point (frames 1024..2047 → slots 1024..2047: the motion planes' SECOND image row).
  const S1 = newRing(1024), S2 = newRing(2048);
  run(S1, 0, 2000); run(S2, 0, 2000);
  let moving = 0; for (let f = S2.ms.lo; f <= S2.ms.hi; f++) if (f % 2048 >= 1024 && (S2.ms.h[f % 2048] !== 0 || S2.ms.g[f % 2048] !== 1)) moving++;
  ok('the drawn frames include moving slots ≥ 1024 in the 2048 ring (the planes\' second image row is exercised)', moving > 20, `${moving}`);
  for (const [name, cont] of [['settled', undefined], ['mid-glide', 0.4]] as const) {
    const a = draw(S1, 900, cont), b = draw(S2, 900, cont);
    ok(`5. ${name}: a 2048-row ring draws the 900 newest rows pixel-identical to the 1024-row ring (old layout)`, diff(a, b, 0, 900) === 0, `${diff(a, b, 0, 900)} bytes differ`);
  }
  // Rows older than the ring are the floor, not a wrapped newer row: a 1024 ring drawn 1200 rows tall
  {
    const px = draw(S1, 1200);
    let lit = 0; for (let y = 1030; y < 1200; y++) for (let x = 0; x < W; x++) if (px[(y * W + x) * 4] !== 0) lit++;
    ok('5. rows older than the ring draw as the floor (the shader\'s guard) — no wrapped history at the bottom', lit === 0, `${lit} lit px`);
    let top = 0; for (let y = 0; y < 1000; y++) for (let x = 0; x < W; x += 7) if (px[(y * W + x) * 4] !== 0) top++;
    ok('… while the rows it holds still draw', top > 50000, `${top}`);
  }
  // A view taller than 1024 over a ring GROWN at 1500 ≡ one born at 2048, over the frames both hold
  {
    const G = newRing(1024); run(G, 0, 1500); grow(G, 2048); run(G, 1500, 2000);
    const a = draw(G, 1400), b = draw(S2, 1400);
    // frames shown 1999 down to 600; G holds 1500−1024 = 476 onward — all of them
    ok('5. a 1400-row view over a ring grown mid-session is pixel-identical to one born at 2048', diff(a, b, 0, 1400) === 0, `${diff(a, b, 0, 1400)} bytes differ`);
    const a2 = draw(G, 1600), b2 = draw(S2, 1600);
    // lines 0..~1520 = frames ≥ 478 (held by both); below that G has only the floor where S2 still has rows
    ok('… and at 1600 rows the lines both hold are identical', diff(a2, b2, 0, 1515) === 0, `${diff(a2, b2, 0, 1515)}`);
    let lit = 0; for (let y = 1530; y < 1600; y++) for (let x = 0; x < W; x++) if (a2[(y * W + x) * 4] !== 0) lit++;
    ok('… and below them the grown ring shows the floor (history it never kept), not a wrapped row', lit === 0, `${lit}`);
  }
}

console.log(`  waterfall ring growth: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
