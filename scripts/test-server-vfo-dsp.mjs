// test-server-vfo-dsp.mjs — beta93: VFO A and VFO B each have their OWN noise reduction, auto notch and noise blanker, and
// the NR ECHO STORM is gone (Stuart, 2026-11-24, XCover web page: the NR slider slid by itself, 38 → 33 → 31 % with A+B and
// 30 → 2 % with A alone; the spectrum socket at ~1 MB/s; VFO B would not switch off).
// Through the REAL server on a fake rtl_tcp (the stations of test-server-vfo-ab.mjs), on a one-pipeline radio and a shared
// dial:
//   1. A's NR in hwinfo; B on → B's own NR in the watch state (seeded from A's); B set to 70 % → B reports 70 %, A still
//      30 %; A set to 20 % → B untouched; B's notch + blanker on → reported for B, A's notch still off.
//   2. NO ANSWER TO AN UNCHANGED SETTING (30 unchanged nr / notch / nbx → no hwinfo), and a 5-value NR drag is answered
//      ONCE, with its last value (the debounce that kills the ring even for a beta68–beta92 page that echoes).
//   3. B's AUTO NOTCH acts on B's audio (B's steady 620 Hz tone, right ear) and not on A's (400 Hz, left).
//   4. THE RATE GUARD (one-pipeline): 30 s of the OLD echoing page with A+B on, different NR per VFO, drags in flight on
//      both — the spectrum socket stays at its idle rate (beta92: ~1 MB/s), and both values settle where they were put.
//   5. VFO B OFF with NR on both closes it: logged, no chan 1 in the watch state, still off 3 s later.
// Silent: audio is analysed in memory, never played.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-vfo-dsp.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000, CENTRE = 7000000;
// ★ The stations sit 30 kHz above the radio's centre: one ON the centre is DC, which the capture cleaner removes, and two
//   either side of it are each other's I/Q image.
const TONES_AT = CENTRE + 30000;
const A_HZ = TONES_AT - 120000, MID_HZ = TONES_AT, KEY_HZ = TONES_AT + 120000;   // 400, 620, 840 Hz tones
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, bytes: 0 };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    st.bytes += d.length;
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
const lastWatch = (st) => { const w = jsons(st, 'watch'); return w[w.length - 1]; };

/** Raw PCM frames → { ch, L, R } (Float32, mono duplicated). Opus frames counted, not decoded. */
function pcmOf(bins) {
  const L = [], R = []; let ch = 0, opus = 0, st2 = 0;
  for (const b of bins) {
    if (b.length < 6) continue;
    if (b[1] === 3) { opus++; if (b[0] === 2) st2++; continue; }
    if (b[1] !== 0) continue;
    ch = b[0];
    const n = (b.length - 6) / 2;
    for (let i = 0; i < n; i += ch) {
      const l = b.readInt16LE(6 + 2 * i) / 32768, r = ch === 2 ? b.readInt16LE(6 + 2 * (i + 1)) / 32768 : l;
      L.push(l); R.push(r);
    }
  }
  return { ch, L, R, opus, st2 };
}
function goertzel(x, f, fs = 48000) {
  const n = x.length, w = (2 * Math.PI * f) / fs, cw = 2 * Math.cos(w);
  let s1 = 0, s2 = 0;
  for (let i = 0; i < n; i++) { const s = x[i] + cw * s1 - s2; s2 = s1; s1 = s; }
  return Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / Math.max(1, n);
}
/** Record `ms` of audio, then analyse the tones in each ear (a Hann-free Goertzel over a whole-cycle length). */
async function listen(aud, ms) {
  aud.bin.length = 0; aud.keepBin = true;
  await sleep(ms);
  aud.keepBin = false;
  const p = pcmOf(aud.bin);
  const n = Math.min(p.L.length, 48000);   // 1 s: whole cycles of every tone
  const L = p.L.slice(-n), R = p.R.slice(-n);
  const t = (x) => ({ 400: goertzel(x, 400), 620: goertzel(x, 620), 840: goertzel(x, 840) });
  return { ch: p.ch, n, L: t(L), R: t(R), same: L.every((v, i) => Math.abs(v - R[i]) < 1e-3), opus: p.opus, st2: p.st2 };
}
const fmt = (t) => `400:${t[400].toFixed(4)} 620:${t[620].toFixed(4)} 840:${t[840].toFixed(4)}`;
/** Tone `f` wholly in ear `e` ('L' or 'R') — at least 30 dB above it in the other ear. */
const only = (a, f, e) => { const o = e === 'L' ? 'R' : 'L'; return a[e][f] > 0.002 && a[e][f] > 30 * a[o][f]; };

async function server(args, tag, radioExtra = {}, fakeExtra = [], extraEnv = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-vfodsp-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'VFO A/B test', locator: 'IO92nh', adminPass: 'vfoab-test-admin',
    radios: [{ serial: 'vfoab-test', label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--tones', '3', '--tones-at', String(TONES_AT), '--key', '3', ...fakeExtra], { stdio: ['ignore', 'pipe', 'ignore'] });
  // ★ every tune the fake is sent — the tuner's frequency IS where a dongle's DC is (§7)
  const tunes = [];
  rtl.stdout.on('data', (d) => { for (const m of d.toString().matchAll(/tune ([0-9.]+) MHz/g)) { tunes.push(Math.round(Number(m[1]) * 1e6)); if (tunes.length > 200) tunes.shift(); } });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  delete env.VIBE_TCP_DC_OFFSET_HZ;   // ★ the R820T header decides the offset (15 kHz), as on a real rtl_tcp dongle
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'am', ...args],
                    { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  // ★ The scratch directory goes with the server unless something failed (then its server.log is the evidence).
  const failAtStart = fail;
  return { port, dir, pid: srv.pid, tunes, dcNow: () => (tunes.length ? tunes[tunes.length - 1] : NaN), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM');
                                    if (fail === failAtStart) setTimeout(() => fs.rmSync(dir, { recursive: true, force: true }), 300); }, log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8') };
}
async function listener(port, sid, f, opus = false) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}${opus ? '&codec=opus' : ''}`);
  await sleep(400);
  if (f) L.spec.send({ type: 'tune', frequency: f, mode: 'am' });
  await sleep(500);
  return L;
}
const adminStatus = async (port) => {
  const n = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q = `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', 'vfoab-test-admin').update(n).digest('hex')}`;
  return fetch(`http://127.0.0.1:${port}/vibeserver/admin/status?${q}`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json());
};
const hwWatch = (L) => { const h = jsons(L.spec, 'hwinfo'); return h.length ? h[h.length - 1].watch : undefined; };

// ── The page's NR map (web/client/src/vfoDsp.ts) — the strength a slider percentage sends ────────────────────────────────
const nrStrength = (p) => (p <= 80 ? p / 80 : 1 + ((p - 80) / 20) * 0.4);
const nrPercent = (s) => (s <= 1 ? s * 80 : 80 + ((s - 1) / 0.4) * 20);
const hw = (st) => jsons(st, 'hwinfo');
const lastHw = (st) => { const h = hw(st); return h[h.length - 1]; };
const chanB = (st) => { const w = lastWatch(st); return w && w.chans.find((c) => c.id === 1); };
/** The beta68–beta92 page's apply path, the WORST case a server must survive: every reported strength that differs from
 *  the slider is set AND RE-SENT; and VFO B's watch report, treated the same way. Returns a stop(). */
function echoClient(L, start) {
  const v = { A: start.A, B: start.B };
  let seenTxt = L.spec.txt.length;
  const t = setInterval(() => {
    const txt = L.spec.txt; const n = txt.length;
    for (let i = seenTxt; i < n; i++) {
      const s = txt[i];
      if (s.includes('"type":"hwinfo"')) {
        const m = JSON.parse(s);
        if (typeof m.nr !== 'boolean') continue;
        const want = !m.nr ? 0 : Math.max(1, Math.round(nrPercent(m.nrStrength)));
        if (want !== v.A) { v.A = want; L.spec.send({ type: 'nr', on: want > 0, strength: nrStrength(want) }); echoClient.sends++; }
      } else if (s.includes('"type":"watch"')) {
        const c = JSON.parse(s).chans?.find((x) => x.id === 1);
        if (!c || typeof c.nr !== 'boolean') continue;
        const want = !c.nr ? 0 : Math.max(1, Math.round(nrPercent(c.nrStrength)));
        if (want !== v.B) { v.B = want; L.spec.send({ type: 'watch', id: 1, nr: want > 0, nrStrength: nrStrength(want) }); echoClient.sends++; }
      }
    }
    seenTxt = n;
  }, 2);
  return () => clearInterval(t);
}
echoClient.sends = 0;
/** A fresh hwinfo (asked for) and a fresh watch state (the half-second beat) — nothing older. */
async function fresh(L) {
  L.spec.txt.length = 0; L.spec.send({ type: 'state' });
  await until(() => !!lastHw(L.spec) && !!chanB(L.spec), 3000);
}

for (const [tag, args, what] of [['one', ['--users', '1'], 'one-pipeline radio (users 1)'],
                                 ['shared', ['--users', '3'], 'shared dial (users 3, unlocked)']]) {
  console.log(`── ${what} ──`);
  const S = await server(args, `dsp-${tag}`);
  try {
    const L = await listener(S.port, `vfodsp${tag}1`, A_HZ);
    await until(() => lastHw(L.spec) !== undefined, 4000);
    // ── 1. A's NR, then B on: B is seeded from A, then set separately and reported separately ──
    L.spec.send({ type: 'nr', on: true, strength: nrStrength(30) });
    await until(() => { const h = lastHw(L.spec); return h && h.nr === true && Math.abs(h.nrStrength - 0.375) < 1e-6; }, 3000);
    ok(Math.abs(lastHw(L.spec).nrStrength - 0.375) < 1e-6, `VFO A's NR 30 % reported in hwinfo (${lastHw(L.spec).nrStrength})`);
    L.spec.send({ type: 'watch', id: 1, frequency: MID_HZ, mode: 'am' });
    await until(() => !!chanB(L.spec), 3000);
    let b = chanB(L.spec);
    ok(b && b.nr === true && Math.abs(b.nrStrength - 0.375) < 1e-6 && b.notch === false && typeof b.nbx === 'boolean',
       `VFO B on: its own NR reported in the watch state, seeded from A's (${JSON.stringify(b && { nr: b.nr, s: b.nrStrength, notch: b.notch, nbx: b.nbx })})`);
    L.spec.send({ type: 'watch', id: 1, nr: true, nrStrength: nrStrength(70) });
    await until(() => { const c = chanB(L.spec); return c && Math.abs(c.nrStrength - nrStrength(70)) < 1e-6; }, 3000);
    await fresh(L);
    b = chanB(L.spec);
    ok(Math.abs(nrPercent(b.nrStrength) - 70) < 1e-6 && Math.abs(lastHw(L.spec).nrStrength - 0.375) < 1e-6,
       `B set to 70 %: B reports 70 %, A still reports 30 % (A ${nrPercent(lastHw(L.spec).nrStrength)} %, B ${nrPercent(b.nrStrength)} %)`);
    L.spec.send({ type: 'nr', on: true, strength: nrStrength(20) });
    await until(() => { const h = lastHw(L.spec); return h && Math.abs(h.nrStrength - 0.25) < 1e-6; }, 3000);
    await sleep(700);
    ok(Math.abs(lastHw(L.spec).nrStrength - 0.25) < 1e-6 && Math.abs(nrPercent(chanB(L.spec).nrStrength) - 70) < 1e-6,
       `A set to 20 %: B untouched at 70 % (${nrPercent(chanB(L.spec).nrStrength)} %)`);
    L.spec.send({ type: 'watch', id: 1, notch: true, nbx: true });
    await until(() => { const c = chanB(L.spec); return c && c.notch === true && c.nbx === true; }, 3000);
    await fresh(L);
    ok(chanB(L.spec).notch === true && chanB(L.spec).nbx === true && lastHw(L.spec).notch === false,
       `B's AUTO NOTCH and NOISE BLANKER on: B reports them, A's notch still off (A notch ${lastHw(L.spec).notch})`);
    L.spec.send({ type: 'watch', id: 1, notch: false, nbx: false });
    await sleep(500);

    // ── 2. No answer to an unchanged setting ──
    const nbxNow = lastHw(L.spec).nbx === true;
    await sleep(800);   // ★ any answer still due (the NR answer is debounced) lands before the count starts
    L.spec.txt.length = 0;
    for (let i = 0; i < 10; i++) L.spec.send({ type: 'nr', on: true, strength: 0.25 });
    for (let i = 0; i < 10; i++) L.spec.send({ type: 'notch', on: false });
    for (let i = 0; i < 10; i++) L.spec.send({ type: 'nbx', on: nbxNow });
    await sleep(1200);
    ok(hw(L.spec).length === 0, `30 unchanged nr / notch / nbx: no hwinfo answered (${hw(L.spec).length})`);
    // ── a drag answers ONCE, with the last value ──
    L.spec.txt.length = 0;
    for (const p of [22, 25, 28, 31, 34]) L.spec.send({ type: 'nr', on: true, strength: nrStrength(p) });
    await sleep(1200);
    const dh = hw(L.spec);
    ok(dh.length === 1 && Math.abs(dh[0].nrStrength - nrStrength(34)) < 1e-6,
       `a 5-value drag is answered once, with its last value (${dh.length} hwinfo, ${dh.map((h) => nrPercent(h.nrStrength)).join(', ')} %)`);

    // ── 3. Audio: B's own notch takes B's steady tone and leaves A's (the one-pipeline radio's raw PCM stream; the shared
    //    dial's B runs the same WatchChannel, on an Opus stream this test does not decode — test-server-vfo-ab §3) ──
    if (tag === 'one') {
    L.spec.send({ type: 'nr', on: false, strength: 0 });
    L.spec.send({ type: 'watch', id: 1, nr: false });
    await sleep(2500);
    let a0 = await listen(L.aud, 1600);
    L.spec.send({ type: 'watch', id: 1, notch: true });
    await sleep(2500);
    let a1 = await listen(L.aud, 1600);
    console.log(`      B notch off: L ${fmt(a0.L)} · R ${fmt(a0.R)}\n      B notch on : L ${fmt(a1.L)} · R ${fmt(a1.R)}`);
    ok(a1.R[620] < 0.3 * a0.R[620] && a1.L[400] > 0.7 * a0.L[400],
       `B's AUTO NOTCH acts on B's audio only: B's 620 Hz ${a0.R[620].toFixed(4)} → ${a1.R[620].toFixed(4)}, A's 400 Hz ${a0.L[400].toFixed(4)} → ${a1.L[400].toFixed(4)}`);
    L.spec.send({ type: 'watch', id: 1, notch: false });
    }

    // ── 4. The rate guard: the WORST client (the old echoing page), A+B on, different NR per VFO, drags in flight ──
    if (tag === 'one') {
      await sleep(800);
      let b0 = L.spec.bytes ?? 0;
      const t0 = Date.now();
      await sleep(5000);
      const base = ((L.spec.bytes ?? 0) - b0) / ((Date.now() - t0) / 1000);
      echoClient.sends = 0;
      const stop = echoClient(L, { A: 0, B: 0 });
      for (const p of [40, 44, 48, 52]) L.spec.send({ type: 'nr', on: true, strength: nrStrength(p) });
      for (const p of [80, 76, 72, 68]) L.spec.send({ type: 'watch', id: 1, nr: true, nrStrength: nrStrength(p) });
      b0 = L.spec.bytes; const t1 = Date.now();
      await sleep(30000);
      stop();
      const storm = (L.spec.bytes - b0) / ((Date.now() - t1) / 1000);
      ok(storm < base * 1.5 + 4000 && echoClient.sends < 50,
         `30 s with an echoing page, A+B NR different and in flight: spectrum ${Math.round(storm / 1024)} KB/s vs ${Math.round(base / 1024)} KB/s idle, the page re-sent ${echoClient.sends} (beta92: ~1 MB/s, thousands)`);
      const h = lastHw(L.spec), c = chanB(L.spec);
      ok(Math.abs(nrPercent(h.nrStrength) - 52) < 1e-6 && Math.abs(nrPercent(c.nrStrength) - 68) < 1e-6,
         `…and both settled where they were put: A ${nrPercent(h.nrStrength)} %, B ${nrPercent(c.nrStrength)} %`);
    }

    // ── 5. VFO B OFF closes it — with NR set on both — and it stays off ──
    const stop2 = echoClient(L, { A: 0, B: 0 });
    L.spec.send({ type: 'nr', on: true, strength: nrStrength(45) });
    L.spec.send({ type: 'watch', id: 1, off: true });
    await until(() => { const w = lastWatch(L.spec); return w && !w.chans.some((c) => c.id === 1); }, 3000);
    await sleep(3000);
    stop2();
    const w = lastWatch(L.spec);
    ok(/VFO B off for session/.test(S.log()) && w && !w.chans.some((c) => c.id === 1),
       `VFO B OFF with NR on both: the server logs the close, the watch state has no chan 1, and 3 s later it is still off`);
    L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
