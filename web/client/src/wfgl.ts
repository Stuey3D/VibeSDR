/**
 * wfgl.ts — WebGL waterfall renderer (the GPU path for Waterfall).
 *
 * The 2D path (waterfall.ts) writes each coloured row with putImageData and unwraps the scroll ring
 * with two drawImage blits every frame — all CPU/main-thread work that scales with the PIXEL count, so
 * a Retina canvas pays 4× and a dpr-3 phone pays 9×. This moves the whole thing to the GPU:
 *
 *   • The history is a RING TEXTURE, `bins` wide × `rows` tall, single channel (the raw dB index 0-255,
 *     a LUMINANCE texture — see ensureRing for why that is colour-identical to the old RGBA ring).
 *     A new row is one texSubImage2D of the bins — NO palette loop, NO downsample, NO per-pixel JS.
 *   • A fullscreen quad + fragment shader unwraps the ring (a `head` uniform, free) and colours it
 *     through the palette held as a 256×1 LUT texture. The GPU scales bins→width and does dpr for free,
 *     so full crispness costs the same as half — the reason the Detail control could be retired.
 *   • Peak-preserving: the shader maxes two adjacent bins per pixel so a narrow carrier isn't sampled
 *     away between texels (the 2D path's exact per-bucket max, approximated cheaply on the GPU).
 *
 * The renderer draws into its OWN (offscreen) canvas; Waterfall composites it onto the visible canvas
 * with a single drawImage, keeping all the spectrum-trace/marker/axis 2D code untouched. If a WebGL
 * context can't be created the constructor throws and Waterfall falls back to the 2D path.
 */

const VERT = `
attribute vec2 aPos;
uniform vec2 uMap;         // ★ the zoom fan (wfZoomFan.ts): this run of rows reads u = uMap.x * x + uMap.y
uniform vec2 uMapNext;     // ★ the map of the run BELOW this one — its first row is this run's last row's blend partner
uniform float uVisible;    // how many rows the display actually shows (the waterfall pixel height)
varying vec2 vUv;
varying highp float vU;
varying highp float vU2;   // the same x through uMapNext
varying highp float vRow;  // display row (0 = newest at the top), fractional — floor() is the row
void main() {
  // aPos in [-1,1]; vUv in [0,1] with y=0 at the TOP (newest row).
  vUv = vec2((aPos.x + 1.0) * 0.5, (1.0 - aPos.y) * 0.5);
  vRow = vUv.y * uVisible;
  // ★ Worked out here, at the quad's corners, in highp — linear in x, so the interpolation is exact.
  vU = uMap.x * vUv.x + uMap.y;
  vU2 = uMapNext.x * vUv.x + uMapNext.y;
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const FRAG = `
precision mediump float;
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define HP highp
#else
#define HP mediump
#endif
uniform sampler2D uRing;   // LUMINANCE, ringW × ringH — the dB index (WebGL1 samples L as .rgb)
uniform sampler2D uLut;    // 256×1 RGBA palette
// ★ V12 the full-capture layer (wfWide.ts), in CAPTURE space, one row per ring row (the same row index as uRing):
uniform sampler2D uFloorT; // F × ringH LUMINANCE, linear: the floor (palette index; 0 = no data)
uniform sampler2D uPeakT;  // C × ringH LUMINANCE_ALPHA, nearest: per cell the peak's index (0 = none) and its shape byte
uniform sampler2D uMT;     // 1 × ringH RGBA, nearest: the row's m (24 bits — wfWide.encodeM)
uniform float uHasCoarse;  // 0 until the first full-capture row (an older server never sends one)
uniform HP float uX;       // this run's k (row span ÷ capture span): u_capture = k·u + m. 0 = no layer
uniform HP float uXNext;   // the next run's
uniform HP float uCells;   // C
uniform HP float uPxX;     // one output pixel in x units (1 / width)
uniform float uFade;       // the cross-fade at a detailed row's edge, in x units (a few pixels)
uniform HP float uHead;    // newest row index in the ring
uniform HP float uRows;       // ring height (fixed, generous)
uniform float uCols;       // ring width (bins)
uniform float uSharp;      // 0 = off; unsharp-mask amount (see below)
uniform float uContrast;   // -1..1 S-curve mix (see below)
uniform float uTaps;       // ★ zoom fan: samples per pixel for a run drawn SHRUNK (zoomed out past it) — 1 otherwise
uniform float uTapsNext;   // the same for uMapNext
uniform HP vec2 uMapF;     // this run's map again, for layerAt (its own name: a uniform shared with the vertex shader must match its precision)
uniform HP vec2 uMapNextF;
uniform float uTapDu;      // u between those samples (one output pixel's worth at the identity)
uniform HP float uRunEnd;  // display row where this run ends (exclusive) — its last row blends with the next run's first
varying vec2 vUv;
varying HP float vU;       // the row's own u for this x — wfZoomFan.ts (identity until a zoom)
varying HP float vU2;      // this x through the NEXT run's map
varying HP float vRow;

// One sample of the ring at (u, ty): blank outside the row's own extent, the brightest of the bins a pixel covers when the
// run is drawn shrunk, then the along-frequency unsharp mask. ty may sit between two ring rows — the LINEAR filter blends.
float ringAt(HP float ux, HP float ty, float taps) {
  // ★★ THE ZOOM FAN: outside the row's own extent there is no data — blank (the palette floor, as a cleared ring), never
  //    a smear of the edge bin. That is the "slight gap at a newly exposed edge" the hard rule allows, and nothing else.
  if (ux < 0.0 || ux > 1.0) return 0.0;
  // ★★★ ONE BIN, NOT THE BRIGHTER OF TWO. This used to sample the NEXT bin along as well and keep
  //     the larger — "peak-preserve" — which paints every carrier a bin wider than it is. That is a
  //     horizontal DILATION: on a busy band each signal grows into its neighbour, the gaps between
  //     them fill in, and the whole display reads brighter and softer than it should. It also
  //     fights the unsharp mask below, which then has a smeared neighbourhood to work against.
  //     ★ The app's shader (WaterfallView.tsx, WF_SKSL) samples a single point, and the app is the
  //     sharper of the two on the same signal — Stuart, comparing side by side on the same server:
  //     "server appears brighter and softer", "that waterfall is the best I've ever seen".
  float v = texture2D(uRing, vec2(ux, ty)).r;
  // ★ A run shrunk by a zoom out spans several of its own bins per pixel; one sample would step over a one-bin carrier
  //   and the old trail would flicker in and out. The brightest of the bins the pixel covers, as resampleRow keeps it.
  //   One sample (the loop never runs) for every run at or above its own scale — the single-bin rule above stands.
  if (taps > 1.0) {
    for (int i = 1; i < 8; i++) {
      if (float(i) >= taps) break;
      float uu = ux + float(i) * uTapDu;
      if (uu <= 1.0) v = max(v, texture2D(uRing, vec2(uu, ty)).r);
    }
  }
  // ★★ UNSHARP MASK — the web waterfall was visibly SOFTER than the app's for the
  // same signal, and the sharpness slider did nothing because nothing here read it
  // (Stuart, 2026-07-28). The app sharpens in its SkSL shader; this is the WebGL
  // equivalent.
  //
  // ★ ALONG FREQUENCY ONLY. The detail a DXer is looking for is a narrow carrier —
  // one bin wide, many rows tall — so blurring across TIME to sharpen would fight
  // the very thing being sharpened, and would also make the scroll shimmer. The
  // neighbours are one bin either side; at the edges the sample clamps to itself,
  // which yields zero correction rather than a bright rim.
  if (uSharp > 0.0) {
    float dx = 1.0 / uCols;
    float l = texture2D(uRing, vec2(max(ux - dx, 0.0), ty)).r;
    float r = texture2D(uRing, vec2(min(ux + dx, 1.0), ty)).r;
    // v - blur, i.e. how much this bin stands above its neighbourhood.
    float hi = v - (l + r) * 0.5;
    v = clamp(v + hi * uSharp, 0.0, 1.0);
  }
  return v;
}

// ★ V12 THE FULL-CAPTURE LAYER (wfWide.ts): the detailed row where it has data, the full-capture layer everywhere else —
//   black only where the radio was not capturing. Read through the SAME map (the fan, the bend and a resize move both
//   together). Where the detailed row's edge is ON SCREEN the two cross-fade over uFade (no hard-edged block).
// ★★ v2, "the same waterfall, just less sharp": the layer is the FLOOR (linear between its bins) with every PEAK drawn
//    as a line at its own frequency, its own width — at least 0.6 px, so a thin carrier stays a thin line at any zoom.
HP float rowM(HP float tyRow) {
  vec4 t = texture2D(uMT, vec2(0.5, tyRow));
  HP vec3 b = floor(t.rgb * 255.0 + 0.5);
  return (b.r * 65536.0 + b.g * 256.0 + b.b) / 16777215.0 * 4.0 - 2.0;
}
float coarseAt(HP float u, HP float k, HP float a, HP float tyRow) {
  HP float uc = k * u + rowM(tyRow);
  if (uc < 0.0 || uc > 1.0) return -1.0;
  float f = texture2D(uFloorT, vec2(uc, tyRow)).r;
  if (f < 0.5 / 255.0) return -1.0;
  float v = f;
  HP float pxU = max(k * abs(a) * uPxX, 1e-7);   // one screen pixel, in capture fraction
  HP float cell = floor(uc * uCells);
  for (int d = -2; d <= 2; d++) {
    HP float ci = cell + float(d);
    if (ci < 0.0 || ci >= uCells) continue;
    vec4 p = texture2D(uPeakT, vec2((ci + 0.5) / uCells, tyRow));
    if (p.r < 0.5 / 255.0) continue;
    float sh = floor(p.a * 255.0 + 0.5);
    float off = mod(sh, 16.0), w16 = floor(sh / 16.0) + 1.0;
    HP float c0 = (ci + (off + 0.5) / 16.0) / uCells;
    HP float hw = max(0.6 * pxU, w16 / 32.0 / uCells);
    if (abs(uc - c0) <= hw) v = max(v, p.r);
  }
  return v;
}
// How far inside the detailed row's ON-SCREEN edges this pixel is, 0..1 over uFade (1 = fully detailed).
float edgeE(HP vec2 map) {
  HP float x = vUv.x;
  HP float xl = -map.y / map.x, xr = (1.0 - map.y) / map.x;
  float e = 1.0;
  if (xl > 0.0) e = min(e, float((x - xl) / uFade));
  if (xr < 1.0) e = min(e, float((xr - x) / uFade));
  return clamp(e, 0.0, 1.0);
}
// One ring row (at its exact centre tyRow): detailed, the layer, or the cross-fade between them.
float layerRow(HP float u, HP vec2 map, HP float k, HP float tyRow, float taps) {
  bool inside = u >= 0.0 && u <= 1.0;
  float d = inside ? ringAt(u, tyRow, taps) : 0.0;
  if (uHasCoarse < 0.5 || k <= 0.0) return d;
  float e = inside ? edgeE(map) : 0.0;
  if (e >= 1.0) return d;
  float c = coarseAt(u, k, map.x, tyRow);
  if (c < 0.0) return d;
  return inside ? mix(c, d, e) : c;
}

void main() {
  // Top of the output (vUv.y = 0) is the newest row = head; walk down uVisible rows into the ring.
  // The ring is taller than the display so a resize never reallocates it — history is just preserved.
  // ★★ THE SOFT VERTICAL LOOK IS RESTORED (Stuart, 2026-10-28 02:02, beta26: "the sharpness on tuning looks broken").
  //    Display row j has always shown a 50/50 blend of ring rows head+j and head+j+1 (the old addressing sampled exactly
  //    between them through the LINEAR filter). 1130bbe14 took that away to make the zoom fan's rows exact; it is back:
  //    • inside a run (both rows share one map) the hardware does the blend in ONE fetch at ty between the two rows —
  //      exactly the old picture at the old cost;
  //    • the LAST row of a run blends with the FIRST row of the next run, each read through its OWN map (two fetches),
  //      and so does the row where the ring wraps — so the fan stays correct at every boundary.
  //    ★ Addressed in highp where there is any: in mediump (2560 rows > 2048) the row could be off by one outright.
  HP float j = floor(vRow);
  HP float r0 = mod(uHead + j, uRows);
  float v;
  if (j < uRunEnd - 1.0 && r0 < uRows - 1.0) {
    // ★ inside the detailed rows (the common case): the one-fetch blend, exactly as before; the layer is per row (each row
    //   has its own m), so where it shows the two rows are read separately and averaged
    if (uHasCoarse < 0.5 || uX <= 0.0 || (vU >= 0.0 && vU <= 1.0 && edgeE(uMapF) >= 1.0))
      v = ringAt(vU, (r0 + 1.0) / uRows, uTaps);
    else
      v = 0.5 * (layerRow(vU, uMapF, uX, (r0 + 0.5) / uRows, uTaps) + layerRow(vU, uMapF, uX, (r0 + 1.5) / uRows, uTaps));
  } else {
    HP float r1 = mod(r0 + 1.0, uRows);
    v = 0.5 * (layerRow(vU, uMapF, uX, (r0 + 0.5) / uRows, uTaps) + layerRow(vU2, uMapNextF, uXNext, (r1 + 0.5) / uRows, uTapsNext));
  }

  // ★★★ CONTRAST — THE SAME BUG AS THE SHARPNESS SLIDER ABOVE, in the same file, one control
  // along. wfContrast was handed to applySettings and stored, and NOTHING HERE EVER READ IT: the
  // manual contrast slider under Brightness moved from end to end with no visible change at all,
  // on the Pi and on Android alike, because they share this web client (Stuart, 2026-07-31).
  //
  // ★★ Ported from the app's SkSL so the two look the same on the same signal — an S-CURVE mix,
  // not a multiply:
  //     positive → mix toward smoothstep(raw): darks darker, brights brighter, midtones spread
  //     negative → mix toward a flattened ramp: everything pulled to the middle
  // A plain gain would clip the strong signals off the top of the palette, which on a waterfall
  // means losing the very carriers the user turned contrast up to see.
  float raw = clamp(v, 0.0, 1.0);
  float sc  = raw * raw * (3.0 - 2.0 * raw);
  v = uContrast > 0.0 ? mix(raw, sc, uContrast)
                      : mix(raw, raw * 0.5 + 0.25, -uContrast);
  gl_FragColor = texture2D(uLut, vec2(clamp(v, 0.0, 1.0), 0.5));
}`;

function compile(gl: WebGLRenderingContext, type: number, src: string): WebGLShader {
  const s = gl.createShader(type)!;
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
    throw new Error('wfgl shader: ' + gl.getShaderInfoLog(s));
  }
  return s;
}

export class WaterfallGL {
  private gl: WebGLRenderingContext;
  private prog: WebGLProgram;
  private quad: WebGLBuffer;
  private ring: WebGLTexture;
  private lut: WebGLTexture;
  private ringW = 0;
  private ringH = 0;
  private uHead: WebGLUniformLocation;
  private uRows: WebGLUniformLocation;
  private uVisible: WebGLUniformLocation;
  private uCols: WebGLUniformLocation;
  private uSharp: WebGLUniformLocation;
  private uContrast: WebGLUniformLocation;
  private uMap: WebGLUniformLocation;
  private uTaps: WebGLUniformLocation;
  private uTapDu: WebGLUniformLocation;
  private uMapNext: WebGLUniformLocation;
  private uTapsNext: WebGLUniformLocation;
  private uRunEnd: WebGLUniformLocation;
  private uFloorT: WebGLUniformLocation;
  private uPeakT: WebGLUniformLocation;
  private uMT: WebGLUniformLocation;
  private uCells: WebGLUniformLocation;
  private uPxX: WebGLUniformLocation;
  private uHasCoarse: WebGLUniformLocation;
  private uX: WebGLUniformLocation;
  private uXNext: WebGLUniformLocation;
  private uFade: WebGLUniformLocation;
  private uMapF: WebGLUniformLocation;
  private uMapNextF: WebGLUniformLocation;
  /** ★ V12 the full-capture layer (wfWide.ts): three rings beside the detailed one — the floor (F wide), the peaks (C wide,
   *  two bytes a cell) and each row's m (1 wide, RGBA) — allocated at the first full-capture row (an older server never
   *  sends one, and the layer then costs nothing). `blank` is a 1×1 zero for the samplers until then. */
  private floorT: WebGLTexture | null = null;
  private peakT: WebGLTexture | null = null;
  private mT: WebGLTexture | null = null;
  private layerF = 0;
  private layerC = 0;
  private layerH = 0;
  private blank: WebGLTexture;
  // ★ Looked up ONCE. render() used to call getAttribLocation + getUniformLocation ×2 every frame;
  //   they are string lookups on the program and cost a driver round-trip on some stacks.
  private aPos: number;
  private uRing: WebGLUniformLocation;
  private uLut: WebGLUniformLocation;
  /** 0…10 from the UI, mapped to the mask amount at draw time. */
  sharpness = 0;
  /** −10..10 from the slider; the shader takes −1..1. See the S-curve in the fragment shader. */
  contrast = 0;

  constructor(private canvas: HTMLCanvasElement) {
    // preserveDrawingBuffer keeps the backbuffer valid for the drawImage composite that follows the
    // render in the same frame. (Turning it off was tried to tame Chromium's GPU clocks but the win
    // wasn't worth the risk of a blank waterfall — Safari, the primary target, is happy either way.)
    const gl = (canvas.getContext('webgl', { alpha: false, antialias: false, depth: false,
      preserveDrawingBuffer: true }) ||
      canvas.getContext('experimental-webgl', { alpha: false })) as WebGLRenderingContext | null;
    if (!gl) throw new Error('no webgl');
    this.gl = gl;

    const prog = gl.createProgram()!;
    gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VERT));
    gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      throw new Error('wfgl link: ' + gl.getProgramInfoLog(prog));
    }
    this.prog = prog;

    this.quad = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);

    this.ring = gl.createTexture()!;
    this.lut = gl.createTexture()!;
    this.uHead = gl.getUniformLocation(prog, 'uHead')!;
    this.uRows = gl.getUniformLocation(prog, 'uRows')!;
    this.uVisible = gl.getUniformLocation(prog, 'uVisible')!;
    this.uCols = gl.getUniformLocation(prog, 'uCols')!;
    this.uSharp = gl.getUniformLocation(prog, 'uSharp')!;
    this.uContrast = gl.getUniformLocation(prog, 'uContrast')!;
    this.uMap = gl.getUniformLocation(prog, 'uMap')!;
    this.uTaps = gl.getUniformLocation(prog, 'uTaps')!;
    this.uTapDu = gl.getUniformLocation(prog, 'uTapDu')!;
    this.uMapNext = gl.getUniformLocation(prog, 'uMapNext')!;
    this.uTapsNext = gl.getUniformLocation(prog, 'uTapsNext')!;
    this.uRunEnd = gl.getUniformLocation(prog, 'uRunEnd')!;
    this.uFloorT = gl.getUniformLocation(prog, 'uFloorT')!;
    this.uPeakT = gl.getUniformLocation(prog, 'uPeakT')!;
    this.uMT = gl.getUniformLocation(prog, 'uMT')!;
    this.uCells = gl.getUniformLocation(prog, 'uCells')!;
    this.uPxX = gl.getUniformLocation(prog, 'uPxX')!;
    this.uHasCoarse = gl.getUniformLocation(prog, 'uHasCoarse')!;
    this.uX = gl.getUniformLocation(prog, 'uX')!;
    this.uXNext = gl.getUniformLocation(prog, 'uXNext')!;
    this.uFade = gl.getUniformLocation(prog, 'uFade')!;
    this.uMapF = gl.getUniformLocation(prog, 'uMapF')!;
    this.uMapNextF = gl.getUniformLocation(prog, 'uMapNextF')!;
    this.blank = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, this.blank);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, 1, 1, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, new Uint8Array(1));
    this.aPos = gl.getAttribLocation(prog, 'aPos');
    this.uRing = gl.getUniformLocation(prog, 'uRing')!;
    this.uLut = gl.getUniformLocation(prog, 'uLut')!;
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  }

  /** Upload the palette LUT (256×4 RGBA, as getColorLUT returns). */
  setLUT(lut: Uint8Array) {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, lut);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  /** Allocate (or grow) the ring texture. The ring is deliberately taller than any display so a window
   *  resize NEVER reallocates it — history is preserved because the ring is untouched; the display just
   *  reads more or fewer of its newest rows. Only a bin-count change or growing past the current height
   *  reallocates (and clears), both rare. */
  ensureRing(cols: number, rows: number) {
    if (cols === this.ringW && rows <= this.ringH) return;
    const gl = this.gl;
    const h = Math.max(rows, this.ringH);   // grow-only in height
    const next = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, next);
    // ★★ LUMINANCE, ONE BYTE PER TEXEL — was RGBA with the index in R and G/B unused, 4× the VRAM
    //    (1024 × 2560 ring: 10.5 MB → 2.6 MB) and a per-row JS expansion loop in pushRow.
    //    ★ COLOUR-IDENTICAL: WebGL1 samples a LUMINANCE texel L as vec4(L, L, L, 1), so the shader's
    //      `.r` reads exactly the byte it read from R before (same UNSIGNED_BYTE /255 normalisation,
    //      same LINEAR filter between the same byte values). Alpha was never read.
    //    ★ The old comment here said RGBA was needed because an FBO copied the ring on resize. That
    //      FBO no longer exists — the ring is never rendered INTO (resize leaves it alone), so it
    //      does not need to be colour-renderable. LUMINANCE is core WebGL1: every device that can
    //      make this context can make this texture.
    //    ★ null = zero-filled by the WebGL spec (no 10 MB JS array). Index 0 = palette floor.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, cols, h, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.deleteTexture(this.ring);
    this.ring = next;
    this.ringW = cols;
    this.ringH = h;
  }

  get rows(): number { return this.ringH; }

  /** Wipe the ring to the palette floor (clearHistory). Called on every discontinuous retune.
   *  ★ Passes null: WebGL guarantees a texture allocated from null reads as zeros, so this no longer
   *    builds a ring-sized JS array (10.5 MB under the old RGBA ring) on every jump for the GC. */
  clear() {
    const gl = this.gl;
    if (!this.ringW) return;
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, this.ringW, this.ringH, 0,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, null);
    if (this.floorT) this.layerH = 0;   // ★ re-made (zeroed) by the next ensureLayer
  }

  /** ★ V12 the full-capture layer's rings: allocated (zeroed) at F floor bins and C cells × the ring's height; re-made if
   *  the ring grew or was cleared. */
  ensureLayer(F: number, C: number) {
    if (this.floorT && this.layerF === F && this.layerC === C && this.layerH === this.ringH) return;
    const gl = this.gl;
    if (!this.ringH) return;
    const make = (old: WebGLTexture | null, w: number, fmt: number, linear: boolean) => {
      if (old) gl.deleteTexture(old);
      const t = gl.createTexture()!;
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texImage2D(gl.TEXTURE_2D, 0, fmt, w, this.ringH, 0, fmt, gl.UNSIGNED_BYTE, null);
      const f = linear ? gl.LINEAR : gl.NEAREST;
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, f);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, f);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return t;
    };
    this.floorT = make(this.floorT, F, gl.LUMINANCE, true);
    this.peakT = make(this.peakT, C, gl.LUMINANCE_ALPHA, false);
    this.mT = make(this.mT, 1, gl.RGBA, false);
    this.layerF = F; this.layerC = C; this.layerH = this.ringH;
  }
  get hasCoarse(): boolean { return !!this.floorT && this.layerH === this.ringH; }
  /** One row of the layer at `headRow` (the same row as the detailed ring's): floor indices (F), (index, shape) per cell
   *  (2C), and the row's m (4 bytes). */
  pushLayer(floor: Uint8Array, peaks: Uint8Array, m: Uint8Array, headRow: number) {
    const gl = this.gl;
    if (!this.hasCoarse || floor.length !== this.layerF || peaks.length !== 2 * this.layerC || headRow >= this.layerH) return;
    gl.bindTexture(gl.TEXTURE_2D, this.floorT);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, headRow, this.layerF, 1, gl.LUMINANCE, gl.UNSIGNED_BYTE, floor);
    gl.bindTexture(gl.TEXTURE_2D, this.peakT);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, headRow, this.layerC, 1, gl.LUMINANCE_ALPHA, gl.UNSIGNED_BYTE, peaks);
    gl.bindTexture(gl.TEXTURE_2D, this.mT);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, headRow, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, m);
  }

  /** Upload one row of dB indices (length must equal the ring width) at `headRow`. Uploaded as-is
   *  (LUMINANCE, one byte per bin; UNPACK_ALIGNMENT is 1) — the shader samples .r and colours through
   *  the palette LUT. */
  pushRow(row: Uint8Array, headRow: number) {
    const gl = this.gl;
    if (!this.ringW || row.length !== this.ringW) return;
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, headRow, this.ringW, 1, gl.LUMINANCE, gl.UNSIGNED_BYTE, row);
  }

  /** Render the waterfall into this renderer's canvas at outW×outH. `head` is the newest row and
   *  `visibleRows` is how many rows of the ring to show (the waterfall pixel height).
   *  ★ `runs` (wfZoomFan.mapRunsX: [j0, j1, a, b, X] per run, display rows from the top) draws each run of rows through its
   *    own map — the zoom fan. Omitted / one identity run = the single full-screen draw this always was. */
  render(head: number, visibleRows: number, outW: number, outH: number, runs?: Float64Array, nRuns = 0) {
    const gl = this.gl;
    if (!this.ringW) return;
    if (this.canvas.width !== outW || this.canvas.height !== outH) {
      this.canvas.width = outW; this.canvas.height = outH;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, outW, outH);
    gl.useProgram(this.prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.enableVertexAttribArray(this.aPos);
    gl.vertexAttribPointer(this.aPos, 2, gl.FLOAT, false, 0, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.uniform1i(this.uRing, 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(this.uLut, 1);
    const has = this.hasCoarse;
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, has ? this.floorT : this.blank); gl.uniform1i(this.uFloorT, 2);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, has ? this.peakT : this.blank); gl.uniform1i(this.uPeakT, 3);
    gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D, has ? this.mT : this.blank); gl.uniform1i(this.uMT, 4);
    gl.uniform1f(this.uHasCoarse, has ? 1 : 0);
    gl.uniform1f(this.uCells, Math.max(1, this.layerC));
    gl.uniform1f(this.uPxX, 1 / Math.max(1, outW));
    gl.uniform1f(this.uFade, 12 / Math.max(1, outW));   // ★ ~12 px cross-fade at a detailed row's on-screen edge
    gl.uniform1f(this.uHead, head);
    gl.uniform1f(this.uRows, this.ringH);
    gl.uniform1f(this.uVisible, Math.min(visibleRows, this.ringH));
    gl.uniform1f(this.uCols, this.ringW);
    // ★★ THE APP'S EXACT MAPPING, so the same slider position means the same thing in
    // both clients. WaterfallView.tsx: uSharp = base × (wfSharpness / 5), with base
    // chosen per frame rate (3 at 30fps, 2 at 20fps, 1.5 native). The web has no
    // frame-interpolation blur to compensate for, so it takes the 20fps base — the
    // app's common case — giving 2.0 at the default slider 5 and 4.0 at 10.
    // ★ The mask itself is the same formula the SkSL shader uses:
    //   c + uSharp * (c - (l + r) * 0.5)
    // ★ LINEAR, not squared. The app's curve used to be quadratic and the bottom half
    // of the slider did nothing (slider 2 = 0.16× base, imperceptible); it was made
    // linear so every step moves something. Do not reintroduce the curve here.
    const SHARP_BASE = 2.0;
    gl.uniform1f(this.uSharp, Math.max(0, Math.min(10, this.sharpness)) / 5 * SHARP_BASE);
    // ★ /10 to reach the shader's −1..1, matching WaterfallView.tsx:370 exactly.
    gl.uniform1f(this.uContrast, Math.max(-1, Math.min(1, this.contrast / 10)));
    gl.uniform1f(this.uTapDu, 1 / Math.max(1, outW));
    const taps = (a: number) => a > 1.01 ? Math.min(8, Math.ceil(a)) : 1;
    // ★ Each run carries its own map AND the next run's (its last row blends with that run's first — see the shader).
    const setMap = (a: number, b: number, an: number, bn: number, end: number, x = 0, xn = 0) => {
      gl.uniform1f(this.uX, x);
      gl.uniform1f(this.uXNext, xn);
      gl.uniform2f(this.uMapF, a, b);
      gl.uniform2f(this.uMapNextF, an, bn);
      gl.uniform2f(this.uMap, a, b);
      gl.uniform1f(this.uTaps, taps(a));
      gl.uniform2f(this.uMapNext, an, bn);
      gl.uniform1f(this.uTapsNext, taps(an));
      gl.uniform1f(this.uRunEnd, end);
    };
    const vis = Math.max(1, Math.min(visibleRows, this.ringH));
    // ★ runs are [j0, j1, a, b, X] quintuples (wfZoomFan.mapRunsX).
    const R = 5;
    if (!runs || nRuns <= 1) {
      const a = runs && nRuns === 1 ? runs[2] : 1, b = runs && nRuns === 1 ? runs[3] : 0, x = runs && nRuns === 1 ? runs[4] : 0;
      setMap(a, b, a, b, 1e9, x, x);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      return;
    }
    // ★ One scissored draw per run: the fill is still exactly one screen, only the uniforms change between runs.
    //   Display row j is GL row (outH − 1 − j) — GL counts from the bottom. Rows map 1:1 to pixels (visible = outH).
    const scale = outH / vis;
    gl.enable(gl.SCISSOR_TEST);
    for (let i = 0; i < nRuns; i++) {
      const o = i * R;
      const y0 = Math.round(runs[o] * scale), y1 = Math.round(runs[o + 1] * scale);
      if (y1 <= y0) continue;
      gl.scissor(0, outH - y1, outW, y1 - y0);
      const last = i === nRuns - 1;
      const a = runs[o + 2], b = runs[o + 3], x = runs[o + 4];
      setMap(a, b, last ? a : runs[o + R + 2], last ? b : runs[o + R + 3], last ? 1e9 : runs[o + 1], x, last ? x : runs[o + R + 4]);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    gl.disable(gl.SCISSOR_TEST);
  }

  get element(): HTMLCanvasElement { return this.canvas; }
}
