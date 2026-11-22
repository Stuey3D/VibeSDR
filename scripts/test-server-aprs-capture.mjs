// test-server-aprs-capture.mjs — APRS AUTO on a LISTENER'S OWN VFO through the REAL server, with the developer capture on
// (VIBE_DEC_AUDIO_DUMP, android/app/src/main/cpp/vibe_dec_dump.h; docs/v12/APRS-ON-AIR-2.md): a range-locked radio (each
// listener their own channel, as the Pi 500's), the beta90 client's attach ({"packet":"auto"}: 1200 on the audio + 9600 on
// the channel IQ, FX.25, IL2P), squelch ON and closed below the packets (the decoder must hear pre-squelch audio), the
// packets decoded; the capture's .wav/.cf32/.txt written by the server; tool-dec-replay of that capture = the same packets.
// (Header below copied from test-server-ais-aprs.mjs, which it shares its harness with.)
// ----
// test-server-ais-aprs.mjs — V12 AIS mode and the APRS decoder through the REAL server (docs/v12/AIS-APRS.md).
//   AIS: a fake rtl_tcp replaying a synthetic 162 MHz clip (`test-ais --write-clip`: ships on BOTH channels, GMSK, 20 dB),
//        the capability (ais, modes), entering AIS, the ship list with names merged from type 5, a ship's track, the
//        receiver given back exactly as found, the day's log and summary card in the gallery.
//   APRS: a synthetic 144.800 MHz NFM clip (`test-aprs --write-clip`: four stations, twice), a listener tuned in NFM with
//        the 'aprs' decoder: packets, the station list (position, weather, an object, a message), the duplicate rule,
//        the gallery's APRS card.
// Silent: an audio socket is opened only so the listener has audio, and its frames are dropped unread.
//   VIBESERVER_BIN=…/vibeserver VIBE_AIS_CLIP=…/ais.cu8 VIBE_APRS_CLIP=…/aprs.cu8 node scripts/test-server-ais-aprs.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const APRS = process.env.VIBE_APRS_CLIP || '', REPLAY = process.env.VIBE_DEC_REPLAY_BIN || '';
if (!BIN || !fs.existsSync(BIN) || !APRS || !fs.existsSync(APRS)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_APRS_CLIP (test-aprs --write-clip b.cu8 2400000); VIBE_DEC_REPLAY_BIN (tool-dec-replay) for the replay half');
  process.exit(3);
}
const RATE = 2400000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, dropBin: false };
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
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(clip, centre, args, tag, radioExtra = {}, envExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'AIS/APRS test', locator: 'IO92nh', adminPass: 'aisaprs-test-admin',
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 1, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...envExtra };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const gallery = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  return { port, dir, status, gallery, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}


{
  console.log('── APRS AUTO on a listener\'s own VFO, the capture on ──');
  const capDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-aprscap-dump-'));
  const S = await server(APRS, 144800000, ['--freq', '144800000', '--mode', 'nfm', '--users', '4', '--lock-freq', '144800000'], 'aprscap',
                         { users: 4 }, { VIBE_DEC_AUDIO_DUMP: capDir, VIBE_DEC_AUDIO_DUMP_MB: '64' });
  try {
    const sid = 'aprscap01';
    const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
    await sleep(400);
    const au = ws(S.port, `/ws/audio?user_session_id=${sid}`); au.dropBin = true;
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    await sleep(600);
    sp.send({ type: 'tune', frequency: 144800000, mode: 'nfm' });
    await sleep(400);
    // ★ squelch ON at a level the packets never reach: the listener hears nothing, the decoder must still hear them
    sp.send({ type: 'squelch', on: true, db: 40 });
    await sleep(400);
    dx.send({ type: 'audio_extension_attach', extension_name: 'aprs', packet: 'auto' });
    const want = ['G4ABC-9', 'MB7UH', 'M0XYZ', 'obj:REPEATER'];
    const all = () => { const l = lastOf(dx, 'aprs_list'); return l && want.every((k) => l.data.stations.some((s) => s.key === k)) ? l : null; };
    ok(await until(() => !!all(), 40000), `AUTO, squelch closed: every station listed: ${(lastOf(dx, 'aprs_list')?.data.stations || []).map((s) => s.key).join(' ')}`);
    const live = jsons(dx, 'aprs_packet').map((p) => p.data.raw);
    ok(live.length >= 4, `aprs_packet frames (${live.length})`);
    const lg = fs.readFileSync(path.join(S.dir, 'server.log'), 'utf8');
    ok(/decoder attached: aprs AUTO \(1200 \+ 9600\)/.test(lg), 'the server ran AUTO (1200 + 9600)');
    dx.send({ type: 'audio_extension_detach' });
    await sleep(800);
    const files = fs.readdirSync(capDir);
    const txt = files.find((f) => f.endsWith('.txt') && f.includes('-aprs-'));
    ok(!!txt && files.some((f) => f.endsWith('.wav')) && files.some((f) => f.endsWith('.cf32')), `the capture: ${files.join(' ')}`);
    const side = txt ? fs.readFileSync(path.join(capDir, txt), 'utf8') : '';
    const lines = side.split('\n').filter((l) => l && !l.startsWith('#'));
    ok(/# params .*"packet":"auto"/.test(side) && /# session aprscap01/.test(side), 'the .txt names the attach and the listener');
    const last = lines[lines.length - 1] || '';
    ok(lines.length >= 3 && /mode nfm/.test(last) && /squelch on/.test(last) && /dial 144800000 Hz/.test(last) && !/listener gone/.test(last),
       `a line a second with the listener's state (${lines.length}): ${last}`);
    const fed = Number((last.split(' ')[1]) || 0);
    ok(fed > 5, `the audio fed to the decoder: ${fed} s`);
    if (REPLAY && fs.existsSync(REPLAY) && txt) {
      const r = spawnSync(REPLAY, [path.join(capDir, txt), '--quiet'], { encoding: 'utf8', timeout: 120000 });
      const raws = (r.stdout || '').split('\n').filter((l) => l.startsWith('APRS  ')).map((l) => l.slice(6));
      const liveSet = new Set(live), repSet = new Set(raws);
      ok(r.status === 0 && [...liveSet].every((x) => repSet.has(x)), `tool-dec-replay of the capture: ${raws.length} packets, every live one among them (${live.length} live)`);
    } else console.log('   (replay half not run — VIBE_DEC_REPLAY_BIN)');
    sp.close(); au.close(); dx.close();
  } finally { S.stop(); }
  for (const f of fs.readdirSync(capDir)) fs.rmSync(path.join(capDir, f));
  fs.rmdirSync(capDir);
}
console.log(`\ntest-server-aprs-capture: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
