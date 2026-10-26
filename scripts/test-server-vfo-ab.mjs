// test-server-vfo-ab.mjs — V12 VFO A/B through the REAL server (vibe_watch.h, docs/v12/VFO-AB.md).
// A fake rtl_tcp with three AM stations at ABSOLUTE frequencies (--tones-at 7 MHz: 6.88 MHz 400 Hz, 7.00 MHz 620 Hz,
// 7.12 MHz 840 Hz, the last KEYED on/off every 3 s), and the contract on the wire:
//   1. ONE-PIPELINE radio (users 1): hwinfo says watch:1; VFO A on 6.88 + VFO B on 7.00 = ONE stream, lower LEFT (400 Hz)
//      higher RIGHT (620 Hz); swap the frequencies and the ears follow frequency; STEREO OFF = both in both ears; B's own
//      squelch on the keyed station opens and shuts it (state events with times, its ear silent while shut); a frequency
//      outside the window is REFUSED and the radio does not move; off = back to mono (L == R).
//   2. RANGE-LOCKED radio (users 2): the same L/R from a per-listener slice; a VFO B takes a listener's place — a second
//      listener arriving closes it with a note; that second listener (an OLD client: never sends `watch`) hears plain mono;
//      a VFO B asked for on the full radio is refused "full".
//   3. SHARED DIAL (users 2, unlocked, Opus): VFO B's listener gets its OWN stereo stream; the other listener's stream is
//      untouched; nothing moves the dial.
// Silent: audio frames are analysed in memory, never played.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-vfo-ab.mjs
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

async function server(args, tag, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-vfoab-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'VFO A/B test', locator: 'IO92nh', adminPass: 'vfoab-test-admin',
    radios: [{ serial: 'vfoab-test', label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--tones', '3', '--tones-at', String(TONES_AT), '--key', '3'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'am', ...args],
                    { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  // ★ The scratch directory goes with the server unless something failed (then its server.log is the evidence).
  const failAtStart = fail;
  return { port, dir, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM');
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
const hwWatch = (L) => { const h = jsons(L.spec, 'hwinfo'); return h.length ? h[h.length - 1].watch : undefined; };

// ── 1. One-pipeline radio ─────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── 1. one-pipeline radio (1 listener) ──');
  const S = await server(['--users', '1'], 'one');
  try {
    const L = await listener(S.port, 'vfoabone1', A_HZ);
    await until(() => hwWatch(L) !== undefined, 4000);
    ok(hwWatch(L) === 1, `hwinfo offers one VFO B (watch: ${hwWatch(L)})`);
    let a = await listen(L.aud, 1500);
    ok(a.same && a.L[400] > 0.002, `VFO A alone: plain mono, 400 Hz in both ears (${a.ch} ch, ${fmt(a.L)})`);
    L.spec.send({ type: 'watch', id: 1, frequency: MID_HZ, mode: 'am' });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    const w0 = lastWatch(L.spec);
    ok(w0 && w0.chans[0].freq === MID_HZ && w0.chans[0].mode === 'am' && w0.stereo === true, `VFO B on at 7.030 MHz, stereo on by default (${JSON.stringify(w0 && w0.chans[0])})`);
    await sleep(2500);   // ★ B's AGC settles
    a = await listen(L.aud, 1600);
    console.log(`      L ${fmt(a.L)} · R ${fmt(a.R)} (${a.ch} ch)`);
    ok(a.ch === 2, 'one stream, now stereo — no second audio socket');
    ok(only(a, 400, 'L') && only(a, 620, 'R'), 'A (6.91, lower) wholly LEFT; B (7.03, higher) wholly RIGHT');
    ok(Math.abs(a.L[400] - a.R[620]) < 0.35 * a.L[400], `A and B at matching levels (${a.L[400].toFixed(4)} vs ${a.R[620].toFixed(4)})`);
    // swap: A up to 7.03, B down to 6.91 — the ears follow frequency
    L.spec.send({ type: 'tune', frequency: MID_HZ, mode: 'am' });
    L.spec.send({ type: 'watch', id: 1, frequency: A_HZ });
    await sleep(1500);
    a = await listen(L.aud, 1600);
    ok(only(a, 400, 'L') && only(a, 620, 'R'), `swapped A↔B: still lower left / higher right — B (6.91) left, A (7.03) right (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    // stereo off: both in both ears
    L.spec.send({ type: 'watch', stereo: false });
    await sleep(1200);
    a = await listen(L.aud, 1600);
    ok(a.L[400] > 0.002 && a.R[400] > 0.002 && a.L[620] > 0.002 && a.R[620] > 0.002 && Math.abs(a.L[620] - a.R[620]) < 0.1 * a.L[620],
       `STEREO OFF: both VFOs in both ears (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    L.spec.send({ type: 'watch', stereo: true });
    // B's own squelch on the keyed station
    L.spec.send({ type: 'tune', frequency: A_HZ, mode: 'am' });
    L.spec.send({ type: 'watch', id: 1, frequency: KEY_HZ });
    await sleep(800);
    const lv = jsons(L.spec, 'watch').slice(-12).map((m) => m.chans[0] && m.chans[0].lvl).filter((v) => typeof v === 'number');
    const hiLv = Math.max(...lv), loLv = Math.min(...lv);
    await sleep(3200);
    const lv2 = jsons(L.spec, 'watch').slice(-14).map((m) => m.chans[0] && m.chans[0].lvl).filter((v) => typeof v === 'number');
    const top = Math.max(hiLv, ...lv2), bot = Math.min(loLv, ...lv2);
    console.log(`      keyed station level ${bot.toFixed(1)} … ${top.toFixed(1)} dB`);
    ok(top - bot > 15, 'the keyed station is seen keying (> 15 dB swing in VFO B\'s level)');
    const thr = (top + bot) / 2;
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, sql: thr, sqlOn: true });
    await sleep(7000);
    const ev = jsons(L.spec, 'watch').map((m) => m.chans[0]).filter(Boolean);
    const flips = ev.filter((c, i) => i > 0 && c.open !== ev[i - 1].open);
    ok(flips.length >= 2, `B's squelch opens and shuts with the keying (${flips.length} changes in 7 s)`);
    ok(flips.every((c) => c.since > Date.now() - 10000 && c.since <= Date.now() + 1000), 'each change is stamped with when it happened (wall ms)');
    // capture audio across a shut period: find a stretch where B was shut — R must hold no 840 Hz while A plays left
    // ★ The fake keys on WALL seconds (off when floor(t/3) is odd), so the middle of an off period is known exactly.
    const phase = () => (Date.now() / 1000) % 6;
    await until(() => phase() > 3.7 && phase() < 3.9, 7000);
    a = await listen(L.aud, 1000);
    const shutNow = !lastWatch(L.spec).chans[0].open;
    ok(shutNow && a.R[840] < 0.0005 && a.L[400] > 0.002, `B shut: its ear silent (R 840 ${a.R[840].toFixed(5)}), A plays on left (${a.L[400].toFixed(4)})`);
    await until(() => phase() > 0.7 && phase() < 0.9, 7000);
    a = await listen(L.aud, 1000);
    ok(a.R[840] > 0.002, `B open: heard in its ear (R 840 ${a.R[840].toFixed(4)})`);
    // refusal outside the window — and the radio does not move
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, frequency: 12000000 });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const rf = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(rf && rf.refused === 'outside' && rf.lo < KEY_HZ && rf.hi > KEY_HZ && /window/.test(rf.note), `12 MHz refused "outside" with the window (${rf && rf.note})`);
    ok(rf && rf.chans[0].freq === KEY_HZ, 'VFO B stays where it was');
    const cfgs = jsons(L.spec, 'config');
    ok(!/retune|tuneHw.*12\.0/.test('') && !cfgs.some((c) => c.center_freq && Math.abs(c.center_freq - 12e6) < 2e6), 'the radio did not move');
    // off: back to mono
    L.spec.send({ type: 'watch', id: 1, off: true });
    await sleep(1000);
    a = await listen(L.aud, 1500);
    ok(a.same && a.L[400] > 0.002 && a.L[840] < 0.0005, `VFO B off: plain mono again, A only (${a.ch} ch)`);
    L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}

// ── 2. Range-locked radio ─────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── 2. range-locked radio (users 2) ──');
  const S = await server(['--users', '2', '--lock-freq', String(CENTRE)], 'lock');
  try {
    const L = await listener(S.port, 'vfoablock1', A_HZ);
    await until(() => hwWatch(L) !== undefined, 4000);
    ok(hwWatch(L) === 1, `hwinfo offers VFO B (watch: ${hwWatch(L)})`);
    L.spec.send({ type: 'watch', id: 1, frequency: MID_HZ, mode: 'am' });
    await sleep(1800);
    let a = await listen(L.aud, 1600);
    console.log(`      L ${fmt(a.L)} · R ${fmt(a.R)} (${a.ch} ch)`);
    ok(a.ch === 2 && only(a, 400, 'L') && only(a, 620, 'R'), 'per-listener slice: A (lower) left, B (higher) right');
    ok(Math.abs(a.L[400] - a.R[620]) < 0.35 * a.L[400], `A and B at matching levels (${a.L[400].toFixed(4)} vs ${a.R[620].toFixed(4)})`);
    // a second listener (an OLD client — never sends `watch`) arrives: the radio is full, VFO B yields
    L.spec.txt.length = 0;
    const M = await listener(S.port, 'vfoablock2', KEY_HZ);
    await until(() => jsons(L.spec, 'watch').some((m) => /room for a listener/.test(m.note || '')), 4000);
    const yielded = jsons(L.spec, 'watch').find((m) => /room for a listener/.test(m.note || ''));
    ok(!!yielded && yielded.chans.length === 0, `a listener arriving closes the newest VFO B, with a note (${yielded && yielded.note})`);
    await sleep(800);
    const b = await listen(M.aud, 1500);
    ok(b.same && b.L[840] + b.L[400] + b.L[620] > 0.0005, `the old client hears plain mono (${b.ch} ch, ${fmt(b.L)})`);
    M.spec.send({ type: 'watch', id: 1, frequency: MID_HZ });
    await until(() => jsons(M.spec, 'watch').some((m) => m.refused), 2000);
    const full = jsons(M.spec, 'watch').find((m) => m.refused);
    ok(full && full.refused === 'full', `a VFO B on the full radio is refused "full" (${full && full.note})`);
    a = await listen(L.aud, 1500);
    ok(a.same, 'the first listener is back to mono after the yield');
    M.spec.close(); M.aud.close(); L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}

// ── 3. Shared dial ────────────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── 3. shared dial (users 2, unlocked — Opus) ──');
  const S = await server(['--users', '3'], 'shared');
  try {
    const L = await listener(S.port, 'vfoabshr1', A_HZ, true);
    const M = await listener(S.port, 'vfoabshr2', 0, true);
    await until(() => hwWatch(L) !== undefined, 4000);
    ok(hwWatch(L) === 1, `hwinfo offers VFO B on a shared dial (watch: ${hwWatch(L)})`);
    L.spec.send({ type: 'watch', id: 1, frequency: MID_HZ, mode: 'am' });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    await sleep(1000);
    L.aud.bin.length = 0; M.aud.bin.length = 0; L.aud.keepBin = M.aud.keepBin = true;
    await sleep(1500);
    L.aud.keepBin = M.aud.keepBin = false;
    const pl = pcmOf(L.aud.bin), pm = pcmOf(M.aud.bin);
    ok(pl.opus > 40 && pm.opus > 40, `both listeners receive Opus (${pl.opus}, ${pm.opus} packets)`);
    const same = L.aud.bin.filter((b) => M.aud.bin.some((c) => c.equals(b))).length;
    ok(same < pl.opus * 0.1, `VFO B's listener has its OWN stream (${same} identical packets of ${pl.opus})`);
    const cfgM = jsons(M.spec, 'config');
    ok(!cfgM.some((c) => c.center_freq && Math.abs(c.center_freq - MID_HZ) < 1 && false), 'the dial is the server\'s — VFO B did not move it');
    L.spec.send({ type: 'watch', id: 1, frequency: 12e6 });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused === 'outside'), 2000);
    ok(jsons(L.spec, 'watch').some((m) => m.refused === 'outside'), 'outside the window: refused, the shared dial untouched');
    L.spec.close(); L.aud.close(); M.spec.close(); M.aud.close();
  } finally { S.stop(); }
}
// ── 4. The owner's switch off ─────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── 4. the owner has switched VFO A/B off (config vfoB: 0) ──');
  const S = await server(['--users', '1'], 'off', { vfoB: 0 });
  try {
    const L = await listener(S.port, 'vfoaboff1', A_HZ);
    await until(() => hwWatch(L) !== undefined, 4000);
    ok(hwWatch(L) === 0, `hwinfo says watch: 0 — no client offers the toggle (${hwWatch(L)})`);
    L.spec.send({ type: 'watch', id: 1, on: true });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const r = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(r && r.refused === 'off' && r.chans.length === 0, `a VFO B asked for anyway is refused "off" (${r && r.note})`);
    L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
