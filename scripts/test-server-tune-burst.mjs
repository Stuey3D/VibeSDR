// test-server-tune-burst.mjs — the REAL server, end to end: a burst of tunes must leave the server
// responsive and the radio on the LAST frequency asked for, and a clump of tunes that arrives at once
// must be applied as its newest (vibe_tune_pace.h).
//
// ★★★ WHY (Stuart, 2026-10-05): "if the server's CPU is reporting that it is struggling we need to slow
//     down the amount of tune commands so that we don't overload it, I had a bit of a nightmare setting
//     up the pi2 in the garage last night probably due to network connection and the server CPU getting
//     bogged down." A slow link delivers a drum spin as a CLUMP; the reader used to apply every tune in
//     it, in order, to reach a frequency the user had already left.
// ★ Needs a BUILT vibeserver (it is the thing under test): VIBESERVER_BIN=/path/to/vibeserver.
//   Without one it says so and reports "not run" — a result, not a pass (see run-tests.sh).
// ★ SILENT: nothing here plays audio.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-tune-burst.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
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
const getJson = (port, p) => new Promise((res) => {
  http.get({ host: '127.0.0.1', port, path: p }, (r) => { let b = ''; r.on('data', (d) => { b += d; }); r.on('end', () => { try { res(JSON.parse(b)); } catch { res(null); } }); })
    .on('error', () => res(null));
});

/** A minimal WebSocket client (the probes' own — same as test-server-vfo-background.mjs). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, closed: false };
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
      if (op === 0x1) st.txt.push({ at: Date.now(), t: p.toString() });
    }
  });
  const frame = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    return Buffer.concat([h, b]);   // zero mask: payload unchanged
  };
  st.send = (o) => s.write(frame(o));
  /** Many messages in ONE write — what a slow link hands the server after a stall. */
  st.sendClump = (list) => s.write(Buffer.concat(list.map(frame)));
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
/** Ask for the full state and time the answer — the "is it still responsive" probe. */
async function askState(sp) {
  const n0 = sp.txt.length, t0 = Date.now();
  sp.send({ type: 'state' });
  for (let i = 0; i < 100; i++) {
    const c = sp.txt.slice(n0).find((m) => m.t.includes('"type":"config"'));
    if (c) {
      // ★ The FIRST config times the answer; the LAST one, a moment later, is where the radio settled
      //   (a held tune applied just before the state reply sends its own config first).
      await sleep(300);
      const all = sp.txt.slice(n0).filter((m) => m.t.includes('"type":"config"'));
      return { ms: c.at - t0, cfg: JSON.parse(all[all.length - 1].t) };
    }
    await sleep(20);
  }
  return { ms: Infinity, cfg: null };
}

/** One server + fake dongle; `extraEnv` lets a phase set the pacing floor. */
async function startServer(tag, extraEnv = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-tuneburst-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000',
                          '--freq', '96000000', '--mode', 'wfm', '--users', '10', '--idle-grace', '0'], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const readLog = () => fs.readFileSync(logPath, 'utf8');
  return { port, logPath, retunes: () => (readLog().match(/retune -> /g) || []).length,
           merged: async () => (await getJson(port, '/vibeserver.json'))?.tunesMerged ?? 0,
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
const logs = [];
const sid = 'cccccccc-3333';

{
  console.log('── idle server (pace 0) ──');
  const S = await startServer('idle');
  logs.push(S.logPath);
  try {
    const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
    const aud = ws(S.port, `/ws/audio?user_session_id=${sid}`);
    await sleep(1200);
    ok(sp.open && aud.open, 'both sockets open');

    const j0 = await getJson(S.port, '/vibeserver.json');
    ok(j0 && j0.tunePaceMs === 0 && typeof j0.tunesMerged === 'number',
       `/vibeserver.json states the pacing — an idle Mac paces nothing (tunePaceMs ${j0 && j0.tunePaceMs}, tunesMerged ${j0 && j0.tunesMerged})`);

    console.log('   50 tunes in 1 s, 20 ms apart, across the FM band (re-centres included)');
    const freqs = Array.from({ length: 50 }, (_, k) => 88_000_000 + k * 400_000);
    for (const f of freqs) { sp.send({ type: 'tune', frequency: f, mode: 'wfm' }); await sleep(20); }
    const a = await askState(sp);
    ok(a.ms < 1500, `the server answers a state request straight after the burst (${a.ms} ms)`);
    ok(a.cfg && a.cfg.vfo === freqs[49], `...and it is on the LAST frequency asked for (${a.cfg && a.cfg.vfo}, want ${freqs[49]})`);
    ok(!sp.closed && !aud.closed, 'neither socket was dropped');

    console.log('   the same 50 as ONE clump (a slow link after a stall)');
    const r0 = S.retunes(), m0 = await S.merged();
    const back = Array.from({ length: 50 }, (_, k) => 107_600_000 - k * 400_000);
    sp.sendClump(back.map((f) => ({ type: 'tune', frequency: f, mode: 'wfm' })));
    const b = await askState(sp);
    const r1 = S.retunes(), m1 = await S.merged();
    ok(b.cfg && b.cfg.vfo === back[49], `the clump ends on its LAST frequency (${b.cfg && b.cfg.vfo}, want ${back[49]})`);
    ok(m1 - m0 >= 40, `★ the clump was merged, not worked through (${m1 - m0} of 49 merged)`);
    ok(r1 - r0 <= 5, `★ ...so the hardware retuned a handful of times, not 50 (${r1 - r0})`);
    ok(b.ms < 1500, `and the server is responsive after it (${b.ms} ms)`);

    console.log('   a mode switch inside a clump is never merged away');
    sp.sendClump([{ type: 'tune', frequency: 96_100_000, mode: 'wfm' }, { type: 'tune', frequency: 96_200_000, mode: 'wfm' },
                  { type: 'tune', frequency: 96_200_000, mode: 'nfm' }]);
    const c = await askState(sp);
    ok(c.cfg && c.cfg.vfo === 96_200_000 && c.cfg.mode === 'nfm', `the mode switch landed (${c.cfg && c.cfg.mode} at ${c.cfg && c.cfg.vfo})`);
    sp.close(); aud.close();
  } finally { S.stop(); }
}

{
  console.log('── a LOADED server (pace floor 150 ms, as dspCpu >= 70 or the snail would set) ──');
  const S = await startServer('loaded', { VIBESERVER_TUNE_PACE_MS: '150' });
  logs.push(S.logPath);
  try {
    const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
    const aud = ws(S.port, `/ws/audio?user_session_id=${sid}`);
    await sleep(1200);
    const j0 = await getJson(S.port, '/vibeserver.json');
    ok(j0 && j0.tunePaceMs === 150, `/vibeserver.json reports the pace (${j0 && j0.tunePaceMs} ms)`);
    const r0 = S.retunes();
    const freqs = Array.from({ length: 50 }, (_, k) => 88_000_000 + k * 400_000);
    for (const f of freqs) { sp.send({ type: 'tune', frequency: f, mode: 'wfm' }); await sleep(20); }
    // ★ No state request: the TRAILING apply must land on its own, from the reader's hold timer.
    await sleep(600);
    const r1 = S.retunes(), m = await S.merged();
    ok(r1 - r0 <= 12, `★ 50 tunes over 1 s became ${r1 - r0} hardware retunes (one per ~150 ms at most)`);
    ok(m >= 35, `...the rest merged into newer ones (${m})`);
    const a = await askState(sp);
    ok(a.cfg && a.cfg.vfo === freqs[49], `★ the LAST tune landed on its own, by the hold timer (${a.cfg && a.cfg.vfo}, want ${freqs[49]})`);
    ok(a.ms < 1500 && !sp.closed && !aud.closed, `still responsive (${a.ms} ms), nothing dropped`);
    sp.close(); aud.close();
  } finally { S.stop(); }
}
if (fail) console.log(`   server logs: ${logs.join(' ')}`);
console.log(`tune burst: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
