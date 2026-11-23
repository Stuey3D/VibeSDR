// test-server-iq-priority.mjs — BACKGROUND DECODING TAKES THE BACK SEAT (beta92, 2026-11-23), through a real vibeserver.
//
// ★★★ THE FAULT (Stuart's Pi 500, V4L serial 00000001, beta91): the owner's background-decoding profile kept decoding in
//     range with a listener on; the listener, on a satellite pick, switched RAW IQ OUT on at 250 kHz. At once: the audio
//     broke up, the log filled with "IQ overrun — dropping a buffer (the DSP thread was blocked)", iqDrops 449 / iqGaps
//     231 in a minute, and the IQ consumer (nc on a fast Mac) got ~57 KB/s of 500. Stuart: "they need to take the back
//     seat". Every background VFO's demodulator ran at the listener's own AUDIO rung (RxPipeline's worker, ranked by
//     name as vibe-demod), and the IQ stream's conversion ran on the listener's demod thread.
// ★★ THE CONDITION, REPRODUCED: a 16-VFO background profile in range of the listener, every VFO's demodulator made to
//    SATURATE a core (VIBESERVER_BG_TEST_BURN — test-only: each spins 1.5× its audio's duration), raw IQ out at 250 kHz
//    with a consumer reading at full speed, and the listener's audio socket open. On Linux the server is pinned to two
//    cores (taskset), the Pi-like squeeze; elsewhere the profile alone outnumbers the cores.
//   · the IQ consumer gets ≥ 95 % of 500 000 bytes/s; the stream itself drops nothing (iqOutDrops);
//   · the radio drops nothing (iqDrops, iqGaps) — the DSP thread never waited;
//   · the listener's audio keeps the rate it had with no background profile (≥ 90 %), no silence ≥ 400 ms;
//   · Linux: the background threads (vibe-bgdsp, vibe-bgdemod, vibe-bgdec) are at nice 19, the listener's chain is not.
// ★ SILENT: audio frames are only counted; no browser, no audio device. Loopback only.
// usage: VIBESERVER_BIN=… node scripts/test-server-iq-priority.mjs   (~70 s)
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

/** A minimal WebSocket client: text frames kept, binary frames timed and counted (the audio — never played). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], binAt: [], binLen: [], closed: false };
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
      if (op === 0x2) { st.binAt.push(Date.now()); st.binLen.push(p.length); }
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
/** Audio bytes/s and the longest silence over [from, to). */
function cadence(st, from, to) {
  const idx = st.binAt.map((x, i) => i).filter((i) => st.binAt[i] >= from && st.binAt[i] < to);
  const t = idx.map((i) => st.binAt[i]);
  const bytes = idx.reduce((a, i) => a + st.binLen[i], 0);
  let gap = 0;
  for (let i = 1; i < t.length; i++) gap = Math.max(gap, t[i] - t[i - 1]);
  if (t.length) gap = Math.max(gap, t[0] - from, to - t[t.length - 1]);
  return { bps: bytes * 1000 / (to - from), gap: t.length ? gap : to - from };
}
/** Linux: every thread of `pid` — {comm, nice}. */
function threads(pid) {
  const out = [];
  let tids = [];
  try { tids = fs.readdirSync(`/proc/${pid}/task`); } catch { return out; }
  for (const t of tids) {
    try {
      const st = fs.readFileSync(`/proc/${pid}/task/${t}/stat`, 'utf8');
      const f = st.slice(st.lastIndexOf(')') + 2).split(' ');
      out.push({ comm: fs.readFileSync(`/proc/${pid}/task/${t}/comm`, 'utf8').trim(), nice: +f[16] });
    } catch {}
  }
  return out;
}

const RATE = 2400000, FREQ = 145800000, IQ_RATE = 250000;
const VFOS = []; for (let i = 0; i < 16; i++) VFOS.push({ hz: 145000000 + i * 100000 + 25000, mode: 'nfm', dec: 'aprs' });
const PROFILE = { on: true, waitMin: 1, keepInRange: true, profiles: [{ centre: FREQ, from: '00:00', to: '00:00', vfos: VFOS }] };

/** One server run: the listener (spectrum + audio) on 145.8 NFM, raw IQ out at 250 kHz with a full-speed consumer. */
async function run(withBg) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-iqprio-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'IQ priority test', adminPass: 'iq-priority-admin', decoderMax: 16,
    radios: [{ serial: 'iqprio', label: 'Test radio', enabled: true, configured: true, users: 1, rawIq: 1,
               ...(withBg ? { bgDecode: JSON.stringify(PROFILE) } : {}) }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_BG_WAIT_SEC: '1', VIBESERVER_BG_TEST_BURN: withBg ? '1.5' : '0',
                VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(FREQ), '--mode', 'nfm', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
  const status = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
  const out = { L };
  let consumer = null, spec = null, aud = null;
  try {
    for (let i = 0; i < 100 && !(await status()); i++) await sleep(100);
    let pinned = false;
    if (withBg && process.platform === 'linux') {
      // ★ The Pi-like squeeze: the whole server on two cores, against sixteen saturating background demodulators.
      try { execFileSync('taskset', ['-a', '-p', '-c', '0,1', String(srv.pid)], { stdio: 'ignore' }); pinned = true; } catch {}
    }
    out.pinned = pinned;
    if (withBg) {
      // ★ nobody here: the profile starts after the (test) wait at the listener's own centre
      for (let i = 0; i < 100 && !/background decoding: profile 1, RF centre 145800\.0 kHz, 16 VFO/.test(L()); i++) await sleep(100);
    }
    const S = 'iqprio0001';
    spec = ws(port, `/ws/user-spectrum?user_session_id=${S}&bins=256`);
    await sleep(300);
    aud = ws(port, `/ws/audio?user_session_id=${S}`);
    await sleep(1500);
    spec.send({ type: 'tune', frequency: FREQ, mode: 'nfm' });
    await sleep(1500);
    if (withBg) out.inRange = /16 VFO\(s\) still in range carry on while a listener is on/.test(L());
    spec.send({ type: 'iqout', on: 1, rate: IQ_RATE });
    let iq = null;
    for (let i = 0; i < 50 && !iq; i++) { for (const t of spec.txt) if (t.includes('"type":"iqout"')) iq = JSON.parse(t); if (!iq) await sleep(100); }
    out.iq = iq;
    if (!iq || iq.on !== 1) return out;
    let got = 0;
    consumer = net.connect(iq.port, '127.0.0.1');
    consumer.on('data', (b) => { got += b.length; });
    consumer.on('error', () => {});
    await sleep(3000);                                   // ★ settle: the stream's first blocks, the resamplers built
    const s0 = await status(), g0 = got, t0 = Date.now();
    await sleep(15000);
    const s1 = await status(), t1 = Date.now();
    out.iqBps = (got - g0) * 1000 / (t1 - t0);
    out.iqDrops = (s1?.iqDrops ?? 0) - (s0?.iqDrops ?? 0);
    out.iqGaps = (s1?.iqGaps ?? 0) - (s0?.iqGaps ?? 0);
    out.iqOutDrops = (s1?.iqOutDrops ?? 0) - (s0?.iqOutDrops ?? 0);
    out.hasOutDrops = s1 && 'iqOutDrops' in s1;
    out.audio = cadence(aud, t0, t1);
    if (process.platform === 'linux') out.threads = threads(srv.pid);
    return out;
  } finally {
    try { consumer?.destroy(); } catch {}
    try { spec?.close(); aud?.close(); } catch {}
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(500);
  }
}

console.log('── the reference: the same listener and IQ stream, no background profile ──');
const ref = await run(false);
ok(ref.iq?.on === 1 && ref.iq.rate === IQ_RATE, `raw IQ out on at ${IQ_RATE} Hz (port ${ref.iq?.port})`);
const kb = (b) => `${(b / 1000).toFixed(1)} kB/s`;
console.log(`   IQ ${kb(ref.iqBps ?? 0)} · audio ${kb(ref.audio?.bps ?? 0)}, longest gap ${ref.audio?.gap} ms`);
ok(ref.iqBps >= IQ_RATE * 2 * 0.95, `the consumer gets its rate with no background work (${kb(ref.iqBps ?? 0)} of ${kb(IQ_RATE * 2)})`);
ok(ref.audio?.bps > 1000, `the listener's audio flows (${kb(ref.audio?.bps ?? 0)})`);
ok(ref.hasOutDrops, 'vibeserver.json reports iqOutDrops (the stream\'s own losses, apart from the radio\'s)');

console.log('── sixteen background VFOs in range, each demodulator saturating a core, raw IQ out at 250 kHz ──');
const bg = await run(true);
if (process.platform === 'linux') console.log(`   server pinned to two cores: ${bg.pinned ? 'yes' : 'NO (taskset missing) — the profile alone loads the machine'}`);
ok(bg.inRange, 'the background profile carries on in range with the listener on (all 16 VFOs)');
ok(bg.iq?.on === 1, 'raw IQ out on');
console.log(`   IQ ${kb(bg.iqBps ?? 0)} · audio ${kb(bg.audio?.bps ?? 0)}, longest gap ${bg.audio?.gap} ms · iqDrops ${bg.iqDrops} iqGaps ${bg.iqGaps} iqOutDrops ${bg.iqOutDrops}`);
ok(bg.iqBps >= IQ_RATE * 2 * 0.95, `★★★ the IQ consumer gets ≥ 95 % of its rate (${kb(bg.iqBps ?? 0)} of ${kb(IQ_RATE * 2)}) — the Pi 500 got 57 of 500`);
ok(bg.iqOutDrops === 0, `★★ the stream dropped nothing (iqOutDrops +${bg.iqOutDrops})`);
ok(bg.iqDrops === 0 && bg.iqGaps === 0, `★★★ the radio dropped nothing — the DSP thread never waited (iqDrops +${bg.iqDrops}, iqGaps +${bg.iqGaps})`);
ok(!/IQ overrun/.test(bg.L()), 'no "IQ overrun" in the log');
ok(bg.audio?.bps >= (ref.audio?.bps ?? 0) * 0.9, `★★★ the listener's audio keeps its rate (${kb(bg.audio?.bps ?? 0)} vs ${kb(ref.audio?.bps ?? 0)} without the profile)`);
ok(bg.audio?.gap < 400, `★★ no silence in the listener's audio (longest gap ${bg.audio?.gap} ms)`);
if (process.platform === 'linux' && bg.threads) {
  const of = (n) => bg.threads.filter((t) => t.comm === n);
  const nices = (n) => of(n).map((t) => t.nice);
  console.log(`   ranks: ${['vibe-dsp', 'vibe-demod', 'vibe-iqout', 'vibe-bgdsp', 'vibe-bgdemod', 'vibe-bgdec'].map((n) => `${n} ${nices(n).join('/') || '-'}`).join(' · ')}`);
  ok(of('vibe-bgdemod').length >= 16 && nices('vibe-bgdemod').every((x) => x === 19), `★★★ every background demodulator is at the background rung, 19 (${of('vibe-bgdemod').length} threads)`);
  ok(of('vibe-bgdsp').length === 1 && nices('vibe-bgdsp')[0] === 19, `★★ the background channeliser at 19 (${nices('vibe-bgdsp').join('/')})`);
  ok(of('vibe-bgdec').length >= 1 && nices('vibe-bgdec').every((x) => x === 19), `★★ the background decoders at 19 (${of('vibe-bgdec').length} threads)`);
  ok(of('vibe-demod').every((x) => x.nice < 19) && of('vibe-iqout').every((x) => x.nice < 19), '...and the listener\'s demod and the IQ writer are not');
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
