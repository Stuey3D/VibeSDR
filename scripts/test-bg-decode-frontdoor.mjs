// test-bg-decode-frontdoor.mjs — BACKGROUND DECODING ON A MULTI-RADIO LINUX BOX: a save made through the FRONT DOOR must
// reach the RADIO's own process (Lenovo, 2026-10-22, 12.0.0~beta2).
//
// ★★★ THE BUG: the setup page saves through whichever process answers it — on a multi-radio box, the front door (it owns
//     no radio). The live apply ran THERE ("background decoding: ON" in the door's log) and the RSP's own process
//     (vibeserver@<serial>) never heard of it: it idle-parked, the keepalive released the radio, Nick got nothing. The
//     fix: every radio process watches config.json and applies its own bgDecode within ~2 s of any save, by any process.
// The layout here is the Lenovo's: a front door (port P) and one process per radio started the way systemd starts
// vibeserver@<serial> (here by the test — VIBESERVER_NO_RADIO_SPAWN=1 — because a front door cannot spawn fake radios).
// The radio is set to RELEASE when idle with no grace, so the hold-off is tested too: once a profile is due, it is not
// released. SILENT: fake rtl_tcp, no audio device. usage: VIBESERVER_BIN=… node scripts/test-bg-decode-frontdoor.mjs
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

const ADMIN = 'bgfd-admin';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgfd-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
// ★ The door's port and the two radios behind it (portForRadio: P+1, P+2) — three free ports in a row.
let P = 0;
for (let i = 0; i < 20 && !P; i++) {
  const p = await freePort();
  const free = await Promise.all([p + 1, p + 2].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
  if (free.every(Boolean)) P = p;
}
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Front door test', adminPass: ADMIN, port: P,
  radios: [
    { serial: 'bgfd-rsp', driver: 'sdrplay', label: 'RSP1A', enabled: true, configured: true, users: 4, rate: 2048000,
      releaseWhenIdle: true, idleGrace: 0 },
    { serial: 'bgfd-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2048000 },
  ],
}));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_BG_WAIT_SEC: '3',
              VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const doorLog = path.join(dir, 'door.log'), radioLog = path.join(dir, 'radio.log');
const door = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(doorLog, 'w'), fs.openSync(doorLog, 'a')] });
// ★ The radio's process exactly as vibeserver@bgfd-rsp is started — by serial — on a fake rtl_tcp in place of the RSP.
const radio = spawn(BIN, ['--radio-serial', 'bgfd-rsp', '--tcp', `127.0.0.1:${rtlPort}`, '--freq', '7074000', '--mode', 'usb'],
                    { env, stdio: ['ignore', fs.openSync(radioLog, 'w'), fs.openSync(radioLog, 'a')] });
const stop = () => { for (const p of [door, radio, rtl]) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stop);
const up = async (port) => { for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const R = () => fs.readFileSync(radioLog, 'utf8'), D = () => fs.readFileSync(doorLog, 'utf8');
const count = (t, re) => (t.match(new RegExp(re.source, 'g')) || []).length;
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };
const H = `http://127.0.0.1:${P}`;
const adminQ = async () => {
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};

try {
  ok(await up(P) && await up(P + 1), `the front door (${P}) and the RSP's own process (${P + 1}) are up`);
  ok(/front door on port/.test(D()), 'the door is a front door');
  // ★ A fake rtl_tcp cannot be RELEASED (that is a USB device's teardown), so the beta2 state shows here as the park
//   being armed; on the Lenovo the same armed park released the RSP. The hold-off is checked below on the park itself.
ok(await until(() => /no listeners — idle park in/.test(R()), 20000), 'before the save, the idle radio arms its park / release as configured (the beta2 state)');

  const NICK = { on: true, waitMin: 1, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
    vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
  const got = await fetch(`${H}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  const body = { ...got, radios: got.radios.map((r) => (r.serial === 'bgfd-rsp' ? { ...r, bgDecode: JSON.stringify(NICK) } : r)) };
  const res = await fetch(`${H}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  ok(res.status === 200, `★ saved THROUGH THE FRONT DOOR (${res.status})`);
  const releasedBefore = count(R(), /radio RELEASED/);

  ok(await until(() => /background decoding re-read from/.test(R()) && /background decoding: ON, wait 60 s, 1 profile\(s\)/.test(R()), 6000),
     '★★ the RADIO\'s process picked the save up from config.json (ON, wait 60 s — waitMin 1)');
  ok(!/background decoding: ON/.test(D()), '...and the front door, which owns no radio, did not take it on');
  ok(await until(() => /profile 1, RF centre 500\.0 kHz, 2 VFO\(s\), 2 decoder\(s\) running/.test(R()), 15000),
     '★★ the radio runs Nick\'s profile (the test wait, 3 s): 500 kHz, two NAVTEX decoders');
  const releasedAtStart = count(R(), /radio RELEASED/);
  await sleep(8000);
  ok(count(R(), /radio RELEASED/) === releasedAtStart, `★ while a profile is due and running, the radio is NOT released (${releasedAtStart - releasedBefore} release(s) before it started, none since)`);
  ok(count(R(), /background decoding stopped/) === 0, '...and it keeps running with nobody there');
  {
    const t = R(), from = t.indexOf('background decoding: ON, wait 60 s');
    ok(from > 0 && !/idle park in|radio RELEASED/.test(t.slice(from)), '★★ from the moment the profile was due, no park or release was armed (the Lenovo\'s RSP keepalive release cannot happen)');
  }

  // ★ An unrelated save (the other radio) must not restart it.
  const got2 = await fetch(`${H}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  const body2 = { ...got2, radios: got2.radios.map((r) => (r.serial === 'bgfd-rtl' ? { ...r, label: 'RTL two' } : r)) };
  await fetch(`${H}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body2) });
  await sleep(4000);
  ok(count(R(), /profile 1, RF centre 500\.0 kHz/) === 1 && count(R(), /background decoding stopped/) === 0,
     'a save that does not touch this radio\'s profile restarts nothing');

  // ★ Switched off through the door: the radio stops and puts its dial back.
  const got3 = await fetch(`${H}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  const body3 = { ...got3, radios: got3.radios.map((r) => (r.serial === 'bgfd-rsp' ? { ...r, bgDecode: JSON.stringify({ ...NICK, on: false }) } : r)) };
  await fetch(`${H}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body3) });
  ok(await until(() => /background decoding stopped — switched off/.test(R()) && /the radio is back on 7074\.000 kHz/.test(R()), 8000),
     '★ switched off through the door: the radio stops and goes back to 7074.000 kHz');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs: ${radioLog} ${doorLog})`);
process.exit(fail ? 1 : 0);
