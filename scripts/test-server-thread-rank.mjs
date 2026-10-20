// test-server-thread-rank.mjs — the REAL server: each thread holds the rung of the ladder it should (vibe_thread.h,
// 2026-10-20), read from /proc while a listener is connected, and the audio keeps its cadence while other work at the
// DSP's own priority eats every core.
//
// ★★★ THE FINDING THIS GUARDS (Pi 500, 2026-10-20): on a multi-radio server every radio is its own process and nice is
//     compared across all of them, so one radio's DSP (-19) must never outrank another radio's AUDIO send. The rule is
//     Network > Audio > Spectrum > Decoders: audio send ≥ DSP > waterfall send ≥ spectrum.
// ★★ THE LOAD HALF stands in for "another radio's DSP": one busy loop per core at nice -19 (the DSP's rung), in other
//    processes. The fake dongle is moved to SCHED_FIFO for it: it plays the HARDWARE, which DMAs its samples whatever
//    the CPU is doing — and it is not light (node synthesising 2 MS/s took ~80 % of a core against the server's ~18 %
//    in the measuring container), so at a mere -20 it was the fake that starved and the test measured that: audio
//    fell to 35-40 % with every server thread on its rung. This script, which plays the listener's network, takes -20.
//    What is under test is the server between them: its audio must arrive at the same rate, without a long gap.
// ★ LINUX ONLY (thread ranks are read from /proc); elsewhere "not run" (exit 3). Without CAP_SYS_NICE (a developer's
//   shell) the server cannot take its negative rungs at all — reported NOT RUN, never as a pass. In a container:
//   `docker run --cap-add SYS_NICE …` as root gives exactly the unit's AmbientCapabilities=CAP_SYS_NICE.
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=…; without one, "not run" (exit 3). SILENT: audio frames are only counted.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
if (process.platform !== 'linux') { console.log(`   not run — thread ranks are read from /proc (Linux); this is ${process.platform}`); process.exit(3); }
let pass = 0, fail = 0, notRun = 0;   // ★ a half that could not run makes the whole result NOT RUN (exit 3), never a pass
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { binAt: [], binLen: [], closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
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
      if (op === 0x2) { st.binAt.push(Date.now()); st.binLen.push(p.length); }
    }
  });
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

/** Every thread of `pid` and of its descendants (a multi-radio server forks one process per radio): {pid, comm, nice}. */
function threads(pid) {
  const pids = new Set([pid]);
  for (let grew = true; grew; ) {
    grew = false;
    for (const e of fs.readdirSync('/proc')) {
      if (!/^\d+$/.test(e) || pids.has(+e)) continue;
      try {
        const st = fs.readFileSync(`/proc/${e}/stat`, 'utf8');
        const ppid = +st.slice(st.lastIndexOf(')') + 2).split(' ')[1];
        if (pids.has(ppid)) { pids.add(+e); grew = true; }
      } catch {}
    }
  }
  const out = [];
  for (const p of pids) {
    let tids = [];
    try { tids = fs.readdirSync(`/proc/${p}/task`); } catch { continue; }
    for (const t of tids) {
      try {
        const st = fs.readFileSync(`/proc/${p}/task/${t}/stat`, 'utf8');
        const f = st.slice(st.lastIndexOf(')') + 2).split(' ');   // f[0] = field 3 (state) → nice (field 19) = f[16]
        out.push({ pid: p, comm: fs.readFileSync(`/proc/${p}/task/${t}/comm`, 'utf8').trim(), nice: +f[16] });
      } catch {}
    }
  }
  return out;
}

/** Audio BYTES per second (frames may batch differently under load — the bytes are the sound), frames per second,
 *  and the longest silence between two frames, over [from, to). */
function cadence(st, from, to) {
  const idx = st.binAt.map((x, i) => i).filter((i) => st.binAt[i] >= from && st.binAt[i] < to);
  const t = idx.map((i) => st.binAt[i]);
  const bytes = idx.reduce((a, i) => a + st.binLen[i], 0);
  let gap = 0;
  for (let i = 1; i < t.length; i++) gap = Math.max(gap, t[i] - t[i - 1]);
  if (t.length) gap = Math.max(gap, t[0] - from, to - t[t.length - 1]);
  return { fps: t.length * 1000 / (to - from), bps: bytes * 1000 / (to - from), gap: t.length ? gap : to - from };
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-rank-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--wfm', '100.0'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
// ★ The server starts at this script's nice (0, like the unit's process) and takes its own rungs; nothing here is
//   reniced until it is up, or its threads would inherit the change.
const server = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '100000000',
                           '--mode', 'wfm', '--users', '4', '--idle-grace', '0'], { env, stdio: ['ignore', log, log] });
// ★ May we lower nice at all? (root with CAP_SYS_NICE.) Tried on the fake dongle, which takes -20 if so: it plays the
//   hardware. Decides whether the load half can run honestly.
let privileged = false;
try { os.setPriority(rtl.pid, -20); privileged = true; } catch {}
const burners = [];
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const S = 'cccccccc-rank';
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${S}&bins=256`);
  await sleep(300);
  const aud = ws(port, `/ws/audio?user_session_id=${S}`);
  await sleep(4000);

  console.log('── the thread ladder, read from /proc on the running server ──');
  const th = threads(server.pid);
  const of = (name) => th.filter((t) => t.comm === name);
  const niceOf = (name) => { const a = of(name); return a.length ? Math.max(...a.map((t) => t.nice)) : null; };   // the WORST of them
  const summary = ['vibeTx-audio', 'vibeTx-wf', 'vibeTx', 'vibe-rtl', 'vibe-dsp', 'vibe-listener', 'vibe-spec']
    .map((n) => `${n} ${of(n).map((t) => t.nice).join('/') || '-'}`).join(' · ');
  console.log(`   ${summary}`);
  if (process.env.VERBOSE) console.log('   all: ' + th.map((t) => `${t.comm}:${t.nice}`).join(' '));
  ok(of('vibeTx-audio').length >= 1, `the audio socket's writer is classified and named (${of('vibeTx-audio').length})`);
  ok(of('vibeTx-wf').length >= 1, `the waterfall socket's writer is classified and named (${of('vibeTx-wf').length})`);
  ok(of('vibe-dsp').length >= 1 && aud.binAt.length > 0, `the DSP runs and audio arrives (${aud.binAt.length} frames)`);
  const dsp = niceOf('vibe-dsp');
  if (dsp !== -19) {
    notRun++;
    console.log(`   NOT RUN the negative rungs — vibe-dsp is at ${dsp}: this process may not lower nice (no CAP_SYS_NICE). `
                + 'Run as root with --cap-add SYS_NICE, as the unit grants it.');
    ok(niceOf('vibeTx-audio') <= dsp, 'refused: the audio send is still not below the DSP');
  } else {
    const a = niceOf('vibeTx-audio'), w = niceOf('vibeTx-wf'), sp = niceOf('vibe-spec');
    ok(a === -20, `★★★ the audio send at -20 (${a})`);
    ok(a <= dsp, `★★★ audio send (${a}) ≥ DSP (${dsp}) — another radio's DSP cannot outrank this radio's audio writer`);
    ok(w === -10 && dsp < w, `★★ DSP (${dsp}) > waterfall send (${w})`);
    ok(sp === null || w <= sp, `waterfall send (${w}) ≥ spectrum (${sp})`);
    ok(of('vibeTx').length === 0, `no writer left unclassified at the top (${of('vibeTx').length})`);
  }

  console.log('── under load: one busy loop per core at the DSP\'s rung (-19), in other processes ──');
  if (!privileged || dsp !== -19) {
    notRun++;
    console.log('   NOT RUN — needs the privilege to put the load at -19 and the fake dongle at -20');
  } else {
    let t0 = Date.now();
    await sleep(5000);
    const idle = cadence(aud, t0, Date.now());
    const n = os.availableParallelism ? os.availableParallelism() : os.cpus().length;
    for (let i = 0; i < n; i++) {
      const b = spawn('sh', ['-c', 'while :; do :; done'], { stdio: 'ignore' });
      try { os.setPriority(b.pid, -19); } catch {}
      burners.push(b);
    }
    try { os.setPriority(0, -20); } catch {}   // this script plays the listener's network: it must not be what starves
    let fifo = false;
    try { execFileSync('chrt', ['-a', '-f', '-p', '10', String(rtl.pid)], { stdio: 'ignore' }); fifo = true; } catch {}
    ok(fifo, 'the fake dongle runs SCHED_FIFO (plays the hardware) — without it the load measures the fake, not the server');
    await sleep(1000);
    t0 = Date.now();
    await sleep(8000);
    const busy = cadence(aud, t0, Date.now());
    const kb = (c) => `${(c.bps / 1024).toFixed(1)} KB/s in ${c.fps.toFixed(1)} frames/s, longest gap ${c.gap} ms`;
    console.log(`   idle ${kb(idle)} · loaded (${n} burners at -19) ${kb(busy)}`);
    ok(idle.bps > 1000, `audio flowing before the load (${(idle.bps / 1024).toFixed(1)} KB/s)`);
    ok(busy.bps >= idle.bps * 0.9, `★★★ the audio keeps its rate under the load (${(busy.bps / 1024).toFixed(1)} vs ${(idle.bps / 1024).toFixed(1)} KB/s)`);
    ok(busy.gap < 400, `★★ no long silence under the load (longest ${busy.gap} ms)`);
    ok(!aud.closed, 'the listener stayed connected');
  }
  spec.close(); aud.close();
} finally {
  for (const b of burners) b.kill('SIGKILL');
  server.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n   ${pass} passed, ${fail} failed${notRun ? `, ${notRun} NOT RUN` : ''}`);
process.exit(fail ? 1 : notRun ? 3 : 0);
