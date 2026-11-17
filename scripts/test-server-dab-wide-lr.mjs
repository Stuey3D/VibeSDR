// test-server-dab-wide-lr.mjs — V12 DAB PHASE 2 end to end: in a LOCKED WIDE RANGE every listener is in DAB on their OWN
// multiplex, and the radio never moves (docs/v12/DAB-OWN-STATION.md §11; Stuart, 2026-11-17: "the wideband locked range
// multiple users scenario for DAB").
//
// The REAL vibeserver, locked to 8 MS/s round 215.140 MHz, on fake rtl_tcp replaying a 10 s slice of the RSP1A capture
// that holds 10C (Northamptonshire), 10D (Herts Beds Bucks) and 11A (SDL National) — ~/VibeSDR-test-audio/dab-wide.
// Asserts:
//   · vibeserver.json offers DAB on this locked radio (it never did before: a locked centre could not do whole-radio DAB);
//   · listener A on 10C and listener B on 11A, at once: each one's reports name THEIR block and ensemble, each one's audio
//     socket carries audio at real time; one multiplex decoder per block, opened on the first listener;
//   · C on 11A picks a different service from B: a STATION on 11A (C hears theirs, B keeps theirs); D picks B's: shares;
//   · a listener asking for a block outside the range lands on the nearest inside; the block keys step inside the range;
//   · the radio NEVER MOVES: the fake logs no tune and no rate change after the server's start-up;
//   · E, not in DAB, keeps their own VFO and its audio; A leaves DAB: 10C's decoder closes; everyone gone: all close.
// ★ SILENT: audio frames are counted and discarded, never played. ★ Bounded: a hard deadline; the clip is 160 MB.
//   usage: VIBESERVER_BIN=… node scripts/test-server-dab-wide-lr.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CAP = process.env.DAB_WIDE_CAPTURE || path.join(os.homedir(), 'VibeSDR-test-audio/dab-wide/2026-11-16/dab-10C-10D-11A-215140kHz-8Msps.cu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
if (!fs.existsSync(CAP)) { console.log(`   not run — the wide capture is missing (${CAP})`); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const now = () => performance.now();

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, binBytes: 0, last: {}, onText: null, sq: 0, ns: 0 };
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
        st.bin++; st.binBytes += p.length;
        // ★ PCM (format 0): the samples' energy — bytes alone cannot tell a station from the silence a starved one sends
        if (p.length > 6 && p[1] === 0) for (let i = 6; i + 1 < p.length; i += 2) { const v = p.readInt16LE(i); st.sq += v * v; st.ns++; }
      }
      if (op === 0x1) {
        const t = p.toString();
        try { const j = JSON.parse(t); if (j && j.type) { st.last[j.type] = { at: now(), ...j }; if (st.txt.length < 4000) st.txt.push(j.type); } } catch {}
        if (st.onText) st.onText(t);
      }
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
/** One listener: an audio socket and a spectrum socket under one session. */
function listener(port, tag) {
  const sid = `${tag}-${crypto.randomBytes(6).toString('hex')}`;
  const aud = ws(port, `/ws/audio?user_session_id=${sid}&codec=pcm`);
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256`);
  return { sid, aud, spec, dab: () => spec.last.dab, close: () => { aud.close(); spec.close(); } };
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dablr-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
// ★ 10 s of the capture (160 MB), from 5 s in — looped by the fake
const CLIP = path.join(dir, 'wide-10s.cu8');
{ const fd = fs.openSync(CAP, 'r'); const b = Buffer.alloc(8000000 * 2 * 10); fs.readSync(fd, b, 0, b.length, 8000000 * 2 * 5); fs.closeSync(fd); fs.writeFileSync(CLIP, b); }
const rtlPort = await freePort(), port = await freePort();
const RATE = 8000000, PHYS = 215140000;
const LOCK = PHYS - 15000;   // ★ rtl_tcp: the server tunes the dongle 15 kHz high (its DC off the dial) — physical = 215.140
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, fullMode: true, sharing: 'public',
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: 8, mode: 'locked', lockFreq: LOCK,
             rate: RATE, freq: LOCK, demodMode: 'wfm', dabOwnStation: 1, autoPpm: true }],
}));
const rtlLog = [];
const rtl = spawn(process.execPath, ['--max-old-space-size=512', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--dab-map', `${PHYS}=${CLIP}`], { stdio: ['ignore', 'pipe', 'ignore'] });
rtl.stdout.on('data', (d) => { for (const l of d.toString().split('\n')) if (l && rtlLog.length < 5000) rtlLog.push({ t: now(), l }); });
await sleep(2500);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--idle-grace', '0'], { env, stdio: ['ignore', logFd, logFd] });
const log = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
const cleanup = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
const deadline = setTimeout(() => { console.log('   FAIL: hard deadline'); cleanup(); process.exit(1); }, 6 * 60 * 1000);
for (let i = 0; i < 150; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
await sleep(3000);
const until = async (f, ms) => { const t = now(); while (now() - t < ms) { if (f()) return true; await sleep(100); } return !!f(); };
const moves = (from) => rtlLog.filter((e) => e.t >= from && /\] (tune|sample rate) /.test(e.l)).map((e) => e.l.replace(/^.*\] /, ''));

const L = [];
try {
  const info = await fetch(`http://127.0.0.1:${port}/vibeserver.json`).then((r) => r.json()).catch(() => ({}));
  ok(info.dab === true, `★ vibeserver.json offers DAB on this locked 8 MS/s radio (dab: ${info.dab})`);
  const t0 = now();

  console.log('── A on 10C, B on 11A, at once ──');
  const A = listener(port, 'lrA'), B = listener(port, 'lrB'), E = listener(port, 'lrE');
  L.push(A, B, E);
  await sleep(1500);
  A.spec.send({ type: 'dab', on: 1, channel: '10C' });
  B.spec.send({ type: 'dab', on: 1, channel: '11A' });
  const locked = (x, ch) => x.dab() && x.dab().channel === ch && x.dab().locked === true && (x.dab().label || '').length > 0;
  ok(await until(() => locked(A, '10C') && locked(B, '11A'), 45000), `★ both locked on their own block: A ${A.dab()?.channel} "${A.dab()?.label}", B ${B.dab()?.channel} "${B.dab()?.label}"`);
  ok(/Northampton/i.test(A.dab()?.label || '') && /SDL/i.test(B.dab()?.label || ''), 'the ensembles are the ones those blocks carry (Northamptonshire, SDL National)');
  ok(/\[DAB-LR\] multiplex 10C opened/.test(log()) && /\[DAB-LR\] multiplex 11A opened/.test(log()) && !/multiplex 10D opened/.test(log()),
     'one multiplex decoder per block with a listener — and none for 10D, which nobody asked for');
  ok(A.dab()?.lr === true && B.dab()?.lr === true, 'each report says it is this listener\'s own (lr)');
  const rms = (x) => (x.ns ? Math.sqrt(x.sq / x.ns) : 0);
  A.aud.sq = A.aud.ns = B.aud.sq = B.aud.ns = 0;
  const a0 = A.aud.binBytes, b0 = B.aud.binBytes, e0 = E.aud.binBytes;
  await sleep(6000);
  const aRate = (A.aud.binBytes - a0) / 6, bRate = (B.aud.binBytes - b0) / 6;
  ok(aRate > 100000 && bRate > 100000, `★ each one's audio socket carries their station at real time (A ${Math.round(aRate / 1024)} kB/s, B ${Math.round(bRate / 1024)} kB/s of PCM)`);
  ok(rms(A.aud) > 200 && rms(B.aud) > 200, `★ …and it is programme, not silence (RMS A ${rms(A.aud).toFixed(0)}, B ${rms(B.aud).toFixed(0)} of 32767)`);
  ok(E.aud.binBytes - e0 > 0, `E (not in DAB) keeps their own VFO audio (${E.aud.binBytes - e0} bytes)`);
  ok(!E.spec.last.dab, 'E is never sent a DAB report');
  const svc11A = (B.dab()?.services || []).filter((s) => !s.isData && s.sid);
  ok(svc11A.length >= 10, `11A's list arrived (${svc11A.length} services)`);

  console.log('── C on 11A picks another service; D picks B\'s ──');
  const C = listener(port, 'lrC'), D = listener(port, 'lrD');
  L.push(C, D);
  await sleep(1200);
  const bSid = B.dab()?.mine || B.dab()?.sid;
  const other = svc11A.find((s) => s.sid !== bSid)?.sid;
  C.spec.send({ type: 'dab', on: 1, channel: '11A' });
  D.spec.send({ type: 'dab', on: 1, channel: '11A' });
  await until(() => C.dab()?.channel === '11A' && D.dab()?.channel === '11A', 15000);
  C.spec.send({ type: 'dab_service', sid: other });
  D.spec.send({ type: 'dab_service', sid: bSid });
  ok(await until(() => C.dab()?.mine === other && D.dab()?.mine === bSid, 15000), `★ C hears ${other?.toString(16)}, D shares B's ${bSid?.toString(16)}`);
  ok(/11A: station [0-9A-F]+ opened/.test(log()), '★ C\'s pick opened a STATION on 11A (Phase 1\'s station mode, on this multiplex)');
  await sleep(1500);
  ok((B.dab()?.mine || B.dab()?.sid) === bSid, 'B still hears their own service');
  const pl = B.dab()?.playing || [];
  ok(pl.some((p) => p.sid === bSid && p.n === 2) && pl.some((p) => p.sid === other && p.n === 1), `11A's playing list: ${JSON.stringify(pl)}`);
  ok(!(A.dab()?.playing || []).some((p) => p.sid === other), '10C\'s playing list knows nothing of 11A');
  C.aud.sq = C.aud.ns = 0;
  const c0 = C.aud.binBytes; await sleep(4000);
  ok(rms(C.aud) > 200, `C's station is programme, not silence (RMS ${rms(C.aud).toFixed(0)})`);
  ok((C.aud.binBytes - c0) / 4 > 100000, `★ C's station plays (${Math.round((C.aud.binBytes - c0) / 4096)} kB/s)`);

  console.log('── blocks outside the range ──');
  const F = listener(port, 'lrF'); L.push(F);
  await sleep(1200);
  F.spec.send({ type: 'dab', on: 1, channel: '12B' });
  ok(await until(() => F.dab()?.channel === '11A', 15000), `★ 12B (outside) → the nearest block inside, 11A (got ${F.dab()?.channel})`);
  F.spec.send({ type: 'dab', on: 1, channel: '11B' });
  await sleep(2500);
  ok(F.dab()?.channel === '11A', `stepping up past the top of the range stays inside it (${F.dab()?.channel})`);
  F.spec.send({ type: 'dab', on: 1, channel: '10B' });
  ok(await until(() => F.dab()?.channel === '10D', 15000), `stepping down from 11A to a block outside lands on the next inside below, 10D (${F.dab()?.channel})`);
  F.close();

  // ★★ §11.2: ONE clock reading for the radio, from the listeners' multiplexes (the replayed capture is the RSP's own air)
  ok(/\[DAB-LR\] clock: [23] multiplexes, MER-weighted median [-+][0-9.]+ ppm/.test(log()), `★ §11.2: ONE clock reading for the radio, the MER-weighted median of the multiplexes (${(log().match(/\[DAB-LR\] clock:[^\n]*/) || ['none'])[0].slice(0, 110)})`);
  ok(moves(t0).length === 0, `★★ THE RADIO NEVER MOVED: no tune, no rate change while listeners were in DAB (${JSON.stringify(moves(t0).slice(0, 4))})`);

  console.log('── leaving ──');
  A.spec.send({ type: 'dab', on: 0 });
  ok(await until(() => !!A.spec.last.dab_off, 5000), 'A is told DAB is off');
  ok(await until(() => /\[DAB-LR\] multiplex 10C closed/.test(log()), 5000), '★ 10C\'s decoder closed with its last listener');
  const a1 = A.aud.binBytes; await sleep(2500);
  ok(A.aud.binBytes - a1 > 0, `A's own VFO audio is back (${A.aud.binBytes - a1} bytes)`);
  for (const x of L) x.close();
  ok(await until(() => /\[DAB-LR\] multiplex 11A closed/.test(log()) || /multiplex 1[0-9][A-D] closed[^\n]*\(0 open\)/.test(log()), 8000), '★ everyone gone: every multiplex decoder closed');
  ok(!/Segmentation|Assertion|terminate called|ABORT/.test(log()), 'no crash in the server log');
} finally {
  clearTimeout(deadline);
  cleanup();
}
console.log(`\n   ${pass} passed, ${fail} failed`);
if (fail) console.log(`   log: ${path.join(dir, 'server.log')}`);
else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
