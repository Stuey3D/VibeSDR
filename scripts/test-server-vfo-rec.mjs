// test-server-vfo-rec.mjs — V12 PER-VFO RECORDING through the REAL server (WatchSet::recA / recB, watchSendRec).
// Stuart, 2026-11-07: "a 2nd record line in the audio menu for the B VFO, both can be triggered independently … the signal
// is recorded as it should be before we push it to one ear". While a listener records a VFO, the server sends that VFO's
// own audio as well — FORMAT 5: [0] ch · [1] 5 · [2..5] 48000 · [6] VFO (0 A, 1 B) · [7] 0 PCM / 3 Opus · payload.
//   On each kind of radio (one pipeline, range-locked per-listener, shared dial with Opus):
//   · nothing tagged before the listener asks (an older client never sees format 5); the state says recVfo:true
//   · recA → VFO A's own audio only (its tone, nothing of B's); recB → B's only; both at once → both, each its own
//   · the listener's mix is unchanged meanwhile (A left, B right); recA off stops A's frames, B's carry on
//   · a second listener (never asked) gets no format 5
// Silent: frames are analysed in memory, never played.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-vfo-rec.mjs
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

/** Format-5 frames → per VFO: PCM decoded to mono samples, Opus packets counted. */
function recOf(bins) {
  const r = { 0: { x: [], ch: 0, opus: 0, n: 0 }, 1: { x: [], ch: 0, opus: 0, n: 0 } };
  for (const b of bins) {
    if (b.length < 8 || b[1] !== 5) continue;
    const v = r[b[6]]; if (!v) continue;
    v.n++; v.ch = b[0];
    if (b[7] === 3) { v.opus++; continue; }
    const ch = b[0], n = (b.length - 8) / 2;
    for (let i = 0; i < n; i += ch) v.x.push(b.readInt16LE(8 + 2 * i) / 32768);
  }
  return r;
}
async function grab(aud, ms) { aud.bin.length = 0; aud.keepBin = true; await sleep(ms); aud.keepBin = false; return aud.bin.slice(); }
const tones = (x) => { const s = x.slice(-48000); return { 400: goertzel(s, 400), 620: goertzel(s, 620) }; };

for (const [label, args, opus] of [['one pipeline', ['--users', '1'], false],
                                   ['range-locked', ['--users', '2', '--lock-freq', String(CENTRE)], false],
                                   ['shared dial (Opus)', ['--users', '3'], true]]) {
  console.log(`── ${label} ──`);
  const tag = label.split(' ')[0].replace(/[^a-z]/g, '');
  const S = await server(args, 'rec' + tag);
  try {
    const L = await listener(S.port, 'vforec' + tag + '1', A_HZ, opus);
    L.spec.send({ type: 'watch', id: 1, frequency: MID_HZ, mode: 'am' });
    await until(() => lastWatch(L.spec) && (lastWatch(L.spec).chans || []).length === 1, 4000);
    ok(lastWatch(L.spec).recVfo === true, 'the state offers per-VFO recording (recVfo)');
    await sleep(2500);
    let r = recOf(await grab(L.aud, 1200));
    ok(r[0].n === 0 && r[1].n === 0, 'nothing tagged before the listener asks');
    L.spec.send({ type: 'watch', recA: true });
    await sleep(600);
    let bins = await grab(L.aud, 1600);
    r = recOf(bins);
    ok(r[0].n > 0 && r[1].n === 0, `recA: VFO A's own stream only (${r[0].n} A frames, ${r[1].n} B)`);
    if (!opus) { const t = tones(r[0].x); ok(t[400] > 0.002 && t[400] > 30 * t[620], `…carrying A's 400 Hz and none of B's 620 Hz (${t[400].toFixed(4)} / ${t[620].toFixed(4)}, ${r[0].ch} ch)`); }
    else ok(r[0].opus === r[0].n, `…as Opus (${r[0].opus} packets, ${r[0].ch} ch)`);
    const mix = pcmOf(bins);
    if (!opus) {
      const n = Math.min(mix.L.length, 48000), Lx = mix.L.slice(-n), Rx = mix.R.slice(-n);
      ok(mix.ch === 2 && goertzel(Lx, 400) > 30 * goertzel(Rx, 400) && goertzel(Rx, 620) > 30 * goertzel(Lx, 620), 'the listener\'s mix is unchanged meanwhile: A left, B right');
    } else ok(mix.opus > 0, `the listener's own stream carries on (${mix.opus} Opus packets)`);
    L.spec.send({ type: 'watch', recB: true });
    await sleep(600);
    r = recOf(await grab(L.aud, 1600));
    ok(r[0].n > 0 && r[1].n > 0, `recA + recB: both, each its own (${r[0].n} A, ${r[1].n} B)`);
    if (!opus) {
      const ta = tones(r[0].x), tb = tones(r[1].x);
      ok(tb[620] > 0.002 && tb[620] > 30 * tb[400] && r[1].ch === 1, `B's stream: 620 Hz only, mono (${tb[620].toFixed(4)} / ${tb[400].toFixed(4)}, ${r[1].ch} ch)`);
      ok(ta[400] > 30 * ta[620], 'A\'s stream still A alone');
      ok(Math.abs(r[0].x.length - r[1].x.length) < 4800, `A and B run at one clock (${r[0].x.length} vs ${r[1].x.length} samples)`);
    }
    const w = lastWatch(L.spec);
    ok(w.recA === true && w.recB === true, 'the state says both are recording');
    L.spec.send({ type: 'watch', recA: false });
    await sleep(600);
    r = recOf(await grab(L.aud, 1200));
    ok(r[0].n === 0 && r[1].n > 0, `recA off: A's frames stop, B's carry on (${r[0].n} A, ${r[1].n} B)`);
    // a second listener that never asked (users > 1 — on a range-locked radio it also takes B's place, closing it)
    let L2 = null;
    if (args[1] !== '1') {
      L2 = await listener(S.port, 'vforec' + tag + '2', opus ? 0 : A_HZ, opus);
      const r2 = recOf(await grab(L2.aud, 1200));
      ok(r2[0].n === 0 && r2[1].n === 0, 'a second listener who never asked gets nothing tagged');
    }
    L.spec.send({ type: 'watch', id: 1, off: true });
    await sleep(1500);
    r = recOf(await grab(L.aud, 1000));
    ok(r[0].n === 0 && r[1].n === 0, 'VFO B off: nothing tagged');
    for (const x of [L, L2]) if (x) { x.spec.close(); x.aud.close(); }
  } catch (e) { console.error(e); fail++; }
  S.stop();
  await sleep(500);
}
console.log(`\n${fail ? 'FAILED' : 'all passed'} — ${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
