// test-bg-decode-admin.mjs — THE ADMIN PAGE'S BACKGROUND-DECODING LINE, end to end (V12 beta 4, 2026-10-22).
//   The radio's admin status ("bgDecode", Impl::bgStatusJson + BgRunner::statusJson) through a real vibeserver on the
//   fake dongle: waiting (with its countdown) → running at the profile centre, with MEASURED CPU > 0 per VFO and for
//   the shared channeliser → a listener on 648 kHz: carrying on in range → a listener's decoder takes a slot: that VFO
//   paused, and why → the listener tunes away: paused. And none of it reaches anybody without the admin password.
// SILENT. usage: VIBESERVER_BIN=… node scripts/test-bg-decode-admin.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
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
  st.has = (k) => st.txt.some((t) => t.includes(k));
  return st;
}


const ADMIN = 'bgadm-admin';
/** One server run: config.json with this radio, fake rtl_tcp, the test wait. Returns helpers. */
async function run(radio, extraArgs, envExtra = {}, decoderMax = 2) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgadm-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Admin line test', adminPass: ADMIN, decoderMax,
    radios: [{ serial: 'bgadm', label: 'Test radio', enabled: true, configured: true, ...radio }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rate = String(radio.rate || 3000000);
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', rate], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_BG_WAIT_SEC: '3', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...envExtra };
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', rate, ...extraArgs], { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const L = () => fs.readFileSync(logPath, 'utf8');
  return { port, logPath, L, count: (re) => (L().match(new RegExp(re.source, 'g')) || []).length,
           stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} } };
}
const until = async (f, ms = 10000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };
const cfgOf = (st) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes('"type":"config"')) return JSON.parse(st.txt[i]); return null; };
const NICK = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
let s = null;
try {
  s = await run({ users: 4, rate: 3000000, bgDecode: JSON.stringify(NICK) }, ['--freq', '648000', '--mode', 'am', '--users', '4'],
                { VIBESERVER_BG_WAIT_SEC: '6' });
  let adminQ, bg, untilBg;
  adminQ = async () => { const n = (await fetch(`http://127.0.0.1:${s.port}/vibeserver/auth`).then((r) => r.json())).nonce;
    return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n).digest('hex')}`; };
  bg = async () => (await fetch(`http://127.0.0.1:${s.port}/vibeserver/admin/status?${await adminQ()}`).then((r) => r.json())).bgDecode;
  untilBg = async (f, ms = 15000) => { for (let t = 0; t < ms; t += 500) { const b = await bg(); if (f(b)) return b; await sleep(500); } return null; };

  console.log('── nobody here: waiting, with the countdown ──');
  const w = await bg();
  ok(w && w.state === 'waiting' && w.profile === 1 && typeof w.startsIn === 'number' && w.startsIn > 0 && w.startsIn <= 6,
     `waiting, profile 1, starts in ${w && w.startsIn} s`);

  console.log('── after the wait: running at the profile centre, CPU measured ──');
  const r1 = await untilBg((b) => b && b.state === 'running' && b.runner && b.runner.vfos.every((v) => v.state === 'running'));
  ok(!!r1 && r1.runner.centreHz === 500000 && r1.runner.vfos.length === 2, `running, 2 of 2 VFOs, centre ${r1 && r1.runner.centreHz}`);
  ok(!!r1 && r1.runner.vfos.every((v) => v.label === 'NAVTEX' && typeof v.lock === 'string' && v.lock.length > 0), `each VFO says NAVTEX and its lock (${r1 && r1.runner.vfos.map((v) => v.lock)})`);
  await sleep(3000);
  const r2 = await untilBg((b) => b && b.runner && typeof b.runner.cpuPct === 'number', 8000);
  ok(!!r2 && r2.runner.cpuPct > 0, `★ measured CPU > 0 % of one core: ${r2 && r2.runner.cpuPct}`);
  ok(!!r2 && r2.runner.vfos.every((v) => typeof v.cpuPct === 'number' && v.cpuPct > 0), `★ each VFO's own CPU > 0: ${r2 && r2.runner.vfos.map((v) => v.cpuPct)}`);
  ok(!!r2 && typeof r2.runner.channelsCpuPct === 'number' && r2.runner.channelsCpuPct > 0, `the shared channeliser reported apart: ${r2 && r2.runner.channelsCpuPct}`);
  ok(!!r2 && r2.runner.cpuPct + 0.05 >= r2.runner.channelsCpuPct + r2.runner.vfos.reduce((a, v) => a + v.cpuPct, 0) - 0.05, 'the total is the parts');

  console.log('── nothing for anyone without the admin password ──');
  const anon = await fetch(`http://127.0.0.1:${s.port}/vibeserver/admin/status`);
  ok(anon.status === 401, `the admin status without credentials: ${anon.status}`);
  const pub = await fetch(`http://127.0.0.1:${s.port}/vibeserver.json`).then((r) => r.text()).catch(() => '');
  ok(pub.length > 0 && !/bgDecode|cpuPct|channelsCpuPct/.test(pub), 'vibeserver.json (public) carries none of it');

  console.log('── a listener on 648 kHz: carrying on in range ──');
  const a = ws(s.port, '/ws/user-spectrum?user_session_id=bgadm001&bins=256');
  await sleep(1200);
  a.send({ type: 'tune', frequency: 648000, mode: 'am' });
  const l = await untilBg((b) => b && b.state === 'listener' && b.runner && b.runner.listenerOn);
  ok(!!l && l.runner.vfos.filter((v) => v.state === 'running').length === 2, `★ "listener": both VFOs carry on (${l && l.runner.vfos.map((v) => v.state)})`);
  ok(!(await fetch(`http://127.0.0.1:${s.port}/vibeserver.json`).then((r) => r.text())).includes('bgDecode'), '...and still nothing public');

  console.log('── a listener\'s decoder takes a slot: VFO 2 paused, and why ──');
  const dx = ws(s.port, '/ws/dxcluster?user_session_id=bgadm001');
  await sleep(600);
  dx.send({ type: 'audio_extension_attach', extension_name: 'navtex' });
  const p = await untilBg((b) => b && b.runner && b.runner.vfos[1] && b.runner.vfos[1].state === 'paused', 8000);
  ok(!!p && /a listener needed a decoder slot/.test(p.runner.vfos[1].why) && p.runner.vfos[0].state === 'running',
     `★ VFO 2 "paused" (${p && p.runner.vfos[1].why}), VFO 1 running`);
  ok(!dx.txt.some((t) => /bgDecode|cpuPct/.test(t)) && !a.txt.some((t) => /bgDecode|cpuPct/.test(t)), 'the listener\'s sockets carry none of it');
  dx.close();

  console.log('── the listener tunes far away: paused ──');
  a.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  const q = await untilBg((b) => b && b.state === 'paused', 10000);
  ok(!!q && !q.runner, '★ "paused" — a listener is on and no VFO is in their window');
  a.close();
  s.stop(); s = null;

  console.log('── an FT8 VFO: the admin line carries its live count ──');
  s = await run({ users: 4, rate: 2048000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 1840000, from: '00:00', to: '00:00',
                  vfos: [{ hz: 1840000, mode: 'usb', dec: 'ft8' }] }] }) }, ['--freq', '1840000', '--mode', 'usb', '--users', '4'],
                { VIBESERVER_BG_WAIT_SEC: '2' });
  const f = await untilBg((b) => b && b.state === 'running' && b.runner && b.runner.vfos[0] && b.runner.vfos[0].state === 'running');
  const v = f && f.runner.vfos[0];
  ok(!!v && /FT8/.test(v.label) && v.spots === 0 && v.countries === 0, `★ FT8 VFO: "spots" and "countries" so far (${v && v.spots}, ${v && v.countries}) — the fake dongle sends no FT8`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  if (s) s.stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
