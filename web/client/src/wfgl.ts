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
varying vec2 vUv;
void main() {
  // Top of the output (vUv.y = 0) is the newest row = head; walk down uVisible rows into the ring.
  // The ring is taller than the display so a resize never reallocates it — history is just preserved.
  float ry = mod(uHead + vUv.y * uVisible, uRows);
  float ty = (ry + 0.5) / uRows;
  float a = texture2D(uRing, vec2(vUv.x, ty)).r;
  float b = texture2D(uRing, vec2(vUv.x + 1.0 / uCols, ty)).r;   // peak-preserve across the bin
  float v = max(a, b);
  gl_FragColor = texture2D(uLut, vec2(v, 0.5));
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
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  get element(): HTMLCanvasElement { return this.canvas; }
}
