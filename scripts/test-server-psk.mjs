// test-server-psk.mjs — V12 PSK31/63 through the REAL server: a fake rtl_tcp replaying a synthetic HF clip (`psk_bench
// clip`: five PSK stations at known offsets from 14.0710 MHz — one PSK63 — each calling "cq cq cq de <call> ×3 pse k"),
// and the contract of vibe_psk_run.h on the decoder socket:
//   • vibeserver.json says "psk": true;
//   • the SKIMMER ('pskskim', width 12 kHz) on a ONE-PIPELINE radio (vibe_wide_tap.h) lists the stations and spots their
//     calls, each at its RF frequency, the PSK63 one as BPSK63;
//   • the same on a RANGE-LOCKED radio (per-listener channels: one more slice of the shared FFT), and a second
//     listener's own, narrower segment;
//   • ONE SIGNAL ('psk', centre 1500) on a listener in USB with one station in the middle of their passband: its text in
//     the 0x01 frames, its spot at the right RF.
// Silent: no audio is played (an audio socket is opened only so the listener has audio, and its frames are dropped).
//   VIBESERVER_BIN=…/vibeserver VIBE_PSK_CLIP=…/clip.cu8 node scripts/test-server-psk.mjs
//   (make the clip: scripts/test-psk.sh clip clip.cu8)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_PSK_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_PSK_CLIP (make one: scripts/test-psk.sh clip clip.cu8)');
  process.exit(3);
}
const RATE = 960000, CENTRE = 14071000;
// ★ The clip's stations (tools/psk-bench/psk_bench.cpp, kClip): offset from CENTRE, call, baud.
const STATIONS = [[-3000, 'G4ABC', 31], [-1100, 'DL1XYZ', 31], [700, 'EA7XX', 31], [2300, 'OK1RR', 63], [4100, 'SP9ZZ', 31]];
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
      if (op === 0x2 && !st.dropBin) st.bin.push(Buffer.from(p));
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
const text01 = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-psk-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'PSK test', locator: 'IO92nh', adminPass: 'psk-test-admin',
    radios: [{ serial: 'psk-test', label: 'Test radio', enabled: true, configured: true }],
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
  return { port, dir, status, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  return L;
}
/** These stations spotted, each within 10 Hz of where it is, nothing else? */
function checkSpots(spots, label, which = STATIONS) {
  for (const [off, call, baud] of which) {
    const s = spots.find((x) => x.data.callsign === call);
    ok(!!s, `${label}: ${call} spotted${s ? ` at ${(s.data.frequency / 1e3).toFixed(3)} kHz, ${s.data.mode}, ${s.data.how}` : ''}`);
    if (s) ok(Math.abs(s.data.frequency - (CENTRE + off)) <= 10 && s.data.mode === (baud > 40 ? 'BPSK63' : 'BPSK31'),
              `${label}: ${call} at its RF frequency and mode (${s.data.frequency} vs ${CENTRE + off}, ${s.data.mode})`);
  }
  const bad = spots.filter((x) => !STATIONS.some(([, c]) => c === x.data.callsign));
  ok(bad.length === 0, `${label}: no false spots ${bad.map((x) => x.data.callsign).join(' ')}`);
}

// ── 1. A one-pipeline radio: the skimmer's own slice (vibe_wide_tap.h), then ONE SIGNAL ──────────────────────────────
{
  console.log('── one-pipeline radio (1 listener): the skimmer, then one signal ──');
  const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'one');
  try {
    const st = await S.status();
    ok(st && st.psk === true, `vibeserver.json: psk ${JSON.stringify(st && st.psk)}`);
    const L = await listener(S.port, 'pskone01', CENTRE, 'usb');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'pskskim', width: 12000 });
    const all = () => STATIONS.every(([, c]) => jsons(L.dx, 'psk_spot').some((x) => x.data.callsign === c));
    ok(await until(all, 40000), `every station spotted within 40 s (${jsons(L.dx, 'psk_spot').map((x) => x.data.callsign).join(' ')})`);
    checkSpots(jsons(L.dx, 'psk_spot'), 'skimmer');
    const lists = jsons(L.dx, 'psk_signals');
    const last = lists[lists.length - 1];
    ok(lists.length > 0 && last.data.skimmer === true, `psk_signals lists arrive (${lists.length}), marked skimmer`);
    ok(last && Math.abs(last.data.width - 12000) < 1500, `the segment searched is ≈ 12 kHz (${last && last.data.width})`);
    ok(last && last.data.signals.filter((x) => x.active).length === 5, `5 active signals in the list (${last && last.data.signals.filter((x) => x.active).length})`);
    const gRow = () => jsons(L.dx, 'psk_signals').reverse().map((l) => l.data.signals.find((x) => x.call === 'G4ABC')).find(Boolean);
    await until(() => !!gRow(), 4000);
    const g = gRow();
    ok(g && /g4abc g4abc/.test(g.text) && g.baud === 31, `a row carries its text and mode: "${g ? g.text.slice(-40) : 'no G4ABC row'}" (${g && g.baud})`);
    const o = jsons(L.dx, 'psk_signals').reverse().map((l) => l.data.signals.find((x) => x.call === 'OK1RR')).find(Boolean);
    ok(o && o.baud === 63, `the PSK63 station's row says 63 (${o && o.baud})`);

    // ★ ONE SIGNAL: USB with EA7XX in the middle of the passband (audio 1500 Hz)
    L.dx.bin.length = 0; L.dx.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: CENTRE + 700 - 1500, mode: 'usb' });
    await sleep(500);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'psk', centre: 1500 });
    ok(await until(() => /ea7xx ea7xx/.test(text01(L.dx)), 40000), `one signal: the text arrives as 0x01 frames ("${text01(L.dx).slice(-48).replace(/\n/g, '|')}")`);
    ok(!/g4abc|dl1xyz|ok1rr|sp9zz/.test(text01(L.dx)), 'one signal: only that station\'s text in the box');
    ok(await until(() => jsons(L.dx, 'psk_spot').some((x) => x.data.callsign === 'EA7XX'), 30000), 'one signal: EA7XX spotted');
    const sp = jsons(L.dx, 'psk_spot').find((x) => x.data.callsign === 'EA7XX');
    ok(sp && Math.abs(sp.data.frequency - (CENTRE + 700)) <= 10, `one signal: the spot's RF from dial + audio (${sp && sp.data.frequency}, audio ${sp && sp.data.audio} Hz)`);
    const ol = jsons(L.dx, 'psk_signals');
    ok(ol.length > 0 && ol[ol.length - 1].data.skimmer === false && ol[ol.length - 1].data.primary > 0, 'one signal: a list, not a skimmer, with a primary');
    const states = L.dx.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
    ok(states.includes(3), `state 3 (decoding) was sent (${[...new Set(states)].join(',')})`);
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(300);
    for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

// ── 2. A range-locked radio: the skimmer is one more slice of the shared FFT ─────────────────────────────────────────
{
  console.log('── range-locked radio (per-listener channels): the skimmer ──');
  const S = await server(['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'lock');
  try {
    const A = await listener(S.port, 'psklock1', CENTRE, 'usb');
    A.dx.send({ type: 'audio_extension_attach', extension_name: 'pskskim', width: 12000 });
    const all = () => STATIONS.every(([, c]) => jsons(A.dx, 'psk_spot').some((x) => x.data.callsign === c));
    ok(await until(all, 40000), `every station spotted within 40 s (${jsons(A.dx, 'psk_spot').map((x) => x.data.callsign).join(' ')})`);
    checkSpots(jsons(A.dx, 'psk_spot'), 'per-listener skimmer');
    // ★ A second listener tuned 3 kHz up skims ITS 6 kHz segment: the stations below the centre are outside it.
    const B = await listener(S.port, 'psklock2', CENTRE + 3000, 'usb');
    B.dx.send({ type: 'audio_extension_attach', extension_name: 'pskskim', width: 6000 });
    ok(await until(() => ['EA7XX', 'SP9ZZ'].every((c) => jsons(B.dx, 'psk_spot').some((x) => x.data.callsign === c)), 40000),
       'listener B (dial +3 kHz, 6 kHz) spots EA7XX and SP9ZZ, inside its segment');
    ok(!jsons(B.dx, 'psk_spot').some((x) => x.data.callsign === 'G4ABC' || x.data.callsign === 'DL1XYZ'), 'listener B never spots a station outside its segment');
    for (const L of [A, B]) for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-psk: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
