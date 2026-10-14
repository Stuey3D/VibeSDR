/**
 * vibesdr.net — THE LIVE BUZZER. A real recording, drawn by the real renderer.
 *
 * ★★★ WHY IT EXISTS (Stuart, 2026-10-04): people criticised the Sonar Green + orange VFO look — and every
 *     screenshot uses it, because it is his favourite (a nod to his years as a Royal Navy submarine sonar
 *     operator). Screenshots can only ever show one look. This lets a visitor try their own: palette, VFO
 *     colour, brightness, contrast, sharpness — on a live signal, not a picture.
 * ★★★ NOTHING HERE IS A MOCK-UP. The pixels come from the web client's own Waterfall (web/client/src/
 *     waterfall.ts) and SignalProcessor and palette tables (src/assets), the same code a listener's browser
 *     runs against a VibeServer. Only the SOURCE differs: instead of a server's FFT frames, the rows are
 *     computed here from raw IQ — demo/buzzer.cs8, The Buzzer (UVB-76) on 4625 kHz, captured on the Pi 500's
 *     Airspy HF+ at 21:56 BST on 4 October 2026 (commit 707c29c7; its loop seam is cut in a gap between
 *     buzzes, so the 13.75 s loop does not show a join).
 * ★★ CHEAP BY DESIGN. 165 kB of IQ, 275 FFTs once at load, then one push per 50 ms. No socket, no server,
 *    nothing for a visitor's phone to do that a waterfall in the app would not.
 * ★ THE PAGE BACKGROUND FOLLOWS THE PICKER (Stuart's request): the hero spectrogram keeps its dB bytes and is
 *   repainted through the same palette, brightness and contrast — see paintBackground.
 */
import { Waterfall } from '../client/src/waterfall';
import { COLORMAP_NAMES, getColorLUT } from '../../src/assets/colormapUtils';

interface ClipMeta {
  sampleRate: number; centreHz: number; loopSamples: number;
  suggestedFft: { size: number; rowsPerSec: number };
}

const VFO_HZ = 4625000;                  // The Buzzer, tuned USB as in the app's own screenshots
const PASS_LO = 100, PASS_HI = 2800;     // USB passband, Hz from the carrier — drives the acrylic sidebands
const DEFAULT_LOOK = { palette: 'Sonar Green', vfo: '#ff8c1a', bright: 0, contrast: 0, sharp: 3 };
const VFO_SWATCHES: [string, string][] = [
  ['Orange', '#ff8c1a'], ['Red', '#ff2020'], ['White', '#ffffff'],
  ['Green', '#39ff6a'], ['Blue', '#3da5ff'], ['Magenta', '#ff4dff'], ['Amber', '#ffc21a'],
];

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T | null;

// ── DSP: radix-2 FFT, Blackman-Harris window ─────────────────────────────────────────────────────────
function fft(re: Float32Array, im: Float32Array) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const p = i + k, q = p + len / 2;
        const tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
        re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti;
        const nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

/** Every row of the loop, in dBFS, low frequency first. The loop is circular, so the last rows read past
 *  the end into the start — exactly what playing it round again does. */
function computeRows(iq: Int8Array, meta: ClipMeta): Float32Array[] {
  const N = meta.suggestedFft.size, L = meta.loopSamples;
  const hop = Math.round(meta.sampleRate / meta.suggestedFft.rowsPerSec);
  const win = new Float32Array(N);
  let wsum = 0;
  for (let i = 0; i < N; i++) {
    const x = (2 * Math.PI * i) / (N - 1);
    win[i] = 0.35875 - 0.48829 * Math.cos(x) + 0.14128 * Math.cos(2 * x) - 0.01168 * Math.cos(3 * x);
    wsum += win[i];
  }
  const norm = 20 * Math.log10(wsum * 128);   // int8 full scale through the window = 0 dBFS
  const re = new Float32Array(N), im = new Float32Array(N);
  const rows: Float32Array[] = [];
  for (let start = 0; start < L; start += hop) {
    for (let i = 0; i < N; i++) {
      const s = ((start + i) % L) * 2;
      re[i] = iq[s] * win[i]; im[i] = iq[s + 1] * win[i];
    }
    fft(re, im);
    const row = new Float32Array(N);
    for (let k = 0; k < N; k++) {
      const b = (k + N / 2) % N;              // fftshift: negative frequencies first
      row[k] = 10 * Math.log10(re[b] * re[b] + im[b] * im[b] + 1e-12) - norm;
    }
    rows.push(row);
  }
  return rows;
}

/** USB at the VFO, as audio: mix the carrier to 0 Hz, keep +PASS_LO..+PASS_HI with a complex band-pass,
 *  take the real part, then up-sample ×8 to 48 kHz (6 kHz buffers are not accepted everywhere). */
function demodUsb(iq: Int8Array, meta: ClipMeta): Float32Array<ArrayBuffer> {
  const fs = meta.sampleRate, L = meta.loopSamples;
  const shift = (2 * Math.PI * (meta.centreHz - VFO_HZ)) / fs;
  const I = new Float32Array(L), Q = new Float32Array(L);
  for (let n = 0; n < L; n++) {
    const c = Math.cos(shift * n), s = Math.sin(shift * n), x = iq[2 * n], y = iq[2 * n + 1];
    I[n] = x * c - y * s; Q[n] = x * s + y * c;
  }
  const taps = 129, mid = (taps - 1) / 2, half = (PASS_HI - PASS_LO) / 2 / fs, f0 = (PASS_HI + PASS_LO) / 2 / fs;
  const hr = new Float32Array(taps), hi = new Float32Array(taps);
  for (let k = 0; k < taps; k++) {
    const m = k - mid;
    const lp = (m === 0 ? 2 * half : Math.sin(2 * Math.PI * half * m) / (Math.PI * m))
      * (0.54 - 0.46 * Math.cos((2 * Math.PI * k) / (taps - 1)));
    hr[k] = lp * Math.cos(2 * Math.PI * f0 * m); hi[k] = lp * Math.sin(2 * Math.PI * f0 * m);
  }
  const a = new Float32Array(L);
  let peak = 1e-9;
  for (let n = 0; n < L; n++) {
    let acc = 0;
    for (let k = 0; k < taps; k++) {
      const j = (n - k + L) % L;            // circular, so the loop plays without a click
      acc += I[j] * hr[k] - Q[j] * hi[k];
    }
    a[n] = acc; if (Math.abs(acc) > peak) peak = Math.abs(acc);
  }
  const up = 8, out = new Float32Array(L * up), g = 0.7 / peak;
  for (let n = 0; n < L; n++) {
    const x0 = a[n] * g, x1 = a[(n + 1) % L] * g;
    for (let k = 0; k < up; k++) out[n * up + k] = x0 + ((x1 - x0) * k) / up;
  }
  return out;
}

// ── The page background, repainted through the chosen look ───────────────────────────────────────────
/** Set by the page's own spectrogram loader once its data has arrived (website/index.html). */
interface SpgData { u8: Uint8Array; off: number; bins: number; rows: number; lo: number; hi: number; cv: HTMLCanvasElement }
declare global { interface Window { vibeSpg?: SpgData; vibeSpgPaint?: () => void } }

let look = { ...DEFAULT_LOOK };

function paintBackground() {
  const d = window.vibeSpg;
  if (!d) return;
  const lut = getColorLUT(look.palette), c = Math.max(-1, Math.min(1, look.contrast / 10));
  const g = d.cv.getContext('2d');
  if (!g) return;
  const img = g.createImageData(d.bins, d.rows);
  for (let row = 0; row < d.rows; row++) {
    const src = d.off + row * d.bins, y = d.rows - 1 - row;
    for (let b = 0; b < d.bins; b++) {
      // ★ Same order as the waterfall: brightness in dB before the window, then the shader's S-curve mix.
      let t = Math.max(0, Math.min(1, (d.u8[src + b] + look.bright - d.lo) / (d.hi - d.lo)));
      t = Math.pow(t, 0.8);
      const sc = t * t * (3 - 2 * t);
      t = c > 0 ? t + (sc - t) * c : t + (t * 0.5 + 0.25 - t) * -c;
      const li = Math.max(0, Math.min(255, Math.round(t * 255))) * 4, p = (y * d.bins + b) * 4;
      img.data[p] = lut[li]; img.data[p + 1] = lut[li + 1]; img.data[p + 2] = lut[li + 2]; img.data[p + 3] = 255;
    }
  }
  g.putImageData(img, 0, 0);
}

// ── Wiring ───────────────────────────────────────────────────────────────────────────────────────────
async function main() {
  const canvas = $<HTMLCanvasElement>('demoCanvas');
  const box = $('demoBox');
  if (!canvas || !box) return;

  const base = canvas.dataset.src || 'assets/demo/';
  const [meta, raw] = await Promise.all([
    fetch(base + 'buzzer.json').then((r) => r.json() as Promise<ClipMeta>),
    fetch(base + 'buzzer.cs8').then((r) => r.arrayBuffer()),
  ]);
  const iq = new Int8Array(raw);
  const rows = computeRows(iq, meta);
  const rowMs = 1000 / meta.suggestedFft.rowsPerSec;

  const wf = new Waterfall(canvas, { specRatio: 0.35, palette: look.palette });
  wf.vfoHz = VFO_HZ; wf.filterLow = PASS_LO; wf.filterHigh = PASS_HI;
  wf.vfoColor = look.vfo;
  wf.applySettings({ wfBrightness: look.bright, wfContrast: look.contrast, wfSharpness: look.sharp, peakHold: false });
  // ★ Inspection only (#demodebug): the renderer's state, for a headless check. Nothing reads it otherwise.
  if (location.hash.includes('demodebug')) (window as unknown as { __demoWf: Waterfall }).__demoWf = wf;
  window.addEventListener('resize', () => wf.resize());
  new ResizeObserver(() => wf.resize()).observe(canvas);

  // ── Controls ──
  const pal = $<HTMLSelectElement>('demoPalette');
  if (pal) {
    for (const n of [...COLORMAP_NAMES].sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }))) {
      const o = document.createElement('option'); o.value = n; o.textContent = n; pal.appendChild(o);
    }
    pal.value = look.palette;
    pal.onchange = () => { look.palette = pal.value; wf.setPalette(pal.value); paintBackground(); };
  }
  const sw = $('demoVfo');
  if (sw) {
    for (const [name, hex] of VFO_SWATCHES) {
      const b = document.createElement('button');
      b.type = 'button'; b.className = 'sw'; b.title = name; b.setAttribute('aria-label', `VFO colour ${name}`);
      b.style.background = hex;
      b.setAttribute('aria-pressed', String(hex === look.vfo));
      b.onclick = () => {
        look.vfo = hex; wf.vfoColor = hex;
        sw.querySelectorAll('button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
      };
      sw.appendChild(b);
    }
  }
  const slider = (id: string, key: 'bright' | 'contrast' | 'sharp', apply: (v: number) => void) => {
    const el = $<HTMLInputElement>(id), out = $(id + 'Val');
    if (!el) return;
    el.value = String(look[key]);
    const show = () => { if (out) out.textContent = (key !== 'sharp' && look[key] > 0 ? '+' : '') + look[key]; };
    show();
    el.oninput = () => { look[key] = Number(el.value); apply(look[key]); show(); };
  };
  slider('demoBright', 'bright', (v) => { wf.applySettings({ wfBrightness: v }); paintBackground(); });
  slider('demoContrast', 'contrast', (v) => { wf.applySettings({ wfContrast: v }); paintBackground(); });
  slider('demoSharp', 'sharp', (v) => wf.applySettings({ wfSharpness: v }));

  // ── Sound: off until asked, as every browser requires — and nobody wants a buzzer on page load ──
  const listen = $<HTMLButtonElement>('demoListen');
  let ac: AudioContext | null = null, src: AudioBufferSourceNode | null = null, audio: Float32Array<ArrayBuffer> | null = null;
  let audioStart = 0;
  if (listen) listen.onclick = async () => {
    if (src) { src.stop(); src = null; listen.textContent = 'Listen'; listen.setAttribute('aria-pressed', 'false'); return; }
    ac ??= new AudioContext();
    audio ??= demodUsb(iq, meta);
    const buf = ac.createBuffer(1, audio.length, meta.sampleRate * 8);
    buf.copyToChannel(audio, 0);
    src = ac.createBufferSource(); src.buffer = buf; src.loop = true;
    src.connect(ac.destination);
    await ac.resume();
    // ★ Start the sound where the picture is, so the buzz you hear is the line arriving at the top.
    const pos = ((performance.now() - t0) % (rows.length * rowMs)) / 1000;
    src.start(0, pos); audioStart = ac.currentTime - pos;
    listen.textContent = 'Stop'; listen.setAttribute('aria-pressed', 'true');
  };

  // ── Loop: one row per 50 ms of wall time, renderer ticks every frame ──
  // ★ While the sound plays the AUDIO clock is the master, so picture and buzz never drift apart.
  const t0 = performance.now();
  let lastRow = -1;
  const frame = () => {
    const ms = src && ac ? (ac.currentTime - audioStart) * 1000 : performance.now() - t0;
    const r = Math.floor(ms / rowMs);
    if (r !== lastRow) {
      // A tab that slept skips ahead rather than flooding the renderer with the rows it missed.
      if (lastRow < 0 || r - lastRow > 10 || r < lastRow) lastRow = r - 1;
      for (let k = lastRow + 1; k <= r; k++) {
        const row = rows[((k % rows.length) + rows.length) % rows.length];
        wf.push(row, meta.centreHz, meta.sampleRate);
      }
      lastRow = r;
    }
    wf.tick(); wf.draw();
    requestAnimationFrame(frame);
  };
  box.classList.add('on');
  requestAnimationFrame(frame);

  // The background may already be waiting, or may arrive later — either way it gets this look.
  window.vibeSpgPaint = paintBackground;
  paintBackground();
}

main().catch(() => { /* no demo, no claim: the static caption and screenshots still stand */ });
