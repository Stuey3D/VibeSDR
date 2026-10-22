// test-bg-decode-inrange.mjs — BACKGROUND DECODING WHILE LISTENERS ARE ON, and on LOCKED-RANGE radios (Stuart, 2026-10-22):
//   · "Keep decoding stations still in range while listeners are on" (default on): a listener on 648 kHz at 3 MS/s, Nick's
//     NAVTEX profile — 489.5 and 517.5 keep decoding at the radio's own centre; the listener's dial is untouched; tuning far
//     away pauses them, tuning back resumes them; a listener's decoder with the slots full pauses the LAST VFO;
//   · switched off: a listener arriving stops background decoding, as before;
//   · a LOCKED radio: never moved; a due profile starts at once; a real listener connects normally; the user count is
//     real listeners only; a VFO outside the locked range is refused at the save.
// SILENT. usage: VIBESERVER_BIN=… node scripts/test-bg-decode-inrange.mjs
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


const ADMIN = 'bgir-admin';
/** One server run: config.json with this radio, fake rtl_tcp, the test wait. Returns helpers. */
async function run(radio, extraArgs, envExtra = {}, decoderMax = 2) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgir-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'In range test', adminPass: ADMIN, decoderMax,
    radios: [{ serial: 'bgir', label: 'Test radio', enabled: true, configured: true, ...radio }] }));
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
  console.log('── a listener on 648 kHz, Nick\'s profile, 3 MS/s: the NAVTEX VFOs keep decoding, nothing moves ──');
  s = await run({ users: 4, rate: 3000000, bgDecode: JSON.stringify(NICK) }, ['--freq', '648000', '--mode', 'am', '--users', '4']);
  const a = ws(s.port, '/ws/user-spectrum?user_session_id=bgir0001&bins=256');
  await sleep(1500);
  a.send({ type: 'tune', frequency: 648000, mode: 'am' });
  ok(await until(() => /carrying on in range — profile 1's VFOs inside the radio's window at 648\.0 kHz: 2 of 2 decoding — with a listener on/.test(s.L()), 10000),
     '★ with the listener on 648 kHz, both NAVTEX VFOs (489.5, 517.5) run — at the radio\'s own centre, and the log says "carrying on in range"');
  ok(!/profile 1, RF centre 648/.test(s.L()), '...never logged as the profile starting at 648 kHz');
  // ★★ beta4 oddity: a save that changes only the wait restarts nothing (it rebuilt the runner and cut messages).
  {
    const adminQ = async () => { const n = (await fetch(`http://127.0.0.1:${s.port}/vibeserver/auth`).then((r) => r.json())).nonce;
      return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n).digest('hex')}`; };
    const got = await fetch(`http://127.0.0.1:${s.port}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
    const startsBefore = (s.L().match(/carrying on in range —/g) || []).length;
    const body = { ...got, radios: got.radios.map((r) => ({ ...r, bgDecode: JSON.stringify({ ...NICK, waitMin: 1 }) })) };
    const res = await fetch(`http://127.0.0.1:${s.port}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
    ok(res.status === 200 && await until(() => /settings saved — profile 1 unchanged, carrying on/.test(s.L()), 6000), '★ the wait changed (5 → 1 min) while carrying on in range: "settings saved — profile 1 unchanged, carrying on"');
    await sleep(2500);
    ok((s.L().match(/carrying on in range —/g) || []).length === startsBefore && !/background decoding stopped — profile switch/.test(s.L()), '...and the runner was NOT restarted (no message cut)');
  }
  ok(!/moving the radio/.test(s.L()), '★ the radio was never moved for them');
  ok(Number(cfgOf(a)?.vfo) === 648000, `the listener's dial is untouched (${cfgOf(a)?.vfo})`);
  console.log('── the listener retunes far away: they pause; back: they resume ──');
  a.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  ok(await until(() => /no background VFO is inside the radio's window now|left the radio's window/.test(s.L()), 8000), '★ 7074 kHz: the VFOs left the window and stopped');
  a.send({ type: 'tune', frequency: 648000, mode: 'am' });
  ok(await until(() => (s.L().match(/2 of 2 decoding — with a listener on|came back in/g) || []).length >= 2, 10000), '★ back to 648 kHz: they run again');
  console.log('── a listener\'s decoder with both slots taken: the last VFO pauses, the decoder starts ──');
  await sleep(1500);
  const dx = ws(s.port, '/ws/dxcluster?user_session_id=bgir0001');
  await sleep(600);
  const t0 = Date.now();
  dx.send({ type: 'audio_extension_attach', extension_name: 'navtex' });
  ok(await until(() => dx.has('audio_extension_attached'), 5000) && !dx.has('decoder_refused'), `★ the listener's NAVTEX starts (${Date.now() - t0} ms), not refused`);
  ok(/VFO 2 \(517\.5 kHz NAVTEX\) paused — a listener needed a decoder slot/.test(s.L()), '★ the LAST VFO (2) paused; VFO 1 runs on');
  dx.close(); a.close();
  s.stop(); s = null;

  console.log('── "keep decoding in range" switched off: a listener arriving stops it, as before ──');
  s = await run({ users: 4, rate: 3000000, bgDecode: JSON.stringify({ ...NICK, keepInRange: false }) }, ['--freq', '648000', '--mode', 'am', '--users', '4']);
  ok(await until(() => /profile 1, RF centre 500\.0 kHz, 2 VFO\(s\), 2 decoder\(s\) running/.test(s.L()), 15000), 'nobody here: after the wait it runs at the profile centre');
  const b = ws(s.port, '/ws/user-spectrum?user_session_id=bgir0002&bins=256');
  ok(await until(() => /background decoding stopped — a listener arrived/.test(s.L()), 4000), '★ a listener arrives: stopped (switched off), the dial back');
  b.close(); s.stop(); s = null;

  console.log('── a LOCKED radio: never moved, runs at once, listeners first for its user slots ──');
  s = await run({ users: 3, rate: 2048000, mode: 'locked', lockFreq: 600000, bgDecode: JSON.stringify({ ...NICK,
    profiles: [{ ...NICK.profiles[0], vfos: [...NICK.profiles[0].vfos, { hz: 518500, mode: 'lsb', dec: 'navtex' }] }] }) },
    ['--users', '3'], { VIBESERVER_BG_WAIT_SEC: '300' }, 4);
  ok(await until(() => /profile 1, RF centre 600\.0 kHz, 3 VFO\(s\)/.test(s.L()), 8000), '★ due → runs AT ONCE (the 300 s wait is only for radios it would move), at the locked centre');
  ok(!/moving the radio|retune ->/.test(s.L()), '★ the locked radio\'s centre is never moved');
  ok(/3 VFO\(s\), 3 decoder\(s\) running/.test(s.L()), 'nobody here: all 3 VFOs run (3 user slots free, 4 decoder slots)');
  const l1 = ws(s.port, '/ws/user-spectrum?user_session_id=bgir0003&bins=256');
  ok(await until(() => cfgOf(l1) !== null, 4000), '★ a real listener connects normally — never refused for background');
  ok(await until(() => /VFO 3 \(518\.5 kHz NAVTEX\) paused — a listener took this radio's last user slot/.test(s.L()), 6000),
     '★ 3 user slots, 3 background VFOs: the listener takes one, the LOWEST VFO (3) gives up its channel');
  const info = await fetch(`http://127.0.0.1:${s.port}/vibeserver.json`).then((r) => r.json()).catch(() => ({}));
  const users = info.users ?? info.listeners ?? info.clients;
  ok(users === 1 || users === undefined, `★ the user count shows only the real listener (${JSON.stringify(users)})`);
  ok(!/moving the radio/.test(s.L()) && !/the radio moved to/.test(s.L()), 'still no centre move with a listener on');
  l1.close();
  ok(await until(() => /VFO 3 \(518\.5 kHz NAVTEX\) resumed/.test(s.L()), 8000), '★ the listener leaves: VFO 3 resumes');
  // ★ A VFO outside the locked range is refused at the save, in words.
  const nonce = (await fetch(`http://127.0.0.1:${s.port}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
  const got = await fetch(`http://127.0.0.1:${s.port}/vibeserver/config?${q}`).then((r) => r.json());
  const bad = { ...got, radios: got.radios.map((r) => ({ ...r, bgDecode: JSON.stringify({ ...NICK, profiles: [{ ...NICK.profiles[0], vfos: [{ hz: 1800000, mode: 'usb', dec: 'navtex' }] }] }) })) };
  const n2 = (await fetch(`http://127.0.0.1:${s.port}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q2 = `vs_admin_nonce=${n2}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n2).digest('hex')}`;
  const res = await fetch(`http://127.0.0.1:${s.port}/vibeserver/config?${q2}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(bad) });
  const j = await res.json().catch(() => ({}));
  ok(res.status === 400 && /outside this radio's locked range/.test(j.error || ''), `★ a VFO outside the locked range is refused at the save: "${j.error}"`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  if (s) s.stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
