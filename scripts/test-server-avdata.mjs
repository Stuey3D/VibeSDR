// test-server-avdata.mjs — V12 AIRCRAFT DATALINK mode and the HFDL endpoints through the REAL server (docs/v12/AVIATION.md).
//   A fake rtl_tcp replays a synthetic 136.850 MHz VDL2 clip (`test-vdl2 --write-clip`: five aircraft, ACARS over AVLC on
//   three channels, 25 dB). Checked: the capability (avdata, avdataBands, modes, hfdl), entering the mode on VDL2, the
//   aircraft table (ICAO addresses, registrations, flights), NO message body without the owner's opt-in and — with it —
//   still none for a listener while the owner's socket gets it, a tune held, the band step to ACARS (131.675 MHz) and
//   back, the receiver given back exactly as found, the mode refused where the owner switched it off, and
//   /addon/hfdl/aircraft + /addon/hfdl/groundstations (the HFDL map's shape).
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/vdl2.cu8 node scripts/test-server-avdata.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_VDL2_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_VDL2_CLIP (test-vdl2 --write-clip v.cu8 2400000)');
  process.exit(3);
}
const RATE = 2400000;
const PASS = 'avdata-test-admin';
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
    configured: true, name: 'Aircraft datalink test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '96100000', '--mode', 'wfm',
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return { status: r.status, cors: r.headers.get('access-control-allow-origin'), j: await r.json() }; } catch { return null; } };
  const status = async () => (await getJson('/vibeserver.json'))?.j || null;
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, getJson, L, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function signIn(S, sock) {
  const nonce = (await S.getJson('/vibeserver/auth'))?.j?.nonce || '';
  sock.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', PASS).update(nonce).digest('hex') });
  return until(() => sock.txt.some((m) => m.includes('"type":"admin","ok":true')), 4000);
}

// ── 1. the mode, bodies off (the default) ─────────────────────────────────────────────────────────────────────────────
{
  console.log('── Aircraft datalink: VDL2 / ACARS bands, bodies off ──');
  const S = await server('avd');
  try {
    const st = await S.status();
    ok(st && st.avdata === true, `vibeserver.json: avdata ${JSON.stringify(st && st.avdata)} ${st && st.avdataWhy || ''}`);
    const ids = (st && st.avdataBands || []).map((b) => b.id);
    ok(ids.join(',') === 'acars,vdl2', `avdataBands ${JSON.stringify(st && st.avdataBands)}`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('avdata') && st.modes.includes('hfdl'), `modes lists avdata and hfdl (${st && st.modes && st.modes.join(' ')})`);
    ok(st && st.hfdl === true, `hfdl true on an RTL (direct sampling reaches HF) — ${JSON.stringify(st && st.hfdl)} ${st && st.hfdlWhy || ''}`);
    ok(st && !st.avdataText, 'no avdataText without the owner\'s opt-in');
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avdtest01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    await sleep(1200);
    const before = lastOf(sp, 'config');
    sp.send({ type: 'avdata', on: 1, band: 'vdl2' });
    ok(await until(() => !!lastOf(sp, 'avdata'), 6000), 'an avdata report arrives');
    const cfg = lastOf(sp, 'config');
    ok(cfg && Math.abs(cfg.centerFreq - 136.8625e6) < 1, `the radio on 136.8625 MHz (centre ${cfg && cfg.centerFreq})`);
    const five = () => { const r = lastOf(sp, 'avdata'); return r && r.aircraft.filter((a) => a.icao && a.reg).length >= 5 ? r : null; };
    ok(await until(() => !!five(), 30000), `five aircraft, by ICAO and registration: ${(lastOf(sp, 'avdata')?.aircraft || []).map((a) => `${a.icao}/${a.reg}/${a.flight}`).join(', ')}`);
    const r = lastOf(sp, 'avdata');
    const ei = r && r.aircraft.find((a) => a.icao === '4CA97B');
    ok(ei && ei.reg === 'EI-EST' && ei.flight === 'FR1234' && ei.label === 'H1' && /^VDL2 136\.\d{3}$/.test(ei.via), `a row: ${JSON.stringify(ei)}`);
    ok(r && r.band === 'vdl2' && r.bandName === 'VDL2' && r.msgs >= 5, `band vdl2, msgs ${r && r.msgs}`);
    ok(!sp.txt.some((m) => m.includes('TEST MESSAGE BODY')), '★ no message body anywhere without the owner\'s opt-in');
    sp.send({ type: 'tune', frequency: 145000000, mode: 'nfm' });
    await sleep(800);
    ok(Math.abs(lastOf(sp, 'config').centerFreq - 136.8625e6) < 1, 'a tune is ignored in the datalink mode');
    sp.send({ type: 'avdata', on: 1, step: -1 });
    ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs(c.centerFreq - 131.675e6) < 1 && lastOf(sp, 'avdata')?.band === 'acars'; }, 5000),
       `step −1: ACARS, the radio on 131.675 MHz (${lastOf(sp, 'config')?.centerFreq})`);
    sp.send({ type: 'avdata', on: 1, step: -1 });
    await sleep(800);
    ok(lastOf(sp, 'avdata')?.band === 'acars', 'clamped at the first band');
    sp.send({ type: 'avdata', on: 1, step: 1 });
    ok(await until(() => lastOf(sp, 'avdata')?.band === 'vdl2', 5000), 'step +1: back to VDL2');
    sp.send({ type: 'avdata', on: 0 });
    ok(await until(() => jsons(sp, 'avdata_off').length > 0, 4000), 'avdata_off');
    await sleep(800);
    const back = lastOf(sp, 'config');
    ok(back && before && Math.abs(back.vfo - before.vfo) < 1 && back.mode === before.mode, `the receiver given back (vfo ${back && back.vfo} ${back && back.mode} vs ${before && before.vfo} ${before && before.mode})`);
    ok(/\[AVDATA\] mode ON: VDL2/.test(S.L()) && /\[AVDATA\] mode OFF/.test(S.L()), 'the log says ON and OFF');
    // ── the HFDL map's endpoints (nobody has decoded HFDL here: every station, none heard; no aircraft) ──
    const gs = await S.getJson('/addon/hfdl/groundstations');
    ok(gs && gs.status === 200 && gs.cors === '*' && Array.isArray(gs.j) && gs.j.length === 16 && gs.j.some((g) => g.gs_id === 7 && g.location === 'Shannon' && Math.abs(g.lat - 52.7) < 0.2),
       `/addon/hfdl/groundstations: ${gs && gs.j && gs.j.length} stations (CORS ${gs && gs.cors})`);
    ok(gs && gs.j.every((g) => Array.isArray(g.frequencies) && g.frequencies.length === 0 && g.last_heard == null), 'none heard: no frequencies, no last_heard');
    const ac = await S.getJson('/addon/hfdl/aircraft');
    ok(ac && ac.status === 200 && Array.isArray(ac.j) && ac.j.length === 0, `/addon/hfdl/aircraft: [] (VDL2 rows are not HFDL rows) — ${ac && JSON.stringify(ac.j).slice(0, 80)}`);
    sp.close();
  } finally { S.stop(); }
}

// ── 2. the owner's opt-in: bodies to the OWNER only ──────────────────────────────────────────────────────────────────
{
  console.log('── the owner\'s message-body opt-in (avdata+text) ──');
  const S = await server('avdt', { blockedModes: 'avdata+text' });
  try {
    const st = await S.status();
    ok(st && st.avdataText === true, 'vibeserver.json: avdataText true');
    const owner = ws(S.port, '/ws/user-spectrum?user_session_id=avdown01&bins=256&proto=1');
    const guest = ws(S.port, '/ws/user-spectrum?user_session_id=avdgst01&bins=256&proto=1');
    await until(() => owner.open && guest.open, 3000);
    await sleep(800);
    ok(await signIn(S, owner), 'the owner signs in');
    owner.send({ type: 'avdata', on: 1, band: 'vdl2' });
    ok(await until(() => (lastOf(owner, 'avdata')?.aircraft || []).some((a) => /TEST MESSAGE BODY/.test(a.text || '')), 30000), 'the owner\'s report carries the message text');
    await sleep(1500);
    ok(jsons(guest, 'avdata').length > 0 && !guest.txt.some((m) => m.includes('TEST MESSAGE BODY')), `★ a listener never gets it (${jsons(guest, 'avdata').length} reports, none with text)`);
    owner.send({ type: 'avdata', on: 0 });
    owner.close(); guest.close();
  } finally { S.stop(); }
}

// ── 3. switched off by the owner ──────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── the owner switched the aircraft datalink off ──');
  const S = await server('avdoff', { blockedModes: 'avdata' });
  try {
    const st = await S.status();
    ok(st && st.avdata === false && /switched the aircraft datalink off/.test(st.avdataWhy || ''), `avdata false with its reason: ${st && st.avdataWhy}`);
    ok(st && !st.modes.includes('avdata'), 'not in modes');
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avdoff01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    sp.send({ type: 'avdata', on: 1 });
    ok(await until(() => jsons(sp, 'avdata_error').length > 0, 4000), `refused: ${JSON.stringify(lastOf(sp, 'avdata_error'))}`);
    ok(!lastOf(sp, 'avdata'), 'no report');
    sp.close();
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
