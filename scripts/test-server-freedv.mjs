// test-server-freedv.mjs — V12 FreeDV AS A MODE through the REAL server (vibe_freedv.h, docs/v12/FREEDV.md): a fake rtl_tcp
// replaying test-freedv's synthetic clip — three FreeDV stations made by codec2's own transmitter on a synthetic voice:
// 700D at 14.210, 700E at 14.220 and 1600 at 14.190 MHz, 12 dB in 3 kHz — and the contract:
//   • vibeserver.json offers "freedv" in its modes, with the sub-modes this libcodec2 runs (freedvSubs);
//   • a listener who tunes a station in mode "freedv" gets {"type":"freedv"} reports on their spectrum socket: searching,
//     then in sync with the sub-mode AUTO found named, its SNR and the station's text ("VIBE TEST");
//   • the audio socket carries the decoded speech once in sync (energy measured, never played) — and silence before;
//   • {"type":"freedv_sub"} with no sub answers with the report at once; a manual sub-mode that is not on air finds nothing,
//     AUTO again finds it; a new station gets a new search (AUTO names each of the three);
//   • on a ONE-PIPELINE radio (the shared receiver) and on a RANGE-LOCKED one (the listener's own channel);
//   • the sideband follows FreeDV's own SSB convention — USB on 20 m, LSB on 40 m (a second, mirrored clip) — and the
//     report says which ("sb");
//   • leaving FreeDV for SSB stops the reports and brings the SSB audio back;
//   • a server whose libcodec2 does not load (VIBE_FREEDV_DISABLE=1) does not offer the mode, and refuses it.
// Silent: audio frames are decoded to numbers, never played.
//   VIBESERVER_BIN=…/vibeserver VIBE_FREEDV_CLIP=…/fdv.cu8 node scripts/test-server-freedv.mjs
//   (make the clips: test-freedv clip fdv.cu8 288000 20 12; …clip fdv-lsb.cu8 288000 20 12 lsb → VIBE_FREEDV_CLIP_LSB)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_FREEDV_CLIP || '';
const CLIP_LSB = process.env.VIBE_FREEDV_CLIP_LSB || '';   // ★ the same stations on the lower sideband, for 40 m
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_FREEDV_CLIP (test-freedv clip …; exit 3 there = no libcodec2)');
  process.exit(3);
}
const RATE = 288000;
let CENTRE = 14200000, CLIPNOW = CLIP;
let STATIONS = [];
const stationsAt = (c) => [[c + 10000, '700D'], [c + 20000, '700E'], [c - 10000, '1600']];
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
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-fdv-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'FreeDV test', locator: 'IO92nh', adminPass: 'fdv-test-admin',
    radios: [{ serial: 'fdv-test', label: 'Test radio', enabled: true, configured: true }],
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

async function scenario(label, args, tag, sb = 'usb') {
  console.log(`── ${label} ──`);
  const S = await server(args, tag);
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && st.modes.includes('freedv'), `vibeserver.json offers freedv (${JSON.stringify(st && st.modes)})`);
    ok(st && Array.isArray(st.freedvSubs) && st.freedvSubs.includes('700D'), `…with its sub-modes (${JSON.stringify(st && st.freedvSubs)})`);
    const L = await listener(S.port, `fdv${tag}001`, STATIONS[0][0], 'freedv');
    L.aud.countOnly = false;
    const reps = () => jsons(L.spec, 'freedv');
    const last = () => reps().at(-1);
    for (const [hz, sub] of STATIONS) {
      if (!st.freedvSubs.includes(sub)) continue;
      const n0 = reps().length;
      L.spec.send({ type: 'tune', frequency: hz, mode: 'freedv' });
      const got = () => reps().slice(n0).some((r) => r.state === 'sync' && r.mode === sub);
      ok(await until(got, 15000), `${(hz / 1e6).toFixed(3)} MHz: AUTO in sync on ${sub} within 15 s (${JSON.stringify(last())})`);
      const r = reps().slice(n0).filter((x) => x.state === 'sync').at(-1);
      ok(r && r.sub === 'auto' && typeof r.snr === 'number' && r.snr > 3, `…the report: sub auto, SNR ${r && r.snr} dB`);
      ok(r && r.sb === sb, `…demodulated on the ${sb.toUpperCase()} side, by FreeDV's convention (${r && r.sb})`);
      // ★ 50 s: the text is shown once the same line has come twice (vibe_freedv.h); one line takes ~7 s, the clip's 20 s
      //   loop garbles one line each time round, and ~16 s after sync was measured on a clean feed (2026-11-16) —
      //   30 s failed ~1 run in 6 with three servers running at once
      ok(await until(() => reps().slice(n0).some((x) => x.text === 'VIBE TEST'), 50000), `…the station's text: ${JSON.stringify(reps().slice(n0).map((x) => x.text).filter(Boolean).at(-1))}`);
      L.aud.bin.length = 0;
      await sleep(3000);
      const pcm = await pcmOf(L.aud.bin);
      const syncNow = last() && last().state === 'sync';
      ok(pcm.length > 48000 * 2 && (rms(pcm) > 300 || !syncNow), `…decoded speech on the audio socket (${(pcm.length / 48000).toFixed(1)} s, rms ${rms(pcm).toFixed(0)})`);
    }
    // ★ the report at once on asking
    { const n0 = L.spec.txt.length; L.spec.send({ type: 'freedv_sub' });
      ok(await until(() => L.spec.txt.slice(n0).some((t) => t.includes('"type":"freedv"')), 3000), 'freedv_sub with no sub: the report comes back at once'); }
    // ★ manual: 1600 chosen while on the 700D station → nothing; AUTO again → 700D
    L.spec.send({ type: 'tune', frequency: STATIONS[0][0], mode: 'freedv' });
    await until(() => last() && last().state === 'sync' && last().mode === '700D', 15000);
    { const n0 = reps().length;
      L.spec.send({ type: 'freedv_sub', sub: '1600' });
      ok(await until(() => reps().slice(n0).some((r) => r.sub === '1600'), 5000), 'manual 1600: the report says sub 1600');
      await sleep(5000);
      ok(!reps().slice(n0).some((r) => r.sub === '1600' && r.state === 'sync'), `…and never syncs on a 700D station (${JSON.stringify(last())})`);
      L.aud.bin.length = 0; await sleep(2000);
      const pcm = await pcmOf(L.aud.bin);
      ok(pcm.length > 48000 && rms(pcm) < 5, `…silence on the audio socket meanwhile (rms ${rms(pcm).toFixed(1)}) — the stream keeps flowing (${(pcm.length / 48000).toFixed(1)} s)`);
      const n1 = reps().length;
      L.spec.send({ type: 'freedv_sub', sub: 'auto' });
      ok(await until(() => reps().slice(n1).some((r) => r.sub === 'auto' && r.state === 'sync' && r.mode === '700D'), 15000), 'back to AUTO: 700D again'); }
    // ★ leaving FreeDV: no more reports, the SSB audio back (noise is not silence)
    { L.spec.send({ type: 'tune', frequency: STATIONS[0][0], mode: sb });
      await sleep(1500);
      const n0 = reps().length;
      L.aud.bin.length = 0; await sleep(3000);
      const pcm = await pcmOf(L.aud.bin);
      ok(reps().length === n0, `${sb.toUpperCase()}: no FreeDV reports (${reps().length - n0})`);
      ok(rms(pcm) > 50, `${sb.toUpperCase()}: the demodulator's own audio again (rms ${rms(pcm).toFixed(0)})`); }
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S.stop(); }
}

STATIONS = stationsAt(CENTRE);
await scenario('one-pipeline radio (the shared receiver), 20 m, USB', ['--freq', String(STATIONS[0][0]), '--mode', 'usb', '--users', '1'], 'one');
await scenario('range-locked radio (the listener\'s own channel), 20 m, USB', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'lock');
if (CLIP_LSB && fs.existsSync(CLIP_LSB)) {
  // ★ 40 m: FreeDV's convention puts it on LSB below 10 MHz — the server picks the sideband from the dial
  CENTRE = 7200000; CLIPNOW = CLIP_LSB; STATIONS = stationsAt(CENTRE);
  await scenario('one-pipeline radio, 40 m, LSB', ['--freq', String(STATIONS[0][0]), '--mode', 'lsb', '--users', '1'], 'onel', 'lsb');
  await scenario('range-locked radio, 40 m, LSB', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'lsb', '--idle-grace', '0'], 'lockl', 'lsb');
  CENTRE = 14200000; CLIPNOW = CLIP; STATIONS = stationsAt(CENTRE);
} else console.log('   (40 m LSB scenarios not run — set VIBE_FREEDV_CLIP_LSB)');

{
  console.log('── no libcodec2 (VIBE_FREEDV_DISABLE=1) ──');
  const S = await server(['--freq', String(STATIONS[0][0]), '--mode', 'usb', '--users', '1'], 'none', { VIBE_FREEDV_DISABLE: '1' });
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && !st.modes.includes('freedv') && st.freedvSubs === undefined, `vibeserver.json does not offer freedv (${JSON.stringify(st && st.modes)})`);
    const L = await listener(S.port, 'fdvnone01', STATIONS[0][0], 'freedv');
    await sleep(3000);
    ok(jsons(L.spec, 'freedv').length === 0, 'a client that asks anyway gets no FreeDV receiver');
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
