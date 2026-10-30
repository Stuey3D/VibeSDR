/**
 * render_wf_spin.ts — PICTURES of the APP's waterfall (WaterfallView's real SkSL shader, CanvasKit) through a run of tune
 * steps and zoom steps (v12-wf-spin): an AM band (a carrier every 9 kHz), a 200 kHz view at 1052 kHz, a 10 fps feed, the
 * fixed clock at Default 30 — and, where this tree has it, the interaction boost (wfClock.ts boostRowRate) on a 120 Hz or
 * 60 Hz panel. Runs in an older tree too (no boost, no slope cap there): copy it to <tree>/scripts and run it there.
 *   WFSPIN_OUT=<dir> WFSPIN_TAG=after WFSPIN_HZ=120 node --no-warnings scripts/render_wf_spin.ts
 * Nothing is asserted (scripts/test_wf_spin.ts does that).
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as RP from '../src/utils/wfRowPlace.ts';
import * as CL from '../src/assets/wfClock.ts';
import { COLORMAPS_256 } from '../src/assets/colormaps.ts';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = process.env.WFSPIN_OUT!; mkdirSync(OUT, { recursive: true });
const TAG = process.env.WFSPIN_TAG ?? 'after', HZ = +(process.env.WFSPIN_HZ ?? 120);
const require = createRequire(join(ROOT, 'package.json'));
const ckDir = join(ROOT, 'node_modules/canvaskit-wasm/bin');
if (!existsSync(join(ckDir, 'canvaskit.js'))) { console.log('canvaskit-wasm not installed'); process.exit(0); }
const CK = await require(join(ckDir, 'canvaskit.js'))({ locateFile: (f: string) => join(ckDir, f) });
const src = readFileSync(join(ROOT, 'src/components/WaterfallView.tsx'), 'utf8');
const SKSL = src.slice(src.indexOf('const WF_SKSL = `') + 17, src.indexOf('}`;\nconst WF_EFFECT') + 1);
const EFFECT = CK.RuntimeEffect.Make(SKSL, (e: string) => console.log(e));
const lut = new Uint8Array(256 * 4);
{ const p = COLORMAPS_256.gqrx; for (let i = 0; i < 256; i++) lut.set([parseInt(p.slice(i * 6, i * 6 + 2), 16), parseInt(p.slice(i * 6 + 2, i * 6 + 4), 16), parseInt(p.slice(i * 6 + 4, i * 6 + 6), 16), 255], i * 4); }

const N = 1024, ROWS = 1024;
let seed = 11; const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const stations: { f: number; lvl: number }[] = [];
for (let f = 531_000; f <= 1_602_000; f += 9_000) if (rnd() < 0.55) stations.push({ f, lvl: 0.55 + rnd() * 0.4 });
function bandRow(centre: number, span: number): Uint8Array {
  const r = new Uint8Array(N), hz = span / N, lo = centre - span / 2, v = new Float32Array(N);
  for (let i = 0; i < N; i++) v[i] = 0.24 + rnd() * 0.16;
  for (const s of stations) {
    const c = (s.f - lo) / hz; if (c < -80 || c > N + 80) continue;
    const half = 4_500 / hz;
    for (let i = Math.max(0, Math.floor(c - half)); i <= Math.min(N - 1, Math.ceil(c + half)); i++) {
      const d = Math.abs(i - c) / half; v[i] = Math.max(v[i], s.lvl * (0.72 - 0.25 * d) * (0.75 + 0.35 * rnd()));
    }
    const ci = Math.round(c); for (let k = -1; k <= 1; k++) if (ci + k >= 0 && ci + k < N) v[ci + k] = Math.max(v[ci + k], s.lvl * (k === 0 ? 1 : 0.85));
  }
  for (let i = 0; i < N; i++) r[i] = Math.max(1, Math.min(255, Math.round(v[i] * 255)));
  return r;
}

type Ev = { t: number; c: number; s: number };
const C0 = 1_052_000, S0 = 200_000;
const steps = (n: number, every: number, dc: number): Ev[] => Array.from({ length: n }, (_, i) => ({ t: 3000 + i * every, c: C0 + (i + 1) * dc, s: S0 }));
const zooms = (ks: number[], every: number): Ev[] => { let s = S0; return ks.map((k, i) => ({ t: 3000 + i * every, c: C0, s: (s *= k) })); };
const SCEN: Record<string, Ev[]> = {
  'a-500Hz-steps': steps(8, 330, -500), 'b-9k-step': steps(1, 1, -9_000), 'c-9k-fast-spin': steps(15, 125, -9_000),
  'd-9k-slow-spin': steps(6, 500, -9_000), 'e-100k-step': steps(1, 1, -100_000), 'f-zoom-sqrt2-key': zooms([Math.SQRT1_2], 1),
  'g-zoom-sqrt2-x4-quick': zooms([Math.SQRT1_2, Math.SQRT1_2, Math.SQRT1_2, Math.SQRT1_2], 150), 'h-zoom-x2-rungs': zooms([0.5, 0.5, 2, 2], 700),
  'i-zoom-x4': zooms([0.25], 1),
};
const hasBoost = typeof (CL as any).boostRowRate === 'function';
for (const [name, ev] of Object.entries(SCEN)) {
  seed = 11;
  const buf = new Uint8Array(N * (ROWS + RP.MOTION_ROWS)), ms = RP.newMotionState(ROWS, N), fan = RP.newFanState(ROWS);
  const clk = CL.newRowClock(), bs = hasBoost ? (CL as any).newBoost() : null;
  const frames: { c: number; s: number }[] = []; let fr = 0, ei = 0, view = { c: C0, s: S0 }, lastIn = 0, lastEase = 0;
  const end = ev[ev.length - 1].t + 2500, out: number[] = [];
  for (let t = 0; t <= end; t++) {
    while (ei < ev.length && ev[ei].t <= t) { view = { c: ev[ei].c, s: ev[ei].s }; lastIn = t; ei++; }
    if (t % 100 === 0) {
      const id = frames.push({ ...view }) - 1;
      let rate = 30;
      if (bs) {
        const el = fan.easeLeft, started = el > lastEase; lastEase = el;
        if (lastIn > bs.lastMove && t - lastIn < (CL as any).BOOST_TAIL_MS) (CL as any).boostNote(bs, lastIn);
        if (started) (CL as any).boostNote(bs, t);
        if ((CL as any).boostActive(bs, t, el > 0)) rate = (CL as any).boostRowRate(30, 10, HZ);
      }
      fan.rowRate = rate;
      CL.clockSetRate(clk, rate, t); CL.clockArrive(clk, id, t, lastIn > 0 && t - lastIn < 1000);
    }
    out.length = 0; CL.clockRun(clk, t, out);
    for (const id of out) { const f = frames[id]; RP.placeRow(buf, ROWS, N, fr, bandRow(f.c, f.s), f.c, f.s / N, f.c, f.s, ms, fan, 600); fr++; }
  }
  // the app's draw: 2 device px a row, 420 rows on screen, 800 px wide — the newest at the top
  const W = 800, RPX = 2, VIS = 420, H = VIS * RPX;
  const bytes = RP.wfImageBytes(fan, ms, buf, N, ROWS);
  const img = CK.MakeImage({ width: N, height: ROWS + RP.MOTION_ROWS, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.Gray_8, colorSpace: CK.ColorSpace.SRGB }, bytes, N);
  const li = CK.MakeImage({ width: 256, height: 1, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.RGBA_8888, colorSpace: CK.ColorSpace.SRGB }, lut, 256 * 4);
  const wf = img.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const lu = li.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const u = [fr, 1, 1, 1, ROWS, VIS, N, W, H, 0, 0, ms.lo, ms.hi, 1 / RPX, W / N, 1, 0];
  const sh = EFFECT.makeShaderWithChildren(u, [wf, lu]);
  const surf = CK.MakeSurface(W, H); const paint = new CK.Paint(); paint.setShader(sh);
  surf.getCanvas().drawRect(CK.LTRBRect(0, 0, W, H), paint);
  const f = join(OUT, `${TAG}-app-${hasBoost ? HZ + 'hz' : '30rows'}-${name}.png`);
  writeFileSync(f, surf.makeImageSnapshot().encodeToBytes());
  console.log(`   wrote ${f} (${fr} rows, the boost ${hasBoost ? 'on' : 'absent'})`);
  surf.delete(); paint.delete(); sh.delete(); wf.delete(); lu.delete(); img.delete(); li.delete();
}
