// test-server-m17.mjs — V12 M17 AS A MODE through the REAL server (vibe_m17.h, docs/v12/M17.md): a fake rtl_tcp replaying
// test-m17's synthetic clip — our own M17 transmitter (from the spec) on 433.425 MHz, its speech encoded by the system's
// libcodec2: a voice over from G4ABC to @ALL with META text "VIBE TEST", then an SMS from G4ABC to M0XYZ, on a 10 s loop,
// 20 dB C/N in 9 kHz — and the contract:
//   • vibeserver.json offers "m17" in its modes (an RTL reaches 70 cm, and libcodec2 loaded);
//   • a listener who tunes the station in mode "m17" gets {"type":"m17"} reports on their spectrum socket: in sync with the
//     callsigns, the kind (voice) and the META text, then the SMS (its text and both callsigns), then searching with the
//     last station named;
//   • the audio socket carries Codec 2 speech while the over lasts (energy measured, never played), silence between overs;
//   • {"type":"m17_status"} answers with the report at once;
//   • on a ONE-PIPELINE radio (the shared receiver) and on a RANGE-LOCKED one (the listener's own channel) — and there a
//     second listener in NFM next to it gets no M17 reports and keeps its NFM audio;
//   • leaving M17 for NFM stops the reports and brings the FM audio back;
//   • a server whose libcodec2 does not load (VIBE_CODEC2_DISABLE=1) does not offer the mode, and gives a client that asks
//     anyway no receiver.
// Silent: audio frames are decoded to numbers, never played. Every socket sends the client's {type:'ping'} keepalive.
//   VIBESERVER_BIN=…/vibeserver VIBE_M17_CLIP=…/m17.cu8 node scripts/test-server-m17.mjs   (test-m17 clip m17.cu8 288000 10 20)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_M17_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_M17_CLIP (test-m17 clip …)');
  process.exit(3);
}
const RATE = 288000;
const CENTRE = 433400000, STATION = CENTRE + 25000, CLIPNOW = CLIP;   // ★ 70 cm: the clip's station is 25 kHz above its centre
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

/** A minimal WebSocket client (the probes' own), recording text and binary frames. */
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
      if (op === 0x2) { if (st.countOnly) st.n = (st.n || 0) + 1; else st.bin.push(Buffer.from(p)); }
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

/** Audio frames → int16-scaled mono samples at 48 kHz (the left channel). [0] channels, [1] codec (0 PCM, 3 Opus — a SHARED
 *  DIAL always sends Opus), [2..5] rate. Opus is decoded with the web client's own decoder package (opus-decoder), so a shared
 *  dial's programme is judged by the same checks as a one-pipeline radio's raw PCM. */
async function pcmOf(frames) {
  const out = [];
  let opusDec = null;   // ★ one per stream: an Opus decoder carries state from packet to packet
  for (const f of frames) {
    if (f.length < 7) continue;
    if (f[1] === 0) { const ch = f[0] || 1; for (let i = 6; i + 1 < f.length; i += 2 * ch) out.push(f.readInt16LE(i)); continue; }
    if (f[1] !== 3) continue;
    if (!opusDec) { const { OpusDecoder } = await import('opus-decoder'); opusDec = new OpusDecoder({ channels: 2 }); await opusDec.ready; }
    let r; try { r = opusDec.decodeFrame(new Uint8Array(f.subarray(6))); } catch { continue; }
    if (r && r.samplesDecoded) for (let i = 0; i < r.samplesDecoded; i++) out.push(Math.round(r.channelData[0][i] * 32767));
  }
  if (opusDec) try { opusDec.free(); } catch {}
  return out;
}

/** ★ "Port already in use": freePort() hands out a port that something else on this machine can take before the server
 *  binds it (other benches run beside this one). A server that says so is started again on fresh ports — a harness race,
 *  not a result. */
async function server(args, tag, extraEnv) {
  for (let attempt = 0; ; attempt++) {
    const S = await serverOnce(args, tag, extraEnv);
    if (S.ok || attempt >= 2 || !S.portTaken()) return S;
    console.log('   (the port was taken before the server bound it — starting again on fresh ports)');
    await S.stop();
  }
}
async function serverOnce(args, tag, extraEnv) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-m17-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'M17 test', locator: 'IO92nh', adminPass: 'm17-test-admin',
    radios: [{ serial: 'm17-test', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIPNOW, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, ...(extraEnv || {}), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const up = await until(async () => !!(await status()), 15000);
  const portTaken = () => { try { return /already in use/.test(fs.readFileSync(path.join(dir, 'server.log'), 'utf8')); } catch { return false; } };
  return { ok: up, portTaken, port, dir, status, stop: async () => {
    // ★ AWAITED: a timer here outlived the last scenario — the process exited first and left 16 MB behind per run
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    if (process.env.VIBE_KEEP) { console.log('   kept', dir); return; }
    await sleep(500);
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
  } };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  L.aud.countOnly = true;
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  return L;
}

const rms = (pcm) => { let e = 0; for (const v of pcm) e += v * v; return Math.sqrt(e / Math.max(1, pcm.length)); };


/** ★ the client's keepalive — the server drops a silent session at 30 s */
function keepAlive(L) { const t = setInterval(() => { try { L.spec.send({ type: 'ping' }); L.aud.send({ type: 'ping' }); } catch {} }, 5000); return () => clearInterval(t); }

async function scenario(label, args, tag) {
  console.log(`── ${label} ──`);
  const S = await server(args, tag);
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && st.modes.includes('m17'), `vibeserver.json offers m17 (${JSON.stringify(st && st.modes)})`);
    const L = await listener(S.port, `m17${tag}001`, STATION, 'm17');
    const stopKa = keepAlive(L);
    L.aud.countOnly = false;
    const reps = () => jsons(L.spec, 'm17');
    const last = () => reps().at(-1);
    ok(await until(() => reps().length > 0, 5000), `reports come on the spectrum socket (${JSON.stringify(last())})`);
    // ★ in sync on the voice over: the callsigns, the kind, the META text (the loop is 10 s: allow two)
    const sync = () => reps().find((r) => r.state === 'sync' && r.kind === 'voice' && r.src === 'G4ABC' && r.dst === '@ALL');
    ok(await until(sync, 25000), `in sync on the over: G4ABC → @ALL, voice (${JSON.stringify(sync() || last())})`);
    ok(await until(() => reps().some((r) => r.text === 'VIBE TEST'), 25000), `…the META text "VIBE TEST" (${JSON.stringify(reps().map((r) => r.text).filter(Boolean).at(-1))})`);
    const r0 = sync();
    ok(r0 && typeof r0.ber === 'number' && r0.ber < 5 && typeof r0.off === 'number' && Math.abs(r0.off) <= 500, `…its BER ${r0 && r0.ber} % and tuning offset ${r0 && r0.off} Hz`);
    // ★ speech while in sync: collect the audio of one whole over
    L.aud.bin.length = 0;
    const n0 = reps().length;
    await until(() => reps().slice(n0).some((r) => r.state === 'sync' && r.kind === 'voice'), 15000);
    L.aud.bin.length = 0;   // ★ from the start of an over to its end
    await until(() => reps().slice(n0).some((r) => r.state === 'search' || r.kind === 'packet'), 8000);
    const pcm = await pcmOf(L.aud.bin);
    ok(pcm.length > 48000 && rms(pcm) > 300, `…Codec 2 speech on the audio socket during the over (${(pcm.length / 48000).toFixed(1)} s, rms ${rms(pcm).toFixed(0)})`);
    // ★ the SMS
    const sms = () => reps().find((r) => r.msg && r.msg.text === 'Hello from VibeServer');
    ok(await until(sms, 25000), `the SMS: ${JSON.stringify((sms() || last() || {}).msg)}`);
    ok(sms() && sms().msg.src === 'G4ABC' && sms().msg.dst === 'M0XYZ' && sms().msg.ptype === 5, '…from G4ABC to M0XYZ, an SMS (protocol 5)');
    ok(await until(() => reps().some((r) => r.state === 'search' && r.last === 'G4ABC'), 15000), `between overs: searching, the last station named (${JSON.stringify(reps().filter((r) => r.state === 'search').at(-1))})`);
    // ★ silence between overs: wait for the quiet part (after the SMS), measure 1.5 s of it
    { const n1 = reps().length;
      await until(() => reps().slice(n1).some((r) => r.msg && r.state === 'search'), 15000);
      await sleep(1500);
      L.aud.bin.length = 0; await sleep(1500);
      const q = await pcmOf(L.aud.bin);
      const quiet = !reps().slice(-1).some((r) => r.state === 'sync');
      ok(q.length > 48000 && (rms(q) < 5 || !quiet), `silence between overs (rms ${rms(q).toFixed(1)}) — the stream keeps flowing (${(q.length / 48000).toFixed(1)} s)`); }
    // ★ the report at once on asking
    { const n1 = L.spec.txt.length; L.spec.send({ type: 'm17_status' });
      ok(await until(() => L.spec.txt.slice(n1).some((t) => t.includes('"type":"m17"')), 3000), 'm17_status: the report comes back at once'); }
    // ★ range-locked: a second listener in NFM beside it — no M17 for them, their own FM audio
    if (tag === 'lock') {
      const B = await listener(S.port, `m17${tag}002`, STATION + 50000, 'nfm');
      const stopB = keepAlive(B);
      B.aud.countOnly = false;
      await sleep(4000);
      const pb = await pcmOf(B.aud.bin);
      ok(jsons(B.spec, 'm17').length === 0 && rms(pb) > 50, `a second listener in NFM: no M17 reports (${jsons(B.spec, 'm17').length}), its own FM audio (rms ${rms(pb).toFixed(0)})`);
      stopB(); for (const k of ['spec', 'aud']) B[k].close();
    }
    // ★ leaving M17: no more reports, the NFM audio back (noise is not silence)
    { L.spec.send({ type: 'tune', frequency: STATION + 100000, mode: 'nfm' });
      await sleep(1500);
      const n1 = reps().length;
      L.aud.bin.length = 0; await sleep(3000);
      const p2 = await pcmOf(L.aud.bin);
      ok(reps().length === n1, `NFM: no M17 reports (${reps().length - n1})`);
      ok(rms(p2) > 50, `NFM: the demodulator's own audio again (rms ${rms(p2).toFixed(0)})`); }
    stopKa();
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S.stop(); }
}

await scenario('one-pipeline radio (the shared receiver), 70 cm', ['--freq', String(STATION), '--mode', 'nfm', '--users', '1'], 'one');
await scenario('range-locked radio (the listener\'s own channel), 70 cm', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'nfm', '--idle-grace', '0'], 'lock');

{
  console.log('── no libcodec2 (VIBE_CODEC2_DISABLE=1) ──');
  const S = await server(['--freq', String(STATION), '--mode', 'nfm', '--users', '1'], 'none', { VIBE_CODEC2_DISABLE: '1' });
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && !st.modes.includes('m17') && !st.modes.includes('freedv'), `vibeserver.json offers neither m17 nor freedv (${JSON.stringify(st && st.modes)})`);
    const L = await listener(S.port, 'm17none01', STATION, 'm17');
    await sleep(3000);
    L.spec.send({ type: 'm17_status' });
    await sleep(1000);
    ok(jsons(L.spec, 'm17').length === 0, 'a client that asks anyway gets no M17 receiver');
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
