// test-server-vfo-background.mjs — the REAL server, end to end: on a per-VFO radio a listener's channel
// outlives their SPECTRUM socket while their AUDIO socket is open, and the returning spectrum socket
// takes the same channel back.
//
// ★★★ WHY (Stuart, 2026-10-05, iOS on the Pi 500's RSP — locked range, a VFO per listener): "why did the
//     full socket drop when minimised though, especially as I had audio and a decoder running". The app
//     closes ONLY its spectrum socket in the background; the channel (pipeline, VFO, encoder, the decoder
//     host it feeds) was keyed by that socket and died with it — audio went silent, the decoder starved.
//     See "THE CHANNEL OUTLIVES ITS SPECTRUM SOCKET" in local_sdr_shim.cpp.
//
// ★ Needs a BUILT vibeserver (it is the thing under test): VIBESERVER_BIN=/path/to/vibeserver.
//   Without one it says so and reports "not run" — a result, not a pass (see run-tests.sh).
// ★ SILENT: nothing here plays audio; the audio frames are only counted.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-vfo-background.mjs
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

/** A minimal WebSocket client (the probes' own — same as test-server-decoders.mjs), recording everything. */
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
      if (op === 0x2) st.bin.push(p.length);      // ★ only counted — keep no audio in memory
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
const texts = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`));
const firstConfig = (st) => { const c = texts(st, 'config')[0]; return c ? JSON.parse(c) : null; };
/** WEFAX line frames on a decoder socket — 9-byte header + the image width, as the decoders test reads them. */
let dxLines = null;

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-bg-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', ...args], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {            // wait for the port
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, log: () => fs.readFileSync(logPath, 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
const WEFAX = { type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false };

{
  console.log('── per-VFO radio (locked range): the app goes to the background and comes back ──');
  const S = await server(['--freq', '14100000', '--lock-freq', '14100000', '--users', '10', '--mode', 'usb', '--idle-grace', '0'], 'vfo');
  const X = 14080000;
  const sid = 'aaaaaaaa-1111';
  const spec = `/ws/user-spectrum?user_session_id=${sid}&bins=256`;
  try {
    // A second listener elsewhere, so the radio is genuinely shared while ours is pocketed.
    const other = ws(S.port, '/ws/user-spectrum?user_session_id=bbbbbbbb-2222&bins=256');
    const otherAud = ws(S.port, '/ws/audio?user_session_id=bbbbbbbb-2222');
    let sp = ws(S.port, spec);
    await sleep(300);
    const aud = ws(S.port, `/ws/audio?user_session_id=${sid}`);
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    await sleep(400);
    sp.send({ type: 'tune', frequency: X, mode: 'usb' });
    other.send({ type: 'tune', frequency: 14300000, mode: 'usb' });
    await sleep(600);
    dx.send(WEFAX);
    await sleep(2500);
    dxLines = () => dx.bin.filter((n) => n === 9 + 1809).length;
    ok(texts(dx, 'audio_extension_attached').length === 1, 'the decoder (WEFAX) is attached');
    ok(dxLines() >= 2, `...and drawing lines from this listener's audio (${dxLines()})`);

    // ── Background: ONLY the spectrum socket closes ──
    sp.close();
    await sleep(500);
    const a0 = aud.bin.length, l0 = dxLines();
    await sleep(4000);
    const a1 = aud.bin.length, l1 = dxLines();
    ok(!aud.closed, 'the audio socket stays open with the spectrum socket gone');
    ok(a1 - a0 >= 40, `audio KEEPS ARRIVING for 4 s with no spectrum socket (${a1 - a0} frames)`);
    ok(l1 - l0 >= 2, `the decoder KEEPS DECODING in the background (${l1 - l0} more lines)`);
    ok(/spectrum socket closed, audio still open — their channel keeps running/.test(S.log()),
       'the server says the channel was kept, not torn down');

    // ── Foreground: the same session's spectrum socket returns ──
    sp = ws(S.port, spec);
    await sleep(700);
    const cfg = firstConfig(sp);
    ok(cfg && cfg.vfo === X, `the returning socket's FIRST config reports its own VFO (${cfg && cfg.vfo}, want ${X})`);
    ok(cfg && cfg.mode === 'usb', `...and its mode (${cfg && cfg.mode})`);
    const lg = S.log();
    ok(/returned — their channel kept running .* adopted, not rebuilt/.test(lg), 'the channel was ADOPTED, not rebuilt');
    ok(!new RegExp(`listener ${sid}: own channel at`).test(lg.split('adopted, not rebuilt')[1] || ''),
       '...and no new channel was built for the session after it');
    ok(!/new session — landing on/.test(lg.split('adopted, not rebuilt')[1] || ''), '...and nothing re-landed');
    const a2 = aud.bin.length, l2 = dxLines();
    await sleep(1500);
    ok(aud.bin.length - a2 >= 15, `audio still flows after the return (${aud.bin.length - a2} frames)`);
    ok(dxLines() > l2, 'the decoder is still running after the return — same host, never restarted');
    // ★ A tune from the returned socket moves THIS channel (it is keyed by the new socket now) — proven
    //   below by the memo it leaves — and nobody else's: the other listener, pocketed and brought back
    //   the same way, still finds its own VFO.
    sp.send({ type: 'tune', frequency: 14090000, mode: 'usb' });
    await sleep(500);
    other.close();
    await sleep(500);
    const other2 = ws(S.port, '/ws/user-spectrum?user_session_id=bbbbbbbb-2222&bins=256');
    await sleep(700);
    const ocfg = firstConfig(other2);
    ok(ocfg && ocfg.vfo === 14300000, `the other listener, pocketed and back, is still on its own VFO (${ocfg && ocfg.vfo})`);

    // ── Everything closes: spectrum first, then audio. The audio close retires the channel and
    //    remembers it, so the next visit resumes it (sessionVfo, 3e556f9c). ──
    dx.close(); sp.close();
    await sleep(400);
    aud.close();
    await sleep(800);
    const back = ws(S.port, spec);
    await sleep(700);
    const bcfg = firstConfig(back);
    ok(bcfg && bcfg.vfo === 14090000, `after EVERY socket closed, a return resumes the remembered VFO (${bcfg && bcfg.vfo})`);
    ok(/returned — resuming their own VFO, not the landing/.test(S.log()), '...through the memo, the channel having been retired');
    back.close(); other2.close(); otherAud.close();
    await sleep(300);
  } finally { S.stop(); }
}

console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
