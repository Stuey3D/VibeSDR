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
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const AIS = process.env.VIBE_AIS_CLIP || '', APRS = process.env.VIBE_APRS_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !AIS || !fs.existsSync(AIS) || !APRS || !fs.existsSync(APRS)) {
  console.log('   not run — set VIBESERVER_BIN, VIBE_AIS_CLIP (test-ais --write-clip a.cu8 2400000) and VIBE_APRS_CLIP (test-aprs --write-clip b.cu8 2400000)');
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

async function server(clip, centre, args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'AIS/APRS test', locator: 'IO92nh', adminPass: 'aisaprs-test-admin',
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 1 }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const gallery = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  return { port, dir, status, gallery, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}

// ── 1. AIS mode ───────────────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── AIS mode ──');
  const S = await server(AIS, 162000000, ['--freq', '96100000', '--mode', 'wfm', '--users', '1'], 'ais');
  try {
    const st = await S.status();
    ok(st && st.ais === true, `vibeserver.json: ais ${JSON.stringify(st && st.ais)} ${st && st.aisWhy || ''}`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('ais') && st.modes.includes('aprs'), 'modes lists ais and aprs');
    ok(st && st.aprs === true && st.aprsHz === 144800000, `aprs true, the UK channel ${st && st.aprsHz}`);
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=aistest01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    await sleep(1200);
    const before = lastOf(sp, 'config');
    sp.send({ type: 'ais', on: 1 });
    ok(await until(() => !!lastOf(sp, 'ais'), 6000), 'an ais report arrives');
    const cfg = lastOf(sp, 'config');
    ok(cfg && Math.abs(cfg.centerFreq - 162e6) < 1 && Math.abs(cfg.vfo - 162e6) < 1, `the radio on 162.000 MHz (centre ${cfg && cfg.centerFreq}, vfo ${cfg && cfg.vfo})`);
    const named = () => { const r = lastOf(sp, 'ais'); return r && r.vessels.filter((v) => /^TEST SHIP \d+$/.test(v.name || '') && v.lat != null).length >= 5 ? r : null; };
    ok(await until(() => !!named(), 30000), `ships named and placed: ${(lastOf(sp, 'ais')?.vessels || []).map((v) => v.name || v.mmsi).join(', ')}`);
    const r = lastOf(sp, 'ais');
    ok(r.chA > 0 && r.chB > 0, `both channels decoded (A ${r.chA}, B ${r.chB})`);
    const v = r.vessels.find((x) => x.name === 'TEST SHIP 0');
    ok(v && v.flag === 'GB' && v.family === 'cargo' && v.dest === 'SOUTHAMPTON' && v.line.startsWith('TEST SHIP 0 · cargo'), `a merged row: ${v && v.line} (${v && v.flag})`);
    ok(r.vessels.every((x) => x.mmsi >= 235000000 && x.mmsi < 235001000), `no stray MMSIs: ${r.vessels.map((x) => x.mmsi).join(' ')}`);
    sp.send({ type: 'ais_trail', mmsi: v.mmsi });
    ok(await until(() => jsons(sp, 'ais_trail').some((t) => t.mmsi === v.mmsi && t.pts.length >= 1), 4000), `its track: ${JSON.stringify(lastOf(sp, 'ais_trail'))}`);
    // tune / zoom are held while in AIS
    sp.send({ type: 'tune', frequency: 145000000, mode: 'nfm' });
    await sleep(800);
    ok(Math.abs(lastOf(sp, 'config').centerFreq - 162e6) < 1, 'a tune is ignored in AIS');
    sp.send({ type: 'ais', on: 0 });
    ok(await until(() => jsons(sp, 'ais_off').length > 0, 4000), 'ais_off');
    await sleep(800);
    const back = lastOf(sp, 'config');
    ok(back && before && Math.abs(back.vfo - before.vfo) < 1 && back.mode === before.mode, `the receiver given back (vfo ${back && back.vfo} ${back && back.mode} vs ${before && before.vfo} ${before && before.mode})`);
    const g = await S.gallery();
    const items = (g && (g.items || g)) || [];
    const ais = Array.isArray(items) ? items.filter((i) => i.kind === 'ais') : [];
    ok(ais.length >= 2, `the gallery: the day's AIS log and its card (${ais.map((i) => i.title).join(' | ')})`);
    ok(ais.some((i) => /ships and stations/.test(i.title)), 'the card names the count');
    sp.close();
  } finally { S.stop(); }
}

// ── 2. APRS decoder on a listener's NFM ──────────────────────────────────────────────────────────────────────────────
{
  console.log('── APRS decoder ──');
  const S = await server(APRS, 144800000, ['--freq', '144800000', '--mode', 'nfm', '--users', '1'], 'aprs');
  try {
    const sid = 'aprstest01';
    const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
    await sleep(400);
    const au = ws(S.port, `/ws/audio?user_session_id=${sid}`); au.dropBin = true;
    const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
    await sleep(600);
    sp.send({ type: 'tune', frequency: 144800000, mode: 'nfm' });
    await sleep(600);
    dx.send({ type: 'audio_extension_attach', extension_name: 'aprs' });
    const want = ['G4ABC-9', 'MB7UH', 'M0XYZ', 'obj:REPEATER'];
    const all = () => { const l = lastOf(dx, 'aprs_list'); return l && want.every((k) => l.data.stations.some((s) => s.key === k)) ? l : null; };
    ok(await until(() => !!all(), 40000), `every station listed: ${(lastOf(dx, 'aprs_list')?.data.stations || []).map((s) => s.key).join(' ')}`);
    const l = lastOf(dx, 'aprs_list').data;
    const car = l.stations.find((s) => s.key === 'G4ABC-9');
    ok(car && Math.abs(car.lat - 51.5) < 1e-4 && Math.abs(car.lon + 1 / 6) < 1e-4 && car.sym === '/>' && Math.round(car.alt) === 37, `the car: ${car && car.line}`);
    const wx = l.stations.find((s) => s.key === 'MB7UH');
    ok(wx && wx.wx && wx.wx.tempC === 10 && wx.wx.hPa === 1012, `the weather station: ${wx && wx.line}`);
    ok(l.messages.some((m) => m.from === 'M0XYZ' && m.to === 'G4ABC-9' && m.text === 'See you at the club'), 'the message');
    const packets = jsons(dx, 'aprs_packet');
    ok(packets.length >= 4, `aprs_packet frames (${packets.length})`);
    ok(l.stations.every((s) => s.count <= 2), `each packet counted once per transmission (${l.stations.map((s) => s.key + ':' + s.count).join(' ')})`);
    dx.send({ type: 'audio_extension_detach' });
    await sleep(1500);
    const g = await S.gallery();
    const items = (g && (g.items || g)) || [];
    const ap = Array.isArray(items) ? items.filter((i) => i.kind === 'aprs') : [];
    ok(ap.length >= 1, `the gallery: APRS items (${ap.map((i) => i.title).join(' | ')})`);
    sp.close(); au.close(); dx.close();
  } finally { S.stop(); }
}

console.log(`\ntest-server-ais-aprs: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
