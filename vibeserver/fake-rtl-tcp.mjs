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
const dabArg = (() => { const i = process.argv.indexOf('--dab'); return i >= 0 ? process.argv[i + 1] : ''; })();
const DAB_IQ = dabArg ? fs.readFileSync(dabArg) : null;
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
  // WFM stereo generator state (see --wfm)
  let fmPhase = 0, pilotPh = 0, lPh = 0, rPh = 0;

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
  let rate = 0, osc = [], aosc = [];
  const retune = () => {
    rate = RATE;
    osc  = STATIONS.map((st) => {
      const w = (2 * Math.PI * st.offset) / rate;
      return { re: 1, im: 0, cw: Math.cos(w), sw: Math.sin(w) };
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
    if (rate !== RATE) retune();
    const n = Math.max(1024, Math.round(rate * CHUNK_MS / 1000));
    const buf = Buffer.allocUnsafe(n * 2);
    if (DAB_IQ && centreHz >= 174e6 && centreHz <= 240e6) {
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
        const env = STATIONS[s].amplitude * (0.6 + 0.4 * a.im);   // AM, ~40% depth
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
      else if (cmd === 0x03) console.log(`[fake-rtl-tcp] gain mode ${val ? 'manual' : 'auto'}`);
      else if (cmd === 0x04) console.log(`[fake-rtl-tcp] gain ${(val / 10).toFixed(1)} dB`);
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
