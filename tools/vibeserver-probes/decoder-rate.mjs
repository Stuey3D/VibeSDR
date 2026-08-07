// ★★★ IS THE DECODER FED AT EXACTLY THE RATE IT ASSUMES?
//
// FskDecoder is constructed with a HARD-CODED 48000 and derives its bit clock from it:
//     bitSampleCount = sampleRate / baud
// It is NOT start-bit synchronised. It free-runs that clock and nudges it from a zero-crossing
// histogram every 16 bits, damped by a factor of 8 — so a small, CONSTANT rate error accumulates
// and is only weakly corrected. It holds lock while the signal is strong and slips when the
// correction can no longer keep up.
//
// ★★ THAT IS EXACTLY THE REPORTED SYMPTOM. Stuart, 2026-08-06: *"it seems to decode clean for a
//    bit then slip out"* — with the RYRY idle runs staying clean and the long text lines
//    degrading. A rate error is the first thing to rule in or out, and until now nobody had
//    measured the rate at all: it was ASSUMED to be 48 kHz because that is what the pipeline is
//    configured to emit.
//
// Measures the delivered rate from `decoderFedSamples` (admin API) over a long window, and
// reports the error in ppm against what the decoder believes.
//
//   usage:  node decoder-rate.mjs <port> [admin-password] [seconds]
import net from 'node:net';
import crypto from 'node:crypto';

const PORT = Number(process.argv[2]), PASS = process.argv[3] ?? 'secret';
const SECS = Number(process.argv[4] ?? 120);
const HOST = process.env.HOST ?? '127.0.0.1';
const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');

async function status() {
  const n = (await (await fetch(`http://${HOST}:${PORT}/vibeserver/auth`)).json()).nonce;
  const q = `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
  return (await fetch(`http://${HOST}:${PORT}/vibeserver/admin/status?${q}`)).json();
}

function ws(path) {
  const key = crypto.randomBytes(16).toString('base64');
  const sock = net.connect(PORT, HOST);
  let hs = false, buf = Buffer.alloc(0);
  sock.on('connect', () => sock.write(
    `GET ${path} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  sock.on('data', (d) => {                       // drain, and answer pings or we get dropped
    buf = Buffer.concat([buf, d]);
    if (!hs) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; hs = true; buf = buf.subarray(i + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const payload = buf.subarray(off, off + len);
      buf = buf.subarray(off + len);
      if (op === 0x9) { const p = Buffer.alloc(6 + payload.length);
                        p[0] = 0x8a; p[1] = 0x80 | payload.length; p.writeUInt32BE(0, 2);
                        payload.copy(p, 6); sock.write(p); }
    }
  });
  sock.on('error', () => {});
  const send = (obj) => {
    const b = Buffer.from(JSON.stringify(obj));
    const ext = b.length >= 126;
    const f = Buffer.alloc((ext ? 8 : 6) + b.length);
    f[0] = 0x81;
    if (ext) { f[1] = 0x80 | 126; f.writeUInt16BE(b.length, 2); f.writeUInt32BE(0, 4); b.copy(f, 8); }
    else     { f[1] = 0x80 | b.length; f.writeUInt32BE(0, 2); b.copy(f, 6); }
    sock.write(f);
  };
  return { sock, send };
}

const SID = 'rateprobe-' + crypto.randomBytes(4).toString('hex');
const spec = ws(`/ws/user-spectrum?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 1500));
const dx = ws(`/ws/dxcluster?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 600));
// ★ THE PROTOCOL IS `audio_extension_attach` WITH extension_name "fsk" — not
//   "decoder_start"/"rtty", which is what I guessed first. It attached nothing, the server
//   logged nothing, and the probe cheerfully reported samples flowing (see below). Read the
//   handler, do not infer the wire format from the UI's vocabulary.
dx.send({ type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1000,
          shift: 450, baud_rate: 50, framing: '5N1.5', encoding: 'ITA2', invert: false });
await new Promise((r) => setTimeout(r, 2000));

const s0 = await status();
if (!s0.decoderAttached) { console.log('decoder did not attach — aborting'); process.exit(2); }

const t0 = Number(process.hrtime.bigint());
const a = s0.decoderFedSamples;
console.log(`\nattached — measuring for ${SECS}s…`);
await new Promise((r) => setTimeout(r, SECS * 1000));
const s1 = await status();
const t1 = Number(process.hrtime.bigint());

const dt = (t1 - t0) / 1e9;
const ds = s1.decoderFedSamples - a;
const rate = ds / dt;
const ppm = (rate - 48000) / 48000 * 1e6;
// One sync period at 50 baud with the 5N1.5 doubling: 16 half-bits of 480 samples.
const driftPerSync = (rate - 48000) / 48000 * 16 * 480;

console.log(`\n  samples delivered : ${ds}`);
console.log(`  elapsed           : ${dt.toFixed(3)} s`);
console.log(`  measured rate     : ${rate.toFixed(1)} Hz`);
console.log(`  decoder assumes   : 48000.0 Hz`);
console.log(`  error             : ${(ppm / 1e4).toFixed(4)} %   (${ppm.toFixed(0)} ppm)`);
console.log(`  drift per sync    : ${driftPerSync.toFixed(2)} samples per 16 half-bits`);
// ★ The loop corrects at most (bitSampleCount/2)/8 = 30 samples per sync period. Anything
//   approaching that cannot be tracked and WILL slip; well under it exonerates the rate.
console.log(`  loop authority    : ~30 samples per sync period`);
console.log(`\n  VERDICT: ${Math.abs(driftPerSync) > 10
  ? 'RATE ERROR IS SIGNIFICANT — the bit clock cannot be held by the sync loop'
  : 'rate is not the problem (drift is well inside what the sync loop corrects)'}`);

spec.sock.destroy(); dx.sock.destroy();
