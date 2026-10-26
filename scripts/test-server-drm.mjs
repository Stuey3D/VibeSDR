// test-server-drm.mjs — V12 DRM through the REAL server: a fake rtl_tcp replaying REAL DRM (Radio Romania International on
// 9570 kHz, recorded off Stuart's UberSDR 2026-10-26 18:00 UTC, 60 s) 30 kHz above the dongle's centre, and the contract of
// vibe_drm_listener.h:
//   • vibeserver.json offers "drm" in its modes;
//   • a listener who tunes 9570 kHz in mode "drm" gets {"type":"drm"} reports on their spectrum socket: locked, robustness
//     mode B, 10 kHz, 16-QAM, the station's label, its codec — and, for this AAC station, playable:false with the
//     sentence that says why (no system decoder plays DRM's error-resilient AAC);
//   • the audio socket keeps flowing (the programme or silence — never a stall);
//   • on a ONE-PIPELINE radio (the shared receiver) and on a RANGE-LOCKED one (the listener's own channel);
//   • leaving DRM for AM stops the reports.
// Silent: audio frames are counted, never played.
//   VIBESERVER_BIN=…/vibeserver VIBE_DRM_CLIP=…/rri9570_288k.cu8 node scripts/test-server-drm.mjs
//   (make the clip from the recording: see docs/v12/DRM.md, "The bench clip")
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DRM_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_DRM_CLIP');
  process.exit(3);
}
const RATE = 288000, CENTRE = 9540000, STATION = 9570000;
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

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-drm-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DRM test', locator: 'IO92nh', adminPass: 'drm-test-admin',
    radios: [{ serial: 'drm-test', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); if (!process.env.VIBE_KEEP) setTimeout(() => { try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} }, 500); else console.log('   kept', dir); } };
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

async function scenario(label, args, tag) {
  console.log(`── ${label} ──`);
  const S = await server(args, tag);
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && st.modes.includes('drm'), `vibeserver.json offers drm (${JSON.stringify(st && st.modes)})`);
    const L = await listener(S.port, `drm${tag}001`, STATION, 'drm');
    const last = () => { const r = jsons(L.spec, 'drm'); return r[r.length - 1]; };
    ok(await until(() => last() && last().locked && last().label, 45000), `locked with a label within 45 s (${JSON.stringify(last())})`);
    ok(await until(() => last() && last().msc && last().snr > 10, 30000), `the MSC decodes (the long interleaver: 2 s more) and the SNR settles (${last() && last().snr} dB)`);
    const r = last() || {};
    ok(r.robustness === 'B' && r.bwKhz === 10 && r.mscQam === 16, `mode B, 10 kHz, 16-QAM (${r.robustness} ${r.bwKhz} ${r.mscQam})`);
    ok(r.label === 'SNR Tiganesti E1', `the station's label: "${r.label}"`);
    ok(r.codec === 'AAC' && r.audioRate === 12000 && r.audioMode === 'mono' && r.bitrate > 10000, `the codec: ${r.codec} ${r.audioMode} ${r.audioRate} Hz, ${r.bitrate} bit/s`);
    // ★ A server built with FAAD2 (VIBE_DRM_FAAD2, the release builds) PLAYS DRM's AAC; one without says why it is silent.
    const faad = r.playable === true;
    if (process.env.VIBE_DRM_EXPECT_AUDIO === '1') ok(faad, `this build plays DRM AAC (decoder: ${r.decoder})`);
    if (!faad) ok(r.playable === false && /cannot be played/.test(r.why || ''), `an AAC station says why it is silent: "${r.why}"`);
    else {
      ok(/FAAD2/.test(r.decoder || ''), `the box names the decoder: ${r.decoder}`);
      // the programme itself, as PCM on this listener's audio socket (int16 frames: [ch][0][rate u32 LE][samples])
      L.aud.countOnly = false; L.aud.bin.length = 0;
      await sleep(8000);
      const pcm = [];
      for (const f of L.aud.bin) if (f[1] === 0) { const ch = f[0]; for (let i = 6; i + 1 < f.length; i += 2 * ch) pcm.push(f.readInt16LE(i)); }
      const N = 480, env = [];
      for (let i = 0; i + N <= pcm.length; i += N) { let e = 0; for (let k = 0; k < N; k++) e += pcm[i + k] * pcm[i + k]; env.push(10 * Math.log10(e / N + 1)); }
      const sorted = [...env].sort((a, b) => a - b), q = (p) => sorted[Math.floor(p * (sorted.length - 1))];
      let zc = 0; for (let i = 1; i < pcm.length; i++) if ((pcm[i] >= 0) !== (pcm[i - 1] >= 0)) zc++;
      const zcr = zc / Math.max(1, pcm.length) * 48000 / 2;
      ok(pcm.length > 48000 * 6, `programme PCM arrives (${(pcm.length / 48000).toFixed(1)} s in 8 s)`);
      ok(q(0.5) > 40, `it is not silence (median level ${q(0.5).toFixed(1)} dB re 1 LSB)${q(0.5) > 40 ? '' : ' — report: ' + JSON.stringify(last())}`);
      ok(q(0.9) - q(0.1) > 10, `it moves like programme, not noise (envelope p10→p90 ${(q(0.9) - q(0.1)).toFixed(1)} dB)`);
      ok(zcr < 3000, `its energy is low in the band, as AAC at 12 kHz is (zero-crossing rate ≈ ${zcr.toFixed(0)} Hz)`);
      // and as Opus to a client that asks for it
      const O = ws(S.port, `/ws/audio?user_session_id=drm${tag}001&codec=opus`);
      await sleep(4000);
      const op = O.bin.filter((f) => f[1] === 3).map((f) => f.length - 6);
      ok(op.length > 50 && Math.max(...op) > 60, `Opus out to a client: ${op.length} packets, up to ${Math.max(0, ...op)} bytes`);
      // ★ one audio socket per session: the Opus one has taken over — it carries on as this listener's audio
      L.aud.close(); O.countOnly = true; L.aud = O;
    }
    ok(r.snr > 10, `SNR ${r.snr} dB`);
    ok(r.sync && r.freq && r.frame && r.fac && r.sdc && r.msc, 'all six lights');
    // ★ VFO A/B: no VFO B in DRM (its audio is the decoded programme) — the toggle's allowance is 0 and a request is refused
    const lastWatch = () => { const w = jsons(L.spec, 'watch'); return w[w.length - 1]; };
    const hwWatch = () => { const h = jsons(L.spec, 'hwinfo'); return h.length ? h[h.length - 1].watch : undefined; };
    // ★ a listener who SWITCHED to DRM is told by a watch state (max 0); one who joined a dial already in DRM by hwinfo
    const told0 = () => (lastWatch() ? lastWatch().max === 0 : hwWatch() === 0);
    ok(await until(told0, 5000), `in DRM the server says VFO B is unavailable (watch max ${lastWatch() && lastWatch().max}, hwinfo watch ${hwWatch()})`);
    L.spec.send({ type: 'watch', id: 1, frequency: STATION + 5000, mode: 'am' });
    ok(await until(() => lastWatch() && lastWatch().refused && !(lastWatch().chans || []).length, 3000),
       `a VFO B request in DRM is refused ("${lastWatch() && lastWatch().note}")`);
    const a0 = L.aud.n || 0; await sleep(3000);
    ok((L.aud.n || 0) > a0 + 20, `audio keeps flowing (${(L.aud.n || 0) - a0} frames in 3 s)`);
    // back to AM: the reports stop
    L.spec.send({ type: 'tune', frequency: STATION, mode: 'am' });
    await sleep(1500);
    const n1 = jsons(L.spec, 'drm').length; await sleep(3000);
    ok(jsons(L.spec, 'drm').length === n1, 'AM again: no more DRM reports');
    ok(lastWatch() && lastWatch().max >= 1, `AM again: VFO B is offered again (watch max ${lastWatch() && lastWatch().max})`);
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { S.stop(); }
}

await scenario('one-pipeline radio (the shared receiver)', ['--freq', String(STATION), '--mode', 'drm', '--users', '1'], 'one');
await scenario('range-locked radio (the listener\'s own channel)', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'am', '--idle-grace', '0'], 'lock');

console.log(`\n${fail ? '✗' : '✓'} test-server-drm: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
