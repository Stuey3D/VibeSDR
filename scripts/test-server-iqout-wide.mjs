// test-server-iqout-wide.mjs — RAW IQ OUT IS THE REAL IQ (V12, docs/v12/IQOUT-WIDE.md), through a real vibeserver.
//
// ★★★ THE FAULT (Stuart's Pi 500 V4L, beta92, 2026-11-24): a 250 kHz iqout delivered only the listener's PASSBAND,
//     resampled up (USB ±3 kHz: everything 10–125 kHz at the floor), and raising the channel rate to feed it ran the
//     whole audio chain at 250 kHz+ — vibe-demod at 99.5 % of a core, 11.3 s of 20 s delivered.
// THE CHECKS, on a one-pipeline radio (users 1 — the Pi 500's shape) and on a listener's own VFO (users 4, range-locked):
//   · a carrier 100 kHz off the dial (0.4 × 250 kHz) is IN the stream, ≥ 30 dB over the floor beside it — the listener is
//     on USB, whose passband ends at 2.7 kHz;
//   · the consumer gets its rate: 250 000 IQ pairs a second (500 000 bytes) within 1 %, and the stream drops nothing
//     (iqOutDrops) while the radio drops nothing (iqDrops, iqGaps);
//   · the listener's audio keeps flowing;
//   · the server's CPU with the stream on, against the same listener with it off (printed — on a Mac it is a few %).
// ★ SILENT: audio frames are only counted; no browser, no audio device. Loopback only.
// usage: VIBESERVER_BIN=… node scripts/test-server-iqout-wide.mjs   (~80 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, closed: false };
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
      if (op === 0x1) st.txt.push(p.toString());
      if (op === 0x2) st.bin += p.length;
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

/** The process's CPU seconds so far (ps TIME, user + system). */
function cpuSecs(pid) {
  try {
    const t = execFileSync('ps', ['-o', 'time=', '-p', String(pid)], { encoding: 'utf8' }).trim();
    const p = t.split(/[:.-]/).map(Number);   // [[d-]hh:]mm:ss.cc
    const [ss, cc] = [p[p.length - 2], p[p.length - 1]];
    const mm = p[p.length - 3] || 0, hh = p.length > 3 ? p[p.length - 4] : 0;
    return hh * 3600 + mm * 60 + ss + cc / 100;
  } catch { return NaN; }
}

/** Amplitude of the complex tone at `hz` in u8 IQ `b` (pairs from `from`, `n` of them, rate `fs`), Blackman-Harris. */
function toneAmp(b, from, n, fs, hz) {
  let sr = 0, si = 0, sw = 0;
  const w = -2 * Math.PI * hz / fs;
  let cr = 1, ci = 0; const rr = Math.cos(w), ri = Math.sin(w);
  for (let k = 0; k < n; k++) {
    const t = 2 * Math.PI * k / (n - 1);
    const win = 0.35875 - 0.48829 * Math.cos(t) + 0.14128 * Math.cos(2 * t) - 0.01168 * Math.cos(3 * t);
    const a = b[2 * (from + k)] - 127.5, q = b[2 * (from + k) + 1] - 127.5;
    sr += win * (a * cr - q * ci); si += win * (a * ci + q * cr); sw += win;
    const nr = cr * rr - ci * ri; ci = cr * ri + ci * rr; cr = nr;
    if ((k & 1023) === 1023) { const m = 1 / Math.hypot(cr, ci); cr *= m; ci *= m; }
  }
  return Math.hypot(sr, si) / sw;
}
const dB = (a) => 20 * Math.log10(Math.max(a, 1e-12));

const RATE = 2400000, FREQ = 145500000, IQ_RATE = 250000, OFF = 100000;

async function run({ users, locked }) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-iqwide-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'IQ wide test', adminPass: 'iq-wide-admin',
    radios: [{ serial: 'iqwide', label: 'Test radio', enabled: true, configured: true, users, rawIq: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--tones', '0', '--carrier', String(FREQ + OFF), '--carrier-amp', '12'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const args = ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(FREQ), '--mode', 'usb', '--users', String(users)];
  if (locked) args.push('--lock-freq', String(FREQ));
  const srv = spawn(BIN, args, { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
  const status = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
  const out = { L };
  let consumer = null, spec = null, aud = null;
  try {
    for (let i = 0; i < 100 && !(await status()); i++) await sleep(100);
    const S = 'iqwide0001';
    spec = ws(port, `/ws/user-spectrum?user_session_id=${S}&bins=256`);
    await sleep(300);
    aud = ws(port, `/ws/audio?user_session_id=${S}`);
    await sleep(1500);
    spec.send({ type: 'tune', frequency: FREQ, mode: 'usb' });
    await sleep(3000);
    { const c0 = cpuSecs(srv.pid), t0 = Date.now(); await sleep(8000); out.cpuOff = (cpuSecs(srv.pid) - c0) / ((Date.now() - t0) / 1000) * 100; }
    spec.send({ type: 'iqout', on: 1, rate: IQ_RATE });
    let iq = null;
    for (let i = 0; i < 50 && !iq; i++) { for (const t of spec.txt) if (t.includes('"type":"iqout"')) iq = JSON.parse(t); if (!iq) await sleep(100); }
    out.iq = iq;
    if (!iq || iq.on !== 1) return out;
    const chunks = []; let got = 0;
    consumer = net.connect(iq.port, '127.0.0.1');
    consumer.on('data', (b) => { got += b.length; chunks.push(b); });
    consumer.on('error', () => {});
    await sleep(3000);                                   // ★ settle
    const s0 = await status(), g0 = got, t0 = Date.now(), c0 = cpuSecs(srv.pid);
    const a0 = aud.bin;
    await sleep(15000);
    const s1 = await status(), t1 = Date.now(), c1 = cpuSecs(srv.pid);
    out.cpuOn = (c1 - c0) / ((t1 - t0) / 1000) * 100;
    out.iqBps = (got - g0) * 1000 / (t1 - t0);
    out.audioBps = (aud.bin - a0) * 1000 / (t1 - t0);
    out.iqDrops = (s1?.iqDrops ?? 0) - (s0?.iqDrops ?? 0);
    out.iqGaps = (s1?.iqGaps ?? 0) - (s0?.iqGaps ?? 0);
    out.iqOutDrops = (s1?.iqOutDrops ?? 0) - (s0?.iqOutDrops ?? 0);
    // ★ the last ~2 s of IQ (after the 12-byte rtl_tcp header), on a pair boundary
    const all = Buffer.concat(chunks).subarray(12);
    const pairs = Math.floor(all.length / 2), n = Math.min(pairs, 2 * IQ_RATE), from = pairs - n;
    out.carrier = dB(toneAmp(all, from, n, IQ_RATE, OFF));
    out.floor = Math.max(...[60000, -70000, -40000, 115000].map((f) => dB(toneAmp(all, from, n, IQ_RATE, f))));
    out.plan = (L().match(/raw IQ out: tap [^\n]*/) || [''])[0];
    return out;
  } finally {
    try { consumer?.destroy(); } catch {}
    try { spec?.close(); aud?.close(); } catch {}
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(500);
  }
}

for (const [title, cfg] of [['one listener, one-pipeline radio (users 1)', { users: 1, locked: false }],
                            ['a listener\'s own VFO (users 4, range-locked — the per-listener path)', { users: 4, locked: true }]]) {
  console.log(`── ${title}: USB, raw IQ out at ${IQ_RATE / 1000} kHz, a carrier ${OFF / 1000} kHz off the dial ──`);
  const r = await run(cfg);
  ok(r.iq?.on === 1 && r.iq.rate === IQ_RATE, `raw IQ out on at ${IQ_RATE} Hz (port ${r.iq?.port})${r.iq?.why ? ' — ' + r.iq.why : ''}`);
  if (r.plan) console.log(`   ${r.plan}`);
  console.log(`   IQ ${(r.iqBps / 1000).toFixed(1)} kB/s · audio ${((r.audioBps ?? 0) / 1000).toFixed(1)} kB/s · carrier ${r.carrier?.toFixed(1)} dB, floor ${r.floor?.toFixed(1)} dB`
              + ` · server CPU ${r.cpuOff?.toFixed(1)} % → ${r.cpuOn?.toFixed(1)} % of a core with the stream on`);
  ok(r.carrier - r.floor >= 30, `★★★ the carrier ${OFF / 1000} kHz off the dial is IN the stream (${(r.carrier - r.floor).toFixed(1)} dB over the floor) — beta92 delivered only the USB passband`);
  ok(Math.abs(r.iqBps - IQ_RATE * 2) <= IQ_RATE * 2 * 0.01, `★★★ the consumer gets its rate (${(r.iqBps / 1000).toFixed(1)} of ${IQ_RATE * 2 / 1000} kB/s, ±1 %)`);
  ok(r.iqOutDrops === 0, `★★ the stream dropped nothing (iqOutDrops +${r.iqOutDrops})`);
  ok(r.iqDrops === 0 && r.iqGaps === 0, `★★ the radio dropped nothing (iqDrops +${r.iqDrops}, iqGaps +${r.iqGaps})`);
  ok(r.audioBps > 1000, `the listener's audio flows (${((r.audioBps ?? 0) / 1000).toFixed(1)} kB/s)`);
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
