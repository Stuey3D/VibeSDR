// test-server-decoders.mjs — the REAL server, end to end: per-listener decoders, the decoder limit,
// Advanced RDS only to the listener who asked, and an audio socket that opens first keeping its codec.
//
// ★★★ WHY END TO END. test-decoder-hosts.cpp proves the hosts and the router with real decoders on
//     real signals. This proves the SHIM wires them the way the design says — which socket reaches
//     which host, whose audio feeds it, what a refused listener is told — through the same
//     WebSockets the web client and the app use. The faults it pins were all measured on the Pi 500
//     (2026-09-30): one WEFAX replacing everybody's RTTY; rdsx shoved at listeners who never opened
//     the panel; an Opus listener sent PCM because its audio socket arrived first.
//
// ★ Needs a BUILT vibeserver (it is the thing under test): VIBESERVER_BIN=/path/to/vibeserver.
//   Without one it says so and reports "not run" — a result, not a pass (see run-tests.sh).
// ★ SILENT: nothing here plays audio; the audio frames are only inspected.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-decoders.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client (the probes' own), recording everything it receives. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, closed: false, sock: s };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin.push(Buffer.from(p));
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));   // zero mask: payload unchanged
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const wefaxLines = (st) => st.bin.filter((b) => b.length === 9 + 1809 && b[0] === 0x01 && b.readUInt32BE(5) === 1809).length;
const texts = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`));
const rdsx = (st) => texts(st, 'rdsx').length;

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dec-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', ...args], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {            // wait for the port
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, { audioFirst = false, codec = '' } = {}) {
  const L = {};
  if (audioFirst) { L.aud = ws(port, `/ws/audio?user_session_id=${sid}${codec ? '&codec=' + codec : ''}`); await sleep(400); }
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(300);
  if (!audioFirst && codec !== null) { L.aud = ws(port, `/ws/audio?user_session_id=${sid}${codec ? '&codec=' + codec : ''}`); }
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(300);
  return L;
}
const WEFAX = { type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false };
const RTTY = { type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1000, shift: 170, baud_rate: 45.45, framing: '5N1.5', encoding: 'ITA2' };

// ── 1. A range-locked radio: every listener their own VFO and their own decoders ─────────────────
{
  console.log('── per-VFO radio (locked range, 10 listeners), decoder limit 3 ──');
  const S = await server(['--freq', '14100000', '--lock-freq', '14100000', '--users', '10', '--mode', 'usb', '--decoder-max', '3', '--idle-grace', '0'], 'vfo');
  try {
    const A = await listener(S.port, 'aaaaaaaa'), B = await listener(S.port, 'bbbbbbbb'),
          C = await listener(S.port, 'cccccccc'), D = await listener(S.port, 'dddddddd');
    A.spec.send({ type: 'tune', frequency: 14080000, mode: 'usb' });
    B.spec.send({ type: 'tune', frequency: 14300000, mode: 'usb' });
    C.spec.send({ type: 'tune', frequency: 14074000, mode: 'usb' });
    await sleep(500);
    A.dx.send(RTTY);
    await sleep(300);
    B.dx.send(WEFAX);
    await sleep(300);
    C.dx.send({ type: 'subscribe_digital_spots' });
    await sleep(300);
    D.dx.send(RTTY);
    await sleep(4500);
    ok(texts(A.dx, 'audio_extension_attached').length === 1, 'A: RTTY attached');
    ok(texts(B.dx, 'audio_extension_attached').length === 1, 'B: WEFAX attached — while A\'s RTTY runs');
    ok(wefaxLines(B.dx) >= 3, `B's decoder socket draws WEFAX lines from B's own audio (${wefaxLines(B.dx)})`);
    ok(wefaxLines(A.dx) === 0, 'A received NO WEFAX — B starting WEFAX did not replace A\'s RTTY');
    ok(wefaxLines(C.dx) === 0 && C.dx.bin.length === 0, 'C (FT8 only) received no WEFAX and no RTTY frames');
    const refused = texts(D.dx, 'decoder_refused');
    ok(refused.length === 1, 'D, the 4th decoder on a 3-slot server, is REFUSED');
    ok(refused.length === 1 && refused[0].includes('All 3 decoder slots on this server are in use') && refused[0].includes('"reason":"limit"'),
       `...with words the listener can read (${(refused[0] || '').slice(0, 140)})`);
    ok(texts(D.dx, 'audio_extension_attached').length === 0 && D.dx.bin.length === 0, '...and D received nothing from anybody else\'s decoder');
    // A slot frees the moment a listener closes theirs.
    C.dx.send({ type: 'unsubscribe_digital_spots' });
    await sleep(300);
    D.dx.send(RTTY);
    await sleep(500);
    ok(texts(D.dx, 'audio_extension_attached').length === 1, 'C closing FT8 frees a slot — D\'s RTTY starts at once');
    const E = await listener(S.port, 'eeeeeeee');
    E.dx.send(WEFAX); await sleep(500);
    ok(texts(E.dx, 'decoder_refused').length === 1, 'full again: E is refused');
    A.dx.close(); await sleep(600);
    E.dx.send(WEFAX); await sleep(600);
    ok(texts(E.dx, 'audio_extension_attached').length === 1, 'A LEAVING (decoder socket closed) frees its slot for E');
    const Bl = wefaxLines(B.dx); await sleep(1500);
    ok(wefaxLines(B.dx) > Bl, 'B\'s WEFAX is still running through all of it');
    // ★ A session-less decoder socket (a tab from before the upgrade) with nobody unique at its address.
    const anon = ws(S.port, '/ws/dxcluster'); await sleep(300);
    anon.send(RTTY); await sleep(400);
    const nr = texts(anon, 'decoder_refused');
    ok(nr.length === 1 && nr[0].includes('"reason":"no_session"'), 'a decoder socket with no session and no unique listener is refused, never guessed');
    for (const L of [A, B, C, D, E]) for (const k of ['spec', 'aud', 'dx']) L[k]?.close();
    anon.close();

    // ── Coordinator A: an audio socket that opens FIRST keeps its codec ──
    console.log('── audio socket before spectrum socket (per-VFO) ──');
    const F = await listener(S.port, 'ffffffff', { audioFirst: true, codec: 'opus' });
    F.spec.send({ type: 'tune', frequency: 14200000, mode: 'usb' });
    await sleep(2500);
    const frames = F.aud.bin.filter((b) => b.length > 6);
    const opus = frames.filter((b) => b[1] === 3).length, pcm = frames.filter((b) => b[1] === 0).length;
    ok(frames.length > 10 && pcm === 0 && opus > 0, `asked codec=opus, audio first: ${opus} Opus / ${pcm} PCM frames`);
    // ★ The other ordering: the spectrum socket reconnects while audio plays on.
    F.spec.close(); await sleep(500);
    F.spec = ws(S.port, '/ws/user-spectrum?user_session_id=ffffffff&bins=256'); await sleep(300);
    F.aud.bin.length = 0; await sleep(1500);
    const f2 = F.aud.bin.filter((b) => b.length > 6);
    ok(f2.length > 10 && f2.every((b) => b[1] === 3), `after a spectrum reconnect the SAME audio socket still gets Opus (${f2.length} frames)`);
    const G = await listener(S.port, 'gggggggg', { codec: 'opus' });
    await sleep(1500);
    const g = G.aud.bin.filter((b) => b.length > 6);
    ok(g.length > 10 && g.every((b) => b[1] === 3), 'spectrum first, then audio: Opus (the order that always worked)');
    for (const L of [F, G]) for (const k of ['spec', 'aud', 'dx']) L[k]?.close();
  } finally { S.stop(); }
}

// ── 2. A shared dial: one decoder, mirrored; Advanced RDS only to whoever asked ─────────────────
{
  console.log('── shared dial (one VFO, 10 listeners) ──');
  const S = await server(['--freq', '100000000', '--users', '10', '--mode', 'wfm', '--decoder-max', '3', '--idle-grace', '0'], 'shared');
  try {
    const X = await listener(S.port, 'xxxxxxxx', { codec: 'opus' }), Y = await listener(S.port, 'yyyyyyyy', { codec: 'opus' });
    await sleep(800);
    X.spec.txt.length = 0; Y.spec.txt.length = 0;
    // Web path: X opens Advanced RDS by attaching `rds` on its decoder socket.
    X.dx.send({ type: 'audio_extension_attach', extension_name: 'rds' });
    await sleep(1500);
    ok(rdsx(X.spec) >= 3, `X opened Advanced RDS and receives it (${rdsx(X.spec)})`);
    ok(rdsx(Y.spec) === 0, `Y never asked and receives NONE (${rdsx(Y.spec)}) — nothing shoved in their face`);
    // App path: Y asks on its control socket too.
    Y.spec.send({ type: 'rdsx', on: 1 });
    await sleep(1200);
    const x1 = rdsx(X.spec), y1 = rdsx(Y.spec);
    ok(y1 >= 3, `Y asked too and now receives it (${y1})`);
    X.dx.send({ type: 'audio_extension_detach' });
    await sleep(500);
    const xa = rdsx(X.spec), ya = rdsx(Y.spec);
    await sleep(1200);
    ok(rdsx(X.spec) === xa, 'X closed the panel: X receives no more');
    ok(rdsx(Y.spec) > ya + 2, 'Y keeps it — X closing changes nothing for Y');
    X.dx.send({ type: 'audio_extension_attach', extension_name: 'rds' });
    await sleep(600);
    Y.spec.close(); await sleep(600);
    const xb = rdsx(X.spec); await sleep(1200);
    ok(rdsx(X.spec) > xb + 2, 'Y LEAVING with it open changes nothing for X');
    X.dx.send({ type: 'audio_extension_detach' });
    await sleep(500);
    const xc = rdsx(X.spec); await sleep(1200);
    ok(rdsx(X.spec) === xc, 'the last reader closes it: nobody receives it any more');
    void x1;
    // Decoders on a shared dial are shared: a mirror, one slot.
    const Z = await listener(S.port, 'zzzzzzzz');
    X.dx.bin.length = 0; Z.dx.bin.length = 0;
    X.dx.send(WEFAX);
    await sleep(3500);
    ok(wefaxLines(X.dx) >= 3 && wefaxLines(Z.dx) >= 3, `on a shared dial the decoder is everybody's (X ${wefaxLines(X.dx)}, Z ${wefaxLines(Z.dx)} lines)`);
    const zb = wefaxLines(Z.dx);
    Z.dx.send(WEFAX);             // the SAME decoder — joins, does not restart the chart
    await sleep(800);
    ok(texts(Z.dx, 'audio_extension_attached').length === 1 && wefaxLines(Z.dx) >= zb, 'opening the same decoder joins it (and is caught up), rather than restarting it');
    for (const L of [X, Z]) for (const k of ['spec', 'aud', 'dx']) L[k]?.close();
    Y.aud?.close(); Y.dx?.close();
  } finally { S.stop(); }
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
