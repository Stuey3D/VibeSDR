/**
 * THE APP'S OWN RENDER of a golden scenario: replays <name>.in.bin through ref_pipeline.ts (the app's TS modules) and, at
 * the requested display frames, draws the ring through the app's REAL shader — WF_SKSL extracted from
 * src/components/WaterfallView.tsx, run by CanvasKit (Skia's CPU backend), as scripts/test_wf_shader_motion.ts does —
 * with the uniforms WaterfallView would set. Writes <out>/<name>_<t>_app.png and .rgba (raw, for compare_png.ts).
 * Run: node --no-warnings --import ./ts_resolve.mjs render_app.ts <goldenDir> <name> <outDir> <W> <H> <palette> <tMs>...
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { RefPipeline, RING_W, RING_ROWS } from './ref_pipeline.ts';
import { getColorLUT } from '../../src/assets/colormapUtils.ts';
import { motionImageRows } from '../../src/utils/wfRowPlace.ts';
import type { WfScroll } from '../../src/assets/wfRowTarget.ts';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '../..');
const [goldenDir, name, outDir, Ws, Hs, palName, ...ts] = process.argv.slice(2);
const W = +Ws, H = +Hs, snaps = ts.map(Number);
const require = createRequire(join(ROOT, 'package.json'));
const ckDir = join(ROOT, 'node_modules/canvaskit-wasm/bin');
if (!existsSync(join(ckDir, 'canvaskit.js'))) { console.log('  render_app: SKIPPED (canvaskit-wasm not installed)'); process.exit(0); }
const CK = await require(join(ckDir, 'canvaskit.js'))({ locateFile: (f: string) => join(ckDir, f) });
const src = readFileSync(join(ROOT, 'src/components/WaterfallView.tsx'), 'utf8');
const SKSL = src.slice(src.indexOf('const WF_SKSL = `') + 17, src.indexOf('}`;\nconst WF_EFFECT') + 1);
const EFFECT = CK.RuntimeEffect.Make(SKSL, (e: string) => { throw new Error(e); });
const lut = getColorLUT(palName);

const d = readFileSync(join(goldenDir, `${name}.in.bin`)); let p = 4;
const u8 = () => d[p++], f64 = () => { const v = d.readDoubleLE(p); p += 8; return v; }, i32 = () => { const v = d.readInt32LE(p); p += 4; return v; };
const scroll = (['min2', 'low', 'sharp', 'eco', 'default', 'smooth'] as WfScroll[])[u8()]; const displayHz = f64();
const P = new RefPipeline({ scroll, displayHz, traceW: W });
let next = 0;
for (;;) {
  const t = u8();
  if (t === 0) break;
  if (t === 1) { const tt = f64(), c = f64(), s = f64(), n = i32(); const b = new Uint8Array(d.subarray(p, p + n)); p += n; P.pushFrameU8(b, true, c, s, tt); }
  else if (t === 2) { f64(); P.setView(f64(), f64()); }
  else if (t === 3) P.noteInteraction(f64());
  else {
    const tt = f64(), gap = f64(); P.tick(tt, gap);
    if (next < snaps.length && tt >= snaps[next]) { render(snaps[next]); next++; }
  }
}

function render(tag: number) {
  // WaterfallView's layout maths at dpr 1: rows = clamp(round(wfH), 64, 1024), wfRenderH = wfH + ceil(wfH / rows) + 2,
  // devRowPx = 1, shaderRows = wfRenderH, uRowPx = 1, uColPx = W / RING_W.
  const rows = Math.min(1024, Math.max(64, Math.round(H)));
  const drawH = H + Math.ceil(H / rows) + 2;
  const u = P.uniforms();
  const img = CK.MakeImage({ width: RING_W, height: RING_ROWS + motionImageRows(RING_ROWS, RING_W), alphaType: CK.AlphaType.Opaque,
                             colorType: CK.ColorType.Gray_8, colorSpace: CK.ColorSpace.SRGB }, P.image(), RING_W);
  const li = CK.MakeImage({ width: 256, height: 1, alphaType: CK.AlphaType.Opaque, colorType: CK.ColorType.RGBA_8888, colorSpace: CK.ColorSpace.SRGB }, lut, 1024);
  const wf = img.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const lu = li.makeShaderOptions(CK.TileMode.Decal, CK.TileMode.Decal, CK.FilterMode.Nearest, CK.MipmapMode.None);
  const uni = [u.headF, u.frac, 1, u.quant, RING_ROWS, drawH, RING_W, W, drawH, u.sharp, u.contrast, u.moLo, u.moHi, 1, W / RING_W, u.zA, u.zB];
  const sh = EFFECT.makeShaderWithChildren(uni, [wf, lu]);
  const surf = CK.MakeSurface(W, H); const paint = new CK.Paint(); paint.setShader(sh);
  surf.getCanvas().drawRect(CK.LTRBRect(0, 0, W, H), paint);
  const px = surf.getCanvas().readPixels(0, 0, { width: W, height: H, colorType: CK.ColorType.RGBA_8888, alphaType: CK.AlphaType.Unpremul, colorSpace: CK.ColorSpace.SRGB });
  const base = join(outDir, `${name}_${W}x${H}_${tag}_app`);
  writeFileSync(base + '.png', surf.makeImageSnapshot().encodeToBytes());
  writeFileSync(base + '.rgba', px);
  console.log(`  app render ${base}.png  (uHead ${u.headF}, quant ${u.quant}, motion ${u.moLo}..${u.moHi})`);
  surf.delete(); paint.delete(); sh.delete(); wf.delete(); lu.delete(); img.delete(); li.delete();
}
