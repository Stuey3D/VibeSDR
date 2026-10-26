// A synthetic rtl_tcp source — lets VibeServer run end-to-end with NO dongle attached.
//
// This is what makes the Mac-first plan actually pay off: server work (protocol foundations,
// multi-client, control token, link management) needs a believable IQ stream, not a real radio.
// Plug this in and the whole pipeline runs — FFT, waterfall, demod, audio, web client — on any
// machine, in CI, at 3am, with no hardware to plug in or share.
//
// Wire format (rtl_tcp): on connect the server sends a 12-byte header — magic "RTL0", then
// tuner type and gain count as big-endian u32 — and then streams unsigned 8-bit I/Q pairs at the
// sample rate. Commands arrive from the client as 5 bytes: one command byte, then a big-endian
// u32 argument. We honour the ones that change what should be HEARD (centre frequency, sample
// rate) and acknowledge the rest, which is all the DSP can tell apart anyway.
//
//   node vibeserver/fake-rtl-tcp.mjs [--port 1234] [--rate 2400000] [--tones 3]
//                                    [--wfm 100.0] [--dab bench-clip/dab-bench-clean.vbu8]
//
//   --wfm MHz   adds a WFM STEREO station at that absolute frequency: 75 kHz deviation, a 19 kHz
//               pilot and a 38 kHz L-R subcarrier locked to it, a different tone in each ear — so
//               the stereo decoder has something real to lock to (2026-09-28, the audio-dropout hunt).
//   --dab FILE  while the dongle is tuned inside Band III (174-240 MHz), replay FILE (unsigned 8-bit
//               IQ at 2.048 MS/s, e.g. bench-clip/dab-bench-clean.vbu8) on a loop instead of the
//               synthetic signal, so DAB locks, decodes and produces real audio on the bench.
//   --adsb FILE while the dongle is tuned within 2 MHz of 1090 MHz, replay FILE (unsigned 8-bit IQ, as
//               rtl_sdr writes it, at the rate the server asked for — record it at that rate) on a loop:
//               real aircraft for ADS-B mode on the bench (V12, 2026-10-24).
//   --ism FILE  while the dongle is tuned within 2 MHz of 434 MHz, replay FILE (unsigned 8-bit IQ at the rate the server
//               asked for, centred where ISM mode puts the radio) on a loop: sensors for ISM mode on the bench (V12,
//               2026-10-25 — `test-ism --write-clip` makes one, scripts/test-server-ism.mjs uses it).
//   --ism868 FILE  the same while the dongle is tuned within 2 MHz of 868.6 MHz — wM-Bus meters for ISM's 868 band (V12,
//               2026-10-26 — `test-wmbus --write-clip` makes one).
//   --hf FILE --hf-centre HZ   while the dongle is tuned within a quarter of the rate of HZ, replay FILE (unsigned 8-bit IQ
//               at the rate the server asked for, centred on HZ) on a loop, mixed to wherever the dongle really is — CW
//               stations for the CW skimmer on the bench (V12, 2026-10-25: `cw_bench clip`, scripts/test-server-cw.mjs).
//   --stall-at HZ  a dongle that FALLS OVER at a rate: while the server has asked for HZ or more, send nothing at all
//               (the socket stays open, the samples just stop) — the boosted-rate guard's bench (vibe_rate_guard.h,
//               scripts/test-rate-guard.mjs). Below HZ the stream resumes.
//
// The signal is deliberately synthetic-but-plausible: a noise floor plus a few AM-modulated
// carriers at fixed offsets from centre, so the waterfall shows real lines you can tune onto and
// hear, and a spectrum bug looks like a spectrum bug rather than like noise.
import net from 'node:net';
import fs from 'node:fs';

const arg = (name, def) => {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] ? Number(process.argv[i + 1]) : def;
};
const PORT  = arg('port', 1234);
let   RATE  = arg('rate', 2_400_000);
const TONES = arg('tones', 3);
const WFM_HZ = arg('wfm', 0) * 1e6;
const STALL_AT = arg('stall-at', 0);
// ★ --tones-at HZ: the synthetic stations sit at ABSOLUTE frequencies (HZ + their offset) instead of moving with the
//   dongle, so a test can tune to them exactly whatever offset the server tunes the dongle at (V12 VFO A/B, 2026-10-27).
const TONES_AT = arg('tones-at', 0);
// ★ --key S: the LAST station is keyed on for S seconds, off for S — a channel whose squelch opens and shuts.
const KEY_S = arg('key', 0);
const dabArg = (() => { const i = process.argv.indexOf('--dab'); return i >= 0 ? process.argv[i + 1] : ''; })();
const DAB_IQ = dabArg ? fs.readFileSync(dabArg) : null;
/* ★ --dab-at HZ[,HZ…] (V12 DAB band scan, 2026-10-26): play the multiplex ONLY on these block centres (±50 kHz — 10A and 10N
 *  are 160 kHz apart) and noise on every other Band III block, so a scan has something to find and something to pass over.
 *  Without it the clip plays across the whole of Band III, as it always has. */
const DAB_AT = (() => { const i = process.argv.indexOf('--dab-at'); return i >= 0 ? process.argv[i + 1].split(',').map(Number).filter((v) => v > 0) : []; })();
const dabHere = (hz) => hz >= 174e6 && hz <= 240e6 && (!DAB_AT.length || DAB_AT.some((c) => Math.abs(c - hz) <= 50e3));
/* ★★★ --dab-mux FILE.cf32 — AN RTL FRONT END THAT OBEYS ITS GAIN (V12 DAB scan AGC, 2026-10-27). Stuart: "the AGC can work
 *  quick enough to allow the multiplex to be found but without taking forever to do a full scan". Every other source here
 *  replays fixed bytes, so the server's gain writes changed nothing and no gain loop could be measured. This one models
 *  the chain the gain acts on, in ADC counts:
 *      out = g·(ensemble + aerial noise) + neighbour·IF-filter + second-stage noise, rounded to u8 and CLIPPED at the rails
 *  with g = 10^((G − 49.6)/20) for the gain G the server last wrote (0x04; auto mode = 35 dB). At the top gain the aerial
 *  noise is NOISE_MAX counts RMS per component; the tuner's own second-stage noise (0.8 counts) does not scale. So a weak
 *  ensemble at a LOW gain sinks under the ADC's own noise and quantisation — the real reason a scan can miss it — and a
 *  strong neighbour drives the rails at a HIGH gain — the reason the AGC cannot simply sit at the top.
 *    --dab-mux FILE      ODR-DabMod's clean IQ (bench-clip/mux.cf32); 60 frames from frame 30 are looped, as impair.py does
 *    --dab-ens HZ:SNR[,…]  an ensemble on that block centre, SNR dB in its 1.536 MHz against the aerial noise
 *    --dab-adj HZ:DB[,…]   a strong neighbour (a block nobody is scanning for — 8 carriers across 100 kHz), DB above the
 *                          aerial noise RMS; it reaches the ADC through the tuner's IF filter while within 3 MHz of the
 *                          dongle (0 dB within 1.5 MHz, −10 dB to 3 MHz), folded to the capture's edge (+0.93 MHz)
 *    --noise-max COUNTS  aerial noise RMS per component at 49.6 dB (default 6)
 *    --dab-mux-from N    the first of the 60 looped frames (default 30)
 *  Everything else in Band III is aerial noise alone. Off Band III the ordinary sources apply. */
const muxArg = (() => { const i = process.argv.indexOf('--dab-mux'); return i >= 0 ? process.argv[i + 1] : ''; })();
const pairs = (name) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1].split(',').map((t) => t.split(':').map(Number)).filter((v) => v[0] > 0) : []; };
const MUX_ENS = pairs('--dab-ens'), MUX_ADJ = pairs('--dab-adj');
const NOISE_MAX = arg('noise-max', 6);
let MUX = null;
if (muxArg) {
  // ★ --dab-mux-from FRAMES: where the 60 looped frames start (30 = clear of ODR's start-up, as impair.py takes them; 0 for a
  //   clip that is already those 60 frames, e.g. bench-clip/weak-6.vbu8 converted — with ITS multipath, fades and offset).
  const FRAME = 196608, N = 60 * FRAME, start = arg('dab-mux-from', 30) * FRAME;
  const fd = fs.openSync(muxArg, 'r');
  const b = Buffer.alloc(N * 8);
  fs.readSync(fd, b, 0, b.length, start * 8);
  fs.closeSync(fd);
  MUX = new Float32Array(b.buffer, b.byteOffset, N * 2);
  let p = 0; for (let i = 0; i < MUX.length; i++) p += MUX[i] * MUX[i];
  const k = 1 / Math.sqrt(p / N);                              // ★ unit complex power
  for (let i = 0; i < MUX.length; i++) MUX[i] *= k;
  console.log(`[fake-rtl-tcp] DAB front end: ${(N / 2.048e6).toFixed(2)} s of multiplex; ensembles ${MUX_ENS.map((e) => `${(e[0] / 1e6).toFixed(3)}@${e[1]}dB`).join(' ') || 'none'}; neighbours ${MUX_ADJ.map((e) => `${(e[0] / 1e6).toFixed(3)}@+${e[1]}dB`).join(' ') || 'none'}`);
}
// ★ A table of unit Gaussians, walked from a random start each chunk — Box-Muller per sample is too slow at 2 MS/s.
const GAUSS = (() => { const n = 1 << 20, g = new Float32Array(n); for (let i = 0; i < n; i += 2) { const u = Math.random() || 1e-12, v = Math.random();
  const r = Math.sqrt(-2 * Math.log(u)); g[i] = r * Math.cos(2 * Math.PI * v); g[i + 1] = r * Math.sin(2 * Math.PI * v); } return g; })();
const adsbArg = (() => { const i = process.argv.indexOf('--adsb'); return i >= 0 ? process.argv[i + 1] : ''; })();
const ADSB_IQ = adsbArg ? fs.readFileSync(adsbArg) : null;
if (ADSB_IQ) console.log(`[fake-rtl-tcp] ADS-B clip ${adsbArg}: ${ADSB_IQ.length / 2} samples`);
const ismArg = (() => { const i = process.argv.indexOf('--ism'); return i >= 0 ? process.argv[i + 1] : ''; })();
const ISM_IQ = ismArg ? fs.readFileSync(ismArg) : null;
const ism868Arg = (() => { const i = process.argv.indexOf('--ism868'); return i >= 0 ? process.argv[i + 1] : ''; })();
const ISM868_IQ = ism868Arg ? fs.readFileSync(ism868Arg) : null;
const hfArg = (() => { const i = process.argv.indexOf('--hf'); return i >= 0 ? process.argv[i + 1] : ''; })();
const HF_IQ = hfArg ? fs.readFileSync(hfArg) : null;
// ★ --hf-utc: the HF clip is UTC-ANCHORED — clip time t plays at UTC ≡ t (mod the clip's length), so a slotted digital
//   mode in it (FT8 at +0.5 s of a 15 s slot) lands in the server's UTC slots as it would off air (test-server-spots.mjs).
const HF_UTC = process.argv.includes('--hf-utc');
const HF_CENTRE = (() => { const i = process.argv.indexOf('--hf-centre'); return i >= 0 ? Number(process.argv[i + 1]) : 7.03e6; })();
if (HF_IQ) console.log(`[fake-rtl-tcp] HF clip ${hfArg}: ${HF_IQ.length / 2} samples centred on ${HF_CENTRE} Hz`);
if (ISM_IQ) console.log(`[fake-rtl-tcp] ISM clip ${ismArg}: ${ISM_IQ.length / 2} samples`);
if (DAB_IQ) console.log(`[fake-rtl-tcp] DAB clip ${dabArg}: ${(DAB_IQ.length / 2 / 2.048e6).toFixed(2)} s at 2.048 MS/s`);

// Offsets from centre (Hz) and audio modulation for each synthetic station.
const STATIONS = Array.from({ length: TONES }, (_, k) => ({
  offset: (k - (TONES - 1) / 2) * 120_000,   // spread either side of centre
  audioHz: 400 + k * 220,                    // a distinct pitch each, so you can hear which is which
  amplitude: 0.28 - k * 0.06,
}));

const srv = net.createServer((sock) => {
  const who = `${sock.remoteAddress}:${sock.remotePort}`;
  console.log(`[fake-rtl-tcp] client ${who} connected`);
  sock.setNoDelay(true);

  // Dongle header: "RTL0", tuner type 5 (R820T), 29 gain steps.
  const hdr = Buffer.alloc(12);
  hdr.write('RTL0', 0, 'ascii');
  hdr.writeUInt32BE(5, 4);
  hdr.writeUInt32BE(29, 8);
  sock.write(hdr);

  let stopped = false;
  let centreHz = 100e6;   // what the client last tuned the "dongle" to
  let dabPos = 0;         // replay position in DAB_IQ (bytes)
  let adsbPos = 0;        // replay position in ADSB_IQ (bytes)
  let ismPos = 0;         // replay position in ISM_IQ (bytes)
  let ism868Pos = 0;      // … and in ISM868_IQ
  let hfPos = 0, hfRe = 1, hfIm = 0;   // replay position in HF_IQ, and the mixer's phase (--hf)
  let hfAnchored = false;               // ★ --hf-utc: the position set from UTC on the first chunk
  // WFM stereo generator state (see --wfm)
  let fmPhase = 0, pilotPh = 0, lPh = 0, rPh = 0;
  let gainTenths = 350, muxPos = 0;   // ★ --dab-mux: the gain the server last wrote, the replay position
  // ★ The neighbour: 8 carriers 12.5 kHz apart from +0.93 MHz, each a rotating unit phasor (no trig per sample).
  const adjOsc = Array.from({ length: 8 }, (_, t) => { const w = 2 * Math.PI * (0.93e6 + t * 12.5e3) / 2.048e6;
    return { re: Math.cos(t * 1.7), im: Math.sin(t * 1.7), cw: Math.cos(w), sw: Math.sin(w) }; });

  // ── Pacing ────────────────────────────────────────────────────────────────
  // ★ DEADLINE-BASED, not sleep-based. The original did `generate 50ms of IQ`
  // then `setTimeout(tick, 50)`, so every cycle took generation + 50ms and the
  // stream delivered only ~46% of the advertised rate (measured: 1.11 of 2.4
  // MS/s). That silently halves the DSP's frame rate via
  // (effective_source / sampleRate) * fps — which makes this source USELESS as a
  // bench for anything rate-related, and looks exactly like a server or link
  // fault. Sleep until the NEXT DUE TIME instead, so generation cost is absorbed.
  const CHUNK_MS = 50;
  let nextDue = Date.now();

  // ── Signal generation ─────────────────────────────────────────────────────
  // ★ TRIG-FREE INNER LOOP. Sustaining 2.4 MS/s means ~2.4M iterations/second;
  // two Math.cos/Math.sin per station per sample could not keep up, which is the
  // other half of the shortfall. Each carrier is a unit complex number advanced
  // by a fixed rotation (one complex multiply), which is the standard cheap
  // oscillator and is exact enough for a synthetic bench.
  let rate = 0, osc = [], aosc = [], oscAt = 0;
  const retune = () => {
    rate = RATE;
    oscAt = centreHz;
    osc  = STATIONS.map((st, k) => {
      const off = TONES_AT ? TONES_AT + st.offset - centreHz : st.offset;
      const w = (2 * Math.PI * off) / rate;
      const o = osc[k] || { re: 1, im: 0 };
      return { re: o.re, im: o.im, cw: Math.cos(w), sw: Math.sin(w) };
    });
    aosc = STATIONS.map((st) => {
      const w = (2 * Math.PI * st.audioHz) / rate;
      return { re: 1, im: 0, cw: Math.cos(w), sw: Math.sin(w) };
    });
  };
  const advance = (o) => {
    const re = o.re * o.cw - o.im * o.sw;
    o.im = o.re * o.sw + o.im * o.cw;
    o.re = re;
  };
  // Repeated complex multiplies drift off the unit circle; a cheap first-order
  // renormalise each chunk keeps amplitude constant without a sqrt per sample.
  const renorm = (o) => {
    const m = 1.5 - 0.5 * (o.re * o.re + o.im * o.im);
    o.re *= m; o.im *= m;
  };

  const tick = () => {
    if (stopped || sock.destroyed) return;
    if (STALL_AT > 0 && RATE >= STALL_AT) { nextDue = Date.now(); setTimeout(tick, CHUNK_MS); return; }   // ★ --stall-at
    if (rate !== RATE || (TONES_AT && oscAt !== centreHz)) retune();
    const keyedOff = KEY_S > 0 && Math.floor(Date.now() / 1000 / KEY_S) % 2 === 1;
    const n = Math.max(1024, Math.round(rate * CHUNK_MS / 1000));
    const buf = Buffer.allocUnsafe(n * 2);
    if (HF_IQ && HF_UTC && !hfAnchored) {
      hfAnchored = true;
      const secs = HF_IQ.length / 2 / rate;
      hfPos = 2 * Math.floor(((nextDue / 1000) % secs) * rate);
    }
    if (HF_IQ && HF_UTC && Math.abs(centreHz - HF_CENTRE) > rate / 4) hfPos = (hfPos + 2 * n) % HF_IQ.length;   // ★ keep the clock
    if (HF_IQ && Math.abs(centreHz - HF_CENTRE) <= rate / 4) {
      // ★ The clip is centred on HF_CENTRE; the dongle may sit off it (the server's own offset tuning) — mix it there.
      const w = (2 * Math.PI * (HF_CENTRE - centreHz)) / rate, cw = Math.cos(w), sw = Math.sin(w);
      for (let i = 0; i < n; i++) {
        const x = (HF_IQ[hfPos] - 127.5), y = (HF_IQ[hfPos + 1] - 127.5);
        hfPos = (hfPos + 2) % HF_IQ.length;
        buf[2 * i] = Math.max(0, Math.min(255, Math.round(127.5 + x * hfRe - y * hfIm)));
        buf[2 * i + 1] = Math.max(0, Math.min(255, Math.round(127.5 + x * hfIm + y * hfRe)));
        const r = hfRe * cw - hfIm * sw; hfIm = hfRe * sw + hfIm * cw; hfRe = r;
      }
      const m = 1.5 - 0.5 * (hfRe * hfRe + hfIm * hfIm); hfRe *= m; hfIm *= m;
    } else if (ISM_IQ && Math.abs(centreHz - 434e6) <= 2e6) {
      for (let k = 0; k < n * 2; ) {
        const take = Math.min(n * 2 - k, ISM_IQ.length - ismPos);
        ISM_IQ.copy(buf, k, ismPos, ismPos + take);
        k += take; ismPos = (ismPos + take) % ISM_IQ.length;
      }
    } else if (ISM868_IQ && Math.abs(centreHz - 868.6e6) <= 2e6) {
      for (let k = 0; k < n * 2; ) {
        const take = Math.min(n * 2 - k, ISM868_IQ.length - ism868Pos);
        ISM868_IQ.copy(buf, k, ism868Pos, ism868Pos + take);
        k += take; ism868Pos = (ism868Pos + take) % ISM868_IQ.length;
      }
    } else if (ADSB_IQ && Math.abs(centreHz - 1090e6) <= 2e6) {
      for (let k = 0; k < n * 2; ) {
        const take = Math.min(n * 2 - k, ADSB_IQ.length - adsbPos);
        ADSB_IQ.copy(buf, k, adsbPos, adsbPos + take);
        k += take; adsbPos = (adsbPos + take) % ADSB_IQ.length;
      }
    } else if (MUX && centreHz >= 170e6 && centreHz <= 245e6) {
      // ★ The front end (--dab-mux): see the note at the top.
      const g = Math.pow(10, (gainTenths / 10 - 49.6) / 20);
      const nA = NOISE_MAX * g;                                        // aerial noise RMS per component at this gain
      const ens = MUX_ENS.find((e) => Math.abs(e[0] - centreHz) <= 50e3);
      // signal power = SNR × noise power in 1.536 of 2.048 MHz; complex noise power = 2·nA²
      const sA = ens ? Math.sqrt(Math.pow(10, ens[1] / 10) * 2 * nA * nA * 0.75) : 0;
      let adjA = 0;
      for (const a of MUX_ADJ) { const d = Math.abs(a[0] - centreHz); if (d < 3e6) adjA += NOISE_MAX * Math.SQRT2 * Math.pow(10, a[1] / 20) * g * (d < 1.5e6 ? 1 : 0.316); }
      const tones = adjOsc.length;
      let gi = Math.floor(Math.random() * (GAUSS.length - 4 * n - 8)) & ~1;
      for (let i = 0; i < n; i++) {
        let re = nA * GAUSS[gi] + 0.8 * GAUSS[gi + 2 * n + 1], im = nA * GAUSS[gi + 1] + 0.8 * GAUSS[gi + 2 * n];
        gi += 2;
        if (sA) { re += sA * MUX[muxPos]; im += sA * MUX[muxPos + 1]; }
        if (adjA) {
          let ar = 0, ai = 0;
          for (let t = 0; t < tones; t++) { const o = adjOsc[t]; ar += o.re; ai += o.im; advance(o); }
          re += adjA / Math.sqrt(tones) * ar; im += adjA / Math.sqrt(tones) * ai;
        }
        muxPos += 2; if (muxPos >= MUX.length) muxPos = 0;
        buf[2 * i] = Math.max(0, Math.min(255, Math.round(127.5 + re)));
        buf[2 * i + 1] = Math.max(0, Math.min(255, Math.round(127.5 + im)));
      }
      for (const o of adjOsc) renorm(o);
    } else if (DAB_IQ && dabHere(centreHz)) {
      for (let k = 0; k < n * 2; ) {
        const take = Math.min(n * 2 - k, DAB_IQ.length - dabPos);
        DAB_IQ.copy(buf, k, dabPos, dabPos + take);
        k += take; dabPos = (dabPos + take) % DAB_IQ.length;
      }
    } else for (let i = 0; i < n; i++) {
      let re = (Math.random() + Math.random() - 1) * 0.06;
      let im = (Math.random() + Math.random() - 1) * 0.06;
      for (let s = 0; s < STATIONS.length; s++) {
        const a = aosc[s], c = osc[s];
        const env = (keyedOff && s === STATIONS.length - 1) ? 0 : STATIONS[s].amplitude * (0.6 + 0.4 * a.im);   // AM, ~40% depth
        re += env * c.re;
        im += env * c.im;
        advance(a); advance(c);
      }
      if (WFM_HZ) {
        const off = WFM_HZ - centreHz;
        if (Math.abs(off) < rate / 2) {
          const L = 0.8 * Math.sin(lPh), R = 0.8 * Math.sin(rPh);
          const mpx = 0.45 * (L + R) + 0.45 * (L - R) * Math.sin(2 * pilotPh) + 0.1 * Math.sin(pilotPh);
          fmPhase += 2 * Math.PI * (off + 75e3 * mpx) / rate;
          if (fmPhase > 1e4 || fmPhase < -1e4) fmPhase %= 2 * Math.PI;
          pilotPh += 2 * Math.PI * 19e3 / rate; if (pilotPh > 2 * Math.PI) pilotPh -= 2 * Math.PI;
          lPh += 2 * Math.PI * 1000 / rate;     if (lPh > 2 * Math.PI) lPh -= 2 * Math.PI;
          rPh += 2 * Math.PI * 1700 / rate;     if (rPh > 2 * Math.PI) rPh -= 2 * Math.PI;
          re += 0.5 * Math.cos(fmPhase); im += 0.5 * Math.sin(fmPhase);
        }
      }
      buf[i * 2]     = Math.max(0, Math.min(255, Math.round(127.5 + re * 127)));
      buf[i * 2 + 1] = Math.max(0, Math.min(255, Math.round(127.5 + im * 127)));
    }
    for (const o of osc)  renorm(o);
    for (const o of aosc) renorm(o);

    nextDue += CHUNK_MS;
    // If we fell badly behind (debugger, laptop sleep), give up on catching up
    // rather than firing a burst of chunks — a flood is a different lie.
    if (Date.now() - nextDue > 500) nextDue = Date.now();
    const delay = Math.max(0, nextDue - Date.now());

    // Respect backpressure: if the consumer is slow, wait rather than buffering the world.
    if (sock.write(buf)) setTimeout(tick, delay);
    else sock.once('drain', () => { nextDue = Date.now(); setTimeout(tick, 0); });
  };
  retune();
  setTimeout(tick, 10);

  // Commands: 5 bytes each. 0x01 = centre freq, 0x02 = sample rate.
  let pending = Buffer.alloc(0);
  sock.on('data', (d) => {
    pending = Buffer.concat([pending, d]);
    while (pending.length >= 5) {
      const cmd = pending[0], val = pending.readUInt32BE(1);
      pending = pending.subarray(5);
      if (cmd === 0x01) { centreHz = val; console.log(`[fake-rtl-tcp] tune ${(val / 1e6).toFixed(4)} MHz`); }
      else if (cmd === 0x02) { RATE = val; console.log(`[fake-rtl-tcp] sample rate ${val}`); }
      // ★ The front-end settings are only ACKNOWLEDGED (nothing here can hear them), but logged, so a
      //   bench run can show what the server actually asserted on the radio — bias-T, ppm, direct
      //   sampling, gain mode and gain (the DAB quick scan's "full hardware set", 2026-09-29).
      else if (cmd === 0x03) { if (!val) gainTenths = 350; console.log(`[fake-rtl-tcp] gain mode ${val ? 'manual' : 'auto'}`); }
      else if (cmd === 0x04) { gainTenths = val; console.log(`[fake-rtl-tcp] gain ${(val / 10).toFixed(1)} dB`); }
      else if (cmd === 0x05) console.log(`[fake-rtl-tcp] ppm ${val | 0}`);
      else if (cmd === 0x08) console.log(`[fake-rtl-tcp] RTL digital AGC ${val ? 'on' : 'off'}`);
      else if (cmd === 0x09) console.log(`[fake-rtl-tcp] direct sampling ${val}`);
      else if (cmd === 0x0e) console.log(`[fake-rtl-tcp] bias-T ${val ? 'ON' : 'off'}`);
    }
  });

  const bye = () => { stopped = true; console.log(`[fake-rtl-tcp] client ${who} gone`); };
  sock.on('close', bye);
  sock.on('error', bye);
});

srv.listen(PORT, '127.0.0.1', () =>
  console.log(`[fake-rtl-tcp] listening on 127.0.0.1:${PORT} @ ${RATE} Hz, ${TONES} synthetic stations`));
