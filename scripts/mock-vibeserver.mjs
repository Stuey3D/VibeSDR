/**
 * mock-vibeserver.mjs — a fake VibeServer for developing the web client.
 *
 *   node scripts/mock-vibeserver.mjs            # PIN 123456, port 48000
 *   node scripts/mock-vibeserver.mjs --no-pin
 *   node scripts/mock-vibeserver.mjs --bad-messages --port 48010   # interleave malformed traffic
 *
 * Speaks the same wire protocol as the real shim (local_sdr_shim.cpp), so the
 * web client can't tell the difference: the auth nonce/HMAC handshake, the
 * text config/hwinfo/rds messages, binary SPEC frames (FFT order, u8 = dB+256),
 * and ADPCM audio. It synthesises a noise floor with a few carriers so there is
 * something real to tune, zoom and listen to.
 *
 * This exists so web-client work doesn't need the Moto + dongle on the bench.
 * It is a DEV TOOL — it is not shipped and the app never talks to it.
 */

import { createServer } from 'node:http';
import crypto from 'node:crypto';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PORT = Number(process.argv[process.argv.indexOf('--port') + 1]) || 48000;
const PIN = process.argv.includes('--no-pin') ? null : '123456';
/* ★ --bad-messages: interleave MALFORMED traffic with the good stream, to prove a client drops one
 *   bad message and carries on (faultLog / PanelBoundary, 2026-09-28). Every kind a real radio or
 *   a broken build could produce: bad JSON, a bare null, truncated / bad-magic / header-only SPEC
 *   frames, wrong-typed RDS / config / hwinfo fields, and truncated or garbage audio frames. */
const BAD = process.argv.includes('--bad-messages');
const BAD_SPEC = [
  () => wsText('{"type":"rds","ps":'),                                        // truncated JSON
  () => wsText('null'),
  () => wsText('not json at all'),
  () => wsText(JSON.stringify({ type: 'rds', ps: 42, radiotext: { x: 1 }, pi: 'zz', stereo: 'yes' })),
  () => wsText(JSON.stringify({ type: 'rdsx', groups: 'x', bler: null, eye: 5, pty: [], af: 'q' })),
  () => wsText(JSON.stringify({ type: 'hwinfo', gains: 'lots', rates: 7, driver: 12 })),
  () => wsText(JSON.stringify({ type: 'health', cpu: 'hot', temp: {} })),
  () => wsBin(Buffer.from('SPEC\x01\x03', 'latin1')),                       // truncated header
  () => wsBin(Buffer.alloc(40, 0x55)),                                        // bad magic
  () => { const b = Buffer.alloc(22); b.write('SPEC', 0, 'ascii'); b.writeUInt8(1, 4); b.writeUInt8(3, 5); return wsBin(b); },  // no bins
  () => wsBin(Buffer.from([0x1f, 0x8b, 0x08, 0x00, 0xde, 0xad])),             // gzip garbage
];
const BAD_AUDIO = [
  () => { const b = Buffer.alloc(9); b.writeUInt8(1, 0); b.writeUInt8(1, 1); b.writeUInt32LE(48000, 2); b.writeUInt16LE(1024, 6); return wsBin(b); }, // ADPCM, 1 byte for 1024 samples
  () => wsBin(Buffer.from([1, 1, 0])),                                        // shorter than a header
  () => wsBin(Buffer.from([1, 9, 0x80, 0xbb, 0, 0, 1, 2, 3])),                // unknown format
  () => wsText('{"type":'),                                                   // truncated JSON
];

const BINS = 4096;
const FS = 2_400_000;          // capture bandwidth
const FPS = 20;

// ── Minimal RFC6455 server (no deps) ─────────────────────────────────────────

function wsAccept(key) {
  return crypto.createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
}

function wsFrame(payload, opcode) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.alloc(2);
    header[1] = len;
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  header[0] = 0x80 | opcode;
  return Buffer.concat([header, payload]);
}
const wsText = (s) => wsFrame(Buffer.from(s, 'utf8'), 0x1);
const wsBin  = (b) => wsFrame(Buffer.from(b), 0x2);

/** Parse client frames (always masked). Calls onText/onClose. */
function attachReader(sock, onText) {
  let buf = Buffer.alloc(0);
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) return;
      const opcode = buf[0] & 0x0f;
      const masked = (buf[1] & 0x80) !== 0;
      let len = buf[1] & 0x7f;
      let off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const maskLen = masked ? 4 : 0;
      if (buf.length < off + maskLen + len) return;
      const mask = masked ? buf.subarray(off, off + 4) : null;
      const payload = Buffer.from(buf.subarray(off + maskLen, off + maskLen + len));
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
      buf = buf.subarray(off + maskLen + len);

      if (opcode === 0x8) { sock.end(); return; }
      if (opcode === 0x9) { sock.write(wsFrame(payload, 0xA)); continue; }
      if (opcode === 0x1) onText(payload.toString('utf8'));
    }
  });
  sock.on('error', () => {});
}

// ── Auth (mirrors the shim's VsAuth) ─────────────────────────────────────────

const nonces = new Set();
function newNonce() {
  const n = crypto.randomBytes(16).toString('hex');
  nonces.add(n);
  return n;
}
function authOk(url) {
  if (!PIN) return true;
  const q = new URL(url, 'http://x').searchParams;
  const nonce = q.get('vs_nonce');
  const token = q.get('vs_auth');
  if (!nonce || !token || !nonces.has(nonce)) return false;
  const want = crypto.createHmac('sha256', PIN).update(nonce).digest('hex');
  return crypto.timingSafeEqual(Buffer.from(want), Buffer.from(token.padEnd(want.length).slice(0, want.length)));
}

// ── Radio state ──────────────────────────────────────────────────────────────

const state = {
  centerFreq: 100_000_000,   // display centre
  binBandwidth: FS / BINS,   // full span initially
  vfo: 100_000_000,
  mode: 'wfm',
};

/** Synthetic band: a few carriers we can see and tune. */
const CARRIERS = [
  { hz: 100_000_000, width: 150_000, amp: 55 },  // wide FM
  { hz: 100_700_000, width: 120_000, amp: 45 },
  { hz:  99_300_000, width: 140_000, amp: 50 },
  { hz: 101_500_000, width:  12_000, amp: 35 },  // narrow
];

function configMsg() {
  return JSON.stringify({
    type: 'config',
    centerFreq: Math.round(state.centerFreq),
    binCount: BINS,
    binBandwidth: state.binBandwidth,
    totalBandwidth: state.binBandwidth * BINS,
    maxBandwidth: FS,
  });
}
const hwinfoMsg = JSON.stringify({
  type: 'hwinfo',
  gains: [0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496],
  rates: [3200000, 2400000, 1800000, 1200000, 960000],
});

/** One SPEC frame: noise floor + carriers, in FFT order, u8 = dB + 256. */
function specFrame() {
  const span = state.binBandwidth * BINS;
  const lo = state.centerFreq - span / 2;
  const buf = Buffer.alloc(22 + BINS);
  buf.write('SPEC', 0, 'ascii');
  buf.writeUInt8(0x01, 4);
  buf.writeUInt8(0x03, 5);                                        // FULL_UINT8
  buf.writeBigUInt64LE(BigInt(Date.now()) * 1000000n, 6);
  buf.writeBigUInt64LE(BigInt(Math.round(state.centerFreq)), 14);

  const half = BINS >> 1;
  for (let i = 0; i < BINS; i++) {
    const hz = lo + (i / BINS) * span;
    let db = -108 + Math.random() * 5;                            // noise floor
    for (const c of CARRIERS) {
      const d = Math.abs(hz - c.hz);
      if (d < c.width) {
        const shape = Math.cos((d / c.width) * Math.PI / 2);      // rounded top
        db = Math.max(db, -108 + c.amp * shape * shape + Math.random() * 3);
      }
    }
    // Emit in FFT order: display index i -> fft index (i + half) % BINS
    const fftIdx = (i + half) % BINS;
    buf.writeUInt8(Math.max(0, Math.min(255, Math.round(db + 256))), 22 + fftIdx);
  }
  return buf;
}

// ── ADPCM audio (matches the shim's encoder) ─────────────────────────────────

const STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767];
const IDX = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];

function adpcmBlock(samples) {
  let pred = samples[0], index = 0;
  const out = Buffer.alloc(4 + Math.ceil(samples.length / 2));
  out.writeInt16LE(pred, 0);
  out[2] = index; out[3] = 0;
  for (let i = 0; i < samples.length; i++) {
    const step = STEP[index];
    let diff = samples[i] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let tmp = step >> 3;
    if (diff >= step)        { code |= 4; diff -= step;      tmp += step; }
    if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; tmp += step >> 1; }
    if (diff >= (step >> 2)) { code |= 1;                    tmp += step >> 2; }
    pred = Math.max(-32768, Math.min(32767, pred + ((code & 8) ? -tmp : tmp)));
    index = Math.max(0, Math.min(88, index + IDX[code]));
    const bi = 4 + (i >> 1);
    if (i % 2 === 0) out[bi] = code & 0x0f;
    else out[bi] |= (code & 0x0f) << 4;
  }
  return out;
}

/** Audio you can hear the tuning in: pitch tracks how far the VFO is from the
 *  nearest carrier, and it goes quiet (just hiss) when you're off-station. */
let phase = 0;
function audioFrame(count) {
  const nearest = CARRIERS.reduce((a, c) =>
    Math.abs(c.hz - state.vfo) < Math.abs(a.hz - state.vfo) ? c : a);
  const off = Math.abs(nearest.hz - state.vfo);
  const onStation = off < nearest.width;
  const strength = onStation ? 1 - (off / nearest.width) : 0;
  const toneHz = 300 + (nearest.hz % 700);

  const pcm = new Int16Array(count);
  for (let i = 0; i < count; i++) {
    const hiss = (Math.random() - 0.5) * 1500 * (1 - strength * 0.9);
    const tone = Math.sin(phase) * 9000 * strength;
    phase += 2 * Math.PI * toneHz / 48000;
    pcm[i] = Math.max(-32768, Math.min(32767, Math.round(tone + hiss)));
  }

  const block = adpcmBlock(pcm);
  const buf = Buffer.alloc(8 + block.length);
  buf.writeUInt8(1, 0);              // channels
  buf.writeUInt8(1, 1);              // format 1 = ADPCM mono
  buf.writeUInt32LE(48000, 2);
  buf.writeUInt16LE(count, 6);
  block.copy(buf, 8);
  return buf;
}

// ── Control channel (mirrors handleControl) ──────────────────────────────────

/** Last control message of each type — exposed at GET /debug/controls so the
 *  test can assert the client's UI actually reaches the server. */
const lastControls = new Map();

function handleControl(raw, send) {
  let m;
  try { m = JSON.parse(raw); } catch { return; }
  switch (m.type) {
    case 'ping': send(wsText(JSON.stringify({ type: 'pong' }))); break;
    case 'zoom':
      if (m.frequency) state.centerFreq = m.frequency;
      if (m.binBandwidth) state.binBandwidth = m.binBandwidth;
      send(wsText(configMsg()));
      break;
    case 'reset':
      state.binBandwidth = FS / BINS;
      state.centerFreq = state.vfo;
      send(wsText(configMsg()));
      break;
    case 'tune':
      if (m.frequency) state.vfo = m.frequency;
      if (m.mode) state.mode = m.mode;
      lastControls.set('tune', m);   // so a test can assert the exact Hz a tune reached (test-web-airband)
      break;
    case 'mode': state.mode = m.mode; break;
    // Hardware + audio-DSP controls. The real shim applies these server-side;
    // here we just record them so the client's UI can be verified end to end.
    case 'bandwidth': case 'gain': case 'biasT': case 'agc':
    case 'ppm': case 'sampleRate': case 'directSampling': case 'set_rate':
    case 'squelch': case 'nr': case 'notch': case 'deemph': case 'stereo':
      lastControls.set(m.type, m);
      console.log('  ctl:', raw);
      break;
  }
}

// ── HTTP + upgrade ───────────────────────────────────────────────────────────

const server = createServer(async (req, res) => {
  if (req.url.startsWith('/vibeserver/auth')) {
    const body = PIN
      ? JSON.stringify({ required: true, nonce: newNonce() })
      : JSON.stringify({ required: false });
    res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': body.length });
    res.end(body);
    return;
  }
  if (req.url === '/debug/controls') {
    const body = JSON.stringify(Object.fromEntries(lastControls));
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(body);
    return;
  }
  if (req.url === '/' || req.url.startsWith('/index')) {
    try {
      const html = await readFile(path.join(root, 'web/dist/vibesdr.html'), 'utf8');
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(html);
    } catch {
      res.writeHead(500).end('run: node scripts/build-web.mjs');
    }
    return;
  }
  res.writeHead(404).end();
});

server.on('upgrade', (req, sock) => {
  const key = req.headers['sec-websocket-key'];
  const isSpec = req.url.startsWith('/ws/user-spectrum');
  const isAudio = req.url.startsWith('/ws/audio');
  if (!key || (!isSpec && !isAudio)) { sock.destroy(); return; }

  if (!authOk(req.url)) {
    console.log('401', req.url.split('?')[0]);
    sock.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n');
    return;
  }

  sock.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${wsAccept(key)}\r\n\r\n`,
  );
  const send = (b) => { if (!sock.destroyed) sock.write(b); };
  attachReader(sock, (txt) => handleControl(txt, send));

  if (isSpec) {
    console.log('spectrum client connected');
    send(wsText(configMsg()));
    send(wsText(hwinfoMsg));
    let n = 0;
    let bad = 0;
    const t = setInterval(() => {
      send(wsBin(specFrame()));
      if (BAD && n % 5 === 0) send(BAD_SPEC[bad++ % BAD_SPEC.length]());
      if (++n % 10 === 0 && state.mode === 'wfm') {
        send(wsText(JSON.stringify({
          type: 'rds', stereo: true, ps: 'VIBE FM',
          radiotext: 'Mock VibeServer — synthetic carriers for web client dev',
          pi: 0xc201, ecc: 0xe1,
        })));
      }
    }, 1000 / FPS);
    sock.on('close', () => { clearInterval(t); console.log('spectrum client gone'); });
  }

  if (isAudio) {
    console.log('audio client connected');
    const CHUNK = 1024;                       // ~21ms at 48k
    let k = 0;
    const t = setInterval(() => {
      send(wsBin(audioFrame(CHUNK)));
      if (BAD && ++k % 25 === 0) send(BAD_AUDIO[(k / 25) % BAD_AUDIO.length]());
    }, (CHUNK / 48000) * 1000);
    sock.on('close', () => { clearInterval(t); console.log('audio client gone'); });
  }
});

server.listen(PORT, () => {
  console.log(`mock VibeServer on http://localhost:${PORT}`);
  console.log(PIN ? `PIN: ${PIN}` : 'no PIN');
  console.log(`carriers: ${CARRIERS.map(c => (c.hz / 1e6).toFixed(1) + 'M').join(', ')}`);
});
