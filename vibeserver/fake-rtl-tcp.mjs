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
//
// The signal is deliberately synthetic-but-plausible: a noise floor plus a few AM-modulated
// carriers at fixed offsets from centre, so the waterfall shows real lines you can tune onto and
// hear, and a spectrum bug looks like a spectrum bug rather than like noise.
import net from 'node:net';

const arg = (name, def) => {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] ? Number(process.argv[i + 1]) : def;
};
const PORT  = arg('port', 1234);
let   RATE  = arg('rate', 2_400_000);
const TONES = arg('tones', 3);

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

  let phase = 0;                    // carrier phases advance continuously across chunks…
  const aphase = STATIONS.map(() => 0);   // …and so do the audio ones, or you hear a click per chunk
  let stopped = false;

  // ~50 ms of IQ per tick keeps latency low without waking the CPU constantly.
  const tick = () => {
    if (stopped || sock.destroyed) return;
    const n = Math.max(1024, Math.round(RATE * 0.05));
    const buf = Buffer.allocUnsafe(n * 2);
    for (let i = 0; i < n; i++) {
      // Noise floor. Two uniforms summed is a cheap, adequate stand-in for Gaussian here.
      let re = (Math.random() + Math.random() - 1) * 0.06;
      let im = (Math.random() + Math.random() - 1) * 0.06;
      for (let s = 0; s < STATIONS.length; s++) {
        const st = STATIONS[s];
        aphase[s] += (2 * Math.PI * st.audioHz) / RATE;
        const env = st.amplitude * (0.6 + 0.4 * Math.sin(aphase[s]));   // AM, ~40% depth
        const ph = phase * st.offset;
        re += env * Math.cos(ph);
        im += env * Math.sin(ph);
      }
      phase += (2 * Math.PI) / RATE;
      // u8 IQ, 127.5 centred — clamp so a loud moment wraps like a real ADC clips, not like a bug.
      buf[i * 2]     = Math.max(0, Math.min(255, Math.round(127.5 + re * 127)));
      buf[i * 2 + 1] = Math.max(0, Math.min(255, Math.round(127.5 + im * 127)));
    }
    // Respect backpressure: if the consumer is slow, wait rather than buffering the world.
    if (sock.write(buf)) setTimeout(tick, 50);
    else sock.once('drain', () => setTimeout(tick, 5));
  };
  setTimeout(tick, 10);

  // Commands: 5 bytes each. 0x01 = centre freq, 0x02 = sample rate.
  let pending = Buffer.alloc(0);
  sock.on('data', (d) => {
    pending = Buffer.concat([pending, d]);
    while (pending.length >= 5) {
      const cmd = pending[0], val = pending.readUInt32BE(1);
      pending = pending.subarray(5);
      if (cmd === 0x01) console.log(`[fake-rtl-tcp] tune ${(val / 1e6).toFixed(4)} MHz`);
      else if (cmd === 0x02) { RATE = val; console.log(`[fake-rtl-tcp] sample rate ${val}`); }
    }
  });

  const bye = () => { stopped = true; console.log(`[fake-rtl-tcp] client ${who} gone`); };
  sock.on('close', bye);
  sock.on('error', bye);
});

srv.listen(PORT, '127.0.0.1', () =>
  console.log(`[fake-rtl-tcp] listening on 127.0.0.1:${PORT} @ ${RATE} Hz, ${TONES} synthetic stations`));
