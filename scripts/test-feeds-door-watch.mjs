// test-feeds-door-watch.mjs — ★★ V12 beta96 follow-up (docs/v12/FEED-TEST.md, bug 1): a feed save made through a RADIO's
// process must reach the FRONT DOOR, which owns the server-level aircraft feeds (vibe_feeds.h: "ONE OWNER PER MACHINE").
//
// ★★★ THE BUG (the Lenovo, 2026-11-27): Stuart saved two feeds in Setup — PiAware (Beast, connect) and UberSDR HFDL (SBS) —
//     and the save went through the radio process on :48001. That process applied what it could and restarted; the feeds
//     belong to the door, whose config.json watch (main.cpp, "every radio process watches it") was skipped in a door
//     (`if (!frontDoorOnly && …)`). The door kept its boot-time "0 sources" until it was restarted by hand.
// THE RULE: a save by ANY process reaches the feed owner within ~2 s; an unchanged config is not re-applied.
// Layout: the Lenovo's — a front door (port P) and one radio process started the way systemd starts vibeserver@<serial>
// (by the test: VIBESERVER_NO_RADIO_SPAWN=1), on a fake rtl_tcp. SILENT: no audio device.
// usage: VIBESERVER_BIN=… node scripts/test-feeds-door-watch.mjs   (exit 3 = not run)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const portFree = (q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); });
const up = async (port) => { for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); };

const ADMIN = 'feeds-door-watch';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-fdw-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 20 && !P; i++) { const p = await freePort(); if ((await Promise.all([p + 1, p + 2].map(portFree))).every(Boolean)) P = p; }
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, name: 'Feeds door watch', adminPass: ADMIN, port: P,
  radios: [
    { serial: 'fdw-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2048000 },
    { serial: 'fdw-rtl2', driver: 'rtlsdr', label: 'RTL two', enabled: true, configured: true, users: 1, rate: 2048000 },
  ],
}));
let rtlPort = 0;
while (!rtlPort || (rtlPort >= P && rtlPort <= P + 2)) rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath,
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const doorLog = path.join(dir, 'door.log'), radioLog = path.join(dir, 'radio.log');
const door = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(doorLog, 'w'), fs.openSync(doorLog, 'a')] });
const radio = spawn(BIN, ['--radio-serial', 'fdw-rtl', '--tcp', `127.0.0.1:${rtlPort}`, '--freq', '96100000', '--mode', 'wfm'],
                    { env, stdio: ['ignore', fs.openSync(radioLog, 'w'), fs.openSync(radioLog, 'a')] });
const stop = () => { for (const p of [door, radio, rtl]) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stop);
const D = () => { try { return fs.readFileSync(doorLog, 'utf8'); } catch { return ''; } };
const count = (t, re) => (t.match(new RegExp(re.source, 'g')) || []).length;
const statusFile = () => { try { return JSON.parse(fs.readFileSync(path.join(dir, 'run', 'aircraft-feeds.json'), 'utf8')); } catch { return null; } };
// ★ The radio's OWN port and its own nonce — exactly the save Stuart made through :48001.
const R = `http://127.0.0.1:${P + 1}`;
const adminQ = async (base) => {
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};
const save = async (base, edit) => {
  await sleep(500); await up(P + 1);   // ★ a radio's save restarts that radio (execv when there is no systemd): wait for it
  const got = await fetch(`${base}/vibeserver/config?${await adminQ(base)}`).then((r) => r.json());
  const res = await fetch(`${base}/vibeserver/config?${await adminQ(base)}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(edit(got)) });
  return res.status;
};

try {
  ok(await up(P) && await up(P + 1), `the front door (${P}) and the radio's own process (${P + 1}) are up`);
  ok(/front door on port/.test(D()), 'the door is a front door');
  ok(await until(() => /\[aircraft feeds\] 0 sources/.test(D()), 8000), 'the door owns the feeds and starts with none');
  const deadPort = await freePort(), hfdlPort = await freePort();
  const FEEDS = [{ fmt: 'beast', dir: 'connect', host: '127.0.0.1', port: deadPort, label: 'adsb', name: 'PiAware' },
                 { fmt: 'sbs', dir: 'listen', port: hfdlPort, label: 'hfdl', name: 'UberSDR HFDL' }];
  const st = await save(R, (c) => ({ ...c, aircraftFeeds: JSON.stringify(FEEDS) }));
  ok(st === 200, `★ two feeds saved THROUGH THE RADIO's process (${st})`);
  const t0 = Date.now();
  const reached = await until(() => { const j = statusFile(); return !!(j && Array.isArray(j.feeds) && j.feeds.length === 2); }, 8000);
  const j = statusFile();
  ok(reached, `★★ the door's status file lists both sources within ${((Date.now() - t0) / 1000).toFixed(1)} s (${JSON.stringify(j && j.feeds && j.feeds.map((f) => f.title))})`);
  ok(!!(j && j.feeds.some((f) => f.title === 'PiAware') && j.feeds.some((f) => f.title === 'UberSDR HFDL' && f.label === 'HFDL')), '…by name and label');
  ok(/\[aircraft feeds\] 2 sources/.test(D()), 'the DOOR applied them (its own log)');
  // ★ the HFDL listener really is open in the door
  ok(await up(hfdlPort), `the HFDL source listens on ${hfdlPort}`);
  // ★ An unrelated save (another radio's label) must not re-apply the feeds.
  const n2 = count(D(), /\[aircraft feeds\] 2 sources/);
  await save(R, (c) => ({ ...c, radios: c.radios.map((r) => (r.serial === 'fdw-rtl2' ? { ...r, label: 'RTL renamed' } : r)) }));
  await sleep(4500);
  ok(count(D(), /\[aircraft feeds\] 2 sources/) === n2, 'a save that does not touch the feeds re-applies nothing');
  // ★ Removing one through the radio: the door follows again.
  await save(R, (c) => ({ ...c, aircraftFeeds: JSON.stringify(FEEDS.slice(1)) }));
  ok(await until(() => { const k = statusFile(); return !!(k && k.feeds && k.feeds.length === 1 && k.feeds[0].title === 'UberSDR HFDL'); }, 8000),
     'one removed through the radio: the door\'s status file follows');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\ntest-feeds-door-watch: ${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs: ${doorLog} ${radioLog})`);
process.exit(fail ? 1 : 0);
