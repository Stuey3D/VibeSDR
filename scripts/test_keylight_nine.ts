// test_keylight_nine.ts — THE FIXED BACKLIGHT (constants/keyLight.ts lightNine / ninePieces), PROVEN IN PIXELS.
//
// The key light used to be drawn at each key's exact size. Now one canonical picture is cut into pieces and the edge
// strips stretched. This renders BOTH with real Skia (CanvasKit, headless) at many key sizes — rest light and pressed
// flood, metal cap keys — and compares them pixel by pixel. A seam between pieces, a misplaced bottom step or a
// stretched corner would all show up as a large difference.
//
//   node --no-warnings scripts/test_keylight_nine.ts
import { createRequire } from 'node:module';
import { drawKeyLight, keyLightSprite, lightNine, ninePieces, domeCap, type RR } from '../src/constants/keyLight.ts';

const require = createRequire(import.meta.url);
const CanvasKitInit = require('canvaskit-wasm');
const CK = await CanvasKitInit();

let pass = 0, fail = 0;
const ok = (name: string, cond: boolean, detail = '') => { if (cond) pass++; else { fail++; console.log(`FAIL ${name}${detail ? '  ' + detail : ''}`); } };

// ── An adapter: drawKeyLight speaks the React Native Skia API; CanvasKit is the same library with a different face. ──
const STYLE = [CK.PaintStyle.Fill, CK.PaintStyle.Stroke];
const CLIP = [CK.ClipOp.Difference, CK.ClipOp.Intersect];
const Sk: any = {
  RRectXY: (r: any, rx: number, ry: number) => CK.RRectXY(r, rx, ry),
  XYWHRect: (x: number, y: number, w: number, h: number) => CK.XYWHRect(x, y, w, h),
  Color: (s: string) => CK.parseColorString(s),
  MaskFilter: { MakeBlur: (_s: number, sigma: number, ctm: boolean) => CK.MaskFilter.MakeBlur(CK.BlurStyle.Normal, sigma, ctm) },
  Paint: () => {
    const p = new CK.Paint();
    return {
      _p: p,
      setAntiAlias: (b: boolean) => p.setAntiAlias(b),
      setColor: (c: any) => p.setColor(c),
      setMaskFilter: (m: any) => p.setMaskFilter(m),
      setStyle: (s: number) => p.setStyle(STYLE[s]),
      setStrokeWidth: (w: number) => p.setStrokeWidth(w),
    };
  },
};
const wrap = (c: any) => ({
  save: () => c.save(), restore: () => c.restore(),
  clipRRect: (r: any, op: number, aa: boolean) => c.clipRRect(r, CLIP[op], aa),
  clipRect: (r: any, op: number, aa: boolean) => c.clipRect(r, CLIP[op], aa),
  drawRRect: (r: any, p: any) => c.drawRRect(r, p._p),
});

const PR = 3;   // render at the iPhone's 3x, where seams would be finest
const led = (a: number) => `rgba(60,140,255,${a})`, hot = (a: number) => `rgba(200,225,255,${a})`;

function draw(W: number, H: number, fn: (c: any) => void) {
  const s = CK.MakeSurface(Math.ceil(W * PR), Math.ceil(H * PR));
  const c = s.getCanvas(); c.clear(CK.TRANSPARENT); c.scale(PR, PR); fn(c);
  const img = s.makeImageSnapshot(); s.delete?.(); return img;
}
function pixels(img: any, w: number, h: number) {
  return img.readPixels(0, 0, { width: w, height: h, colorType: CK.ColorType.RGBA_8888, alphaType: CK.AlphaType.Premul, colorSpace: CK.ColorSpace.SRGB });
}

function compare(label: string, cut: RR, cap: RR, reach: number, pressed: boolean, travel: number) {
  const sp = keyLightSprite(cut, reach);
  const n = lightNine(cut, cap, reach, pressed ? travel : 0);
  if (!n) { ok(`${label}: sliceable`, false, `${cut.w}x${cut.h}`); return; }
  // The exact light, as today.
  const exact = draw(sp.w, sp.h, c => drawKeyLight(Sk, wrap(c) as any, cut, cap, reach, led, hot, pressed, travel));
  // The canonical picture, then its pieces drawn into a live-sized surface, edges stretched.
  const sp0 = keyLightSprite(n.cut, reach);
  const canon = draw(sp0.w, sp0.h, c => drawKeyLight(Sk, wrap(c) as any, n.cut, n.cap, reach, led, hot, pressed, travel));
  const sliced = draw(sp.w, sp.h, c => {
    const p = new CK.Paint();
    for (const pc of ninePieces(n, reach, sp.w, sp.h)) {
      c.drawImageRectOptions(canon,
        CK.XYWHRect(pc.src.x * PR, pc.src.y * PR, pc.src.w * PR, pc.src.h * PR),
        CK.XYWHRect(pc.dst.x, pc.dst.y, pc.dst.w, pc.dst.h), CK.FilterMode.Linear, CK.MipmapMode.None, p);
    }
  });
  const w = Math.ceil(sp.w * PR), h = Math.ceil(sp.h * PR);
  const A = pixels(exact, w, h), B = pixels(sliced, w, h);
  let max = 0, sum = 0, lit = 0;
  for (let i = 0; i < A.length; i += 4) {
    const d = Math.max(Math.abs(A[i] - B[i]), Math.abs(A[i + 1] - B[i + 1]), Math.abs(A[i + 2] - B[i + 2]), Math.abs(A[i + 3] - B[i + 3]));
    if (A[i + 3] > 8) { lit++; sum += d; }
    max = Math.max(max, d);
  }
  const mean = lit ? sum / lit : 0;
  // ★ SEAMS: along every join between pieces (and the half-way step) the two must agree closely — a gap or a doubled
  //   edge between pieces is 60+. Measured on the pixel rows / columns either side of each join, away from corners.
  const joinsX = [n.cx, sp.w - n.cx].map(v => Math.round(v * PR)), joinsY = [n.cy, sp.h / 2, sp.h - n.cy].map(v => Math.round(v * PR));
  let seam = 0;
  const at = (x: number, y: number) => { const i = (y * w + x) * 4; return Math.max(Math.abs(A[i] - B[i]), Math.abs(A[i + 1] - B[i + 1]), Math.abs(A[i + 2] - B[i + 2]), Math.abs(A[i + 3] - B[i + 3])); };
  for (const jx of joinsX) for (let y = Math.round(n.cy * PR); y < h - Math.round(n.cy * PR); y++) for (const x of [jx - 1, jx]) if (x >= 0 && x < w) seam = Math.max(seam, at(x, y));
  for (const jy of joinsY) for (let x = Math.round(n.cx * PR); x < w - Math.round(n.cx * PR); x++) for (const y of [jy - 1, jy]) if (y >= 0 && y < h) seam = Math.max(seam, at(x, y));
  ok(`${label}: no seam at the joins (≤ 12)`, seam <= 12, `seam ${seam}`);
  // ★ The CORNERS may sit up to half a pixel from where the exact drawing put them (a key size on a half-point): a
  //   1 pt line softens a little there — max ≤ 40 of 255, and the light overall within 2.2 levels on average.
  //   Stuart: "slightly uneven illumination just makes it more real".
  ok(`${label}: no pixel off by more than 40`, max <= 40, `max ${max}`);
  ok(`${label}: mean difference over lit pixels ≤ 2.2`, mean <= 2.2, `mean ${mean.toFixed(2)}`);
  return { max, mean };
}

// Metal cap keys (domeCap inset 2 / 1.5) and the outline-style slot keys, portrait and landscape sizes, both reaches.
const sizes: Array<[number, number, number]> = [[86, 44, 10], [90, 54, 10], [74, 28, 6], [64, 26, 6], [120, 40, 8], [44, 44, 8], [200, 60, 10]];
let worst = { max: 0, mean: 0 };
for (const [w, h, r] of sizes) for (const reach of [4, 3, 2.25]) for (const pressed of [false, true]) {
  const cut: RR = { x: 0, y: 0, w, h, r };
  const cap = domeCap(w, h, r, false);
  const res = compare(`cap ${w}x${h} r${r} reach ${reach}${pressed ? ' pressed' : ''}`, cut, cap, reach, pressed, 1.5);
  if (res) worst = { max: Math.max(worst.max, res.max), mean: Math.max(worst.mean, res.mean) };
}
// Too small to slice: lightNine says so (the key keeps its exact light).
ok('a key too small for the corners is not sliced', lightNine({ x: 0, y: 0, w: 12, h: 10, r: 6 }, domeCap(12, 10, 6, false), 4) === null);

console.log(`worst: max ${worst.max}, mean ${worst.mean.toFixed(2)}`);
console.log(`${fail ? 'FAIL' : 'ok '}  keylight nine: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
