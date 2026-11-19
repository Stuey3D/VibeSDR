// test-server-packet9600.mjs — V12 9600 packet, FX.25 and IL2P through the REAL server (docs/v12/AX25-9600-FX25-IL2P.md).
//   A fake rtl_tcp replays `test-ax25hs --write-clip` (9600 G3RUH bursts on 435.000 MHz: an APRS position by plain AX.25,
//   a GRBAlpha telemetry frame by FX.25 that plain AX.25 cannot have, an IL2P status — twice). A listener in NFM attaches
//   the APRS decoder in AUTO; the server must hand the listener's CHANNEL IQ to its 9600 receiver (the shim's
//   packetIqRunning path), and the frames must come out: the frame log (aprs_frame) with each receiver named, the
//   satellite by its callsign, the APRS stations on the list, the counts on aprs_list, the non-APRS frame in the gallery.
//   Then 9600 manual with FX.25 off: the telemetry frame is not had (the switch is real). And the capability.
// Silent: an audio socket is opened only so the listener has audio, and its frames are dropped unread.
//   VIBESERVER_BIN=…/vibeserver VIBE_PKT9600_CLIP=…/pkt9600.cu8 node --max-old-space-size=512 scripts/test-server-packet9600.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_PKT9600_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_PKT9600_CLIP (test-ax25hs --write-clip p.cu8 2400000)');
  process.exit(3);
}
const RATE = 2400000, DIAL = 435000000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
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
      if (op === 0x1 && st.txt.length < 4000) st.txt.push(p.toString());
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Packet 9600 test', locator: 'IO92nh', adminPass: 'pkt9600-test-admin',
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 1 }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(DIAL)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBE_PKT_LOG: '1' };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', '1'],
                    { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const gallery = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, gallery, L, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}

const S = await server('pkt96');
try {
  console.log('── the capability ──');
  const st = await S.status();
  ok(st && st.aprs === true && st.aprsFec === true && st.pkt9600 === true, `vibeserver.json: aprs ${st && st.aprs}, aprsFec ${st && st.aprsFec}, pkt9600 ${st && st.pkt9600} ${st && st.pkt9600Why || ''}`);

  console.log('── AUTO: 1200 on the audio + 9600 on the channel IQ, FX.25 and IL2P ──');
  const sid = 'pkt9600a';
  const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  const au = ws(S.port, `/ws/audio?user_session_id=${sid}`);
  const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(600);
  sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' });
  await sleep(600);
  dx.send({ type: 'audio_extension_attach', extension_name: 'aprs', params: { baud_rate: 1200, packet: 'auto' } });
  ok(await until(() => /decoder attached: aprs AUTO \(1200 \+ 9600\)/.test(S.L()), 5000), 'the server says "decoder attached: aprs AUTO (1200 + 9600)"');
  const want = () => {
    const f = jsons(dx, 'aprs_frame').map((m) => m.data);
    return f.some((x) => x.from === 'G4ABC-9' && x.fec === 'ax25' && x.aprs) && f.some((x) => x.from === 'OM9GRB' && x.fec === 'fx25')
        && f.some((x) => x.from === 'G4ILP' && x.fec === 'il2p');
  };
  ok(await until(want, 40000), `the frame log has all three receivers' frames: ${jsons(dx, 'aprs_frame').map((m) => `${m.data.from}/${m.data.fec}`).join(' ')}`);
  const frames = jsons(dx, 'aprs_frame').map((m) => m.data);
  const grb = frames.find((x) => x.from === 'OM9GRB');
  ok(grb && grb.sat === 'GRBAlpha' && grb.len === 100 && grb.fixed > 0 && /^[0-9a-f]+$/.test(grb.hex) && !grb.aprs,
     `the telemetry frame: GRBAlpha by its callsign, FX.25 fixed ${grb && grb.fixed}, ${grb && grb.len} bytes, hex, not APRS`);
  const il = frames.find((x) => x.from === 'G4ILP');
  ok(il && il.crc === 1 && il.text === '>IL2P 9600 status' && il.aprs === 1, `the IL2P frame: CRC verified, its text, an APRS status (${il && il.text})`);
  ok(frames.every((x) => ['G4ABC-9', 'OM9GRB', 'G4ILP'].includes(x.from)), `no stray frames (${[...new Set(frames.map((x) => x.from))].join(' ')})`);
  ok(await until(() => { const l = lastOf(dx, 'aprs_list'); return l && l.data.auto === 1 && l.data.fx25 >= 1 && l.data.il2p >= 1 && l.data.other >= 1
                                                              && l.data.stations.some((s) => s.key === 'G4ABC-9') && l.data.stations.some((s) => s.key === 'G4ILP')
                                                              && !l.data.stations.some((s) => s.key === 'OM9GRB'); }, 8000),
     `aprs_list: auto, fx25 ${lastOf(dx, 'aprs_list')?.data.fx25}, il2p ${lastOf(dx, 'aprs_list')?.data.il2p}, other ${lastOf(dx, 'aprs_list')?.data.other}; the two APRS stations listed, the telemetry not`);
  const car = lastOf(dx, 'aprs_list')?.data.stations.find((s) => s.key === 'G4ABC-9');
  ok(car && Math.abs(car.lat - 51.5) < 1e-4, `the 9600 car is on the map (${car && car.lat}, ${car && car.lon})`);

  console.log('── 9600 manual, FX.25 OFF: the FX.25-only frame is not had ──');
  const n0 = jsons(dx, 'aprs_frame').length;
  dx.send({ type: 'audio_extension_attach', extension_name: 'aprs', params: { baud_rate: 9600, packet: '9600', fx25: 0, il2p: 1 } });
  ok(await until(() => /decoder attached: aprs 9600 Bd FX\.25 off IL2P on/.test(S.L()), 5000), 'the server says "aprs 9600 Bd FX.25 off IL2P on"');
  await sleep(500);
  const since = jsons(dx, 'aprs_frame').length;
  await until(() => jsons(dx, 'aprs_frame').slice(since).some((m) => m.data.from === 'G4ILP') && jsons(dx, 'aprs_frame').slice(since).some((m) => m.data.from === 'G4ABC-9'), 20000);
  await sleep(3000);   // ★ more than a whole loop of the clip
  const later = jsons(dx, 'aprs_frame').slice(since).map((m) => m.data);
  ok(later.some((x) => x.from === 'G4ILP') && later.some((x) => x.from === 'G4ABC-9') && !later.some((x) => x.from === 'OM9GRB'),
     `with FX.25 off: AX.25 and IL2P still, the damaged FX.25 frame not (${later.map((x) => x.from).join(' ')})`);
  ok(n0 > 0, 'frames before the switch');

  dx.send({ type: 'audio_extension_detach' });
  await sleep(1500);
  const g = await S.gallery();
  const items = (g && (g.items || g)) || [];
  const ap = Array.isArray(items) ? items.filter((i) => (i.k || i.kind) === 'aprs') : [];
  ok(ap.length >= 1, `the gallery: APRS items (${ap.map((i) => i.title).join(' | ')})`);
  const logItem = ap.find((i) => !/station/.test(i.title || ''));
  if (logItem) {
    const body = await (await fetch(`http://127.0.0.1:${S.port}/vibeserver/gallery/file/${logItem.f || logItem.name}`)).text().catch(() => '');
    ok(/GRBAlpha/.test(body) && /FX\.25/.test(body), `the day's log names the satellite frame: ${body.split('\n').filter((x) => /OM9GRB/.test(x)).slice(0, 1).join('')}`);
  } else ok(false, 'a day log item to read');
  sp.close(); au.close(); dx.close();
} finally {
  if (fail) console.log(S.L().split('\n').filter((x) => /packet|aprs|decoder/i.test(x)).slice(-14).join('\n'));
  S.stop();
}
console.log(`\ntest-server-packet9600: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
