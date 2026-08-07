// ★★★ ARE THE DECODERS FED WHEN NOBODY IS LISTENING TO THE AUDIO?
//
// They must be. A decoder needs the SAMPLES, and nothing about whether a human has an audio
// socket open. In per-client mode this was not true: onClientAudio returned early on
// `!sock->isOpen()` BEFORE reaching feedDecoder, so decoding stopped dead whenever the owning
// listener's audio socket blipped — silently, with the UI still showing "decoding…".
//
// ★★ WHAT IT COST: Stuart, 2026-08-06 — RTTY "garbling more than usual" (a perfect line, then a
//    corrupted one: that is a GAP in the feed, not poor reception) and WEFAX missing an ENTIRE
//    transmission (one blip inside a 10-20 minute image loses the whole thing). The second
//    symptom is what ruled out the front end — an AGC degrades an image, it does not delete one.
//
// ★★★ RUN IT AGAINST THE BROKEN BUILD FIRST. Move the decoder block back below the socket check
//     and this must FAIL; otherwise it proves nothing. (Verified: it does.)
//
//   usage:  node decoder-no-audio.mjs <port> [admin-password]
import net from 'node:net';
import crypto from 'node:crypto';

const PORT = Number(process.argv[2]), PASS = process.argv[3] ?? 'secret';
const HOST = process.env.HOST ?? '127.0.0.1';
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };

const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
async function status() {
  const n = (await (await fetch(`http://${HOST}:${PORT}/vibeserver/auth`)).json()).nonce;
  const q = `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
  return (await fetch(`http://${HOST}:${PORT}/vibeserver/admin/status?${q}`)).json();
}

/** A raw WebSocket that never sends anything back. */
function ws(path, onText) {
  const key = crypto.randomBytes(16).toString('base64');
  const sock = net.connect(PORT, HOST);
  let hs = false, buf = Buffer.alloc(0);
  sock.on('connect', () => sock.write(
    `GET ${path} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  sock.on('data', (d) => {
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
                        payload.copy(p, 6); sock.write(p); continue; }
      if (op === 0x1 && onText) onText(payload.toString());
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

const SID = 'nodecaudio-' + crypto.randomBytes(4).toString('hex');

console.log(`\nProbing ${HOST}:${PORT} — a listener with a decoder and NO audio socket`);

// ★ Spectrum socket ONLY. Deliberately no /ws/audio: that is the whole point.
const spec = ws(`/ws/user-spectrum?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 1500));

const before = await status();
ok(before.decoderAttached === false, 'no decoder attached yet',
   `(attached=${before.decoderAttached})`);

// Attach a decoder over the dxcluster socket, exactly as the client does.
const dx = ws(`/ws/dxcluster?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 600));
dx.send({ type: 'decoder_start', decoder: 'rtty',
          center_frequency: 1000, shift: 450, baud_rate: 45.45,
          framing: '5N1.5', encoding: 'ITA2', invert: false });
await new Promise((r) => setTimeout(r, 1500));

const mid = await status();
ok(mid.decoderAttached === true, 'the decoder attached', `(attached=${mid.decoderAttached})`);

console.log('\n★★★ THE POINT OF THIS PROBE');
const t0 = mid.decoderFedSamples ?? 0;
await new Promise((r) => setTimeout(r, 4000));
const after = await status();
const fed = (after.decoderFedSamples ?? 0) - t0;
// 4 s of 48 kHz audio is ~192k samples. Anything above a few thousand proves it is running;
// exactly 0 is the bug.
ok(fed > 10000, 'the decoder IS being fed with no audio socket open',
   `(${fed} samples in 4 s — 0 means the audio-socket guard is back)`);

console.log('\nAnd it keeps running');
{
  const t1 = after.decoderFedSamples ?? 0;
  await new Promise((r) => setTimeout(r, 3000));
  const later = await status();
  ok((later.decoderFedSamples ?? 0) - t1 > 10000, 'still fed three seconds later',
     `(${(later.decoderFedSamples ?? 0) - t1})`);
}

spec.sock.destroy(); dx.sock.destroy();
console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
