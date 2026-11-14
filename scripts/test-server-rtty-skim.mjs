// test-server-rtty-skim.mjs — V12 THE RTTY SKIMMER through the REAL server (docs/v12/RTTY-SKIMMER.md): a fake rtl_tcp
// replaying a synthetic HF clip (`test-rtty-skimmer clip`: four RTTY stations of DIFFERENT settings around 14.080 MHz —
// 45/170 normal, 45/170 reverse, 50/450, 75/850 — calling CQ), and the contract of vibe_rtty_skim_run.h:
//   • vibeserver.json says "rttyskim": true;
//   • 'rttyskim' (48 kHz) on a ONE-PIPELINE radio and a RANGE-LOCKED one lists every station at its RF with its OWN
//     settings, each listed ("set":false — waiting for parameter detection) before it was set, nothing else listed, the
//     calls spotted at their RF, states 1 then 3.
// Silent: no audio is played (the audio socket's frames are dropped unread).
//   VIBESERVER_BIN=…/vibeserver VIBE_RTTY_CLIP=…/clip.cu8 node scripts/test-server-rtty-skim.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_RTTY_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_RTTY_CLIP (make one: test-rtty-skimmer clip clip.cu8)');
  process.exit(3);
}
const RATE = 960000, CENTRE = 14080000;
// ★ The clip's stations (vibeserver/test-rtty-skimmer.cpp mixedFour): offset from CENTRE, call (null = sends none), settings.
const STATIONS = [[-9000, 'G4ABC', '45 baud / 170 Hz / Normal'], [-3000, 'DL1XYZ', '45 baud / 170 Hz / Reverse'],
                  [3000, null, '50 baud / 450 Hz / Normal'], [10000, 'PA3XYZ', '75 baud / 850 Hz / Normal']];
const CALLS = STATIONS.map(([, c]) => c).filter(Boolean);
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

/** A minimal WebSocket client (the probes' own), recording text and binary frames. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
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
      if (op === 0x2 && !st.dropBin) st.bin.push(Buffer.from(p));
      if (op === 0x1) st.txt.push(p.toString());
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
const text01 = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-rttysk-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'RTTY skimmer test', locator: 'IO92nh', adminPass: 'rttysk-test-admin',
    radios: [{ serial: 'rttysk-test', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  return L;
}

/** Every station listed at its RF with its own settings, each listed (set:false) before it was set; spots at their RF. */
function checkRun(D, label) {
  const lists = jsons(D, 'rtty_signals');
  const last = lists.at(-1);
  ok(lists.length > 0 && last.data.skimmer === true, `${label}: rtty_signals lists arrive (${lists.length}), marked skimmer`);
  for (const [off, call, set] of STATIONS) {
    const row = last && last.data.signals.find((x) => Math.abs(x.hz - (CENTRE + off)) <= 15);
    ok(!!row && row.set && row.settings === set, `${label}: ${((CENTRE + off) / 1e3).toFixed(2)} kHz listed with [${row ? row.settings : '—'}] (sent ${set})`);
    if (!row) continue;
    const firstSeen = lists.findIndex((l) => l.data.signals.some((x) => x.id === row.id));
    const firstSet = lists.findIndex((l) => l.data.signals.some((x) => x.id === row.id && x.set));
    ok(firstSeen >= 0 && firstSet > firstSeen && lists[firstSeen].data.signals.find((x) => x.id === row.id).set === false,
       `${label}: …listed (waiting for parameter detection) before its settings were found (lists ${firstSeen} → ${firstSet})`);
    if (call) ok(row.call === call, `${label}: …its call ${row.call || '—'}`);
  }
  const extra = last ? last.data.signals.filter((x) => x.active && !STATIONS.some(([off]) => Math.abs(x.hz - (CENTRE + off)) <= 15)) : [];
  ok(extra.length === 0, `${label}: nothing else listed (${extra.map((x) => x.hz).join(' ')})`);
  const spots = jsons(D, 'rtty_spot');
  for (const [off, call] of STATIONS) {
    if (!call) continue;
    const s = spots.find((x) => x.data.callsign === call);
    ok(!!s && Math.abs(s.data.frequency - (CENTRE + off)) <= 15 && /^RTTY /.test(s.data.mode), `${label}: ${call} spotted at ${s ? s.data.frequency : '—'} (${s ? s.data.mode : ''})`);
  }
  ok(!spots.some((x) => !CALLS.includes(x.data.callsign) && x.data.callsign !== 'DDK2'), `${label}: no false spots`);
}

// ── 1. A one-pipeline radio: the skimmer's own slice (vibe_wide_tap.h) ──
{
  console.log('── one-pipeline radio (1 listener): the RTTY skimmer, 48 kHz ──');
  const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'one');
  try {
    const st = await S.status();
    ok(st && st.rttyskim === true, `vibeserver.json: rttyskim ${JSON.stringify(st && st.rttyskim)}`);
    const L = await listener(S.port, 'rttysk01', CENTRE, 'usb');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'rttyskim', width: 48000 });
    const allSet = () => { const l = jsons(L.dx, 'rtty_signals').at(-1); return !!l && STATIONS.every(([off]) => l.data.signals.some((x) => x.set && Math.abs(x.hz - (CENTRE + off)) <= 15)); };
    ok(await until(allSet, 45000), 'every station listed and set within 45 s');
    await until(() => CALLS.every((c) => jsons(L.dx, 'rtty_spot').some((x) => x.data.callsign === c)), 20000);
    // ★ a row names its call once it is spotted — on the next list (at most a second later)
    await until(() => CALLS.every((c) => (jsons(L.dx, 'rtty_signals').at(-1)?.data.signals ?? []).some((x) => x.call === c)), 5000);
    checkRun(L.dx, 'one-pipeline');
    const last = jsons(L.dx, 'rtty_signals').at(-1);
    ok(last && Math.abs(last.data.width - 48000) < 4000 && last.data.workers >= 1, `the segment searched ≈ 48 kHz (${last && last.data.width}), ${last && last.data.workers} search worker(s)`);
    const states = L.dx.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
    ok(states.includes(1) && states.includes(3), `state 1 (listed) then 3 (decoding) sent (${[...new Set(states)].join(',')})`);
    const g = last && last.data.signals.find((x) => x.call === 'G4ABC');
    ok(g && /G4ABC G4ABC/.test(g.text), `a row carries its text: "${g ? g.text.slice(-40) : ''}"`);
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(300);
    for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

// ── 2. A range-locked radio: the skimmer is one more slice of the shared FFT ──
{
  console.log('── range-locked radio (per-listener channels): the RTTY skimmer ──');
  const S = await server(['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'lock');
  try {
    const A = await listener(S.port, 'rttylck1', CENTRE, 'usb');
    A.dx.send({ type: 'audio_extension_attach', extension_name: 'rttyskim', width: 48000 });
    const allSet = () => { const l = jsons(A.dx, 'rtty_signals').at(-1); return !!l && STATIONS.every(([off]) => l.data.signals.some((x) => x.set && Math.abs(x.hz - (CENTRE + off)) <= 15)); };
    ok(await until(allSet, 45000), 'per-listener: every station listed and set within 45 s');
    await until(() => CALLS.every((c) => jsons(A.dx, 'rtty_spot').some((x) => x.data.callsign === c)), 20000);
    await until(() => CALLS.every((c) => (jsons(A.dx, 'rtty_signals').at(-1)?.data.signals ?? []).some((x) => x.call === c)), 5000);
    checkRun(A.dx, 'per-listener');
    for (const k of ['spec', 'aud', 'dx']) A[k].close();
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-rtty-skim: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
