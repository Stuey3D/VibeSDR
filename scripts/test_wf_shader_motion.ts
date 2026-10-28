/**
 * THE APP'S WATERFALL SHADER, RUN FOR REAL (CanvasKit in node — the same SkSL WaterfallView compiles, on Skia's CPU
 * backend). Stuart, 2026-10-28, the first native build's MW sweep is the reference: "I just wanted the chunky broken
 * diagonal lines joined up so they looked solid"; "NO SLICING when zooming or tuning".
 *
 * A synthetic MW band (AM stations every 9 kHz, a speckled floor) is swept 860 → 670 kHz the way a finger drags it, rows
 * placed by placeRow exactly as WaterfallView does (the ring, the per-row motion, the fan), and drawn through WF_SKSL at
 * iPhone resolution (3 device px a row). Proves, on the real shader:
 *   • a moving station's trail is CONTINUOUS line to line (each device line's lit run touches the one above) — settled
 *     AND mid-glide (continuous) — for a view-only pan (UberSDR/Kiwi/OWRX) AND a tuner retune with rows lagging (#57);
 *     CONTROL: the same rows with motion compensation off (the same-x blend) are broken into dashes;
 *   • steady rows are pixel-identical with the motion machinery on or off (nothing changes outside a move);
 *   • a zoom fans the history (no slices: every history line's carrier at the live column) and the screen ease in mid
 *     flight draws one continuous geometry (no seam between history and the new rows);
 *   • a resize (fewer/more visible rows) keeps the same rows — the ring is not rebuilt;
 *   • V12 THE FULL-CAPTURE LAYER (src/utils/wfWide.ts): zoom in AWAY from the stations and back out — every history line
 *     recorded while zoomed in shows every strong station at its live column, thin, and no black (CONTROL: an older server,
 *     no SPWB, draws those lines black outside the strip). WF_LAYER_PNG=<path> writes the before | after picture.
 * Writes the comparison PNG (reference frame | settled | mid-glide | before) when WF_PNG=<path> is set.
 * Skips (exit 0) if canvaskit-wasm is not installed (it comes with @shopify/react-native-skia's web build).
 * Run: node --no-warnings scripts/test_wf_shader_motion.ts
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { fanRow, MOTION_ROWS, newFanState, newMotionState, placeRow, takeFanStep, wfImageBytes, ZOOM_EASE_ROWS } from '../src/utils/wfRowPlace.ts';
import { layerWriteRow, newLayerRing, newWideState, wideArrive, type WideRow } from '../src/utils/wfWide.ts';
import { COLORMAPS_256 } from '../src/assets/colormaps.ts';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(join(ROOT, 'package.json'));
const ckDir = join(ROOT, 'node_modules/canvaskit-wasm/bin');
if (!existsSync(join(ckDir, 'canvaskit.js'))) { console.log('  waterfall shader: SKIPPED (canvaskit-wasm not installed)'); process.exit(0); }
const CanvasKitInit = require(join(ckDir, 'canvaskit.js'));
const CK = await CanvasKitInit({ locateFile: (f: string) => join(ckDir, f) });

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}

const src = readFileSync(join(ROOT, 'src/components/WaterfallView.tsx'), 'utf8');
const SKSL = src.slice(src.indexOf('const WF_SKSL = `') + 17, src.indexOf('}`;\nconst WF_EFFECT') + 1);
let err = '';
const EFFECT = CK.RuntimeEffect.Make(SKSL, (e: string) => { err = e; });
ok('WF_SKSL compiles under Skia\'s strict ES2 runtime-effect rules', !!EFFECT, err);
if (!EFFECT) { console.log(`  waterfall shader: ${passes} passed, ${fails} failed`); process.exit(1); }

const N = 1024, ROWS = 1024;
const lutGray = new Uint8Array(256 * 4); for (let i = 0; i < 256; i++) { lutGray.set([i, i, i, 255], i * 4); }
const lutGqrx = new Uint8Array(256 * 4);
{ const p = COLORMAPS_256.gqrx; for (let i = 0; i < 256; i++) lutGqrx.set([parseInt(p.slice(i * 6, i * 6 + 2), 16), parseInt(p.slice(i * 6 + 2, i * 6 + 4), 16), parseInt(p.slice(i * 6 + 4, i * 6 + 6), 16), 255], i * 4); }

// ── The band: AM stations every 9 kHz from 531 kHz, each with its own strength and audio; a speckled floor ──
let seed = 11;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const stations: { f: number; lvl: number }[] = [];
for (let f = 531_000; f <= 1_602_000; f += 9_000) if (rnd() < 0.55) stations.push({ f, lvl: 0.55 + rnd() * 0.4 });
/** One row as the server measures it: `span` Hz across N bins centred on `centre`. */
function bandRow(centre: number, span: number): Uint8Array {
  const r = new Uint8Array(N), hz = span / N, lo = centre - span / 2;
  const v = new Float32Array(N);
  for (let i = 0; i < N; i++) v[i] = 0.24 + rnd() * 0.16;
  for (const s of stations) {
    const c = (s.f - lo) / hz;
    if (c < -80 || c > N + 80) continue;
    const half = 4_500 / hz;
    for (let i = Math.max(0, Math.floor(c - half)); i <= Math.min(N - 1, Math.ceil(c + half)); i++) {
      const d = Math.abs(i - c) / half;
      const side = s.lvl * (0.72 - 0.25 * d) * (0.75 + 0.35 * rnd());
      v[i] = Math.max(v[i], side);
    }
    const ci = Math.round(c); for (let k = -1; k <= 1; k++) if (ci + k >= 0 && ci + k < N) v[ci + k] = Math.max(v[ci + k], s.lvl * (k === 0 ? 1 : 0.85));
  }
  for (let i = 0; i < N; i++) r[i] = Math.max(1, Math.min(255, Math.round(v[i] * 255)));
  return r;
}

interface Ring { buf: Uint8Array; ms: ReturnType<typeof newMotionState>; fan: ReturnType<typeof newFanState>; frames: number }
const newRing = (): Ring => ({ buf: new Uint8Array(N * (ROWS + MOTION_ROWS)), ms: newMotionState(ROWS, N), fan: newFanState(ROWS), frames: 0 });
function push(R: Ring, rowCentre: number, span: number, viewC: number, viewBw: number) {
  placeRow(R.buf, ROWS, N, R.frames, bandRow(rowCentre, span), rowCentre, span / N, viewC, viewBw, R.ms, R.fan, 600);
  R.frames++;
}

// ── The draw: WaterfallView's uniforms, at 3 device px a row (an iPhone) ──
const W = 1179, ROW_PX = 3, VIS = 300, H = VIS * ROW_PX;
function draw(R: Ring, o: { lut?: Uint8Array; motion?: boolean; cont?: number; za?: number; zb?: number; vis?: number } = {}): Uint8Array {
  const vis = o.vis ?? VIS, h = vis * ROW_PX;
  const bytes = wfImageBytes(R.fan, R.ms, R.buf, N, ROWS);
  const img = CK.MakeImage({ width: N, height: ROWS + MOTION_ROWS, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.Gray_8, colorSpace: CK.ColorSpace.SRGB }, bytes, N);
  const lut = CK.MakeImage({ width: 256, height: 1, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.RGBA_8888, colorSpace: CK.ColorSpace.SRGB }, o.lut ?? lutGray, 256 * 4);
  const wf = img.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const lu = lut.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const cont = o.cont != null;
  const P = cont ? R.frames - 1 + o.cont! : R.frames;   // settled: shown = the newest; continuous: a position mid-row
  const head = cont ? Math.floor(P) + 1 : P, frac = cont ? P - Math.floor(P) : 1;
  const motion = o.motion ?? true;
  const u = [head, frac, 1, cont ? 0 : 1, ROWS, vis, N, W, h, 0, 0,
             motion ? R.ms.lo : 1, motion ? R.ms.hi : 0, 1 / ROW_PX, W / N, o.za ?? 1, o.zb ?? 0];
  const sh = EFFECT.makeShaderWithChildren(u, [wf, lu]);
  const surf = CK.MakeSurface(W, h);
  const paint = new CK.Paint(); paint.setShader(sh);
  surf.getCanvas().drawRect(CK.LTRBRect(0, 0, W, h), paint);
  const px = surf.getCanvas().readPixels(0, 0, { width: W, height: h, colorType: CK.ColorType.RGBA_8888, alphaType: CK.AlphaType.Unpremul, colorSpace: CK.ColorSpace.SRGB });
  if (o.lut && (process.env.WF_PNG || process.env.WF_LAYER_PNG || process.env.WF_ZOOM_PNG)) lastSurf.push({ surf, h });
  else surf.delete();
  paint.delete(); sh.delete(); wf.delete(); lu.delete(); img.delete(); lut.delete();
  return px as Uint8Array;
}
const lastSurf: { surf: any; h: number }[] = [];

/** Device line y's lit run nearest x0 (gray ≥ thr): [a, b] or null. */
function run(px: Uint8Array, y: number, x0: number, thr: number, reach = 80): [number, number] | null {
  let best = -1, bd = 1e9;
  for (let x = Math.max(0, Math.round(x0 - reach)); x < Math.min(W, Math.round(x0 + reach)); x++) if (px[(y * W + x) * 4] >= thr && Math.abs(x - x0) < bd) { bd = Math.abs(x - x0); best = x; }
  if (best < 0) return null;
  let a = best, b = best;
  while (a > 0 && px[(y * W + a - 1) * 4] >= thr) a--;
  while (b < W - 1 && px[(y * W + b + 1) * 4] >= thr) b++;
  return [a, b];
}
/** (Touching = within 2 device px, a third of a point: the eye reads that as one line; the dashes it must catch are
 *  10–40 px apart.) Follow one carrier DOWN the picture from line y0 (where it sits at x0) until it leaves the screen or line y1; count
 *  lines whose lit run does not touch the one above (a gap — the dashes). `reach` = how far a line may look for it. */
function gaps(px: Uint8Array, y0: number, y1: number, x0: number, thr: number, minX = 12): { gaps: number; lines: number } {
  let g = 0, lines = 0, prev: [number, number] | null = null, x = x0;
  for (let y = y0; y <= y1; y++) {
    if (x < minX) break;                                // it has left the screen (to the left, toward the start)
    const r = run(px, y, x, thr, 40);
    if (!r) { g++; if (process.env.WF_DEBUG) console.log(`none y=${y} x=${x.toFixed(0)}`); continue; }
    lines++;
    if (prev && (r[0] > prev[1] + 2 || r[1] < prev[0] - 2)) { g++; if (process.env.WF_DEBUG) console.log(`gap y=${y} run=${r} prev=${prev}`); }
    prev = r; x = (r[0] + r[1]) / 2;
  }
  return { gaps: g, lines };
}
/** The threshold for a station of level `lvl`: most of its carrier (the floor never reaches it). */
const thrOf = (lvl: number) => Math.round(lvl * 255 * 0.8);

// ── 1. A finger-dragged sweep, view-only (a fixed capture: every row arrives for its view) ──
const span = 150_000;
const sweep = (R: Ring, lag: number) => {
  let c = 860_000; const cs: number[] = [];
  const at = (k: number) => cs[Math.max(0, k - lag)];
  for (let i = 0; i < 80; i++) { cs.push(c); push(R, at(cs.length - 1), span, c, span); }
  const n = 140;
  for (let i = 1; i <= n; i++) {                       // ease in, ease out: 190 kHz in 7 s at 20 rows/s
    c = 860_000 - 190_000 * (0.5 - 0.5 * Math.cos(Math.PI * i / n));
    cs.push(c); push(R, at(cs.length - 1), span, c, span);
  }
  for (let i = 0; i < 70; i++) { cs.push(c); push(R, at(cs.length - 1), span, c, span); }
  return c;
};
const A = newRing(); const endC = sweep(A, 0);
// the carrier to follow: the strongest station on screen at the end
const lo = endC - span / 2;
const vis = stations.filter((s) => s.f > lo + 10_000 && s.f < lo + span - 10_000).sort((a, b) => b.lvl - a.lvl)[0];
const xEnd = (vis.f - lo) / span * W;
// sweep rows on screen: newest at the top; the sweep sits 70..210 rows down → device lines 210..630
const ySweepTop = 70 * ROW_PX, ySweepBot = 210 * ROW_PX;
const settled = draw(A), before = draw(A, { motion: false }), mid = draw(A, { cont: 0.5 });
const THR = thrOf(vis.lvl);
for (const [name, px] of [['settled', settled], ['mid-glide (continuous, t = 0.5)', mid]] as const) {
  // follow it from where it settled (above the sweep) down the bend until it leaves the screen
  const g = gaps(px, ySweepTop - 30, ySweepBot + 30, xEnd, THR);
  ok(`view pan, ${name}: the swept carrier is ONE continuous trail down the bend (every device line touches the next)`, g.gaps === 0 && g.lines >= 120, `${g.gaps} gaps over ${g.lines} lines`);
}
{ const g = gaps(before, ySweepTop - 30, ySweepBot + 30, xEnd, THR);
  ok('CONTROL: the same rows without motion compensation are broken (the chunky dashes the test must see)', g.gaps >= 20, `${g.gaps} gaps over ${g.lines} lines`); }
{ let diff = 0; for (let y = 0; y < ySweepTop - 2 * ROW_PX; y++) for (let x = 0; x < W; x++) if (settled[(y * W + x) * 4] !== before[(y * W + x) * 4]) diff++;
  for (let y = ySweepBot + 2 * ROW_PX; y < H; y++) for (let x = 0; x < W; x++) if (settled[(y * W + x) * 4] !== before[(y * W + x) * 4]) diff++;
  ok('steady rows (before and after the sweep) are pixel-identical with motion on or off', diff === 0, `${diff} px differ`); }
// a trail ends straight where the view puts it
{ const r = run(settled, 10, xEnd, THR); ok('the trail settles straight at the live column', !!r && Math.abs((r[0] + r[1]) / 2 - xEnd) < 4, `${r} vs ${xEnd.toFixed(1)}`); }

// ── 2. A TUNER retune (VibeServer, #57): rows lag the view by 2 and are placed at their own tuner position ──
{ const B = newRing(); sweep(B, 2);
  const px = draw(B);
  const g = gaps(px, ySweepTop - 30, ySweepBot + 30, xEnd, THR, 60);   // (lagging rows leave the honest black edge strip, ~33 px, where the tuner has not been yet)
  ok('tuner retune (rows lagging the view, #57): one continuous trail', g.gaps === 0 && g.lines >= 120, `${g.gaps} gaps over ${g.lines}`); }

// ── 3. A ZOOM (×2 drum step) after the sweep (Stuart 2026-10-28: "a thin line signal should expand out to the width and
//       the history remains the thin line"): the history keeps its zoom, the new rows ease to the new one, ONE line ──
{ const Z = newRing(); sweep(Z, 0);
  const NEW = 24;
  for (let i = 0; i < NEW; i++) push(Z, endC, span / 2, endC, span / 2);
  const [k, c] = takeFanStep(Z.fan);
  ok('a ×2 zoom: no screen ease (the history is not moved)', k === 1 && c === 0, `${k} ${c}`);
  const px = draw(Z), pOff = draw(Z, { motion: false });
  // a strong station well off the dial, so the zoom moves it a long way
  const zs = stations.filter((s) => s.lvl > 0.75 && Math.abs(s.f - endC) > span / 12 && Math.abs(s.f - endC) < span / 5)[0];
  const ZTHR = thrOf(zs.lvl);
  const xOld = (zs.f - (endC - span / 2)) / span * W, xNew = (zs.f - (endC - span / 4)) / (span / 2) * W;
  const yNew1 = (NEW - ZOOM_EASE_ROWS - 1) * ROW_PX, yH0 = (NEW + 1) * ROW_PX, yH1 = ySweepTop + NEW * ROW_PX - 6;
  let offN = 0; for (let y = ROW_PX; y < yNew1; y += ROW_PX) { const r = run(px, y, xNew, ZTHR, 30); if (!r || Math.abs((r[0] + r[1]) / 2 - xNew) > 5) offN++; }
  ok('a ×2 zoom: the new lines show the station at the new zoom', offN === 0, `${offN} lines off`);
  let offH = 0; for (let y = yH0; y < yH1; y += ROW_PX) { const r = run(px, y, xOld, ZTHR, 30); if (!r || Math.abs((r[0] + r[1]) / 2 - xOld) > 5) offH++; }
  ok('a ×2 zoom: the history keeps the station where it was drawn (the thin line stays)', offH === 0, `${offH} lines off`);
  const g = gaps(px, ROW_PX, yH1, xNew, ZTHR);
  ok(`a ×2 zoom: the FAN — one continuous line from the new column (${xNew.toFixed(0)}) down to the old (${xOld.toFixed(0)})`, g.gaps === 0, `${g.gaps} gaps over ${g.lines}`);
  const g0 = gaps(pOff, ROW_PX, yH1, xNew, ZTHR);
  ok('CONTROL: the same rows without the motion join step (the fan needs it)', g0.gaps >= 3, `${g0.gaps} gaps over ${g0.lines}`); }

// WF_ZOOM_PNG=path: a picture of the zoom — steady at 150 kHz, a pinch in to ×4 (5 % a row), then two ×2 drum steps back out
if (process.env.WF_ZOOM_PNG) {
  const Z = newRing(); let bw = span; const c0 = 700_000;
  for (let i = 0; i < 120; i++) push(Z, c0, bw, c0, bw);
  while (bw > span / 4) { bw *= 0.95; push(Z, c0, bw, c0, bw); }
  for (let i = 0; i < 50; i++) push(Z, c0, bw, c0, bw);
  for (const k of [2, 2]) { bw *= k; for (let i = 0; i < 45; i++) push(Z, c0, bw, c0, bw); }
  lastSurf.length = 0; draw(Z, { lut: lutGqrx });
  const sf = lastSurf[0]; writeFileSync(process.env.WF_ZOOM_PNG, sf.surf.makeImageSnapshot().encodeToBytes());
  console.log(`  wrote ${process.env.WF_ZOOM_PNG}`);
}

// ── 4. A resize: the same ring at another visible depth — the rows on screen are the same rows ──
{ const short = draw(A, { vis: 200 }); let diff = 0;
  for (let y = 0; y < 200 * ROW_PX; y++) for (let x = 0; x < W; x += 7) if (short[(y * W + x) * 4] !== settled[(y * W + x) * 4]) diff++;
  // (a handful of px may differ by float rounding of the sub-row position inside a warped row — not a re-draw)
  ok('a resize keeps the history: the newest 200 rows draw identically at a shorter height', diff <= 10, `${diff} px differ`); }

// ── 5. V12 THE FULL-CAPTURE LAYER: zoom in away from the stations, then back out (docs/v12/WATERFALL-FULL-CAPTURE.md) ──
{
  const CAPC = 1_000_000, CAPS = 2_400_000, CLO = CAPC - CAPS / 2, FB = 128, CC = 1024;
  /** The SPWB row the server would send of this synthetic band, in the rows' own units (index = "dB" here, toIdx the
   *  identity): the floor is the speckle's middle, every station a narrow carrier cell and its sidebands' cells. */
  const wideRow = (): WideRow => {
    const floor = new Float32Array(FB).fill(0.32 * 255), spread = new Float32Array(FB);
    const cells: [number, number, number][] = [];
    for (const st of stations) {
      const cf = (st.f - CLO) / CAPS * CC, c = Math.floor(cf), off = Math.min(15, Math.floor((cf - c) * 16));
      for (const d of [-2, -1, 1, 2]) cells.push([c + d, st.lvl * 255 * (0.62 + 0.1 * rnd()), (15 << 4) | 7]);   // a sideband fills its cell
      cells.push([c, st.lvl * 255, off]);                                                                          // the carrier: one bin
    }
    cells.sort((a, b) => a[0] - b[0]);
    const pkCell = new Int32Array(cells.length), pkDb = new Float32Array(cells.length), pkShape = new Uint8Array(cells.length);
    cells.forEach(([c, v, sh], i) => { pkCell[i] = c; pkDb[i] = v; pkShape[i] = sh; });
    return { centreHz: CAPC, spanHz: CAPS, floor, spread, cells: CC, pkCell, pkDb, pkShape, nPk: cells.length };
  };
  const ident = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
  const mkL = (wide: boolean) => { const R = newRing(); return { R, st: newWideState(), wide, placed: { startHz: NaN, spanHz: 0 }, t: 0 }; };
  type LR = ReturnType<typeof mkL>;
  const pushL = (X: LR, c: number, bw: number) => {
    if (X.wide && X.R.frames % 4 === 0) { wideArrive(X.st, wideRow(), X.t, X.placed.startHz, X.placed.spanHz, NaN); if (!X.R.fan.layer) X.R.fan.layer = newLayerRing(ROWS, FB, CC); }
    const slot = placeRow(X.R.buf, ROWS, N, X.R.frames, bandRow(c, bw), c, bw / N, c, bw, X.R.ms, X.R.fan, 600, X.placed);
    if (X.R.fan.layer) { X.R.fan.layer.fadeTx = 6 * N / 393; layerWriteRow(X.R.fan.layer, slot, X.st, X.placed.startHz, X.placed.spanHz, X.t, ident); fanRow(X.R.fan, X.R.buf, N, slot, ROWS); }
    X.R.frames++; X.t += 50;
  };
  const VC = 1_000_000, VS = 600_000, ZC = 1_160_000, ZS = 50_000, ZN = 30;
  const scene = (X: LR) => {
    for (let i = 0; i < 100; i++) pushL(X, VC, VS);
    for (let i = 0; i < ZN; i++) pushL(X, ZC, ZS);    // zoomed in, away from most stations — the newest ZN rows
  };
  const L = mkL(true), O = mkL(false); seed = 21; scene(L); seed = 21; scene(O);
  const pl = draw(L.R), po = draw(O.R);
  // ★ The ZOOM EASE rows (ZN-1 … ZN-ZOOM_EASE_ROWS+1 rows down) are drawn at in-between zooms, wider than their own data:
  //   the layer fills around the detailed strip — black only on an older server
  const yE0 = (ZN - ZOOM_EASE_ROWS + 1) * ROW_PX, yE1 = (ZN - 1) * ROW_PX;
  const blackIn = (px: Uint8Array) => { let k = 0; for (let y = yE0; y < yE1; y++) for (let x = 0; x < W; x += 3) if (px[(y * W + x) * 4] === 0 && px[(y * W + x) * 4 + 1] === 0 && px[(y * W + x) * 4 + 2] === 0) k++; return k; };
  const bl = blackIn(pl), bo = blackIn(po);
  ok(`the layer, on the real shader: a zoom's eased lines have no black around the detailed strip (${bl} px)`, bl === 0);
  ok(`an older server (no SPWB) — those lines continue the history below instead of black (${bo} px)`, bo === 0);
  // the history (below the zoom) keeps every strong station where it was drawn
  const strong = stations.filter((st) => st.lvl > 0.8).map((st) => ({ st, x: (st.f - (VC - VS / 2)) / VS * W })).filter((o) => o.x > 20 && o.x < W - 20);
  let seen = 0, total = 0;
  for (let y = (ZN + 3) * ROW_PX; y < (ZN + 60) * ROW_PX; y += ROW_PX) for (const o of strong) { total++; if (run(pl, y, o.x, Math.round(o.st.lvl * 255 * 0.8), 6)) seen++; }
  ok(`the layer: the history below the zoom shows every strong station where it was drawn (${seen}/${total})`, seen === total && strong.length >= 4);
  if (process.env.WF_LAYER_PNG) {
    lastSurf.length = 0;
    draw(O.R, { lut: lutGqrx }); draw(L.R, { lut: lutGqrx });
    const PW = 600, PH = Math.round(H * PW / W);
    const out = CK.MakeSurface(PW * 2 + 10, PH + 40); const cv = out.getCanvas();
    cv.clear(CK.Color(20, 20, 20, 1));
    lastSurf.forEach((sf, i) => { const im = sf.surf.makeImageSnapshot();
      cv.drawImageRectOptions(im, CK.LTRBRect(0, 0, W, sf.h), CK.LTRBRect(i * (PW + 10), 40, i * (PW + 10) + PW, 40 + PH), CK.FilterMode.Linear, CK.MipmapMode.None, null); });
    const font = new CK.Font(null, 16); const tp = new CK.Paint(); tp.setColor(CK.Color(240, 240, 240, 1));
    ['before (no SPWB): zoom in, then out', 'APP + full-capture layer (SPWB v2)'].forEach((t, i) => cv.drawText(t, i * (PW + 10) + 8, 26, tp, font));
    writeFileSync(process.env.WF_LAYER_PNG, out.makeImageSnapshot().encodeToBytes());
    lastSurf.forEach((sf, i) => writeFileSync(process.env.WF_LAYER_PNG!.replace(/\.png$/, `.full${i}.png`), sf.surf.makeImageSnapshot().encodeToBytes()));
    console.log(`  wrote ${process.env.WF_LAYER_PNG}`);
  }
}

// ── The comparison PNG: reference | settled | mid-glide | before ──
if (process.env.WF_PNG) {
  lastSurf.length = 0;
  draw(A, { lut: lutGqrx }); draw(A, { lut: lutGqrx, cont: 0.5 }); draw(A, { lut: lutGqrx, motion: false });
  const PW = 600, PH = Math.round(H * PW / W);
  const out = CK.MakeSurface(PW * 4 + 30, PH + 40); const cv = out.getCanvas();
  cv.clear(CK.Color(20, 20, 20, 1));
  const ref = process.env.WF_REF ? CK.MakeImageFromEncoded(readFileSync(process.env.WF_REF)) : null;
  if (ref) { const sy = 340, sh2 = Math.min(ref.height() - sy, Math.round(PH * ref.width() / PW));
    cv.drawImageRectOptions(ref, CK.LTRBRect(0, sy, ref.width(), sy + sh2), CK.LTRBRect(0, 40, PW, 40 + PH * sh2 / Math.round(PH * ref.width() / PW)), CK.FilterMode.Linear, CK.MipmapMode.None, null); }
  lastSurf.forEach((s, i) => { const im = s.surf.makeImageSnapshot();
    cv.drawImageRectOptions(im, CK.LTRBRect(0, 0, W, s.h), CK.LTRBRect((i + 1) * (PW + 10), 40, (i + 1) * (PW + 10) + PW, 40 + PH), CK.FilterMode.Linear, CK.MipmapMode.None, null); });
  const font = new CK.Font(null, 16); const tp = new CK.Paint(); tp.setColor(CK.Color(240, 240, 240, 1));
  ['reference (first build)', 'NEW settled', 'NEW mid-glide (120 Hz frame)', 'before: same-x blend'].forEach((t, i) => cv.drawText(t, i * (PW + 10) + 8, 26, tp, font));
  writeFileSync(process.env.WF_PNG, out.makeImageSnapshot().encodeToBytes());
  // the three panels at full device resolution too (WF_PNG with .fullN before .png)
  lastSurf.forEach((s, i) => writeFileSync(process.env.WF_PNG!.replace(/\.png$/, `.full${i}.png`), s.surf.makeImageSnapshot().encodeToBytes()));
  console.log(`  wrote ${process.env.WF_PNG}`);
}

console.log(`  waterfall shader: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
