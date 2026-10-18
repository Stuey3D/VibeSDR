/**
 * BINAURAL TUNING — THE WATERFALL CUE ON A SWEEP, from REAL busy-band I/Q (V12, 2026-10-18).
 *
 * Stuart on 482 (SSB): "it's not panning as I tune through the band". Mid-sweep the voice estimator cannot help — every
 * click moves its evidence and it needs seconds of one voice — and the dial's own reckoning needs a voice locked first.
 * So while the knob turns, only the WATERFALL CUE (binauralOffset.ts SsbCoarse → BinauralTracker → setBinauralSsbHint)
 * can lean the audio. This drives the REAL tracker exactly as SDRScreen does — one spectrum frame at a time, 10 a
 * second, with the dial moving in 100 Hz and 500 Hz clicks — over spectra computed from the off-air I/Q at several
 * phone zooms, and judges the cue against the voices whose carriers are known (test-ssb-offset-real kQsos).
 *
 * TWO BANDS. A COMPOSITE (judged): the known voices cut from the recordings, each its own sideband only, laid out 3.2-6 kHz
 * apart at 8-25 dB in Gaussian noise — real speech, exact truth. And the REAL 40/20 m bands as recorded (reported only):
 * wall to wall, and only some carriers known, so a "wrong way" there is often a voice nobody listed.
 *   in view   — frames where ONE known voice fills ≥ 300 Hz of the passband and no other touches it;
 *   cue       — the share of those frames where native holds a cue (the tracker's last valid hint);
 *   right way — …of those, the share whose carrier − dial has the right sign (|truth| ≥ 150 Hz);
 *   error     — the cue's carrier against the truth: median and 90th percentile, Hz (bias: + = into the voice);
 *   on tune   — within ±60 Hz of the voice, the cue says ≥ 250 Hz (native would lean audibly: kCoarseShrinkHz).
 * ★ JUDGED on the composite at every zoom up to COARSE_MAX_BIN_HZ, both paces: on tune ≤ 10 % of frames; cue on ≥ 60 %;
 *   right way ≥ 95 %; 90th-percentile error ≤ 250 Hz + a bin. 188 / 375 Hz bins are reported (no cue there by design).
 *   Measured 2026-10-18 — build 482's cue: right way 89-92 %, 90 % error 640-3070 Hz, on tune ≥ 150 Hz on 27-83 % of
 *   frames; now: right way 99-100 %, 90 % error 157-237 Hz, on tune ≥ 250 Hz on 0-9 %.
 * ★ SILENT: I/Q is read and measured, never played.
 * ★ The I/Q is not in the repo: set VIBE_TEST_AUDIO to the folder holding iq/. Without it: NOT RUN (exit 3), never a pass.
 * ★ VIBE_SWEEP_EVENTS=<dir>: also writes each composite sweep's dial + hint stream (JSON) and the band's I/Q for the native
 *   half — test-ssb-offset-real §7 replays them through the real BinauralTuning against the demodulated audio.
 *   VIBE_SWEEP_CACHE=<dir> keeps the built band between runs.
 *
 * Run: VIBE_TEST_AUDIO=$HOME/VibeSDR-test-audio node --no-warnings scripts/test_binauralSweep.ts
 */
import { openSync, readSync, closeSync, existsSync, writeFileSync, mkdirSync, readFileSync } from 'node:fs';
import { BinauralTracker, COARSE_MAX_BIN_HZ, type BinauralSink } from '../src/services/binauralOffset.ts';
import { MODE_BANDWIDTHS } from '../src/services/sdrProtocol.ts';

const root = process.env.VIBE_TEST_AUDIO;
if (!root || !existsSync(`${root}/iq/uber_40m_7150000_iq192.wav`)) {
  console.log('   NOT RUN — set VIBE_TEST_AUDIO to the folder holding iq/ (not in the repo)');
  process.exit(3);
}

let fails = 0;
const check = (ok: boolean, what: string) => { console.log(`   ${ok ? 'ok  ' : 'FAIL'}   ${what}`); if (!ok) fails++; };

// ── the I/Q ───────────────────────────────────────────────────────────────────────────────────────────────────────
const FS = 192000;
interface Iq { re: Float32Array; im: Float32Array; centre: number; fs: number }
function readIq(file: string, centre: number): Iq {
  const fd = openSync(`${root}/iq/${file}`, 'r');
  const hdr = Buffer.alloc(12); readSync(fd, hdr, 0, 12, 0);
  let pos = 12; let dataAt = -1; let dataLen = 0;
  for (;;) {
    const ch = Buffer.alloc(8);
    if (readSync(fd, ch, 0, 8, pos) !== 8) break;
    const id = ch.toString('ascii', 0, 4); const len = ch.readUInt32LE(4);
    if (id === 'data') { dataAt = pos + 8; dataLen = len; break; }
    pos += 8 + len + (len & 1);
  }
  if (dataAt < 0) throw new Error(`no data chunk in ${file}`);
  const buf = Buffer.alloc(dataLen); readSync(fd, buf, 0, dataLen, dataAt); closeSync(fd);
  const n = dataLen >> 2;
  const re = new Float32Array(n), im = new Float32Array(n);
  for (let i = 0; i < n; i++) { re[i] = buf.readInt16LE(4 * i) / 32768; im[i] = buf.readInt16LE(4 * i + 2) / 32768; }
  return { re, im, centre, fs: FS };
}

// ── a plain radix-2 FFT (test instrument) ─────────────────────────────────────────────────────────────────────────
function fft(re: Float64Array, im: Float64Array) {
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
        const ur = re[i + k], ui = im[i + k];
        const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

/** One waterfall row as a server sends it: `bins` dB values over [viewCentre ± span/2], from two averaged Hann FFTs
 *  at time t (a server's rows are a short average too). */
function spectrumRow(iq: Iq, t: number, nfft: number, viewCentre: number, nBins: number): Float32Array {
  const fs = iq.fs, binHz = fs / nfft;
  const pow = new Float64Array(nfft);
  const re = new Float64Array(nfft), im = new Float64Array(nfft);
  for (let s = 0; s < 2; s++) {
    const a = Math.floor(t * fs) + s * (nfft >> 1);
    for (let i = 0; i < nfft; i++) {
      const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / nfft);
      const k = (a + i) % iq.re.length;   // a composite band loops its 45 s
      re[i] = iq.re[k] * w; im[i] = iq.im[k] * w;
    }
    fft(re, im);
    for (let i = 0; i < nfft; i++) pow[i] += re[i] * re[i] + im[i] * im[i];
  }
  const out = new Float32Array(nBins);
  const sorted = Array.from(pow).sort((x, y) => x - y);
  const floorDb = 10 * Math.log10(sorted[Math.floor(nfft / 5)] / 2 + 1e-20);
  for (let b = 0; b < nBins; b++) {
    const f = viewCentre + (b - nBins / 2) * binHz - iq.centre;   // relative to the I/Q centre
    // ★ outside the recording's own band (its anti-alias skirts and beyond) the row is the floor, never a wrap-around
    if (Math.abs(f) > 0.42 * fs) { out[b] = floorDb; continue; }
    let k = Math.round(f / binHz);
    k = ((k % nfft) + nfft) % nfft;
    out[b] = 10 * Math.log10(pow[k] / 2 + 1e-20);
  }
  return out;
}

// ── the voices whose carriers are known (test-ssb-offset-real kQsos: carrier + its own audio offset) ─────────────
interface Voice { carrier: number; sb: number }
const voice = (c: number, sb: number, base: number): Voice => ({ carrier: c + sb * base, sb });
const KNOWN: Record<string, { centre: number; voices: Voice[] }> = {
  'uber_40m_7150000_iq192.wav': { centre: 7150000,
    voices: [voice(7087000, -1, -2), voice(7127500, -1, -1), voice(7138000, -1, 2), voice(7175000, -1, 12), voice(7191000, -1, 6)] },
  'uber_20m_14250000_iq192.wav': { centre: 14250000,
    voices: [voice(14216000, 1, -8), voice(14224000, 1, -3), voice(14246000, 1, 100), voice(14282000, 1, -45)] },
  'uber_80m_3700000_iq192.wav': { centre: 3700000, voices: [voice(3720000, -1, 0)] },
};
/** The REAL bands as recorded: busy, wall to wall on 40 m, and only some carriers known — so REPORTED, not judged. */
const REAL: (Band & { file: string })[] = [
  { name: '40m real', file: 'uber_40m_7150000_iq192.wav', sb: -1, voices: KNOWN['uber_40m_7150000_iq192.wav'].voices,
    sweeps: [[7082000, 7096000], [7120000, 7145000], [7170000, 7196000]], judged: false },
  { name: '20m real', file: 'uber_20m_14250000_iq192.wav', sb: +1, voices: KNOWN['uber_20m_14250000_iq192.wav'].voices,
    sweeps: [[14210000, 14230000], [14240000, 14252000], [14276000, 14288000]], judged: false },
];
// ★ Where the voice fills ≥ 300 Hz of the passband: LSB voice C−2700…C−300 against D−2700…D−50 (USB mirrored).
const [PB_LO, PB_HI] = [MODE_BANDWIDTHS.usb[0], MODE_BANDWIDTHS.usb[1]];
function voiceInView(v: Voice, dial: number, minOverlap = 300): boolean {
  const vLo = v.sb > 0 ? v.carrier + 300 : v.carrier - 2700, vHi = v.sb > 0 ? v.carrier + 2700 : v.carrier - 300;
  const pLo = v.sb > 0 ? dial + PB_LO : dial - PB_HI, pHi = v.sb > 0 ? dial + PB_HI : dial - PB_LO;
  return Math.min(vHi, pHi) - Math.max(vLo, pLo) >= minOverlap;
}

// ── a sweep ──────────────────────────────────────────────────────────────────────────────────────────────────────
interface Ev { t: number; k: 'dial' | 'hint' | 'src'; v: number; ok?: boolean; dead?: number; full?: number }
interface SweepStats { inView: number; cue: number; right: number; judged: number; errs: number[]; signed: number[]; near: number; nearFalse: number; second: number }

interface Band { name: string; sb: number; voices: Voice[]; sweeps: [number, number][]; judged: boolean }
function sweep(iq: Iq, band: Band, from: number, to: number, stepHz: number, everySec: number, binTarget: number,
               t0: number, events: Ev[] | null): SweepStats {
  const st: SweepStats = { inView: 0, cue: 0, right: 0, judged: 0, errs: [], signed: [], near: 0, nearFalse: 0, second: 0 };
  const nfft = 1 << Math.round(Math.log2(iq.fs / binTarget));
  const binHz = iq.fs / nfft;
  const nBins = Math.min(1024, Math.floor(nfft * 0.85));
  const span = nBins * binHz;
  let hint = { ok: false, hz: 0 };
  let hint2 = { ok: false, hz: 0 };
  const t = () => now;
  let now = 0;
  const sink: BinauralSink = {
    setBinauralSsbHint: (hz, ok) => { hint = { ok, hz }; events?.push({ t: t(), k: 'hint', v: hz, ok }); },
    setBinauralSsbHint2: (hz, ok) => { hint2 = { ok, hz }; },
    setBinauralDial: (hz) => { events?.push({ t: t(), k: 'dial', v: hz }); },
    setBinauralSource: (s, dead, full) => { events?.push({ t: t(), k: 'src', v: s, dead, full }); },
  };
  const tr = new BinauralTracker(sink);
  const dir = Math.sign(to - from);
  const steps = Math.floor(Math.abs(to - from) / stepHz);
  const dur = steps * everySec + 3;   // and 3 s parked at the end
  const mode = band.sb > 0 ? 'usb' : 'lsb';
  const [lo, hi] = band.sb > 0 ? [PB_LO, PB_HI] : [-PB_HI, -PB_LO];
  let viewCentre = NaN;
  for (let f = 0; f * 0.1 < dur; f++) {
    now = f * 0.1;
    const dial = from + dir * stepHz * Math.min(steps, Math.floor(now / everySec));
    // the view follows the dial the way a phone does: re-centred when the dial nears an edge
    if (!Number.isFinite(viewCentre) || Math.abs(dial - viewCentre) > 0.35 * span) viewCentre = dial;
    const bins = spectrumRow(iq, t0 + now, nfft, viewCentre, nBins);
    tr.onFrame(bins, { centerHz: viewCentre, bwHz: span, tuneHz: dial, lo, hi }, mode, Math.round(now * 1000));
    // judge against the nearest known voice in view
    const inView = band.voices.filter((v) => voiceInView(v, dial));
    // ★ judged only where ONE voice is in the passband and no other touches it (a composite band's truth is exact)
    const touching = band.voices.filter((v) => voiceInView(v, dial, 1)).length;
    if (process.env.VIBE_SWEEP_TRACE && f % 5 === 0) console.log(`      t ${now.toFixed(1)} dial ${dial} cue ${hint.ok ? (hint.hz - dial).toFixed(0) : '-'} known ${band.voices.map((v) => v.carrier - dial).filter((x) => Math.abs(x) < 3500).map((x) => x.toFixed(0)).join(',')}`);
    if (inView.length !== 1 || touching !== 1) continue;
    const v = inView[0];
    st.inView++;
    // ★★★ A LONE voice must not read as two (2026-10-18: the second-voice cue makes native place by frequency, no lean)
    if (hint.ok && hint2.ok) st.second++;
    if (!hint.ok) continue;
    st.cue++;
    const truth = v.carrier - dial, got = hint.hz - dial;
    st.errs.push(Math.abs(hint.hz - v.carrier));
    st.signed.push((hint.hz - v.carrier) * (v.sb > 0 ? 1 : -1));   // + = the cue puts the carrier INTO the voice
    if (Math.abs(truth) >= 150) { st.judged++; if (got * truth > 0) st.right++; }
    // ★ ON TUNE (within ±60 Hz): native leans on a cue by |cue| − kCoarseShrinkHz (100 Hz, binaural.h), so a cue of
    //   ±250 Hz or more would lean an on-tune voice audibly (≥ 22 %)
    if (Math.abs(truth) <= 60) { st.near++; if (Math.abs(got) >= 250) st.nearFalse++; }
  }
  return st;
}

const pct = (v: number[], q: number) => { if (!v.length) return NaN; const s = [...v].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(q * s.length))]; };

// ── THE COMPOSITE BAND: real voices, placed where the truth is exact ─────────────────────────────────────────────
/* Each known voice is cut from its recording (its own sideband only, so a neighbour on the far side of its carrier is
 * left behind), decimated to 24 kHz, and placed at a chosen carrier in Gaussian noise at a chosen strength. The voices
 * and their speech are real; only their neighbours are arranged. */
const CFS = 24000;
function lowpassTaps(fc: number, fs: number, n: number): Float64Array {
  const h = new Float64Array(n); const m = (n - 1) / 2; let sum = 0;
  for (let i = 0; i < n; i++) {
    const t = i - m, x = 2 * fc / fs;
    const sinc = t === 0 ? x : Math.sin(Math.PI * x * t) / (Math.PI * t);
    const w = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1)) + 0.08 * Math.cos(4 * Math.PI * i / (n - 1));
    h[i] = sinc * w; sum += h[i];
  }
  for (let i = 0; i < n; i++) h[i] /= sum;
  return h;
}
/** One voice → 24 kHz complex baseband with its carrier at 0 and only its sideband, `secs` long, unit RMS. */
function cutVoice(iq: Iq, v: Voice, fromSec: number, secs: number): { re: Float32Array; im: Float32Array } {
  const D = FS / CFS, n = Math.floor(secs * CFS);
  const h1 = lowpassTaps(9000, FS, 97);
  const re1 = new Float32Array(n), im1 = new Float32Array(n);
  const w1 = -2 * Math.PI * (v.carrier - iq.centre + v.sb * 1500) / FS;   // the sideband's middle to 0 Hz
  const a0 = Math.floor(fromSec * FS);
  for (let o = 0; o < n; o++) {
    let ar = 0, ai = 0;
    const base = a0 + o * D;
    for (let k = 0; k < h1.length; k++) {
      const i = base + k; const ph = w1 * i; const c = Math.cos(ph), sn = Math.sin(ph);
      const xr = iq.re[i], xi = iq.im[i];
      ar += h1[k] * (xr * c - xi * sn); ai += h1[k] * (xr * sn + xi * c);
    }
    re1[o] = ar; im1[o] = ai;
  }
  const h2 = lowpassTaps(1380, CFS, 301);   // keep 120…2880 Hz of the sideband
  const re = new Float32Array(n), im = new Float32Array(n);
  const w2 = 2 * Math.PI * (v.sb * 1500) / CFS;
  let e = 0;
  for (let o = 0; o < n; o++) {
    let ar = 0, ai = 0;
    for (let k = 0; k < h2.length; k++) { const i = o + k - 150; if (i < 0 || i >= n) continue; ar += h2[k] * re1[i]; ai += h2[k] * im1[i]; }
    const c = Math.cos(w2 * o), sn = Math.sin(w2 * o);
    re[o] = ar * c - ai * sn; im[o] = ar * sn + ai * c;
    e += re[o] * re[o] + im[o] * im[o];
  }
  const g = 1 / Math.sqrt(e / n + 1e-30);
  for (let o = 0; o < n; o++) { re[o] *= g; im[o] *= g; }
  return { re, im };
}
/** Voices (each from a recording) at carriers `at` (Hz from the composite's centre), with in-band SNRs `snrDb`. */
function composite(centre: number, parts: { file: string; v: Voice; at: number; snrDb: number }[], secs: number): { iq: Iq; voices: Voice[] } {
  const n = Math.floor(secs * CFS);
  // ★ VIBE_SWEEP_CACHE=<dir>: keep the built band between runs (a measurement aid; building it takes a minute)
  const cacheDir = process.env.VIBE_SWEEP_CACHE || process.env.VIBE_SWEEP_EVENTS;   // the native half reads it from there
  const cache = cacheDir ? `${cacheDir}/composite_${centre}_${secs}.f32` : '';
  const voicesOut = parts.map((p) => ({ carrier: centre + p.at, sb: p.v.sb }));
  if (cache && existsSync(cache)) {
    const b = readFileSync(cache);
    const f = new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4);
    return { iq: { re: f.slice(0, n), im: f.slice(n, 2 * n), centre, fs: CFS }, voices: voicesOut };
  }
  const re = new Float32Array(n), im = new Float32Array(n);
  let seed = 12345;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  const noiseRms = 0.01;
  for (let i = 0; i < n; i++) {   // complex Gaussian noise, flat across 24 kHz
    const u = Math.max(1e-12, rnd()), w = rnd();
    const r = noiseRms * Math.sqrt(-Math.log(u)); re[i] = r * Math.cos(2 * Math.PI * w); im[i] = r * Math.sin(2 * Math.PI * w);
  }
  const voices: Voice[] = [];
  for (const p of parts) {
    const src = iqs.get(p.file)!;
    const x = cutVoice(src, p.v, 5, secs);
    // in-band noise power = total × (2.6 kHz / 24 kHz)
    const g = noiseRms * Math.sqrt((2600 / CFS) * Math.pow(10, p.snrDb / 10));
    const w = 2 * Math.PI * p.at / CFS;
    for (let i = 0; i < n; i++) {
      const c = Math.cos(w * i), sn = Math.sin(w * i);
      re[i] += g * (x.re[i] * c - x.im[i] * sn); im[i] += g * (x.re[i] * sn + x.im[i] * c);
    }
    voices.push({ carrier: centre + p.at, sb: p.v.sb });
  }
  if (cache) { const all = new Float32Array(2 * n); all.set(re, 0); all.set(im, n); writeFileSync(cache, Buffer.from(all.buffer)); }
  return { iq: { re, im, centre, fs: CFS }, voices };
}

console.log('BINAURAL — the waterfall cue on a sweep, real busy-band I/Q (40 / 20 / 80 m)');
const evDir = process.env.VIBE_SWEEP_EVENTS;
if (evDir) mkdirSync(evDir, { recursive: true });
if (process.env.VIBE_SWEEP_CACHE) mkdirSync(process.env.VIBE_SWEEP_CACHE, { recursive: true });
/** Bin widths: a phone's 40 m view of 25 kHz … 400 kHz across 1024 bins. */
const ZOOMS = [23, 47, 94, 188, 375];
const PACES: [number, number][] = [[100, 0.3], [500, 0.5]];
// ★ VIBE_SWEEP_ONLY=<band name>,<bin Hz>,<step> (e.g. "LSB composite,94,100") runs one — with VIBE_SWEEP_TRACE, a trace.
const only = process.env.VIBE_SWEEP_ONLY?.split(',');
const iqs = new Map<string, Iq>();
for (const f of Object.keys(KNOWN)) iqs.set(f, readIq(f, KNOWN[f].centre));
const k40 = KNOWN['uber_40m_7150000_iq192.wav'].voices, k20 = KNOWN['uber_20m_14250000_iq192.wav'].voices;
const k80 = KNOWN['uber_80m_3700000_iq192.wav'].voices;
const f40 = 'uber_40m_7150000_iq192.wav', f20 = 'uber_20m_14250000_iq192.wav', f80 = 'uber_80m_3700000_iq192.wav';
const SECS = 45;
// ★ Spacings as a busy band has them (3.0-3.4 kHz carrier to carrier: the bands nearly touch) and one voice alone;
//   strengths 8-25 dB in the voice's own band.
const lsbC = composite(7000000, [
  { file: f40, v: k40[0], at: -8800, snrDb: 20 }, { file: f40, v: k40[1], at: -5600, snrDb: 12 },
  { file: f80, v: k80[0], at: -2400, snrDb: 25 }, { file: f40, v: k40[3], at: 3600, snrDb: 15 },
  { file: f40, v: k40[4], at: 7000, snrDb: 8 },
], SECS);
const usbC = composite(14000000, [
  { file: f20, v: k20[0], at: -8000, snrDb: 18 }, { file: f20, v: k20[1], at: -4800, snrDb: 10 },
  { file: f20, v: k20[3], at: 1500, snrDb: 22 }, { file: f20, v: k20[0], at: 6000, snrDb: 14 },
], SECS);
const COMPOSITES: (Band & { iq: Iq })[] = [
  { name: 'LSB composite', iq: lsbC.iq, sb: -1, voices: lsbC.voices, sweeps: [[7000000 - 9500, 7000000 + 9500]], judged: true },
  { name: 'USB composite', iq: usbC.iq, sb: +1, voices: usbC.voices, sweeps: [[14000000 - 9500, 14000000 + 9500]], judged: true },
];
const ALL: (Band & { iq: Iq })[] = [...COMPOSITES, ...REAL.map((b) => ({ ...b, iq: iqs.get(b.file)! }))];
for (const binTarget of ZOOMS) {
  if (only && Number(only[1]) !== binTarget) continue;
  for (const [step, every] of PACES) {
    if (only && Number(only[2]) !== step) continue;
    for (const judged of [true, false]) {
      const tot: SweepStats = { inView: 0, cue: 0, right: 0, judged: 0, errs: [], signed: [], near: 0, nearFalse: 0, second: 0 };
      for (const b of ALL) {
        if (b.judged !== judged || (only && b.name !== only[0])) continue;
        for (const [a, z] of b.sweeps) for (const [from, to] of [[a, z], [z, a]]) {
          const ev: Ev[] | null = evDir ? [] : null;
          const s = sweep(b.iq, b, from, to, step, every, binTarget, judged ? 0 : 5, ev);
          tot.inView += s.inView; tot.cue += s.cue; tot.right += s.right; tot.judged += s.judged; tot.errs.push(...s.errs); tot.signed.push(...s.signed); tot.near += s.near; tot.nearFalse += s.nearFalse; tot.second += s.second;
          // ★ The native half (test-ssb-offset-real §7) replays a composite sweep's dial + hint stream against its audio.
          if (ev && judged && binTarget <= COARSE_MAX_BIN_HZ) writeFileSync(`${evDir}/sweep_${b.name.replace(/ /g, '_')}_${from}_${to}_${step}_${binTarget}.json`,
            JSON.stringify({ band: b.name, iq: `composite_${b.iq.centre}_${SECS}.f32`, fs: b.iq.fs, centre: b.iq.centre, secs: SECS,
                             sb: b.sb, from, to, step, every, binTarget, voices: b.voices, events: ev }));
        }
      }
      if (!tot.inView) continue;
      const cueFrac = tot.cue / tot.inView, rightFrac = tot.judged ? tot.right / tot.judged : 0;
      console.log(`   ${judged ? 'composite' : 'real     '} bins ${String(binTarget).padStart(3)} Hz, ${String(step).padStart(3)} Hz every ${every} s: in view ${String(tot.inView).padStart(4)}`
        + ` · cue ${(100 * cueFrac).toFixed(0).padStart(3)} % · right way ${(100 * rightFrac).toFixed(0).padStart(3)} % of ${tot.judged}`
        + ` · error median ${pct(tot.errs, 0.5).toFixed(0)} / 90 % ${pct(tot.errs, 0.9).toFixed(0)} Hz (bias ${pct(tot.signed, 0.5).toFixed(0)}) · on tune ≥250: ${tot.nearFalse}/${tot.near} · lone voice read as two: ${tot.second}/${tot.inView}`);
      if (judged && binTarget <= COARSE_MAX_BIN_HZ) {
        const errMax = 250 + binTarget;   // the edge is found to about a bin
        check(tot.nearFalse <= 0.1 * tot.near, `composite, bins ${binTarget} Hz, ${step} Hz clicks: on tune, a cue that would lean audibly on ≤ 10 % of frames (${tot.nearFalse} of ${tot.near})`);
        check(tot.second <= 0.05 * tot.inView, `composite, bins ${binTarget} Hz, ${step} Hz clicks: one voice alone in the passband is read as two on ≤ 5 % of frames (${tot.second} of ${tot.inView})`);
        check(cueFrac >= 0.6 && rightFrac >= 0.95 && pct(tot.errs, 0.9) <= errMax,
          `composite, bins ${binTarget} Hz, ${step} Hz clicks: cue on ≥ 60 % of in-view frames (${(100 * cueFrac).toFixed(0)} %), right way ≥ 95 % (${(100 * rightFrac).toFixed(0)} %), 90 % error ≤ ${errMax} Hz (${pct(tot.errs, 0.9).toFixed(0)})`);
      }
    }
  }
}
console.log(fails ? 'BINAURAL SWEEP (JS cue): FAILURES' : 'BINAURAL SWEEP (JS cue): all passed');
process.exit(fails ? 1 : 0);
