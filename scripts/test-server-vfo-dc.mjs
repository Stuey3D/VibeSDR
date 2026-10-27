// test-server-vfo-dc.mjs — V12 A FREE VFO AND THE TUNER'S DC through the REAL server (2026-10-27, branch v12-dc-nudge;
// vibe_vfo_dc.h, local_sdr_shim.cpp vfoDcPlace).
//
// Stuart: "we know where a user is tuning and we know where the rf centre is so as the user approaches the DC spike in Free
// Tune mode just nudge the RF centre so the spike moves away from the listen VFO". A fake rtl_tcp that reports an R820T (so
// the server offset-tunes it 15 kHz, as a USB dongle — 37139747d) and prints every tune command: the tuner's frequency IS
// where the DC is. The radio sits at 7.000 MHz (DC 7.015); an AM station (620 Hz) at 7.035. With the VFO Centre FREE the
// client sends only `tune` — the view stays where it is — so the listener walks across the DC:
//   1. BEFORE (VIBE_VFO_DC=0, the placement as it was): the dial lands on the DC — the fault is caught;
//   2. AFTER, a personal receiver: up across the DC in 500 Hz steps and back down — after every tune the DC is outside the
//      AM passband + guard (±7.5 kHz); ONE retune per crossing; the view (config centerFreq) never moves; the station at
//      the end of the sweep is heard (its 620 Hz tone — no frequency error from the move); the server says why it moved;
//   3. RANGE-LOCKED (users 2, --lock-freq): the same sweep moves nothing — the window is the owner's;
//   4. SHARED DIAL (users 3, unlocked): the owner's LANDING on the DC (not a person's tune) does NOT move the radio; the
//      first tune by a person does; a second listener joining moves nothing and reads the same dial.
// Silent: audio frames are analysed in memory, never played. Every child is killed in a finally.
//   VIBESERVER_BIN=…/vibeserver node --max-old-space-size=512 scripts/test-server-vfo-dc.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000, CENTRE = 7000000, OFF = 15000, DC0 = CENTRE + OFF;
const TONES_AT = DC0 + 20000;            // ★ the fake's middle station (620 Hz); the others ±120 kHz away
const ZONE = 5000 + 2500;                // ★ AM 10 kHz: ±5 kHz passband + the 2.5 kHz guard (vibe_vfo_dc.h)
const STEP = 500;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const kHz = (h) => (h / 1e3).toFixed(1);

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, keepBin: false };
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
      if (op === 0x2 && st.keepBin) st.bin.push(Buffer.from(p));
      if (op === 0x1) { const t = p.toString(); if (t.includes('"type":"config"')) { st.txt.push(t); if (st.txt.length > 2000) st.txt.splice(0, 500); } }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.destroy(); };
  return st;
}
const configs = (st) => st.txt.map((t) => JSON.parse(t));

/** PCM frames (codec 0) → mono Float32; Opus frames are not decoded (a personal receiver sends PCM). */
function pcmOf(bins) {
  const out = [];
  for (const b of bins) {
    if (b.length < 7 || b[1] !== 0) continue;
    const ch = b[0] || 1, n = (b.length - 6) / 2;
    for (let i = 0; i < n; i += ch) out.push(b.readInt16LE(6 + 2 * i) / 32768);
  }
  return Float32Array.from(out);
}
function goertzel(x, f, fs = 48000) {
  const n = x.length, w = (2 * Math.PI * f) / fs, cw = 2 * Math.cos(w);
  let s1 = 0, s2 = 0;
  for (let i = 0; i < n; i++) { const s = x[i] + cw * s1 - s2; s2 = s1; s1 = s; }
  return Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / Math.max(1, n);
}

const children = [];
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-vfodc-'));
async function server(tag, args, extraEnv = {}, radio = {}) {
  const dir = fs.mkdtempSync(path.join(tmp, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'VFO DC test', locator: 'IO92nh', adminPass: 'vfodc-test-admin',
    radios: [{ serial: 'vfodc-test', label: 'Test radio', enabled: true, configured: true, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const tunes = [];
  const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--tones', '3', '--tones-at', String(TONES_AT)], { stdio: ['ignore', 'pipe', 'ignore'] });
  children.push(rtl);
  rtl.stdout.on('data', (d) => { for (const m of d.toString().matchAll(/tune ([0-9.]+) MHz/g)) { tunes.push(Math.round(Number(m[1]) * 1e6)); if (tunes.length > 500) tunes.shift(); } });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  delete env.VIBE_TCP_DC_OFFSET_HZ;   // ★ the R820T header decides the offset, as on a real rtl_tcp dongle
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'am', ...args],
                    { env, stdio: ['ignore', log, log] });
  children.push(srv);
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, tunes, dcNow: () => (tunes.length ? tunes[tunes.length - 1] : NaN),
           log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, f) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  await sleep(600);
  if (f) { L.spec.send({ type: 'tune', frequency: f, mode: 'am' }); await sleep(500); }
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}
/** The VFO Centre FREE: only `tune`, one per step, the view left alone. After each, the DC (the fake's last tune) is read. */
async function sweep(S, L, from, to) {
  const out = [];
  const n0 = S.tunes.length;
  const dir = Math.sign(to - from);
  for (let f = from; dir > 0 ? f <= to : f >= to; f += dir * STEP) {
    L.spec.send({ type: 'tune', frequency: f, mode: 'am' });
    await sleep(90);
    out.push({ dial: f, dc: S.dcNow() });
  }
  return { steps: out, retunes: S.tunes.length - n0, onDc: out.filter((s) => Math.abs(s.dc - s.dial) < ZONE - 100) };   // ★ the fake prints 100 Hz
}
async function tone(L, ms = 2000) {
  L.aud.bin.length = 0; L.aud.keepBin = true;
  await sleep(ms);
  L.aud.keepBin = false;
  const x = pcmOf(L.aud.bin); L.aud.bin.length = 0;
  const y = x.subarray(Math.max(0, x.length - 48000));
  return { n: y.length, t620: goertzel(y, 620), t400: goertzel(y, 400), t840: goertzel(y, 840) };
}

try {
  {
    console.log('── 1. BEFORE (VIBE_VFO_DC=0): a FREE VFO walks onto the DC ──');
    const S = await server('before', ['--users', '1'], { VIBE_VFO_DC: '0' });
    try {
      const L = await listener(S.port, 'vfodcbefore1', CENTRE);
      ok(S.dcNow() === DC0, `the tuner (the DC) at ${kHz(S.dcNow())} kHz with the dial on ${kHz(CENTRE)}`);
      const up = await sweep(S, L, CENTRE, TONES_AT);
      ok(up.onDc.length > 0, `the fault reproduces: ${up.onDc.length} of ${up.steps.length} steps with the DC inside the AM passband + guard (e.g. dial ${up.onDc.length ? kHz(up.onDc[0].dial) : '-'})`);
      L.close();
    } finally { S.stop(); }
  }
  {
    console.log('── 2. AFTER, a personal receiver (users 1) ──');
    const S = await server('after', ['--users', '1']);
    try {
      const L = await listener(S.port, 'vfodcafter01', CENTRE);
      ok(S.dcNow() === DC0, `start: the tuner at ${kHz(S.dcNow())} kHz, dial ${kHz(CENTRE)}`);
      // ★ the view: every spectrum row carries its display centre (SPEC frame, u64 at byte 14) — watched through the sweeps
      L.spec.bin.length = 0; L.spec.keepBin = true;
      const up = await sweep(S, L, CENTRE, TONES_AT);
      ok(up.onDc.length === 0, `up across the DC (${up.steps.length} tunes of ${STEP} Hz): the DC never inside ±${ZONE / 1e3} kHz of the dial`);
      ok(up.retunes >= 1 && up.retunes <= 1, `ONE retune for the crossing (${up.retunes}: ${S.tunes.slice(-Math.max(1, up.retunes)).map(kHz).join(', ')} kHz)`);
      ok(/VFO free: DC [0-9.]+ MHz was inside the VFO's passband \+ guard/.test(S.log()), 'the server says why it moved the tuner');
      const t = await tone(L);
      ok(t.n >= 40000 && t.t620 > 0.002 && t.t620 > 10 * Math.max(t.t400, t.t840),
         `the station at ${kHz(TONES_AT)} heard after the move (620 Hz ${t.t620.toFixed(4)}, 400 ${t.t400.toFixed(4)}, 840 ${t.t840.toFixed(4)})`);
      const down = await sweep(S, L, TONES_AT, CENTRE - 10000);
      ok(down.onDc.length === 0, `back down across it (${down.steps.length} tunes): the DC never inside ±${ZONE / 1e3} kHz`);
      ok(down.retunes <= 1, `at most one retune for the way back (${down.retunes})`);
      const hover = S.tunes.length;
      const h0 = S.dcNow();
      // ★ hover ACROSS the zone's lower edge: the dial ZONE ± 300 Hz below the DC, back and forth, 30 tunes
      for (let i = 0; i < 30; i++) { L.spec.send({ type: 'tune', frequency: h0 - ZONE + (i % 2 ? 300 : -300), mode: 'am' }); await sleep(60); }
      await sleep(200);
      ok(S.tunes.length - hover <= 1, `hovering ±300 Hz across the zone's edge for 30 tunes: ${S.tunes.length - hover} retune(s)`);
      L.spec.keepBin = false;
      const views = L.spec.bin.filter((b) => b.length > 22 && b.toString('latin1', 0, 4) === 'SPEC').map((b) => Number(b.readBigUInt64LE(14)));
      L.spec.bin.length = 0;
      ok(views.length > 50 && views.every((v) => v === CENTRE), `the view never moved (${views.length} spectrum rows, display centre ${[...new Set(views)].map(kHz).join(', ')} kHz)`);
      L.close();
    } finally { S.stop(); }
  }
  {
    console.log('── 3. RANGE-LOCKED (users 2, --lock-freq): nothing moves ──');
    const S = await server('locked', ['--users', '2', '--lock-freq', String(CENTRE)]);
    try {
      const L = await listener(S.port, 'vfodclock001', CENTRE);
      await sleep(500);
      const n0 = S.tunes.length, dc = S.dcNow();
      const up = await sweep(S, L, CENTRE, TONES_AT);
      ok(S.tunes.length === n0, `a sweep across ${kHz(dc)} kHz: ${S.tunes.length - n0} tunes of the radio (the window is the owner's)`);
      ok(!/VFO free: DC/.test(S.log()), 'and no DC move in the log');
      void up;
      L.close();
    } finally { S.stop(); }
  }
  {
    console.log('── 4. SHARED DIAL (users 3, unlocked) ──');
    const S = await server('shared', ['--users', '3'], {}, { landingFreq: DC0, demodMode: 'am' });
    try {
      const A = await listener(S.port, 'vfodcshareA1', 0);   // ★ no tune: the owner's landing puts the dial on the DC
      await sleep(800);
      const nLand = S.tunes.length;
      ok(/new session — landing on 7015\.000 kHz/.test(S.log()), 'the first listener landed on the owner\'s landing, on the DC');
      ok(S.dcNow() === DC0, `the owner's landing (${kHz(DC0)}): the tuner stays at ${kHz(S.dcNow())} kHz — not a person's tune, so a shared dial does not move`);
      ok(!/VFO free: DC/.test(S.log()), 'no DC move for the landing');
      A.spec.send({ type: 'tune', frequency: DC0 + 100, mode: 'am' });
      await sleep(400);
      ok(S.tunes.length === nLand + 1 && Math.abs(S.dcNow() - (DC0 + 100)) >= ZONE - 100,
         `a person's tune (+100 Hz) moves it: the tuner at ${kHz(S.dcNow())} kHz, dial ${kHz(DC0 + 100)}`);
      const up = await sweep(S, A, DC0 + 100, TONES_AT);
      const dialNow = up.steps[up.steps.length - 1].dial;
      ok(up.onDc.length === 0 && up.retunes <= 1, `then up to the station: never on the DC, ${up.retunes} more retune(s)`);
      const n1 = S.tunes.length;
      const B = await listener(S.port, 'vfodcshareB2', 0);
      await sleep(1200);
      ok(S.tunes.length === n1, `a second listener joining moves nothing (${S.tunes.length - n1} tunes)`);
      const bc = configs(B.spec).slice(-1)[0];
      ok(bc && bc.vfo === dialNow, `…and reads the shared dial (${bc ? kHz(bc.vfo) : '-'} kHz)`);
      A.close(); B.close();
    } finally { S.stop(); }
  }
} catch (e) {
  fail++; console.log(`   FAIL ${e && e.stack || e}`);
} finally {
  for (const c of children) { try { c.kill('SIGKILL'); } catch {} }
  if (!fail) fs.rmSync(tmp, { recursive: true, force: true }); else console.log(`   (logs kept in ${tmp})`);
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
