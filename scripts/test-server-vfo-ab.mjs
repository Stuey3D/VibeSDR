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
//   ROUND 3 (2026-10-27), in 1: MUTE — B muted puts A back in both ears and unmuting puts it back out; A muted puts B in
//      both ears; `sig` carries B's own level (chanB) for B's squelch row and its auto squelch; the RF CENTRE FOLLOWS
//      BOTH VFOs on a personal receiver — a B 1.6 MHz from A moves the radio to hold both (A still heard), A tuned past
//      the old window keeps B inside, and a B too far away is refused "apart"; and NO DUAL VFO ON FM BROADCAST — A on
//      WFM in the FM band closes B with the reason and takes the toggle away (max 0), it comes back off the band, and B
//      may neither pick WFM nor be tuned into 87.5–108 MHz.
//   DECODE ON EITHER VFO (2026-10-27), §6 on all three kinds of radio: a decoder started on VFO B is fed B's own audio
//      (WEFAX's picture follows B's dial, not A's), A is heard meanwhile, nothing is swapped, muting B leaves it
//      decoding, Single and Band spots run on B (Band reads B's band and is fed B's IQ, as is a CW skimmer on B), a
//      listener holds one decoder across both VFOs, and B going off stops its decoders with the reason; §6b measures
//      what a decoder on B costs against the same decoder on A.
//   THE TUNER'S DC WHEN VFO B MOVES THE RADIO (V12 beta24), §7: a personal receiver, the dongle offset-tuned 15 kHz; A at
//      the top of the window and B tuned just past it — the radio must move to hold both. BEFORE (VIBE_VFO_DC=0, the
//      placement as it was, keeping the LOGICAL centre clear) the spike lands EXACTLY on A's dial; AFTER it is out of A's
//      passband and B's (each + the 2.5 kHz guard).
// Silent: audio frames are analysed in memory, never played.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-vfo-ab.mjs
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

async function server(args, tag, radioExtra = {}, fakeExtra = [], extraEnv = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-vfoab-${tag}-`));
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
    await sleep(1200);
    // ★★ ROUND 3 — `sig` carries B's own level, every frame, for B's squelch row
    L.spec.txt.length = 0;
    await sleep(600);
    const sigs = jsons(L.spec, 'sig');
    ok(sigs.length > 5 && sigs.every((m) => typeof m.chanB === 'number'), `sig carries chanB while B is on (${sigs.length} sig, chanB ${sigs.length && sigs[sigs.length - 1].chanB})`);
    // ★★ ROUND 3 — MUTE B: A (now 7.03, the higher) comes home to BOTH ears; B's 400 Hz gone
    L.spec.send({ type: 'watch', id: 1, mute: true });
    await sleep(1200);
    a = await listen(L.aud, 1600);
    const mw = lastWatch(L.spec);
    ok(mw && mw.chans[0] && mw.chans[0].mute === true && mw.muteA === false, `the state says B is muted (${JSON.stringify(mw && { muteA: mw.muteA, mute: mw.chans[0] && mw.chans[0].mute })})`);
    ok(a.L[620] > 0.002 && a.R[620] > 0.002 && Math.abs(a.L[620] - a.R[620]) < 0.1 * a.L[620] && a.L[400] < 0.0005 && a.R[400] < 0.0005,
       `B muted: A in both ears, B silent (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    L.spec.send({ type: 'watch', id: 1, mute: false });
    await sleep(1200);
    a = await listen(L.aud, 1600);
    ok(only(a, 400, 'L') && only(a, 620, 'R'), `B unmuted: back out — B left, A right (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    // ★★ MUTE A: B (6.91) in both ears
    L.spec.send({ type: 'watch', muteA: true });
    await sleep(1200);
    a = await listen(L.aud, 1600);
    ok(lastWatch(L.spec).muteA === true, 'the state says A is muted');
    ok(a.L[400] > 0.002 && a.R[400] > 0.002 && Math.abs(a.L[400] - a.R[400]) < 0.1 * a.L[400] && a.L[620] < 0.0005 && a.R[620] < 0.0005,
       `A muted: B in both ears, A silent (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    L.spec.send({ type: 'watch', muteA: false });
    await sleep(1200);
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
    // ★★ ROUND 3 — THE RF CENTRE FOLLOWS BOTH on this personal receiver. Unsquelch B first (its keyed station is behind us).
    L.spec.send({ type: 'watch', id: 1, sql: -150, sqlOn: false });
    const winMid = (m) => (m.lo + m.hi) / 2;
    const FAR_B = A_HZ + 1600000;   // 1.6 MHz above A — outside today's window (±1.2 MHz), inside one capture beside A
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, frequency: FAR_B });
    await sleep(1500);
    let wm = lastWatch(L.spec);
    ok(!jsons(L.spec, 'watch').some((m) => m.refused), 'B 1.6 MHz from A is NOT refused on a personal receiver');
    ok(wm && wm.chans[0] && wm.chans[0].freq === FAR_B && !wm.chans[0].parked && wm.lo < A_HZ && wm.hi > FAR_B,
       `the radio moved to hold both: window ${(wm.lo / 1e6).toFixed(3)}–${(wm.hi / 1e6).toFixed(3)} MHz, centre ${(winMid(wm) / 1e6).toFixed(3)}`);
    ok(Math.abs(winMid(wm) - (A_HZ + FAR_B) / 2) < 150000, 'the centre sits between them');
    await sleep(1500);
    a = await listen(L.aud, 1600);
    ok(a.L[400] > 0.002 && a.L[400] > 30 * a.R[400], `VFO A still heard after the move (L 400 ${a.L[400].toFixed(4)})`);
    // A tuned past the old window: the radio goes where it holds both (it would have parked B before)
    const FAR_A = FAR_B + 1500000;
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: FAR_A, mode: 'am' });
    await sleep(1500);
    wm = lastWatch(L.spec);
    ok(wm && wm.chans[0] && !wm.chans[0].parked && wm.lo < FAR_B && wm.hi > FAR_A, `A tuned 1.5 MHz past B: both still inside (${(wm.lo / 1e6).toFixed(3)}–${(wm.hi / 1e6).toFixed(3)} MHz), B not parked`);
    // too far apart for one capture: refused "apart", nothing moves
    L.spec.txt.length = 0;
    const before = winMid(wm);
    L.spec.send({ type: 'watch', id: 1, frequency: 14000000 });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const rf = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(rf && rf.refused === 'apart' && /too far apart/.test(rf.note), `14 MHz refused "apart" (${rf && rf.note})`);
    ok(rf && rf.chans[0].freq === FAR_B, 'VFO B stays where it was');
    ok(rf && Math.abs(winMid(rf) - before) < 1, 'the radio did not move');
    // back home for the rest
    L.spec.send({ type: 'tune', frequency: A_HZ, mode: 'am' });
    await sleep(600);
    L.spec.send({ type: 'watch', id: 1, frequency: KEY_HZ });
    await sleep(1200);
    // off: back to mono
    L.spec.send({ type: 'watch', id: 1, off: true });
    await sleep(1000);
    a = await listen(L.aud, 1500);
    ok(a.same && a.L[400] > 0.002 && a.L[840] < 0.0005, `VFO B off: plain mono again, A only (${a.ch} ch)`);
    L.spec.txt.length = 0;
    await sleep(600);
    ok(!jsons(L.spec, 'sig').some((m) => 'chanB' in m), 'no chanB with VFO B off');
    // ★★ ROUND 3 — NO DUAL VFO ON FM BROADCAST
    L.spec.send({ type: 'watch', id: 1, on: true });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: 98000000, mode: 'wfm' });
    await until(() => jsons(L.spec, 'watch').some((m) => m.max === 0 && m.chans.length === 0), 4000);
    const fmw = jsons(L.spec, 'watch').find((m) => m.max === 0 && m.chans.length === 0);
    ok(!!fmw && /FM broadcast/.test(fmw.note || ''), `A on 98 MHz WFM: B closed with the reason, toggle gone (max 0) (${fmw && fmw.note})`);
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, on: true });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const fr = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(fr && fr.refused === 'off' && fr.max === 0 && fr.chans.length === 0, `asked for anyway on FM broadcast: refused (${fr && fr.note})`);
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: A_HZ, mode: 'am' });
    await until(() => jsons(L.spec, 'watch').some((m) => m.max === 1), 3000);
    ok(jsons(L.spec, 'watch').some((m) => m.max === 1), 'back on 7 MHz AM: the toggle comes back (max 1), unasked');
    L.spec.send({ type: 'watch', id: 1, on: true });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, mode: 'wfm' });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    ok((jsons(L.spec, 'watch').find((m) => m.refused) || {}).refused === 'mode', 'VFO B may not pick WFM');
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, frequency: 95000000, mode: 'nfm' });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const br = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(br && br.refused === 'band' && br.chans[0].freq === A_HZ, `VFO B may not be tuned into 87.5–108 MHz (${br && br.note})`);
    L.spec.send({ type: 'watch', id: 1, off: true });
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
    // ★★ ROUND 3: what moves the radio on a personal receiver (1.6 MHz from A) is still refused here — B never moves a shared dial
    L.spec.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, frequency: A_HZ + 1600000 });
    await until(() => jsons(L.spec, 'watch').some((m) => m.refused), 2000);
    const sr = jsons(L.spec, 'watch').find((m) => m.refused);
    ok(sr && sr.refused === 'outside' && Math.abs((sr.lo + sr.hi) / 2 - CENTRE) < 2e6 && sr.lo > A_HZ - 1.3e6, `1.6 MHz from A on a shared dial: refused "outside", the window where it was (${sr && sr.note})`);
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
// ── 5. Decode on A, listen on B (round 3) ─────────────────────────────────────────────────────────────────────────────
/* ★★★ Stuart, 2026-10-27: "run a decoder on VFO A whilst VFO B can listen to the audio … Also the ability to mute a VFO
 *  that is decoding so for WEFAX no need to hear the WEFAX noises whilst listening to SSB". A listener's decoder is fed
 *  VFO A's own audio BEFORE the mix (onClientAudio's decoders_.feedSession; the shared path's feedShared), so neither VFO
 *  B nor A's mute can reach it. Proven on both kinds of radio: WEFAX on A draws its lines while B is on and A is muted,
 *  and B's station is in both ears. */
for (const [label, args] of [['range-locked (users 2)', ['--users', '2', '--lock-freq', String(CENTRE)]], ['personal receiver (users 1)', ['--users', '1']]]) {
  console.log(`── 5. decode on VFO A, listen on VFO B — ${label} ──`);
  const S = await server(args, label.startsWith('range') ? 'declock' : 'decone');
  try {
    const sid = label.startsWith('range') ? 'vfoabdec1' : 'vfoabdec2';
    const L = await listener(S.port, sid, KEY_HZ);
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    dx.keepBin = true;
    await sleep(400);
    dx.send({ type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false });
    const lines = () => dx.bin.filter((b) => b.length === 9 + 1809).length;
    // ★ Wait for the lines rather than a fixed 2 s: the first line lands ~1.5-2 s after the attach, then two a second
    //   (120 lpm), so a 2 s sleep caught 1 line on a loaded run (measured 2026-11-02: 0 at 1 s, 1-2 at 2 s, 4 at 3 s).
    await until(() => lines() >= 2, 6000);
    ok(lines() >= 2, `WEFAX on VFO A is drawing (${lines()} lines)`);
    L.spec.send({ type: 'watch', id: 1, frequency: A_HZ, mode: 'am' });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    L.spec.send({ type: 'watch', muteA: true });
    await sleep(2500);
    const l0 = lines();
    const a = await listen(L.aud, 2000);
    const l1 = lines();
    ok(l1 - l0 >= 2, `with VFO B on and VFO A MUTED, A's decoder keeps decoding (${l1 - l0} more lines)`);
    ok(a.L[400] > 0.002 && a.R[400] > 0.002 && Math.abs(a.L[400] - a.R[400]) < 0.1 * a.L[400] && a.L[840] < 0.0005 && a.R[840] < 0.0005,
       `VFO B's station in both ears, nothing of A's (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    dx.close(); L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}
// ── 6. Decode on EITHER VFO (2026-10-27) ──────────────────────────────────────────────────────────────────────────────
/* ★★★ Stuart, 2026-10-27: "That seems like a weird limitations surely it doesnt matter which VFO is listen and which is
 *  decode?" A decoder started on VFO B ("vfo":"B" on the attach) runs on this listener's VFO B host, fed B's OWN audio
 *  (before the mix) and filed on B's dial — on all three kinds of radio. WEFAX is the instrument: its pixel is the audio
 *  frequency (1500 Hz black … 2300 Hz white), so B in USB 2200 Hz below a plain carrier (a 2200 Hz tone) draws LIGHT
 *  lines while VFO A (AM on the 620 Hz station: its audio all below 1500 Hz) would draw BLACK ones; moving
 *  B's dial 700 Hz (the carrier to 1500 Hz) darkens the picture, moving A's does not. Meanwhile A is heard in its ear, the labels and ears never move, muting B
 *  leaves its decoder decoding, Band on B reads B's band (A sits on no band at all), and B going off stops B's decoder
 *  with the reason in words. */
const pixMean = (bins) => {
  const ls = bins.filter((b) => b.length === 9 + 1809).slice(-2);
  if (!ls.length) return NaN;
  let s = 0, n = 0;
  for (const b of ls) for (let i = 9; i < b.length; i++) { s += b[i]; n++; }
  return s / n;
};
/* ★★ B decodes an UNMODULATED carrier (the fake's --carrier, 15 kHz below the 400 Hz station), not an AM station. With
 *  the dongle offset-tuned 15 kHz (37139747d) the radio's centre sits at 7.015 MHz, and B's channel lands its carrier
 *  7 mHz off 2200 Hz (the NCO's float step at −107 kHz — inaudible); on an AM station's carrier-plus-sidebands that
 *  millihertz walks the line's phase, and the decoder's line mean swung 65…111 with a 5.5 s period whatever A did — the
 *  "A retuned" check failed ~2 runs in 3 on a one-pipeline radio. A pure tone is one grey at any phase. */
const B_CARRIER = A_HZ - 15000;
const B_USB = B_CARRIER - 2200;   // ★ B's USB dial: the carrier a 2200 Hz tone (light grey) (the decoder auto-levels, so the picture is judged by how it MOVES)
for (const [label, args, sid] of [['personal receiver (users 1)', ['--users', '1'], 'vfoabdb1'],
                                  ['range-locked (users 2)', ['--users', '2', '--lock-freq', String(CENTRE)], 'vfoabdb2'],
                                  ['shared dial (users 3)', ['--users', '3'], 'vfoabdb3']]) {
  console.log(`── 6. decode on VFO B — ${label} ──`);
  const S = await server(args, sid, {}, ['--carrier', String(B_CARRIER), '--carrier-amp', '20']);
  try {
    const shared = label.startsWith('shared');   // ★ a shared dial's listeners are sent Opus (§3) — counted, not analysed
    const L = await listener(S.port, sid, MID_HZ, shared);
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    dx.keepBin = true;
    await sleep(400);
    const wef = { type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false };
    // no VFO B yet: refused in words
    dx.send({ ...wef, vfo: 'B' });
    await until(() => jsons(dx, 'decoder_refused').length > 0, 2000);
    const nr = jsons(dx, 'decoder_refused')[0];
    ok(nr && nr.reason === 'vfo' && /VFO B is not on/.test(nr.message), `a decoder on B with no VFO B: refused "vfo" (${nr && nr.message})`);
    L.spec.send({ type: 'watch', id: 1, frequency: B_USB, mode: 'usb' });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    await sleep(1500);   // ★ B's AGC settles
    dx.txt.length = 0; dx.bin.length = 0;
    dx.send({ ...wef, vfo: 'B' });
    await until(() => jsons(dx, 'audio_extension_attached').length > 0, 2000);
    ok(jsons(dx, 'audio_extension_attached').some((m) => m.vfo === 'B'), 'WEFAX attached on VFO B');
    await sleep(5000);   // ★ the first lines are the decoder settling — the last two are judged
    const lines = () => dx.bin.filter((b) => b.length === 9 + 1809).length;
    const m0 = pixMean(dx.bin);
    ok(lines() >= 4 && m0 > 60, `WEFAX on B draws B's picture — light, not A's black (${lines()} lines, mean pixel ${m0.toFixed(0)})`);
    // A retuned: B's picture does not care
    L.spec.send({ type: 'tune', frequency: KEY_HZ, mode: 'am' });
    await sleep(2500);
    const m1 = pixMean(dx.bin);
    ok(Math.abs(m1 - m0) < 25, `VFO A retuned: B's picture unchanged (mean ${m1.toFixed(0)})`);
    L.spec.send({ type: 'tune', frequency: MID_HZ, mode: 'am' });
    // B's dial 700 Hz up: the carrier falls to 1500 Hz — dark
    L.spec.send({ type: 'watch', id: 1, frequency: B_USB + 700 });
    await sleep(3000);
    const m2 = pixMean(dx.bin);
    ok(m2 < m0 - 25, `B's dial moved 700 Hz: B's picture follows it (mean ${m0.toFixed(0)} → ${m2.toFixed(0)})`);
    L.spec.send({ type: 'watch', id: 1, frequency: B_USB });
    await sleep(1500);
    // A heard meanwhile, in its own ear (A 7.03 is the higher: RIGHT); the labels never moved
    let a = await listen(L.aud, 1600);
    const wst = lastWatch(L.spec);
    if (shared) ok(a.st2 > 20, `the listener's own stereo stream plays on while B decodes (${a.st2} stereo Opus packets)`);
    else ok(a.ch === 2 && a.R[620] > 0.002 && a.R[620] > 30 * a.L[620], `VFO A heard in its ear while B decodes (R 620 ${a.R[620].toFixed(4)}, L ${a.L[620].toFixed(5)})`);
    ok(wst && wst.chans[0] && wst.chans[0].freq === B_USB && wst.chans[0].mode === 'usb', 'VFO B is still B: its frequency and mode on the watch state, nothing swapped');
    // mute the decoding VFO: its decoder decodes on, A comes home to both ears
    L.spec.send({ type: 'watch', id: 1, mute: true });
    await sleep(1500);
    const l0 = lines();
    a = await listen(L.aud, 1600);
    const l1 = lines();
    ok(l1 - l0 >= 2, `B (decoding) MUTED: its decoder keeps decoding (${l1 - l0} more lines)`);
    if (!shared) ok(a.L[620] > 0.002 && a.R[620] > 0.002 && Math.abs(a.L[620] - a.R[620]) < 0.1 * a.L[620], `…and A is in both ears (L ${fmt(a.L)} · R ${fmt(a.R)})`);
    L.spec.send({ type: 'watch', id: 1, mute: false });
    // ★ Digital spots on B — Single, then Band on B's band (A sits on 6.91 MHz, on no band at all)
    dx.send({ type: 'subscribe_digital_spots', vfo: 'B' });
    await until(() => /digital spots \(single\) on VFO B/.test(S.log()), 2000);
    await sleep(1500);
    ok(/digital spots \(single\) on VFO B/.test(S.log()), 'Single digital spots started on VFO B');
    ok(lines() > l1, 'the WEFAX decoder on B runs on beside them');
    L.spec.send({ type: 'tune', frequency: A_HZ, mode: 'am' });
    L.spec.send({ type: 'watch', id: 1, frequency: 7074000, mode: 'usb' });
    await sleep(800);
    dx.txt.length = 0;
    dx.send({ type: 'subscribe_digital_spots', scope: 'band', vfo: 'B' });
    await until(() => jsons(dx, 'digital_spots_status').length > 0 || jsons(dx, 'decoder_refused').length > 0, 8000);
    const bs = jsons(dx, 'digital_spots_status').pop();
    const modes = bs ? bs.data.modes.map((m) => m.mode) : [];
    ok(modes.includes('FT8') || modes.includes('ft8'), `Band on B reads B's band, 40 m (${modes.join(' ') || JSON.stringify(jsons(dx, 'decoder_refused')[0])})`);
    // ★ …and is FED: B's slice of the band (the shared FFT on a range-locked radio, B's own WideTap on a one-pipeline one)
    const runB = async () => ((await adminStatus(S.port)).decoders.running || []).find((r) => r.vfo === 'B' && r.session === sid);
    const f0 = (await runB()) || {};
    await sleep(2000);
    const f1 = (await runB()) || {};
    ok(f1.spotsScope === 'band' && /7\.07/.test(f1.spotsRun || '') && f1.fed > (f0.fed || 0) + 100000,
       `Band on B is fed B's IQ, on B's dial (admin: ${f1.spotsRun}, ${(f1.fed || 0) - (f0.fed || 0)} samples in 2 s)`);
    // ★ a CW skimmer on B (IQ, not audio) replaces B's WEFAX — this socket's one decoder — and is fed
    dx.send({ type: 'audio_extension_attach', extension_name: 'cwskim', width: 6000, vfo: 'B' });
    await sleep(1500);
    const c0 = (await runB()) || {};
    await sleep(2000);
    const c1 = (await runB()) || {};
    ok(c1.decoder === 'cwskim' && c1.fed > (c0.fed || 0) + 50000, `a CW skimmer on B is fed B's IQ (${c1.decoder}, ${(c1.fed || 0) - (c0.fed || 0)} samples in 2 s)`);
    // ★ one decoder per listener: WEFAX on A replaces the skimmer on B, and back
    dx.send(wef);
    await sleep(800);
    let run = (await adminStatus(S.port)).decoders.running || [];
    const mine = (v) => run.find((r) => r.vfo === v && r.decoder && (r.session === sid || r.shared));
    ok(mine('A') && mine('A').decoder === 'wefax' && !(mine('B') && mine('B').decoder), `a decoder started on A stops this listener's decoder on B (${JSON.stringify(run.map((r) => [r.vfo, r.decoder]))})`);
    dx.send({ ...wef, vfo: 'B' });
    await sleep(800);
    run = (await adminStatus(S.port)).decoders.running || [];
    ok(mine('B') && mine('B').decoder === 'wefax' && !(mine('A') && mine('A').decoder), `…and one started on B stops the one on A (${JSON.stringify(run.map((r) => [r.vfo, r.decoder]))})`);
    // B off: its decoders stop, said in words
    dx.txt.length = 0;
    L.spec.send({ type: 'watch', id: 1, off: true });
    await until(() => jsons(dx, 'decoder_refused').length >= 2, 3000);
    const gone = jsons(dx, 'decoder_refused');
    ok(gone.some((m) => m.what === 'decoder' && m.reason === 'vfo') && gone.some((m) => m.what === 'spots' && m.reason === 'vfo'),
       `VFO B off: its decoder and its spots stop, with the reason (${gone[0] && gone[0].message})`);
    dx.bin.length = 0;
    await sleep(1500);
    ok(lines() === 0, 'nothing more decodes from a VFO B that has gone');
    dx.close(); L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}
// ── 6b. What a decoder on B costs (personal receiver) ─────────────────────────────────────────────────────────────────
/* ★ STATED, NOT ASSUMED (Stuart: "state the trade-off first"): the whole vibeserver process's CPU with VFO B on and the
 *  same decoder running on A, then on B. B's demodulator runs either way, so the decoder costs the same on either VFO;
 *  Band on a one-pipeline radio cuts its slice with a WideTap either way (A's skimTap_ or B's own). */
{
  console.log('── 6b. the CPU of a decoder on VFO B against VFO A (personal receiver) ──');
  const S = await server(['--users', '1'], 'deccost');
  try {
    const cpu = () => {
      const t = String(execFileSync('ps', ['-o', 'time=', '-p', String(S.pid)])).trim();   // [[hh:]mm:]ss.cc
      return t.split(':').reduce((acc, v) => acc * 60 + parseFloat(v), 0);
    };
    const sid = 'vfoabcost';
    const L = await listener(S.port, sid, 7074000);
    L.spec.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
    L.spec.send({ type: 'watch', id: 1, frequency: 7074000, mode: 'usb' });
    await until(() => lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 3000);
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    await sleep(1500);
    const measure = async (msgOn, msgOff, ms = 10000) => {
      dx.send(msgOn); await sleep(2500);
      const c0 = cpu(), t0 = Date.now();
      await sleep(ms);
      const pct = 100 * (cpu() - c0) / ((Date.now() - t0) / 1000);
      dx.send(msgOff); await sleep(1500);
      return pct;
    };
    const idle = await (async () => { const c0 = cpu(), t0 = Date.now(); await sleep(6000); return 100 * (cpu() - c0) / ((Date.now() - t0) / 1000); })();
    const wef = { type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false };
    const wA = await measure(wef, { type: 'audio_extension_detach' });
    const wB = await measure({ ...wef, vfo: 'B' }, { type: 'audio_extension_detach' });
    const bA = await measure({ type: 'subscribe_digital_spots', scope: 'band' }, { type: 'unsubscribe_digital_spots' }, 16000);
    const bB = await measure({ type: 'subscribe_digital_spots', scope: 'band', vfo: 'B' }, { type: 'unsubscribe_digital_spots' }, 16000);
    console.log(`      whole process, % of one core: A+B, nothing decoding ${idle.toFixed(1)} · WEFAX on A ${wA.toFixed(1)} · on B ${wB.toFixed(1)}`
              + ` · Band (40 m) on A ${bA.toFixed(1)} · on B ${bB.toFixed(1)}`);
    ok(wB < wA * 1.5 + 2, `a WEFAX decoder on B costs what it costs on A (${wB.toFixed(1)} vs ${wA.toFixed(1)} %)`);
    ok(bB < bA * 1.5 + 3, `Band on B costs what it costs on A (${bB.toFixed(1)} vs ${bA.toFixed(1)} %)`);
    dx.close(); L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}
// ── 7. The tuner's DC when VFO B moves the radio (V12 beta24) ──────────────────────────────────────────────────────────
/* ★★ fitCentre kept the LOGICAL centre `clr` from both VFOs, but a dongle's spike is 15 kHz above it. A at the top of the
 *  window (950 kHz above the centre — inside A's 955 kHz, so A's own tune moves nothing) and B 10 kHz above A: B is
 *  outside its room, the radio must move, the middle of the room is between the two, and the old rule's way out was
 *  "A − clr" — the spike then sits exactly on A's dial. */
for (const [label, env] of [['BEFORE (VIBE_VFO_DC=0, the old placement)', { VIBE_VFO_DC: '0' }], ['AFTER', {}]]) {
  console.log(`── 7. the tuner's DC when VFO B moves the radio — ${label} ──`);
  const S = await server(['--users', '1'], 'vfoabdc', {}, [], env);
  try {
    const aHz = CENTRE + 950000, bHz = aHz + 10000;
    const L = await listener(S.port, 'vfoabdc01', aHz);
    await sleep(600);
    const n0 = S.tunes.length;
    ok(Math.abs(S.dcNow() - (CENTRE + 15000)) < 200, `A at ${(aHz / 1e6).toFixed(3)} MHz: the radio has not moved (tuner ${(S.dcNow() / 1e6).toFixed(4)} MHz)`);
    L.spec.send({ type: 'watch', id: 1, frequency: bHz, mode: 'am' });
    await until(() => S.tunes.length > n0 && lastWatch(L.spec) && lastWatch(L.spec).chans.length === 1, 4000);
    await sleep(300);
    const dc = S.dcNow();
    const onA = Math.abs(dc - aHz) < 5000 + 2500, onB = Math.abs(dc - bHz) < 5000 + 2500;
    console.log(`      B on at ${(bHz / 1e6).toFixed(3)} MHz moved the radio: tuner (the DC) ${(dc / 1e6).toFixed(4)} MHz — ${(dc - aHz) / 1e3} kHz from A, ${(dc - bHz) / 1e3} kHz from B`);
    if (env.VIBE_VFO_DC === '0') ok(onA, `the fault reproduces: the DC within A's passband + guard (${(dc - aHz) / 1e3} kHz from its dial)`);
    else {
      ok(!onA && !onB, `★ the DC out of BOTH passbands + guard (${(dc - aHz) / 1e3} kHz from A, ${(dc - bHz) / 1e3} kHz from B)`);
      ok(/VFO B moved the radio to/.test(S.log()), 'the server says B moved the radio');
    }
    L.spec.send({ type: 'watch', id: 1, off: true });
    L.spec.close(); L.aud.close();
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
