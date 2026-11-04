// test-server-dab-wide.mjs — DAB BLOCK CHANGES on a real vibeserver against fake rtl_tcp, timed stage by stage.
//
// ★★★ WHY (V12, 2026-11-04). Two reports in one evening:
//   1. Stuart, a USRP the owner had set wide: "the 2.4MHz sample rate killed it after a few tunes". Every DAB block change
//      re-rated that radio twice (2.4 → 2.0 → 2.4: a list with no 2.048, a readback that failed, a fallback). V12 keeps a
//      wide radio at its rate and cuts the block out in software (vibe_dab_ddc.h).
//   2. Stuart, the Sony + V4: moving between multiplexes "slow and a bit unreliable", while a locked one plays perfectly.
// Two modes, one script:
//   DAB_WIDE_MODE=wide   — the radio at 6.4 MS/s with two real multiplexes in its capture (12B at the centre, 12C 1.712 MHz
//                          up; dab-weak --write made the composite), VIBE_DAB_WIDE_ANY=1 so the fake dongle takes the wide
//                          plan a USRP gets. Asserts: entering DAB moves the centre ONCE and never the rate; every block
//                          change inside the span sends the radio NOTHING (no tune, no rate — the fake logs every command);
//                          every step ends locked, labelled and playing; the waterfall never stops (no gap over 500 ms in
//                          the spectrum frames); leaving DAB puts the rate back as it was (it never moved).
//   DAB_WIDE_MODE=native — the ordinary radio at 2.048 MS/s, a different recording on each block (--dab-map), so a lock
//                          carried across a change cannot hide behind identical air. Asserts every step ends locked and
//                          playing, and that no block change re-rates the radio.
// Both print the time of every stage of every step: press → locked → label → services → audio.
// ★ SILENT: audio frames are received and discarded, never played. ★ The clips are real broadcasts: they live in
//   ~/VibeSDR-test-audio/dab/bench (dab-weak --write), not in git — the test says "not run" without them.
//
//   usage: VIBESERVER_BIN=… [DAB_WIDE_MODE=wide|native] [DAB_STEPS=8] node scripts/test-server-dab-wide.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const MODE = process.env.DAB_WIDE_MODE || 'wide';
const STEPS = Number(process.env.DAB_STEPS || 8);
const CLIPS = process.env.DAB_BENCH_DIR || path.join(os.homedir(), 'VibeSDR-test-audio/dab/bench');
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const need = MODE === 'wide' ? ['wide-6400k-12B-12C.vbu8'] : ['12B-2048.vbu8', '12D-v4l-2048.vbu8'];
for (const f of need) if (!fs.existsSync(path.join(CLIPS, f))) { console.log(`   not run — ${f} missing from ${CLIPS} (dab-weak --write makes it)`); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const now = () => performance.now();

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], binAt: [], onText: null };
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
      if (op === 0x2) { if (st.binAt.length < 200000) st.binAt.push(now()); }
      if (op === 0x1) { const t = p.toString(); if (st.onText) st.onText(t); }
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabwide-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort(), front = await freePort();
const FM = 100000000;
const RATE = MODE === 'wide' ? 6400000 : 2048000;
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, fullMode: true, sharing: 'public', port: front,
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: FM, demodMode: 'wfm', landingFreq: FM, users: 1 }],
}));
const map = MODE === 'wide'
  ? `225648000=${path.join(CLIPS, 'wide-6400k-12B-12C.vbu8')}`
  : `225648000=${path.join(CLIPS, '12B-2048.vbu8')},229072000=${path.join(CLIPS, '12D-v4l-2048.vbu8')}`;
const rtlLog = [];
const rtl = spawn(process.execPath, ['--max-old-space-size=512', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), '--dab-map', map],
                  { stdio: ['ignore', 'pipe', 'ignore'] });
rtl.stdout.on('data', (d) => { for (const l of d.toString().split('\n')) if (l && rtlLog.length < 5000) rtlLog.push({ t: now(), l }); });
await sleep(1500);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...(MODE === 'wide' ? { VIBE_DAB_WIDE_ANY: '1' } : {}) };
const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--idle-grace', '0'], { env, stdio: ['ignore', logFd, logFd] });
const log = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
const cleanup = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
const deadline = setTimeout(() => { console.log('   FAIL: hard deadline'); cleanup(); process.exit(1); }, 15 * 60 * 1000);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
await sleep(800);

const cmds = (from) => rtlLog.filter((e) => e.t >= from && /\] (tune|sample rate) /.test(e.l)).map((e) => e.l.replace(/^.*\] /, ''));
let dab = null;
try {
  const SID = 'd1d1d1d1-dab0-wide-0000-' + crypto.randomBytes(6).toString('hex');
  const aud = ws(port, `/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(800);
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
  spec.onText = (t) => {
    if (!t.includes('"type":"dab"') || t.includes('"error"')) return;
    try { const j = JSON.parse(t); if (j.channel) dab = { at: now(), ...j, nsvc: Array.isArray(j.services) ? j.services.length : (dab?.nsvc ?? 0) }; } catch {}
  };
  await sleep(1500);
  const want = MODE === 'wide' ? { '12B': ['BBC National DAB', 0xC229], '12C': ['Coventry', 0xC2FA] }
                               : { '12B': ['BBC National DAB', 0xC229], '12D': ['Coventry', 0xC2FA] };
  const blocks = Object.keys(want);
  const rows = [];
  const step = async (ch, limit = 30000) => {
    const [label, sid] = want[ch];
    const t0 = now(); dab = null;
    spec.send({ type: 'dab', on: 1, channel: ch, sid });
    let tLock = -1, tLabel = -1, tSvc = -1, tAudio = -1, sf0 = null;
    while (now() - t0 < limit) {
      await sleep(50);
      if (!dab || dab.channel !== ch || dab.at < t0) continue;
      const t = (now() - t0) / 1000;
      if (tLock < 0 && dab.locked) tLock = t;
      if (tLabel < 0 && dab.label === label) tLabel = t;
      if (tSvc < 0 && dab.nsvc > 0) tSvc = t;
      if (sf0 === null) sf0 = dab.sfOk ?? 0;
      if (tAudio < 0 && dab.sid === sid && (dab.sfOk ?? 0) > sf0) tAudio = t;
      if (tLock >= 0 && tLabel >= 0 && tAudio >= 0) break;
    }
    const r = { ch, tLock, tLabel, tSvc, tAudio, cmds: cmds(t0) };
    rows.push(r);
    console.log(`     ${ch}: locked ${tLock.toFixed(2)} s, label ${tLabel.toFixed(2)} s, services ${tSvc.toFixed(2)} s, audio ${tAudio.toFixed(2)} s  radio: ${r.cmds.join('; ') || 'nothing'}`);
    return r;
  };
  console.log(`== ${MODE}: ${RATE / 1e6} MS/s, ${STEPS} block changes`);
  const tEnter = now();
  const first = await step(blocks[0], 40000);
  ok(first.tLock >= 0 && first.tLabel >= 0 && first.tAudio >= 0, `entering DAB on ${blocks[0]} locks, labels and plays`);
  const enterCmds = cmds(tEnter);
  ok(!enterCmds.some((c) => c.startsWith('sample rate')) || MODE === 'native', `wide: entering DAB never re-rates the radio (${enterCmds.join('; ') || 'nothing'})`);
  await sleep(2000);
  const wfFrom = now();
  for (let i = 0; i < STEPS; i++) {
    const r = await step(blocks[(i + 1) % blocks.length]);
    ok(r.tLock >= 0 && r.tLabel >= 0 && r.tAudio >= 0, `step ${i + 1} to ${r.ch} ends locked, labelled and playing`);
    ok(!r.cmds.some((c) => c.startsWith('sample rate')), `step ${i + 1}: no re-rate of the radio`);
    if (MODE === 'wide') ok(r.cmds.length === 0, `step ${i + 1}: a block inside the span sends the radio nothing`);
    await sleep(1500);
  }
  // ★ The waterfall never stops: the largest gap between spectrum frames across all the steps.
  const bins = spec.binAt.filter((t) => t >= wfFrom);
  let gap = 0; for (let i = 1; i < bins.length; i++) gap = Math.max(gap, bins[i] - bins[i - 1]);
  ok(bins.length > 50 && gap < 500, `the waterfall kept flowing through every block change (${bins.length} frames, largest gap ${gap.toFixed(0)} ms)`);
  const steps = rows.slice(1);
  const med = (k) => { const v = steps.map((r) => r[k]).filter((x) => x >= 0).sort((a, b) => a - b); return v.length ? v[Math.floor(v.length / 2)] : -1; };
  const worst = (k) => Math.max(...steps.map((r) => r[k]));
  console.log(`   per step (median / worst): locked ${med('tLock').toFixed(2)} / ${worst('tLock').toFixed(2)} s, label ${med('tLabel').toFixed(2)} / ${worst('tLabel').toFixed(2)} s, audio ${med('tAudio').toFixed(2)} / ${worst('tAudio').toFixed(2)} s`);
  if (MODE === 'wide') ok(/wide capture: the radio stays at 6\.400 MS\/s/.test(log()), 'the server log says the radio stays at its own rate');
  // ★ Leaving DAB: the rate is where it always was.
  const tOff = now();
  spec.send({ type: 'dab', on: 0 });
  await sleep(3000);
  ok(!cmds(tOff).some((c) => c.startsWith('sample rate') && !c.endsWith(String(RATE))), `leaving DAB leaves the rate at ${RATE} (${cmds(tOff).join('; ') || 'nothing'})`);
  fs.writeFileSync(path.join(dir, 'steps.json'), JSON.stringify(rows, null, 1));
  aud.close(); spec.close();
} catch (e) { ok(false, 'exception: ' + e.message); }
clearTimeout(deadline);
cleanup();
await sleep(300);
const rss = log().match(/rss[^\n]*/i);
console.log(`   ${pass} ok, ${fail} failed  (server log: ${path.join(dir, 'server.log')})`);
process.exit(fail ? 1 : 0);
