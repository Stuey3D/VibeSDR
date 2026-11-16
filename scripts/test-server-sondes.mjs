// test-server-sondes.mjs — V12 SONDES mode (RS41 radiosondes) through the REAL server (docs/v12/RS41.md).
//   A fake rtl_tcp replays a synthetic 403.005 MHz capture (`test-rs41 --write-clip`: three RS41 sondes from our own
//   transmitter, 20 dB, one frame a second each — one of them descending). Checked: the capability (sondes, sondesBands,
//   modes), entering the mode, the sonde table (serials, positions, climb, the calibration count, NO temperature before
//   the calibration is complete), the channels line, a tune held, the band step (402–404 → 404–406, clamped) and back,
//   the receiver given back exactly as found, and the mode refused where the owner switched it off.
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_RS41_CLIP=…/rs41.cu8 node scripts/test-server-sondes.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_RS41_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_RS41_CLIP (test-rs41 --write-clip r.cu8 2400000)');
  process.exit(3);
}
const RATE = 2400000;
const PASS = 'sondes-test-admin';
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(tag, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Sondes test', locator: 'IO70jf', adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', '403005000'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '96100000', '--mode', 'wfm',
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return { status: r.status, j: await r.json() }; } catch { return null; } };
  const status = async () => (await getJson('/vibeserver.json'))?.j || null;
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, getJson, L, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}

// ── 1. the mode ───────────────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── Sondes: the 402–404 MHz band, three RS41s ──');
  const S = await server('snd');
  try {
    const st = await S.status();
    ok(st && st.sondes === true, `vibeserver.json: sondes ${JSON.stringify(st && st.sondes)} ${st && st.sondesWhy || ''}`);
    const ids = (st && st.sondesBands || []).map((b) => b.id);
    ok(ids.join(',') === 'lo,mid,hi', `sondesBands ${ids.join(',')} (the whole 400–406 band needs a ≥ 6.7 MS/s capture: not offered at 2.4)`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('sondes'), `modes lists sondes`);
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=sndtest01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    await sleep(1200);
    const before = lastOf(sp, 'config');
    sp.send({ type: 'sondes', on: 1 });
    ok(await until(() => !!lastOf(sp, 'sondes'), 6000), 'a sondes report arrives');
    const cfg = lastOf(sp, 'config');
    ok(cfg && Math.abs(cfg.centerFreq - 403.005e6) < 1, `the radio on 403.005 MHz, the default band (centre ${cfg && cfg.centerFreq})`);
    const three = () => { const r = lastOf(sp, 'sondes'); return r && r.sondes.filter((s) => s.lat != null).length >= 3 ? r : null; };
    ok(await until(() => !!three(), 30000), `three sondes with positions: ${(lastOf(sp, 'sondes')?.sondes || []).map((s) => `${s.serial}@${s.hz}`).join(', ')}`);
    const r = lastOf(sp, 'sondes');
    ok(r && r.band === 'mid' && r.lo === 402000000 && r.hi === 404000000, `band mid, searching ${r && r.lo}–${r && r.hi}`);
    const a = r && r.sondes.find((s) => s.serial === 'S3310411');
    ok(a && Math.abs(a.lat - 50.218) < 0.01 && Math.abs(a.lon + 5.327) < 0.01 && a.alt > 3300 && Math.abs(a.climb - 5.2) < 0.2 && a.kind === 'RS41',
       `a row: ${JSON.stringify(a && { serial: a.serial, lat: a.lat, lon: a.lon, alt: a.alt, climb: a.climb, hz: a.hz })}`);
    ok(a && Math.abs(a.hz - 403.3e6) < 2000, `heard on ${a && a.hz} Hz (403.300 MHz sent)`);
    const d = r && r.sondes.find((s) => s.serial === 'U0912345');
    ok(d && d.climb < -13 && d.alt < 26100, `the descending one: climb ${d && d.climb}, alt ${d && d.alt}`);
    ok(r && r.sondes.every((s) => s.cal < 51 && s.temp == null), '★ no temperature: the calibration is not complete (cal < 51)');
    ok(r && Array.isArray(r.chans) && r.chans.length >= 3, `the channels being demodulated: ${r && r.chans.length}`);
    ok(r && Array.isArray(a && a.track) && a.track.length >= 1, 'a track');
    sp.send({ type: 'tune', frequency: 145000000, mode: 'nfm' });
    await sleep(800);
    ok(Math.abs(lastOf(sp, 'config').centerFreq - 403.005e6) < 1, 'a tune is ignored in the sondes mode');
    sp.send({ type: 'sondes', on: 1, step: 1 });
    ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs(c.centerFreq - 405.005e6) < 1 && lastOf(sp, 'sondes')?.band === 'hi'; }, 5000),
       `step +1: 404–406 MHz, the radio on 405.005 MHz (${lastOf(sp, 'config')?.centerFreq})`);
    sp.send({ type: 'sondes', on: 1, step: 1 });
    await sleep(800);
    ok(lastOf(sp, 'sondes')?.band === 'hi', 'clamped at the last band');
    sp.send({ type: 'sondes', on: 1, step: -1 });
    ok(await until(() => lastOf(sp, 'sondes')?.band === 'mid', 5000), 'step −1: back to 402–404');
    sp.send({ type: 'sondes', on: 1, band: 'all' });
    ok(await until(() => jsons(sp, 'sondes_error').length > 0, 4000), `the whole band refused at 2.4 MS/s: ${lastOf(sp, 'sondes_error')?.why}`);
    sp.send({ type: 'sondes', on: 0 });
    ok(await until(() => jsons(sp, 'sondes_off').length > 0, 4000), 'sondes_off');
    await sleep(800);
    const back = lastOf(sp, 'config');
    ok(back && before && Math.abs(back.vfo - before.vfo) < 1 && back.mode === before.mode, `the receiver given back (vfo ${back && back.vfo} ${back && back.mode} vs ${before && before.vfo} ${before && before.mode})`);
    ok(/\[SONDES\] mode ON/.test(S.L()) && /\[SONDES\] mode OFF/.test(S.L()), 'the log says ON and OFF');
    sp.close();
  } finally { S.stop(); }
}

// ── 2. switched off by the owner ──────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── the owner switched radiosondes off ──');
  const S = await server('sndoff', { blockedModes: 'sondes' });
  try {
    const st = await S.status();
    ok(st && st.sondes === false && /switched radiosondes off/.test(st.sondesWhy || ''), `sondes false with its reason: ${st && st.sondesWhy}`);
    ok(st && !st.modes.includes('sondes'), 'not in modes');
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=sndoff01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    sp.send({ type: 'sondes', on: 1 });
    ok(await until(() => jsons(sp, 'sondes_error').length > 0, 4000), `refused: ${JSON.stringify(lastOf(sp, 'sondes_error'))}`);
    ok(!lastOf(sp, 'sondes'), 'no report');
    sp.close();
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
