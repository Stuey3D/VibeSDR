// test-server-dab-exit.mjs — EXIT DAB STICKS: once the server says `dab_off`, it says nothing more about DAB.
//
// ★★★ THE FAULT (Stuart, 2026-10-05 23:06, iPhone → Pi 2, RTL V4): "I pressed the Exit DAB button which
//     took me back to MW … but for some reason after a few seconds the DAB decoder box popped up again
//     this time over the MW signal and needed to have exit DAB pressed again. It's been a bug for a
//     little while now, it sometimes works normally though."
//     The Pi's journal: "[DAB] mode OFF" at 23:06:17, then "[DAB] mode ON: channel 13B" at 23:06:27 with
//     no tune, landing, resume or scan line around it — a CLIENT's `dab on` with no block (the last one).
//     The app re-opened its box on a `dab` report that arrived after it had left (the DSP thread's
//     twice-a-second report was not ordered against the socket thread's dab_off), and the box's EXIT —
//     wired to the DAB toggle — then ENTERED DAB. The app half is pinned by scripts/test_dab_stepper.ts;
//     this pins the server half on a REAL vibeserver against fake rtl_tcp:
//     1. ★ NO `dab` REPORT AFTER `dab_off`, over many enter/exit cycles (the DSP-thread race; a slow box
//        widens it — run under load to see the old binary lose it);
//     2. ★ a `dab_service` out of DAB is not answered with a `dab` report (the old server answered every
//        one with the full DAB state, which a client reads as "the receiver is in DAB") — deterministic;
//     3. after the last exit the radio STAYS out: no `mode ON` in the log and no report for 6 s;
//     4. the server names who asked: "[DAB] ON asked by spectrum [<session>]" / "OFF asked by …".
// ★ SILENT: audio frames are received and discarded, never played.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-dab-exit.mjs
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

const FM = 100000000;
const CYCLES = Number(process.env.DAB_EXIT_CYCLES || 25);

/** A minimal WebSocket client, recording every text frame in arrival order. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0 };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin++;
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
const kind = (t) => { const m = /"type"\s*:\s*"([^"]+)"/.exec(t); return m ? m[1] : ''; };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabexit-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort(), front = await freePort();
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, fullMode: true, sharing: 'public', port: front,
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: FM, demodMode: 'wfm',
             landingFreq: FM, users: 10 }],
}));
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--idle-grace', '0'],
                  { env, stdio: ['ignore', logFd, logFd] });
const log = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
await sleep(500);

try {
  // The app's order: audio first, spectrum a second later.
  const SID = 'e1e1e1e1-dab0-exit-0000-000000000001';
  const aud = ws(port, `/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(1000);
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
  await sleep(1500);

  // ── 1. many enter/exit cycles: nothing about DAB after dab_off ──
  console.log(`── ${CYCLES} enter/exit cycles: no \`dab\` report may follow \`dab_off\` ──`);
  let entered = 0, late = 0, exits = 0;
  for (let i = 0; i < CYCLES; i++) {
    spec.send({ type: 'dab', on: 1, channel: '12B' });   // the server takes a block's name too
    await sleep(1100 + (i % 5) * 97);                 // reports flowing, at varied phases of the 0.5 s tick
    const mark = spec.txt.length;
    spec.send({ type: 'dab', on: 0 });
    await sleep(900);
    const after = spec.txt.slice(mark).map(kind);
    const offAt = after.indexOf('dab_off');
    if (spec.txt.slice(0, mark).some((t) => kind(t) === 'dab')) entered++;
    if (offAt >= 0) { exits++; if (after.slice(offAt + 1).includes('dab')) late++; }
  }
  ok(entered > 0 && exits === CYCLES, `every cycle entered and left (${exits}/${CYCLES} dab_off)`);
  ok(late === 0, `no \`dab\` report after \`dab_off\` in any cycle (${late} of ${exits} had one)`);

  // ── 2. a service pick out of DAB is not answered with a DAB report ──
  console.log('── `dab_service` while out of DAB ──');
  {
    const mark = spec.txt.length;
    spec.send({ type: 'dab_service', sid: 0xC0E1 });
    await sleep(1000);
    const n = spec.txt.slice(mark).filter((t) => kind(t) === 'dab').length;
    ok(n === 0, `no \`dab\` report in reply to a service pick out of DAB (${n})`);
  }

  // ── 3. out stays out ──
  console.log('── after the exit, the radio stays out of DAB ──');
  {
    const mark = spec.txt.length, logMark = log().length;
    await sleep(6000);
    const n = spec.txt.slice(mark).filter((t) => kind(t) === 'dab').length;
    ok(n === 0, `no \`dab\` report in the 6 s after the last exit (${n})`);
    ok(!log().slice(logMark).includes('[DAB] mode ON'), 'no "[DAB] mode ON" in the log after the last exit');
  }

  // ── 4. who asked ──
  console.log('── the log names who asked ──');
  const L = log();
  ok(L.includes(`[DAB] ON asked by spectrum [${SID}] with a block`), 'the entry is logged with its socket kind and session');
  ok(L.includes(`[DAB] OFF asked by spectrum [${SID}]`), 'the exit is logged with its socket kind and session');
  spec.close(); aud.close();
} finally {
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\ndab exit: ${pass} passed, ${fail} failed   (server log: ${path.join(dir, 'server.log')})`);
process.exit(fail ? 1 : 0);
