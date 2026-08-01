/**
 * wfgl.ts — WebGL waterfall renderer (the GPU path for Waterfall).
 *
 * The 2D path (waterfall.ts) writes each coloured row with putImageData and unwraps the scroll ring
 * with two drawImage blits every frame — all CPU/main-thread work that scales with the PIXEL count, so
 * a Retina canvas pays 4× and a dpr-3 phone pays 9×. This moves the whole thing to the GPU:
 *
 *   • The history is a RING TEXTURE, `bins` wide × `rows` tall, single channel (the raw dB index 0-255).
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
varying vec2 vUv;
void main() {
  // aPos in [-1,1]; vUv in [0,1] with y=0 at the TOP (newest row).
  vUv = vec2((aPos.x + 1.0) * 0.5, (1.0 - aPos.y) * 0.5);
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const FRAG = `
precision mediump float;
uniform sampler2D uRing;   // RGBA, ringW × ringH — the dB index in R
uniform sampler2D uLut;    // 256×1 RGBA palette
uniform float uHead;       // newest row index in the ring
uniform float uRows;       // ring height (fixed, generous)
uniform float uVisible;    // how many rows the display actually shows (the waterfall pixel height)
uniform float uCols;       // ring width (bins)
uniform float uSharp;      // 0 = off; unsharp-mask amount (see below)
uniform float uContrast;   // -1..1 S-curve mix (see below)
varying vec2 vUv;
void main() {
  // Top of the output (vUv.y = 0) is the newest row = head; walk down uVisible rows into the ring.
  // The ring is taller than the display so a resize never reallocates it — history is just preserved.
  float ry = mod(uHead + vUv.y * uVisible, uRows);
  float ty = (ry + 0.5) / uRows;
  // ★★★ ONE BIN, NOT THE BRIGHTER OF TWO. This used to sample the NEXT bin along as well and keep
  //     the larger — "peak-preserve" — which paints every carrier a bin wider than it is. That is a
  //     horizontal DILATION: on a busy band each signal grows into its neighbour, the gaps between
  //     them fill in, and the whole display reads brighter and softer than it should. It also
  //     fights the unsharp mask below, which then has a smeared neighbourhood to work against.
  //     ★ The app's shader (WaterfallView.tsx, WF_SKSL) samples a single point, and the app is the
  //     sharper of the two on the same signal — Stuart, comparing side by side on the same server:
  //     "server appears brighter and softer", "that waterfall is the best I've ever seen".
  float v = texture2D(uRing, vec2(vUv.x, ty)).r;

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
    float l = texture2D(uRing, vec2(max(vUv.x - dx, 0.0), ty)).r;
    float r = texture2D(uRing, vec2(min(vUv.x + dx, 1.0), ty)).r;
    // v - blur, i.e. how much this bin stands above its neighbourhood.
    float hi = v - (l + r) * 0.5;
    v = clamp(v + hi * uSharp, 0.0, 1.0);
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
  /** 0…10 from the UI, mapped to the mask amount at draw time. */
  sharpness = 0;
  /** −10..10 from the slider; the shader takes −1..1. See the S-curve in the fragment shader. */
  contrast = 0;
  // Reusable RGBA upload buffer (the dB index packed into R). The ring is RGBA, not LUMINANCE, so it
  // is a color-RENDERABLE format — the FBO that preserves history across a resize needs that.
  private rgba: Uint8Array | null = null;

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
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, cols, h, 0, gl.RGBA, gl.UNSIGNED_BYTE,
      new Uint8Array(cols * h * 4));    // index 0 = palette floor
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.deleteTexture(this.ring);
    this.ring = next;
    this.ringW = cols;
    this.ringH = h;
    this.rgba = new Uint8Array(cols * 4);
  }

  get rows(): number { return this.ringH; }

  /** Wipe the ring to the palette floor (clearHistory). */
  clear() {
    const gl = this.gl;
    if (!this.ringW) return;
    const zero = new Uint8Array(this.ringW * this.ringH * 4);
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, this.ringW, this.ringH, 0,
      gl.RGBA, gl.UNSIGNED_BYTE, zero);
  }

  /** Upload one row of dB indices (length must equal the ring width) at `headRow`. The index goes in
   *  the R channel — the shader samples .r and colours through the palette LUT. */
  pushRow(row: Uint8Array, headRow: number) {
    const gl = this.gl;
    if (!this.ringW || row.length !== this.ringW || !this.rgba) return;
    const rgba = this.rgba;
    for (let i = 0; i < this.ringW; i++) { const o = i << 2; rgba[o] = row[i]; rgba[o + 3] = 255; }
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, headRow, this.ringW, 1, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
  }

  /** Render the waterfall into this renderer's canvas at outW×outH. `head` is the newest row and
   *  `visibleRows` is how many rows of the ring to show (the waterfall pixel height). */
  render(head: number, visibleRows: number, outW: number, outH: number) {
    const gl = this.gl;
    if (!this.ringW) return;
    if (this.canvas.width !== outW || this.canvas.height !== outH) {
      this.canvas.width = outW; this.canvas.height = outH;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, outW, outH);
    gl.useProgram(this.prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    const loc = gl.getAttribLocation(this.prog, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.ring);
    gl.uniform1i(gl.getUniformLocation(this.prog, 'uRing'), 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(gl.getUniformLocation(this.prog, 'uLut'), 1);
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
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  get element(): HTMLCanvasElement { return this.canvas; }
}
