// ★★ V12 DAB OWN STATION — end to end (docs/v12/DAB-OWN-STATION.md). Three listeners on one shared dial, one multiplex.
// Stuart's rule: the benchmark measures N stations at once; N >= the user limit → each listener may pick their own station
// (no slots, no asking, no eviction); N < the limit → one station for everyone, as before. The owner can force either.
//   1. ON (benchmark N 5, limit 3): A, B, C hear the radio's own station; B picks another → B hears it, A and C do not
//      change; C picks a third → three different programmes at once (their audio does not match); the reports say who is
//      on what (`mine`, `sid`) and `playing` counts each station's listeners; A joins B's station → A's audio IS B's
//      (one decode, one encode); B and C leave, A alone picks C's old service → the radio's own station follows A, and A
//      now sounds like what C heard (the station was C's service, not a copy of the primary).
//   2. OFF (benchmark N 2 < limit 3): a pick switches everyone, as before; own.on false.
// ★ Real server, the clean bench multiplex through fake-rtl-tcp (twelve services, each its own generated tones). A shared
//   dial always sends Opus: each listener's packets are put in an Ogg file in a temp dir and decoded by ffmpeg to PCM,
//   analysed in memory — nothing is played (tests are silent). Without VIBESERVER_BIN, the clip or ffmpeg: NOT RUN.
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DAB_CLIP || path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; VIBE_DAB_CLIP= or vibeserver/bench-clip/make-clip.sh)'); process.exit(3); }

if (spawnSync('ffmpeg', ['-version']).status !== 0) { console.log('   (not run — no ffmpeg to decode the Opus stream)'); process.exit(3); }

/* ── a minimal Ogg Opus writer (RFC 7845): OpusHead, OpusTags, then one 20 ms packet per page ── */
const CRC = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let r = i << 24; for (let k = 0; k < 8; k++) r = (r & 0x80000000) ? ((r << 1) ^ 0x04c11db7) >>> 0 : (r << 1) >>> 0; t[i] = r >>> 0; } return t; })();
function oggPage(data, type, granule, seq) {
  const segs = []; let n = data.length; while (n >= 255) { segs.push(255); n -= 255; } segs.push(n);
  const h = Buffer.alloc(27 + segs.length);
  h.write('OggS', 0); h[4] = 0; h[5] = type; h.writeBigInt64LE(BigInt(granule), 6); h.writeUInt32LE(0x56494245, 14); h.writeUInt32LE(seq, 18);
  h[26] = segs.length; segs.forEach((v, i) => { h[27 + i] = v; });
  const page = Buffer.concat([h, data]);
  let crc = 0; for (const b of page) crc = ((crc << 8) ^ CRC[((crc >>> 24) ^ b) & 0xff]) >>> 0;
  page.writeUInt32LE(crc >>> 0, 22);
  return page;
}
function decodeOpus(packets, ch) {
  if (!packets.length) return [];
  const head = Buffer.alloc(19); head.write('OpusHead', 0); head[8] = 1; head[9] = ch; head.writeUInt16LE(312, 10); head.writeUInt32LE(48000, 12);
  const tags = Buffer.concat([Buffer.from('OpusTags'), Buffer.from([4, 0, 0, 0]), Buffer.from('test'), Buffer.alloc(4)]);
  const pages = [oggPage(head, 2, 0, 0), oggPage(tags, 0, 0, 1)];
  let g = 0; packets.forEach((p, i) => { g += 960; pages.push(oggPage(p, i === packets.length - 1 ? 4 : 0, g, i + 2)); });
  const f = path.join(dir, `a${Math.random().toString(16).slice(2, 8)}.opus`);
  fs.writeFileSync(f, Buffer.concat(pages));
  const r = spawnSync('ffmpeg', ['-loglevel', 'error', '-i', f, '-f', 's16le', '-ac', '1', '-ar', '48000', '-'], { maxBuffer: 1 << 28 });
  fs.rmSync(f, { force: true });
  const out = []; const b = r.stdout || Buffer.alloc(0);
  for (let i = 0; i + 1 < b.length; i += 2) out.push(b.readInt16LE(i) / 32768);
  return out;
}

const hdr = fs.readFileSync(CLIP).subarray(0, 28);
const centre = hdr.readDoubleLE(16);
const BAND3 = { '5A': 174928000, '5B': 176640000, '5C': 178352000, '5D': 180064000, '12B': 225648000, '12C': 227360000, '12D': 229072000, '11D': 222064000, '10C': 213360000, '10D': 215072000 };
const home = Object.keys(BAND3).find((k) => Math.abs(BAND3[k] - centre) < 1000);
if (!home) { console.log(`   FAIL the clip's centre ${centre} is not a block this test knows`); process.exit(1); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabown-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const raw = path.join(dir, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
let srv = null;
const log = fs.openSync(path.join(dir, 'server.log'), 'a');
function writeConfig(own, n, rows = []) {
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DAB own', locator: 'IO92nh',
    adminPass: 'dab-own-admin', radios: [{ serial: 'dabown', label: 'T', enabled: true, configured: true, users: 3, dabOwnStation: own }] }));
  // ★ the benchmark's answer, as vibe_benchmark.h writes it (only the block the rule reads)
  fs.writeFileSync(path.join(dir, 'benchmark.json'), JSON.stringify({ v: 1, at: 1, rows, dabStations: { n, perStationPct: 9.5, frontPct: 60, memKB: 900 } }));
}
async function startServer() {
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
              { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); r(true); }); s.on('error', () => r(false)); })) return;
    await sleep(100);
  }
}
async function stopServer() {
  if (!srv) return;
  const done = new Promise((r) => srv.once('exit', r));
  srv.kill('SIGTERM');
  await Promise.race([done, sleep(8000)]);
  srv = null;
}
/** A listener: a spectrum socket (reports, control) and an audio socket (raw PCM), one session. */
function listener(tag) {
  const sess = `do${tag}${Math.random().toString(16).slice(2, 8)}`;
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sess}&bins=256&proto=1`);
  const au = new WebSocket(`ws://127.0.0.1:${port}/ws/audio?user_session_id=${sess}`);
  au.binaryType = 'arraybuffer';
  const c = { tag, ws, au, msgs: [], pkts: [], ch: 2, rec: false, opened: null };
  c.opened = Promise.all([new Promise((r) => { ws.onopen = r; }), new Promise((r) => { au.onopen = r; })]);
  ws.onmessage = (e) => { if (typeof e.data === 'string') { try { c.msgs.push(JSON.parse(e.data)); } catch {} } };
  au.onmessage = (e) => {
    if (!c.rec || !(e.data instanceof ArrayBuffer)) return;
    const b = new Uint8Array(e.data);
    if (b.length < 8 || b[1] !== 3) return;               // [ch][3 = one Opus packet][sr u32][packet]
    c.ch = b[0]; c.pkts.push(Buffer.from(b.subarray(6)));
  };
  // ★ proto 1: an application message at least every 30 s, or the server releases the session as dead
  c.beat = setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 8000);
  c.send = (o) => ws.send(JSON.stringify(o));
  c.dab = (from = 0) => c.msgs.slice(from).filter((m) => m.type === 'dab' && m.channel !== undefined);
  c.last = () => c.dab().at(-1);
  c.close = () => { clearInterval(c.beat); try { ws.close(); } catch {} try { au.close(); } catch {} };
  return c;
}
const until = async (f, ms = 20000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };
/** `secs` of each listener's audio, at once. */
async function record(cs, secs) {
  for (const c of cs) { c.pkts = []; c.rec = true; }
  await sleep(secs * 1000);
  for (const c of cs) c.rec = false;
  return cs.map((c) => fold(decodeOpus(c.pkts, c.ch)));
}
// ★ the same measures as test-dab-own.cpp: mono averaged to 4.8 kHz; the same samples (slid, best normalised correlation);
//   what it sounds like (the 120–900 Hz power spectrum, normalised)
function fold(x) { const m = []; for (let i = 0; i + 10 <= x.length; i += 10) { let s = 0; for (let k = 0; k < 10; k++) s += x[i + k]; m.push(s / 10); } return m; }
function same(a, ref) {
  const w = 4800; if (a.length < 3 * w || ref.length < 2 * w) return 0;
  const at = Math.floor(a.length / 2 - w / 2); let ea = 0; for (let i = 0; i < w; i++) ea += a[at + i] ** 2;
  let best = 0;
  for (let lag = 0; lag + w <= ref.length; lag += 1) {
    let x = 0, er = 0; for (let i = 0; i < w; i++) { x += a[at + i] * ref[lag + i]; er += ref[lag + i] ** 2; }
    if (ea > 0 && er > 0) best = Math.max(best, x / Math.sqrt(ea * er));
  }
  return best;
}
function spectrum(m) {
  const v = [];
  for (let f = 120; f < 900; f += 2) {
    const w = 2 * Math.PI * f / 4800, c = 2 * Math.cos(w); let s1 = 0, s2 = 0;
    for (const x of m) { const s0 = x + c * s1 - s2; s2 = s1; s1 = s0; }
    v.push(Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - c * s1 * s2)));
  }
  const mu = v.reduce((a, b) => a + b, 0) / v.length; let e = 0; for (let i = 0; i < v.length; i++) { v[i] -= mu; e += v[i] ** 2; }
  e = Math.sqrt(e); return v.map((x) => (e > 0 ? x / e : 0));
}
const corr = (a, b) => a.reduce((s, x, i) => s + x * b[i], 0);
const playingOf = (m, sid) => (m?.playing ?? []).find((p) => p.sid === sid)?.n ?? 0;

try {
  console.log('── 1. own station ON (benchmark N 5 >= limit 3) ──');
  writeConfig(-1, 5);
  await startServer();
  // ★ one at a time: three spectrum sockets in the same instant can meet the occupant check before the dial is shared
  const A = listener('a'); await A.opened; await sleep(600);
  const B = listener('b'); await B.opened; await sleep(600);
  const C = listener('c'); await C.opened; await sleep(600);
  ok(await until(() => [A, B, C].every((x) => x.msgs.length > 0 && x.ws.readyState === 1), 5000), 'three listeners connected (spectrum + audio each)');
  A.send({ type: 'dab', on: 1, channel: home });
  ok(await until(() => (A.last()?.sid ?? 0) > 0 && (A.last()?.services?.length ?? 0) >= 3, 30000), `${home} locks and plays a station`);
  const list = (A.last()?.services ?? []).map((s) => s.sid);
  const [s0] = [A.last().sid]; const s1 = list.find((s) => s !== s0); const s2 = list.find((s) => s !== s0 && s !== s1);
  ok(A.last()?.own?.on === true && A.last()?.own?.n === 5 && A.last()?.own?.limit === 3, `own: ${JSON.stringify(A.last()?.own)}`);
  ok(await until(() => playingOf(A.last(), s0) === 3, 5000), `playing: ${s0.toString(16)} × 3 (${JSON.stringify(A.last()?.playing)})`);
  await sleep(3000);
  let [a, b] = await record([A, B], 4);
  ok(same(a, b) > 0.95, `before any pick A and B hear the same programme (r ${same(a, b).toFixed(3)})`);

  B.send({ type: 'dab_service', sid: s1 });
  ok(await until(() => B.last()?.mine === s1 && B.last()?.sid === s1, 8000), `B picks ${s1.toString(16)}: B's report is that station`);
  ok(await until(() => playingOf(A.last(), s1) === 1 && playingOf(A.last(), s0) === 2, 5000), `playing: ${s0.toString(16)} × 2, ${s1.toString(16)} × 1`);
  ok(A.last()?.sid === s0 && C.last()?.sid === s0 && A.last()?.mine === s0, 'A and C still hear the radio\'s own station (their reports did not move)');
  C.send({ type: 'dab_service', sid: s2 });
  ok(await until(() => C.last()?.mine === s2, 8000), `C picks ${s2.toString(16)}: C's own`);
  ok(await until(() => (A.last()?.playing ?? []).length === 3, 5000), `three stations playing (${JSON.stringify(A.last()?.playing)})`);
  await sleep(4000);   // ★ the stations' pre-buffer and first super frames
  let c3;
  [a, b, c3] = await record([A, B, C], 6);
  const rAB = same(a, b), rAC = same(a, c3), rBC = same(b, c3);
  ok(a.length > 20000 && b.length > 20000 && c3.length > 20000, `audio on all three (${a.length}, ${b.length}, ${c3.length} samples at 4.8 kHz)`);
  ok(rAB < 0.6 && rAC < 0.6 && rBC < 0.6, `three different programmes (same-samples r: A/B ${rAB.toFixed(2)}, A/C ${rAC.toFixed(2)}, B/C ${rBC.toFixed(2)})`);
  const sB = spectrum(b), sC = spectrum(c3);

  A.send({ type: 'dab_service', sid: s1 });
  ok(await until(() => A.last()?.mine === s1 && playingOf(A.last(), s1) === 2, 8000), `A joins B's station (${JSON.stringify(A.last()?.playing)})`);
  await sleep(2000);
  [a, b] = await record([A, B], 5);
  ok(same(a, b) > 0.95, `A's audio IS B's — one decode, one encode (r ${same(a, b).toFixed(3)})`);

  B.close(); C.close();
  ok(await until(() => (A.last()?.playing ?? []).length === 1, 8000), `B and C gone: one station playing (${JSON.stringify(A.last()?.playing)})`);
  A.send({ type: 'dab_service', sid: s2 });
  ok(await until(() => A.last()?.sid === s2 && A.last()?.mine === s2, 8000), `A alone picks ${s2.toString(16)}: the radio's own station follows`);
  await sleep(4000);
  [a] = await record([A], 6);
  const sA = spectrum(a);
  ok(corr(sA, sC) > 0.8 && corr(sA, sC) > corr(sA, sB), `A now sounds like what C's station played (spectrum r ${corr(sA, sC).toFixed(2)}; vs B's ${corr(sA, sB).toFixed(2)})`);
  A.close();
  { const t = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
    ok(/several stations playing — this box keeps its waterfall rate/.test(t) && !/a weak box: the waterfall drops to 2 fps/.test(t),
       'a box the benchmark does not call weak keeps its waterfall rate with several stations playing'); }
  await stopServer();

  console.log('── 2. own station OFF (benchmark N 2 < limit 3) ──');
  writeConfig(-1, 2);
  await startServer();
  const D = listener('d'); await D.opened; await sleep(600);
  const E = listener('e'); await E.opened; await sleep(600);
  D.send({ type: 'dab', on: 1, channel: home });
  ok(await until(() => (D.last()?.sid ?? 0) > 0 && (D.last()?.services?.length ?? 0) >= 3, 30000), `${home} plays`);
  ok(D.last()?.own?.on === false && D.last()?.own?.n === 2, `own: ${JSON.stringify(D.last()?.own)}`);
  const t = (D.last().services ?? []).map((s) => s.sid).find((s) => s !== D.last().sid);
  E.send({ type: 'dab_service', sid: t });
  ok(await until(() => D.last()?.sid === t && E.last()?.sid === t, 8000), `E's pick (${t.toString(16)}) switches D too — one station for all, as before`);
  ok(await until(() => playingOf(D.last(), t) === 2, 5000), `playing: ${t.toString(16)} × 2`);
  D.close(); E.close();
} finally {
  await stopServer();

  console.log('── 3. a weak box (the benchmark\'s DAB row 66 % of a core): several stations → the waterfall at 2 fps ──');
  fs.appendFileSync(path.join(dir, 'server.log'), '\n=== WEAK ===\n');
  writeConfig(-1, 5, [{ id: 'dab', label: 'DAB+ @ 2.048 MS/s', rate: 2048000, pct: 66.2, grade: 'green' }]);
  await startServer();
  const weakLog = () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8').split('=== WEAK ===')[1] || '';
  const W1 = listener('w'); await W1.opened; await sleep(600);
  const W2 = listener('x'); await W2.opened; await sleep(600);
  W1.send({ type: 'dab', on: 1, channel: home });
  ok(await until(() => (W1.last()?.sid ?? 0) > 0 && (W1.last()?.services?.length ?? 0) >= 3, 30000), `${home} plays`);
  const w0 = W1.last().sid, w1 = W1.last().services.map((x) => x.sid).find((x) => x !== w0);
  await sleep(2000);
  ok(!/several stations playing/.test(weakLog()), 'one station for both listeners: no throttle');
  W2.send({ type: 'dab_service', sid: w1 });
  ok(await until(() => /several stations playing — a weak box: the waterfall drops to 2 fps/.test(weakLog()), 8000), '★ W2 picks another station: several stations playing');
  ok(await until(() => /engine FFT rate [\d.]+ -> 2\.0 fps/.test(weakLog()), 5000), '★ …and the engine drops to 2 fps');
  W2.send({ type: 'dab_service', sid: w0 });
  await sleep(6000);
  ok(!/one station again/.test(weakLog()), 'back on one station: the 2 fps holds for a while (no flip on every pick)');
  ok(await until(() => /one station again — the waterfall back at the listeners' rate/.test(weakLog()), 30000), '★ …then the waterfall comes back after the hold');
  ok(await until(() => /engine FFT rate 2\.0 -> (?!2\.0)[\d.]+ fps/.test(weakLog()), 5000), '…the engine back above 2 fps');
  W1.close(); W2.close();
  await stopServer();
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-server-dab-own-station: ${pass} passed, ${fail} failed${fail ? ` (log: ${path.join(dir, 'server.log')})` : ''}`);
if (!fail) fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
