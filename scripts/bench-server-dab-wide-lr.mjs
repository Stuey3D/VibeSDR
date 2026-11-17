// bench-server-dab-wide-lr.mjs — V12 DAB PHASE 2's headline (docs/v12/DAB-OWN-STATION.md §11.4: "listen to all 65 stations at
// once"): N listener SESSIONS through the REAL server in a wide locked range, each on a DIFFERENT service across 10C, 10D and
// 11A, every one fed by the server's own Opus encode. Source: fake rtl_tcp replaying the RSP1A's 8 MS/s capture (or, with
// DAB_LR_TCP=host:port, a live rtl_tcp stream at 8 MS/s round 215.140 MHz). Not a pass/fail test — it measures:
//   · the server's CPU (% of one core, process total) and RSS over the window,
//   · per listener: kB/s received, and audio continuity (the longest gap between audio frames; gaps over 200 ms counted),
//   · how many listeners got audio throughout, and on which service (their own `mine`).
// ★ SILENT: audio frames are counted, never decoded or played. ★ One process at a time (RAM-heavy: the clip is in memory).
//   usage: VIBESERVER_BIN=… [DAB_LR_N=65] [DAB_LR_SECS=40] [DAB_LR_CLIP_SECS=20] node scripts/bench-server-dab-wide-lr.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const N = Number(process.env.DAB_LR_N || 65), SECS = Number(process.env.DAB_LR_SECS || 40), CLIP_SECS = Number(process.env.DAB_LR_CLIP_SECS || 20);
const CAP = process.env.DAB_WIDE_CAPTURE || path.join(os.homedir(), 'VibeSDR-test-audio/dab-wide/2026-11-16/dab-10C-10D-11A-215140kHz-8Msps.cu8');
const LIVE = process.env.DAB_LR_TCP || '';
const BASELINE = process.env.DAB_LR_BASELINE === '1';   // ★ the same sessions connected, NOT in DAB: what their own VFOs cost
if (!BIN || !fs.existsSync(BIN)) { console.log('not run — VIBESERVER_BIN'); process.exit(3); }
if (!LIVE && !fs.existsSync(CAP)) { console.log('not run — no capture'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const now = () => performance.now();

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bytes: 0, frames: 0, lastBin: 0, maxGap: 0, gaps200: 0, measuring: false, last: {} };
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
      if (op === 0x2) {
        const t = now();
        if (st.measuring) {
          st.bytes += len + off; st.frames++;
          if (st.lastBin) { const g = t - st.lastBin; if (g > st.maxGap) st.maxGap = g; if (g > 200) st.gaps200++; }
        }
        st.lastBin = t;
      }
      if (op === 0x1) { try { const j = JSON.parse(p.toString()); if (j && j.type === 'dab') st.last.dab = j; } catch {} }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dablr-bench-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let rtl = null, tcp = LIVE;
const RATE = 8000000, PHYS = 215140000;
if (!LIVE) {
  const CLIP = path.join(dir, 'wide.cu8');
  const fd = fs.openSync(CAP, 'r'); const b = Buffer.alloc(RATE * 2 * CLIP_SECS); fs.readSync(fd, b, 0, b.length, RATE * 2 * 5); fs.closeSync(fd); fs.writeFileSync(CLIP, b);
  const rtlPort = await freePort();
  rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), '--dab-map', `${PHYS}=${CLIP}`], { stdio: 'ignore' });
  tcp = `127.0.0.1:${rtlPort}`;
  await sleep(4000);
}
const port = await freePort();
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, fullMode: true, sharing: 'public',
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: N + 5, mode: 'locked', lockFreq: PHYS - 15000,
             rate: RATE, freq: PHYS - 15000, demodMode: 'wfm', dabOwnStation: 1 }],
}));
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', tcp, '--port', String(port), '--rate', String(RATE), '--idle-grace', '0'], { env, stdio: ['ignore', logFd, logFd] });
const cleanup = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl && rtl.kill('SIGTERM'); } catch {} };
process.on('exit', cleanup);
setTimeout(() => { console.log('hard deadline'); cleanup(); process.exit(1); }, 10 * 60 * 1000);
for (let i = 0; i < 150; i++) { const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); }); if (up) break; await sleep(100); }
await sleep(3000);
const ps = () => { try { const [cpu, rss, t] = execFileSync('ps', ['-o', '%cpu=,rss=,time=', '-p', String(srv.pid)], { encoding: 'utf8' }).trim().split(/\s+/); return { cpu: Number(cpu), rssKB: Number(rss), time: t }; } catch { return null; } };
const cpuSecs = (t) => { if (!t) return 0; const p = t.split(':').map(Number); return p.length === 3 ? p[0] * 3600 + p[1] * 60 + p[2] : p[0] * 60 + p[1]; };

// ── one scout per block reads its list; then every audio service gets one listener ──
const L = [];
const mk = (tag) => { const sid = `${tag}-${crypto.randomBytes(5).toString('hex')}`;
  const o = { sid, aud: ws(port, `/ws/audio?user_session_id=${sid}&codec=opus`), spec: ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=64`) };
  L.push(o); return o; };
const blocks = ['10C', '10D', '11A'];
const scouts = blocks.map((b) => mk('scout' + b));
await sleep(1500);
scouts.forEach((s, i) => s.spec.send({ type: 'dab', on: 1, channel: blocks[i] }));
const lists = {}; let stableFor = 0, lastSig = '';
// ★ wait until every block's list has stopped growing for 8 s (a weak multiplex's list fills slowly), at most 120 s
for (let t = 0; t < 120 && stableFor < 8; t++) {
  await sleep(1000);
  scouts.forEach((s, i) => { const d = s.spec.last.dab; if (d && d.channel === blocks[i] && Array.isArray(d.services) && d.label) {
    const sids = d.services.filter((x) => !x.isData && x.sid).map((x) => x.sid);
    if (!lists[blocks[i]] || sids.length > lists[blocks[i]].length) lists[blocks[i]] = sids; } });
  const sig = blocks.map((b) => (lists[b] || []).length).join(',');
  stableFor = sig === lastSig && Object.keys(lists).length === 3 ? stableFor + 1 : 0; lastSig = sig;
}
const plan = [];
for (const b of blocks) for (const sid of (lists[b] || [])) plan.push({ b, sid });
const want = plan.slice(0, N);
console.log(JSON.stringify({ lists: Object.fromEntries(Object.entries(lists).map(([k, v]) => [k, v.length])), listeners: want.length }));
// the scouts take the first service of their block; the rest get their own sessions
const who = [];
for (const w of want) {
  const s = scouts.find((x, i) => blocks[i] === w.b && !x.taken);
  if (s) { s.taken = true; s.spec.send({ type: 'dab_service', sid: w.sid }); who.push({ ...w, l: s }); continue; }
  const l = mk('lr'); who.push({ ...w, l });
}
await sleep(1500);
if (BASELINE) { for (const s of scouts) s.spec.send({ type: 'dab', on: 0 }); }
else {
  for (const w of who) if (!w.l.taken) { w.l.spec.send({ type: 'dab', on: 1, channel: w.b, sid: w.sid }); await sleep(30); }
  await sleep(3000);
  for (const w of who) if (!w.l.taken) { w.l.spec.send({ type: 'dab_service', sid: w.sid }); await sleep(20); }
}
await sleep(10000);   // warm: every station primed
for (const w of who) w.l.aud.measuring = true;
const p0 = ps(); const t0 = now();
const cpuSamples = [];
for (let s = 0; s < SECS; s++) { await sleep(1000); const p = ps(); if (p) cpuSamples.push(p.cpu); }
const p1 = ps(); const dt = (now() - t0) / 1000;
for (const w of who) w.l.aud.measuring = false;
const dec = (d) => { if (!d) return null; const sfT = d.sfTried || 0, sfO = d.sfOk || 0, mo = d.mp2Out || 0, mb = d.mp2Bad || 0;
  return sfT > 0 ? sfO / sfT : mo > 0 ? 1 - mb / mo : null; };
const per = who.map((w) => ({ b: w.b, sid: w.sid, mine: w.l.spec.last.dab?.mine, kBps: w.l.aud.bytes / 1024 / dt, maxGapMs: Math.round(w.l.aud.maxGap), gaps200: w.l.aud.gaps200,
  decodeOk: dec(w.l.spec.last.dab) }));
const ownOk = per.filter((x) => x.mine === x.sid).length;
const playing = per.filter((x) => x.frames !== 0 && x.maxGapMs < 1000 && x.kBps > 0).length;   // ★ audio frames all the way through
const clean = per.filter((x) => x.kBps > 0 && x.gaps200 === 0).length;
const decoding = per.filter((x) => x.decodeOk !== null && x.decodeOk >= 0.9).length;   // ★ ≥ 90 % of super frames (or MP2 frames) good
const log = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
const summary = {
  listeners: who.length, onTheirOwnService: ownOk, audioFramesThroughout: playing, noGapOver200ms: clean, decoding90pct: decoding,
  kBpsPerListener: { min: Math.min(...per.map((x) => x.kBps)).toFixed(1), mean: (per.reduce((a, x) => a + x.kBps, 0) / per.length).toFixed(1), max: Math.max(...per.map((x) => x.kBps)).toFixed(1) },
  kBpsTotal: per.reduce((a, x) => a + x.kBps, 0).toFixed(0),
  serverCpuPctOfOneCore: { fromCpuTime: (100 * (cpuSecs(p1?.time) - cpuSecs(p0?.time)) / dt).toFixed(1), psMean: (cpuSamples.reduce((a, x) => a + x, 0) / Math.max(1, cpuSamples.length)).toFixed(1) },
  rssMB: ((p1?.rssKB || 0) / 1024).toFixed(0), cores: os.cpus().length, machine: os.cpus()[0]?.model, secs: dt.toFixed(1),
  stationsOpened: (log.match(/station [0-9A-F]+ opened/g) || []).length, muxesOpened: (log.match(/\[DAB-LR\] multiplex \S+ opened/g) || []).length,
  byBlock: Object.fromEntries(blocks.map((b) => { const x = per.filter((q) => q.b === b); return [b, { listeners: x.length, decoding90pct: x.filter((q) => q.decodeOk !== null && q.decodeOk >= 0.9).length,
    meanDecode: (x.reduce((a, q) => a + (q.decodeOk ?? 0), 0) / Math.max(1, x.length)).toFixed(2), meanKBps: (x.reduce((a, q) => a + q.kBps, 0) / Math.max(1, x.length)).toFixed(1) }]; })),
  baseline: BASELINE,
  worst: per.sort((a, b) => b.maxGapMs - a.maxGapMs).slice(0, 5),
  source: LIVE ? `live ${LIVE}` : `replay ${CLIP_SECS} s of the capture (looped)`,
};
console.log(JSON.stringify(summary, null, 1));
for (const l of L) { l.aud.close(); l.spec.close(); }
await sleep(1500);
cleanup();
fs.rmSync(dir, { recursive: true, force: true });
process.exit(0);
