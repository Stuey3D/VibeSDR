/**
 * record-audio.mjs — capture a VibeServer's demodulated audio to a WAV.
 *
 *   node scripts/record-audio.mjs ws://host:port 105.4M 20 out.wav
 *
 * ★★★ WHY IT EXISTS. Stuart's test for the 105.4 ghost: "record the audio from 104.2 and then at
 *  105.4 — if they are the same its the ghost, if they are different its capital." One radio can
 *  only be on one frequency, so the honest version runs TWO receivers at once (the Pi on 105.4,
 *  the Sony on 104.2) and compares them. Sequential recordings cannot answer it: the programme
 *  moves on between them, so identical content would not look identical.
 * ★ Opus over the wire, decoded here — no server setting to change, so nobody's receiver is
 *   reconfigured to run a test on it.
 */
import { OpusDecoder } from 'opus-decoder';
import { writeFileSync } from 'node:fs';

const [base, freqTxt, secsTxt, outPath, bwTxt, gainTxt] = process.argv.slice(2);  // gainTxt: tenth-dB, omitted = leave the AGC alone   // bwTxt: optional IF width in Hz, 0 = wide, -1 = auto
if (!base || !freqTxt || !secsTxt || !outPath) {
  console.error('usage: node scripts/record-audio.mjs ws://host:port <105.4M> <seconds> <out.wav>');
  process.exit(1);
}
const hz = (t) => { const m = String(t).match(/^([\d.]+)\s*([kKmM])?$/); return parseFloat(m[1]) * ({ k: 1e3, m: 1e6 }[(m[2] || '').toLowerCase()] || 1); };
const FREQ = hz(freqTxt), SECS = Number(secsTxt);
const SID = 'rec' + Math.random().toString(36).slice(2, 10);

const dec = new OpusDecoder({ channels: 2 });
await dec.ready;

const spec = new WebSocket(`${base.replace(/\/+$/, '')}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
const audio = new WebSocket(`${base.replace(/\/+$/, '')}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.binaryType = 'arraybuffer';

let L = [], R = [], started = 0, rate = 48000;
/* ★★★ WAIT FOR THE GAIN TO STOP MOVING BEFORE KEEPING ANY OF IT.
 *  The first version discarded ONE SECOND and then recorded, which on a receiver whose AGC takes
 *  80-90 s to settle captures the CLIMB — every gain on the way up, including the high-gain region
 *  where a front end manufactures signal. A recording of the transient answers a different
 *  question from the one being asked. Stuart: "did you wait for the agc to settle?" No.
 *  ★ Settled means gainNow unchanged for `STABLE_MS`, or `MAX_WAIT_MS` gone by — a receiver whose
 *    loop never quite stops still has to be recordable, and the log says which it was. */
const STABLE_MS = 15000, MAX_WAIT_MS = 150000;
let gainNow = null, gainSince = 0, settled = false, t0 = Date.now();
spec.onopen = () => {
  spec.send(JSON.stringify({ type: 'zoom', frequency: FREQ, binBandwidth: 1200 }));
  spec.send(JSON.stringify({ type: 'tune', frequency: FREQ, mode: 'wfm' }));
  // ★ The IF filter under test — see the ghost investigation. Sent after the tune, because a
  //   bandwidth write re-applies librtlsdr's last centre (see applyAutoIf's note).
  if (bwTxt !== undefined) spec.send(JSON.stringify({ type: 'tunerbw', value: Number(bwTxt) }));
  // ★ A fixed gain, so the question is about the front end and not about where the loop wandered.
  if (gainTxt !== undefined) {
    spec.send(JSON.stringify({ type: 'gain', value: Number(gainTxt) }));
    setTimeout(() => spec.send(JSON.stringify({ type: 'gain', value: Number(gainTxt) })), 1500);
  }
};
spec.onerror = () => {};
spec.onmessage = (ev) => {
  if (typeof ev.data !== 'string') return;
  let j; try { j = JSON.parse(ev.data); } catch { return; }
  if (j.type !== 'hwinfo' || !Number.isFinite(j.gainNow)) return;
  if (j.gainNow !== gainNow) { gainNow = j.gainNow; gainSince = Date.now(); return; }
  if (!settled && gainSince && Date.now() - gainSince > STABLE_MS) {
    settled = true;
    console.error(`  settled at ${(gainNow / 10).toFixed(1)} dB after ${((Date.now() - t0) / 1000).toFixed(0)}s`);
  }
};
audio.onerror = (e) => { console.error('audio socket failed'); process.exit(1); };
audio.onmessage = (ev) => {
  if (typeof ev.data === 'string') return;
  /* ★★★ EVERY AUDIO FRAME CARRIES A SIX-BYTE HEADER — [0] channels, [1] codec (3 = Opus), then
   *  the sample rate as a 32-bit little-endian. I fed the whole frame, header and all, into the
   *  Opus decoder and wrote the resulting NOISE to disk, then built an entire evening of
   *  correlation results on top of it — including a "validated control" that was two noise files
   *  agreeing with each other. Stuart spotted it in one sentence: "your recordings are just noise,
   *  no audio at all, not even FM noise".
   *  ★ LISTEN TO THE FIRST FILE before trusting any measurement derived from it. */
  const raw = new Uint8Array(ev.data);
  if (raw.length < 8) return;
  const chans = raw[0] || 2;
  const codec = raw[1];
  if (codec !== 3) return;                       // not Opus — this tool only decodes Opus
  rate = raw[2] | (raw[3] << 8) | (raw[4] << 16) | (raw[5] << 24);
  const pkt = raw.subarray(6);
  if (!pkt.length) return;
  let out; try { out = dec.decodeFrame(pkt); } catch { return; }
  if (!out || !out.samplesDecoded) return;
  // ★ Discard the first second: the tune, the AGC settling and the decoder priming are not signal.
  /* ★ Nothing is kept until the loop has stopped moving — see STABLE_MS above.
   *  ★★ START_AFTER_MS overrides it with a FIXED pre-roll, which is what makes two receivers
   *     comparable: each waiting for its own settle starts them at different wall-clock moments,
   *     and two recordings of different moments cannot be compared at all. */
  const fixed = Number(process.env.START_AFTER_MS || 0);
  if (fixed > 0) { if (Date.now() - t0 < fixed) return; }
  else if (!settled && Date.now() - t0 < MAX_WAIT_MS) return;
  if (!started) {
    started = Date.now();
    setTimeout(finish, SECS * 1000);      // ★ SECS of SETTLED audio, then stop — not SECS from launch
    if (!settled) console.error(`  gave up waiting after ${(MAX_WAIT_MS / 1000)}s — recording anyway at ${gainNow === null ? '?' : (gainNow / 10).toFixed(1)} dB`);
    return;
  }
  if (!rate) rate = out.sampleRate || 48000;
  L.push(out.channelData[0].slice()); R.push(out.channelData[1] ? out.channelData[1].slice() : out.channelData[0].slice());
};

function finish() {
  const n = L.reduce((a, b) => a + b.length, 0);
  if (!n) { console.error('no audio decoded'); process.exit(1); }
  const buf = Buffer.alloc(44 + n * 4);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 4, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(2, 22);
  buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * 4, 28); buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(n * 4, 40);
  let o = 44;
  for (let c = 0; c < L.length; c++) {
    for (let i = 0; i < L[c].length; i++) {
      const l = Math.max(-1, Math.min(1, L[c][i])), r = Math.max(-1, Math.min(1, R[c][i]));
      buf.writeInt16LE((l * 32767) | 0, o); o += 2;
      buf.writeInt16LE((r * 32767) | 0, o); o += 2;
    }
  }
  writeFileSync(outPath, buf);
  console.log(`${outPath}  ${(n / rate).toFixed(1)}s @ ${rate} Hz`
            + (gainNow === null ? '' : `  (gain ${(gainNow / 10).toFixed(1)} dB${settled ? ', settled' : ', NOT settled'})`));
  process.exit(0);
}
// ★ A receiver that never sends audio at all must still end the run.
setTimeout(finish, MAX_WAIT_MS + (SECS + 20) * 1000);
