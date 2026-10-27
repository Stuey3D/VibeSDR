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
//   5. (beta25) FREE TUNING WITH THE APP'S OWN MESSAGES — its recentre zooms and edge pans, "free":1 — zoomed (60 kHz) and
//      full span, auto IF and manual: 9 kHz steps and 500 Hz sweeps across the DC, up and back. The centre moves only for
//      the DC, ≤ one nudge's reach, once per crossing, never back onto the dial (no "deferred retune"), and a small pan
//      moves nothing. On beta24 (da9dc130f) the zoomed auto-IF case retunes 14 times for one crossing and ends on the dial;
//   6. LOCKED VFO (a zoom onto the dial every tune), auto IF: the centre stays put while the view is inside the 350 kHz IF
//      filter (175 kHz either side), then follows ONCE, decisively — a handful of retunes for 40 tunes (beta24: one or two
//      per tune), never two on one step, the DC never in the passband, the filter never widened by a tune;
//   7. the clients are told where the spike is: hwinfo `dcHz` on connect, a `dc` message after the nudge.
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
  const st = { bin: [], txt: [], other: [], open: false, keepBin: false };
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
      if (op === 0x1) {
        const t = p.toString();
        if (t.includes('"type":"config"')) { st.txt.push(t); if (st.txt.length > 2000) st.txt.splice(0, 500); }
        else if (t.startsWith('{"type":"dc"') || t.startsWith('{"type":"hwinfo"')) { try { st.other.push(JSON.parse(t)); } catch {} if (st.other.length > 500) st.other.splice(0, 100); }
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
    const n = S.tunes.length;
    L.spec.send({ type: 'tune', frequency: f, mode: 'am' });
    await sleep(90);
    /* ★★ A RETUNE THE SERVER HAS MADE BUT THE FAKE HAS NOT PRINTED YET IS NOT "ON THE DC". The tuner command crosses a
     *  socket to the fake and comes back through its stdout pipe; under load that took more than the 90 ms step, and the
     *  step that CAUSED the move (dial 7000.0, DC 6993.0 — 7.0 kHz, inside the ±7.5 kHz zone) was read with the DC
     *  still where it had been: ~3 runs in 4 under an Xcode build. The server's zone never varied (its own log: 6.9925–
     *  7.0075 MHz every run). So a step that reads inside the zone waits — with a deadline — for a retune to arrive;
     *  a real fault (no retune) still fails. */
    if (Math.abs(S.dcNow() - f) < ZONE - 100) await until(() => S.tunes.length > n, 1500);
    out.push({ dial: f, dc: S.dcNow() });
  }
  return { steps: out, retunes: S.tunes.length - n0, onDc: out.filter((s) => Math.abs(s.dc - s.dial) < ZONE - 100) };   // ★ the fake prints 100 Hz
}
/* ★★★ V12 beta25 — THE APP'S OWN MESSAGES IN FREE TUNING, not "only tune". Stuart, beta24 on the Pi 500: "the RF centre is
 *  following the VFO when in free mode and kinda kills the whole free tuning mode". What the app really sends
 *  (src/services/VibeServerWsClient.ts tune(), src/screens/SDRScreen.tsx keepVfoAtEdge):
 *    · `tune` every step;
 *    · FREE: a `zoom` back onto the dial once the dial is past 45 % of the VIEW span from the view's centre (`beyondCapture`
 *      reads status.bwHz, which is the view's span) or past half of it (`beyondView`); LOCKED: that zoom on EVERY tune;
 *    · then, FREE only, keepVfoAtEdge: a pan to dial ∓ 92 % of the half-span if the dial is still beyond it.
 *  Every view message carries binBandwidth (pan() sends the current one). `hint` adds `"free":1`, as the app and the web
 *  now say (viewFree_ in the shim); without it this is the app as shipped up to beta24. */
function appClient(L, cfg, { free = true, hint = true, spanHz = 60000 } = {}) {
  const n = cfg.binCount;
  const A = { view: { c: cfg.centerFreq, bb: Math.min(spanHz, cfg.maxBandwidth) / n }, dial: cfg.vfo || cfg.centerFreq };
  const sendView = (c, bb) => { A.view = { c: Math.round(c), bb }; L.spec.send({ type: 'zoom', frequency: Math.round(c), binBandwidth: bb, ...(free && hint ? { free: 1 } : {}) }); };
  A.zoom = (c) => sendView(c, A.view.bb);
  A.tune = (f) => {
    const prev = A.dial; A.dial = f;
    L.spec.send({ type: 'tune', frequency: f, mode: 'am' });
    if (Math.abs(f - prev) < 1) return;
    const span = A.view.bb * n, ref = A.view.c;
    const beyond = Math.abs(f - ref) > span * 0.45 || Math.abs(f - ref) > span * 0.5;
    if (!free || beyond) A.zoom(f);
    if (!free) return;
    const lim = (span / 2) * 0.92, off = f - A.view.c;            // ★ keepVfoAtEdge
    if (Math.abs(off) <= lim) return;
    const target = f - Math.sign(off) * lim;
    if (Math.abs(target - A.view.c) < A.view.bb) return;
    A.zoom(target);
  };
  return A;
}
/** A sweep through appClient: after every step, the tuner (the fake's last tune) and the dial. */
async function appSweep(S, A, from, to, step, ms = 120) {
  const out = [];
  const n0 = S.tunes.length, log0 = S.log().length;
  const dir = Math.sign(to - from);
  for (let f = from; dir > 0 ? f <= to : f >= to; f += dir * Math.abs(step)) {
    const n = S.tunes.length;
    A.tune(f);
    await sleep(ms);
    if (Math.abs(S.dcNow() - f) < ZONE - 100) await until(() => S.tunes.length > n, 1500);
    out.push({ dial: f, dc: S.dcNow(), moved: S.tunes.length - n });
  }
  await sleep(400);   // ★ a deferred move (the pan cooldown, 120 ms) has had every chance to land
  const tail = S.log().slice(log0);
  return { steps: out, retunes: S.tunes.length - n0, moves: S.tunes.slice(n0), settled: S.dcNow(),
           onDc: out.filter((s) => Math.abs(s.dc - s.dial) < ZONE - 100),
           nudges: (tail.match(/VFO free: DC/g) || []).length, deferred: (tail.match(/deferred retune applied/g) || []).length,
           edges: (tail.match(/auto IF: the view or the VFO reached the IF filter's edge/g) || []).length,
           maxPerStep: Math.max(0, ...out.map((o) => o.moved)) };
}
async function tone(L, ms = 2000) {
  L.aud.bin.length = 0; L.aud.keepBin = true;
  await sleep(ms);
  L.aud.keepBin = false;
  const x = pcmOf(L.aud.bin); L.aud.bin.length = 0;
  const y = x.subarray(Math.max(0, x.length - 48000));
  return { n: y.length, t620: goertzel(y, 620), t400: goertzel(y, 400), t840: goertzel(y, 840) };
}

setTimeout(() => { console.log('   FAIL the whole test passed its 9-minute deadline'); for (const c of children) { try { c.kill('SIGKILL'); } catch {} } process.exit(1); }, 540000).unref();
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
  /* ★★★ 5. FREE TUNING WITH THE APP'S OWN MESSAGES (beta25). The centre stays put — "the RF centre can be off screen in free
   *  mode it is fine" (Stuart) — and moves only for the DC, by no more than a nudge, or (auto IF) once the view reaches the
   *  IF filter's edge ("a 350KHz IF filter should in theory give 175KHz of breathing room"); a nudge sticks: no "deferred
   *  retune" putting the tuner back on the dial. */
  const freeCase = async (tag, { spanHz, ifHz = 0, from, to, step, back }) => {
    const S = await server(tag, ['--users', '1']);
    try {
      const L = await listener(S.port, `vfodc${tag}`.slice(0, 12).padEnd(12, '0'), CENTRE);
      if (ifHz) { L.spec.send({ type: 'tunerbw', value: ifHz }); await sleep(300); }
      const cfg = configs(L.spec).slice(-1)[0];
      const A = appClient(L, cfg, { spanHz });
      A.zoom(CENTRE);
      await sleep(700);
      const c0 = S.dcNow() - OFF;
      const ifNote = ifHz ? `IF ${ifHz / 1e3} kHz manual` : 'auto IF';
      // ★ auto IF on a 60 kHz view: the filter is 350 kHz (its floor), so the view may walk ±(175 − 30) kHz from the tuner
      //   before it moves — 270 kHz of travel needs at most two edge moves besides the crossing's nudge
      const edgesOk = !ifHz && spanHz < cfg.maxBandwidth ? 2 : 0;
      const check = (r, what, maxRetunes) => {
        ok(r.onDc.length === 0, `${what}: ${r.steps.length} tunes, the DC never inside ±${ZONE / 1e3} kHz of the dial`);
        ok(r.retunes <= maxRetunes + edgesOk, `${what}: ${r.retunes} retune(s) (≤ ${maxRetunes} per crossing + ${edgesOk} at the IF filter's edge; ${r.moves.map(kHz).join(', ') || 'none'})`);
        ok(r.retunes === r.nudges + r.edges && r.edges <= edgesOk && r.deferred === 0 && r.maxPerStep <= 1,
           `${what}: every move a DC nudge or an IF-edge move (${r.nudges} nudge(s), ${r.edges} edge move(s), ${r.deferred} deferred, ≤ ${r.maxPerStep} per step)`);
        const last = r.steps[r.steps.length - 1];
        ok(r.settled === last.dc && Math.abs((r.settled - OFF) - last.dial) > 1000, `${what}: the centre is NOT on the dial at the end, and nothing moved it after (centre ${kHz(r.settled - OFF)}, dial ${kHz(last.dial)})`);
      };
      const up = await appSweep(S, A, from, to, step);
      check(up, `${tag} up (${spanHz >= cfg.maxBandwidth ? 'full span' : `${spanHz / 1e3} kHz view`}, ${ifNote})`, 1);
      if (up.nudges) ok(Math.abs(up.moves[0] - OFF - c0) <= 25000, `${tag}: the nudge moved the centre ${kHz(Math.abs(up.moves[0] - OFF - c0))} kHz (≤ a nudge's reach)`);
      const down = await appSweep(S, A, to, back, -step);
      check(down, `${tag} down`, 1);
      // ★ a small pan of the view (the web's FREE, or a finger): the centre stays where the nudge left it
      const n1 = S.tunes.length;
      A.zoom(A.view.c + 2000);
      await sleep(500);
      ok(S.tunes.length === n1, `${tag}: a 2 kHz pan moves nothing (${S.tunes.length - n1} tune(s))`);
      L.close();
    } finally { S.stop(); }
  };
  console.log('── 5. FREE tuning, the app\'s own messages (tune + its recentre zooms + edge pans, "free":1) ──');
  await freeCase('freezoom', { spanHz: 60000, from: DC0 - 40000, to: DC0 + 230000, step: 9000, back: DC0 - 40000 });
  await freeCase('freefull', { spanHz: 1e9, from: CENTRE, to: TONES_AT, step: STEP, back: CENTRE - 10000 });
  await freeCase('freeman', { spanHz: 60000, ifHz: 1500000, from: DC0 - 40000, to: DC0 + 230000, step: 9000, back: DC0 - 40000 });
  {
    console.log('── 6. LOCKED VFO (the app zooms onto the dial every tune), auto IF: the centre follows at the IF filter\'s edge ──');
    const S = await server('lockedvfo', ['--users', '1']);
    try {
      const L = await listener(S.port, 'vfodclkvfo01', CENTRE);
      const A = appClient(L, configs(L.spec).slice(-1)[0], { free: false, spanHz: 60000 });
      A.zoom(CENTRE);
      await sleep(700);
      const rf0 = S.dcNow();
      const r = await appSweep(S, A, CENTRE + 9000, CENTRE + 9000 * 40, 9000);
      ok(r.onDc.length === 0, `${r.steps.length} tunes of 9 kHz: the DC never inside ±${ZONE / 1e3} kHz of the dial`);
      // ★★ Stuart: "a 350KHz IF filter should in theory give 175KHz of breathing room before the RF centre has to follow".
      //    The view is 60 kHz, so it may reach ±(175 − 30) kHz from the tuner; beta24 moved the tuner on EVERY step
      //    (17 retunes for 16 tunes: a nudge and a follow).
      const firstEdge = r.steps.find((st, i) => st.moved && Math.abs(st.dc - (i ? r.steps[i - 1].dc : rf0)) > 50000);   // ★ a nudge moves ≤ 25 kHz
      const before = firstEdge ? r.steps[r.steps.indexOf(firstEdge) - 1] : null;
      const reach = firstEdge && before ? firstEdge.dial - before.dc : 0;
      ok(firstEdge && reach > 145000 && reach <= 145000 + 9000,
         `the centre stays put until the view nears the 350 kHz filter's edge: first follow with the dial ${kHz(reach)} kHz from the tuner (beyond 175 − 30 = 145 expected)`);
      ok(r.retunes <= 4 && r.retunes >= 2 && r.maxPerStep <= 1 && r.deferred === 0,
         `retunes ≪ tunes: ${r.retunes} for ${r.steps.length} (${r.edges} at the filter's edge, ${r.nudges} DC nudge(s)), never two on one step`);
      const ifs = [...new Set((S.log().match(/IF filter (?:widened|narrowed)?[^\n]*?(\d+) -> (\d+) kHz/g) || []))];
      ok(!/IF filter widened/.test(S.log()), `the filter is never widened by a tune (${ifs.length ? ifs.join('; ') : 'no width changes'})`);
      L.close();
    } finally { S.stop(); }
  }
  {
    console.log('── 7. the DC is told to the clients (hwinfo dcHz, and a `dc` message when the centre moves) ──');
    const S = await server('dctold', ['--users', '1']);
    try {
      const L = await listener(S.port, 'vfodcdctold1', CENTRE);
      const hw0 = L.spec.other.filter((m) => m.type === 'hwinfo' && m.dcHz).map((m) => m.dcHz);
      ok(hw0.length >= 1 && hw0[hw0.length - 1] === S.dcNow(), `hwinfo carries dcHz on connect (${hw0.length ? kHz(hw0[hw0.length - 1]) : '-'}; the tuner at ${kHz(S.dcNow())} kHz)`);
      const A = appClient(L, configs(L.spec).slice(-1)[0], { spanHz: 60000 });
      A.zoom(CENTRE);
      await sleep(500);
      L.spec.other.length = 0;
      const r = await appSweep(S, A, DC0 - 8000, DC0 + 4000, STEP);
      const dcs = L.spec.other.filter((m) => m.type === 'dc');
      const lastDc = dcs.length ? dcs[dcs.length - 1] : null;
      ok(r.retunes >= 1 && lastDc && lastDc.dcHz === S.dcNow() && lastDc.rfCentre === S.dcNow(),
         `after the nudge a \`dc\` message says where the spike is now: ${lastDc ? kHz(lastDc.dcHz) : '-'} kHz (the tuner at ${kHz(S.dcNow())}; ${dcs.length} message(s))`);
      L.close();
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
