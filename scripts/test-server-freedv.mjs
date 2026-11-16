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
// ★★★ RADE V1 (2026-11-16 — what nearly every FreeDV station sends today, compiled in from rade_c): where the clip writer had
//   rade_c's transmitter, a fourth station at +30 kHz carries a RADE over (14 s of the voice, then its end-of-over frame
//   with "G4ABC", then noise to the loop). AUTO names RADE, its speech comes out, and once the over ends the report says
//   "last":"G4ABC" — never as the talker's text. Then: a server WITHOUT libcodec2 (VIBE_CODEC2_DISABLE=1) still offers
//   FreeDV, with RADE alone, and decodes it; the owner's "rade" switch off takes RADE out of freedvSubs and AUTO.
//   Every listener here pings (the server drops a session that has said nothing for 30 s, and RADE's checks take longer).
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
const stationsAt = (c) => [[c + 10000, '700D'], [c + 20000, '700E'], [c - 10000, '1600'], [c + 30000, 'RADE']];
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
async function server(args, tag, extraEnv, radioExtra) {
  for (let attempt = 0; ; attempt++) {
    const S = await serverOnce(args, tag, extraEnv, radioExtra);
    if (S.ok || attempt >= 2 || !S.portTaken()) return S;
    console.log('   (the port was taken before the server bound it — starting again on fresh ports)');
    await S.stop();
  }
}
async function serverOnce(args, tag, extraEnv, radioExtra) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-fdv-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'FreeDV test', locator: 'IO92nh', adminPass: 'fdv-test-admin',
    radios: [{ serial: 'fdv-test', label: 'Test radio', enabled: true, configured: true, ...(radioExtra || {}) }],
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
  // ★ keepalive: the server drops a session that has said nothing for 30 s — RADE's end-of-over check alone waits longer
  L.ping = setInterval(() => { try { L.spec.send({ type: 'ping' }); } catch {} }, 5000);
  const close0 = L.spec.close;
  L.spec.close = () => { clearInterval(L.ping); close0(); };
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
    if (HAVE_RADE) ok(st.freedvSubs[0] === 'RADE', `…RADE first (${JSON.stringify(st.freedvSubs)})`);
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
      if (sub === 'RADE') {
        // ★ speech now, while the over is on (it ends 14 s into each 20 s loop)
        L.aud.bin.length = 0;
        await sleep(2000);
        const pcm = await pcmOf(L.aud.bin);
        ok(pcm.length > 48000 && rms(pcm) > 300, `…decoded speech on the audio socket (${(pcm.length / 48000).toFixed(1)} s, rms ${rms(pcm).toFixed(0)})`);
        ok(!reps().slice(n0).some((x) => x.state === 'sync' && x.text), '…no "text" while in sync (the callsign is the last over\'s, never the talker\'s)');
        // ★ the end-of-over frame: once the over ends (≤ 20 s round the loop) the report keeps the callsign as "last"
        ok(await until(() => reps().slice(n0).some((x) => x.last === 'G4ABC'), 45000), `…the end of the over: "last" ${JSON.stringify(reps().slice(n0).map((x) => x.last).filter(Boolean).at(-1))}`);
        continue;
      }
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

// ★ RADE is in this server's build and in the clip when the server lists it (the clip writer had rade_c's transmitter too)
let HAVE_RADE = false;
{ const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'probe');
  try { const st = await S.status(); HAVE_RADE = !!(st && Array.isArray(st.freedvSubs) && st.freedvSubs.includes('RADE')); } finally { await S.stop(); } }
console.log(`   RADE V1 ${HAVE_RADE ? 'in this server' : 'NOT in this server (its checks are not run)'}`);

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

/* ★★ THE SIDEBAND OVERRIDE (Stuart 2026-11-16): the 20 m (USB) clip replayed at 7.2 MHz — a USB station below 10 MHz, which
 *  the convention's AUTO demodulates on LSB and so never syncs. {"type":"freedv_sub","sb":"usb"} puts the receiver on USB:
 *  it syncs, and the report says what was chosen ("sbSel") and what is demodulated ("sb"). "auto" goes back to LSB. */
async function overrideScenario(label, args, tag) {
  console.log(`── ${label} ──`);
  const S = await server(args, tag);
  try {
    const st = await S.status();
    const L = await listener(S.port, `fdv${tag}001`, STATIONS[0][0], 'freedv');
    const ping = setInterval(() => L.spec.send({ type: 'ping' }), 2000);   // ★ keepalive
    try {
      const reps = () => jsons(L.spec, 'freedv');
      const last = () => reps().at(-1);
      if (!st.freedvSubs.includes('700D')) { console.log('   (no 700D in this libcodec2 — not run)'); return; }
      L.spec.send({ type: 'freedv_sub' });
      ok(await until(() => last() && last().sb === 'lsb' && last().sbSel === 'auto', 5000), `AUTO at 7.21 MHz: LSB by the convention (${JSON.stringify(last())})`);
      await sleep(6000);
      ok(!reps().some((r) => r.state === 'sync'), 'a USB station on LSB never syncs');
      let n0 = reps().length;
      L.spec.send({ type: 'freedv_sub', sb: 'usb' });
      ok(await until(() => reps().slice(n0).some((r) => r.sbSel === 'usb' && r.sb === 'usb'), 5000), `sb usb: the report says sbSel usb, sb usb (${JSON.stringify(last())})`);
      ok(await until(() => reps().slice(n0).some((r) => r.state === 'sync' && r.mode === '700D' && r.sb === 'usb'), 15000), `…and AUTO syncs on 700D (${JSON.stringify(last())})`);
      // ★ a retune keeps the listener's choice (it is theirs, like the sub-mode)
      n0 = reps().length;
      L.spec.send({ type: 'tune', frequency: STATIONS[0][0] + 100, mode: 'freedv' });
      await sleep(1500);
      ok(reps().slice(n0).every((r) => r.sbSel === 'usb'), `a nudge on the dial keeps the chosen side (${JSON.stringify(reps().slice(n0).find((r) => r.sbSel !== 'usb') || null)})`);
      n0 = reps().length;
      L.spec.send({ type: 'freedv_sub', sb: 'auto' });
      ok(await until(() => reps().slice(n0).some((r) => r.sbSel === 'auto' && r.sb === 'lsb'), 5000), `sb auto: back to LSB by the convention (${JSON.stringify(last())})`);
      n0 = reps().length;
      L.spec.send({ type: 'freedv_sub', sb: 'wsb' });
      await sleep(1500);
      ok(reps().slice(n0).every((r) => r.sbSel === 'auto'), 'a sideband that is none of ours is ignored');
    } finally { clearInterval(ping); for (const k of ['spec', 'aud']) L[k].close(); }
  } finally { await S.stop(); }
}
CENTRE = 7200000; CLIPNOW = CLIP; STATIONS = stationsAt(CENTRE);
await overrideScenario('sideband override: a USB station at 7.21 MHz, one-pipeline radio', ['--freq', String(STATIONS[0][0]), '--mode', 'usb', '--users', '1'], 'ovone');
await overrideScenario('sideband override: a USB station at 7.21 MHz, range-locked radio', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'ovlock');
CENTRE = 14200000; CLIPNOW = CLIP; STATIONS = stationsAt(CENTRE);

if (HAVE_RADE) {
  /* ★★ RADE needs no libcodec2: a server without it (VIBE_CODEC2_DISABLE=1 — the loader reports it "not installed")
   *  still offers FreeDV, with RADE alone, and AUTO decodes the RADE station. */
  console.log('── no libcodec2, RADE compiled in (VIBE_CODEC2_DISABLE=1) ──');
  const S = await server(['--freq', String(STATIONS[3][0]), '--mode', 'usb', '--users', '1'], 'radeonly', { VIBE_CODEC2_DISABLE: '1' });
  try {
    const st = await S.status();
    ok(st && st.modes.includes('freedv') && JSON.stringify(st.freedvSubs) === '["RADE"]', `vibeserver.json offers freedv with RADE alone (${JSON.stringify(st && st.freedvSubs)})`);
    const L = await listener(S.port, 'fdvrade01', STATIONS[3][0], 'freedv');
    const reps = () => jsons(L.spec, 'freedv');
    ok(await until(() => reps().some((r) => r.state === 'sync' && r.mode === 'RADE'), 25000), `AUTO in sync on RADE (${JSON.stringify(reps().at(-1))})`);
    const n0 = reps().length;
    L.spec.send({ type: 'freedv_sub', sub: '700D' });
    await sleep(1500);
    ok(!reps().slice(n0).some((r) => r.sub === '700D'), 'a sub-mode this server cannot run (700D) is not applied');
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S.stop(); }

  /* ★★ The owner's RADE switch ("rade", under FreeDV): off, RADE is neither offered nor hunted — the codec2 sub-modes stay. */
  console.log('── the owner switches RADE off (blockedModes "rade") ──');
  const S2 = await server(['--freq', String(STATIONS[3][0]), '--mode', 'usb', '--users', '1'], 'radeoff', {}, { blockedModes: 'rade' });
  try {
    const st = await S2.status();
    ok(st && st.modes.includes('freedv') && !st.freedvSubs.includes('RADE') && st.freedvSubs.includes('700D'), `freedvSubs without RADE (${JSON.stringify(st && st.freedvSubs)})`);
    const L = await listener(S2.port, 'fdvroff01', STATIONS[3][0], 'freedv');
    const reps = () => jsons(L.spec, 'freedv');
    await sleep(8000);
    ok(!reps().some((r) => r.state === 'sync'), `the RADE station: AUTO finds nothing (${JSON.stringify(reps().at(-1))})`);
    const n0 = reps().length;
    L.spec.send({ type: 'freedv_sub', sub: 'RADE' });
    await sleep(1500);
    ok(!reps().slice(n0).some((r) => r.sub === 'RADE'), 'RADE chosen by hand is not applied');
    for (const k of ['spec', 'aud']) L[k].close();
  } finally { await S2.stop(); }
}

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
