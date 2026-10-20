// test-server-audio-first.mjs — the REAL server, end to end: while a listener's AUDIO backs up, their WATERFALL slows
// to ~2 frames a second; once the audio has been clear for ~1 s it is back at full rate. And another listener's
// waterfall never notices. (docs/BRIEF-portable-wifi-realtime.md Phase 2, vibe_wifi_qos.h AudioFirst.)
//
// ★ HOW THE AUDIO IS BACKED UP: A's audio socket is a small Python client asking for a 4 KB receive buffer. It reads
//   normally, STOPS reading until told to resume, then reads again: the server's audio send queue grows (SIOCOUTQ /
//   SO_NWRITE, then a send that blocks) — what an airtime-starved Wi-Fi hop looks like from the server, minus the radio.
//   ★★ WHETHER THAT CAN HAPPEN DEPENDS ON THE KERNEL. Linux honours the 4 KB, so the audio backs up within a second or
//      two. macOS's loopback ignores it and absorbs ~450 KB first (~35 s of Opus) — longer than the server's 20 s
//      ping liveness, which drops the silent client before anything is ever backed up. So on Linux the whole test runs;
//      elsewhere only the half that can run (no false yield, nobody dropped) — and the backed-up half is reported as
//      NOT RUN there, never as a pass.
// ★★★ AND THE FIRST SECONDS OF EVERY STALL ARE A CHECK TOO: while the kernel is still absorbing it nothing is backed up,
//     so nothing may yield. The encoder writes ~8 frames at once; a rule that read that burst as a backlog yielded
//     here with nothing wrong (found by this test, fixed by AudioFirst::kSustainMs).
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=/path/to/vibeserver. Without one: "not run" (exit 3), a result, not a pass.
// ★ SILENT: audio frames are only counted, never played.
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

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], binAt: [], open: false, closed: false, sock: s };
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
      if (op === 0x2) { st.bin.push(p.length); st.binAt.push(Date.now()); }
    }
  });
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
/** A's audio: read READ1 s, stop reading until a line arrives on stdin, read READ2 s. Prints "stall" / "resume". */
const AUDIO_CLIENT = `
import socket, sys, time, base64, os
port, path, read1, read2 = int(sys.argv[1]), sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
s.connect(("127.0.0.1", port))
k = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET %s HTTP/1.1\\r\\nHost: 127.0.0.1\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\n"
           "Sec-WebSocket-Key: %s\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n" % (path, k)).encode())
def drain(sec):
    end = time.time() + sec
    s.settimeout(0.2)
    while time.time() < end:
        try:
            if not s.recv(65536): return False
        except socket.timeout:
            pass
    return True
drain(read1); print("stall", flush=True)
sys.stdin.readline(); print("resume", flush=True)
drain(read2); s.close()
`;

/** ★ Linux honours the client's 4 KB receive buffer (measured: the audio backs up ~1.6 s into the stall); macOS's
 *  loopback ignores it and its receiver takes ~300-450 KB first. A PROBE of that was tried and was racy under load (the
 *  full suite once read macOS as honouring it), so the platform decides — deterministic, and a Linux run that never
 *  yields is a FAIL, never a "not run". */
const probe = { honoured: process.platform === 'linux' };

/** Waterfall frames per second over the last `ms` (frames bigger than a control blip). */
const fps = (st, from, to) => st.binAt.filter((t) => t >= from && t < to).length * 1000 / (to - from);

async function server(args) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-af-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', ...args], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, log: () => fs.readFileSync(logPath, 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}

{
  console.log('── audio first: a listener whose audio backs up gets a slower waterfall, not a broken stream ──');
  if (!probe.honoured)
    console.log(`   (${process.platform}: the loopback receiver ignores a 4 KB receive buffer and absorbs ~300-450 KB — the audio cannot back up `
                + 'here before the 20 s ping liveness drops the client, so only the no-false-yield half runs)');
  // Shared dial, two listeners: A (whose audio we stall) and B (who must not notice). Raw PCM — 192 KB/s fills the
  // kernel's buffers in a few seconds, which is what lets this run in under half a minute.
  const S = await server(['--freq', '100000000', '--users', '4', '--mode', 'wfm', '--idle-grace', '0']);
  try {
    const A = 'aaaaaaaa-af01', B = 'bbbbbbbb-af02';
    const specA = ws(S.port, `/ws/user-spectrum?user_session_id=${A}&bins=256`);
    const specB = ws(S.port, `/ws/user-spectrum?user_session_id=${B}&bins=256`);
    await sleep(300);
    const audB = ws(S.port, `/ws/audio?user_session_id=${B}`);
    const ev = {};
    const py = spawn('python3', ['-I', '-c', AUDIO_CLIENT, String(S.port), `/ws/audio?user_session_id=${A}`, '3', '4'],
                     { stdio: ['pipe', 'pipe', 'inherit'] });
    py.stdout.on('data', (d) => { for (const w of d.toString().split(/\s+/)) if (w) ev[w] = Date.now(); });
    const pyDone = new Promise((r) => py.on('exit', r));
    while (!ev.stall) await sleep(50);
    let t = ev.stall;
    const baseA = fps(specA, t - 2000, t), baseB = fps(specB, t - 2000, t);
    ok(baseA >= 8, `healthy: A's waterfall at ${baseA.toFixed(1)} fps while its audio is read`);

    // ── A stops reading its audio ──
    let yieldAt = 0;
    const limit = probe.honoured ? 30000 : 8000;
    while (!yieldAt && Date.now() - ev.stall < limit && !specA.closed) {
      await sleep(100);
      const n = Date.now();
      if (n - ev.stall > 1000 && fps(specA, n - 1000, n) <= 3) yieldAt = n;
    }
    if (probe.honoured)
      ok(yieldAt > 0, `A's audio backed up: A's waterfall YIELDED ${yieldAt ? ((yieldAt - ev.stall) / 1000).toFixed(1) + ' s' : 'never'} after it stopped reading`);
    else
      ok(yieldAt === 0, 'the kernel absorbed the 8 s stall (nothing backed up): the waterfall never yielded');
    if (yieldAt) await sleep(4000);
    py.stdin.write('resume\n');
    while (!ev.resume) await sleep(50);
    const early = Math.min(yieldAt || ev.resume, ev.stall + 3000);
    ok(fps(specA, ev.stall + 200, early - 1000) >= baseA * 0.7,
       `★★ while the kernel still absorbed the stall, NO false yield (${fps(specA, ev.stall + 200, early - 1000).toFixed(1)} fps) — an encoder burst is not a backlog`);
    const slowA = fps(specA, ev.resume - 3000, ev.resume), duringB = fps(specB, ev.resume - 3000, ev.resume);
    if (probe.honoured) {
      ok(slowA > 0 && slowA <= 3, `…to ~2 fps for the rest of the stall, not frozen (${slowA.toFixed(1)} fps over its last 3 s)`);
      ok(/waterfall yielding to audio/.test(S.log()), 'the server says so in its log');
    } else {
      console.log('   NOT RUN here: the backed-up half (yield to ~2 fps, the log line) — needs a kernel that honours a small receive buffer (Linux)');
    }
    ok(duringB >= baseB * 0.7, `★ B's waterfall is untouched meanwhile (${duringB.toFixed(1)} fps, was ${baseB.toFixed(1)})`);

    // ── A reads again ──
    await pyDone;
    t = Date.now();
    const backA = fps(specA, t - 1500, t);
    ok(!specA.closed, 'A\'s waterfall socket was never dropped — it gave way, the listener stayed');
    ok(!/listener .* dropped — .* backlog/.test(S.log()), '…and nobody was dropped for backlog');
    ok(backA >= baseA * 0.7, `audio clear again: A's waterfall back at full rate (${backA.toFixed(1)} fps, was ${baseA.toFixed(1)})`);
    if (probe.honoured) ok(fps(specA, ev.resume, ev.resume + 600) <= 4, '…and not instantly: it waits for the audio to stay clear (~1 s)');
    for (const s of [specA, specB, audB]) s.close();
    await sleep(300);
  } finally { S.stop(); }
}

console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
