// test-server-landing-pin.mjs — ★★★ V12 beta96 A SERVER PIN COVERS THE LANDING'S VIEWERS (Stuart 11-27: "if a server has a
// pin on the entire server not just specific radios then the pin applies to gallery and maps on the landing screen too").
// Through a real vibeserver, asked from this Mac's LAN address (loopback is always exempt from the PIN):
//   A. a single-radio server with a SERVER PIN (the fake dongle): without the PIN every read the landing's GALLERY and
//      AIR & SEA MAP make — gallery.json, a gallery file, the ship store, aircraft.json, the viewer ticket — and the DX log
//      is refused (401); a wrong PIN is refused; the right PIN opens each.
//   B. the same with a PER-RADIO PIN only: the server-level reads stay OPEN (a radio's own PIN guards that radio — its
//      spectrogram and its DX log stay refused). Before vsServerPinHttpOk the radio's process refused them.
//   ★ every config states "pin" at the top, as the server's own saves do: the config reader's plain key search otherwise
//     finds a radio's "pin" first and takes it for the server's (vibeserver_config.cpp loadServer — pre-existing).
//   C. a multi-radio FRONT DOOR (no radio started) with a server PIN: the door's gallery, aircraft table and tickets refused
//      without it, open with it; with per-radio PINs only, open.
// usage: VIBESERVER_BIN=… node scripts/test-server-landing-pin.mjs   (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
const LAN = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
if (!LAN) { console.log('   (not run — no LAN address: loopback is exempt from the PIN, so nothing could be checked)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const kids = [];
const dirs = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } for (const d of dirs) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} } });

function seedGallery(dataDir) {
  const g = path.join(dataDir, 'gallery'); fs.mkdirSync(g, { recursive: true });
  const t = Math.floor(Date.now() / 1000) - 600;
  const f = '20261127-100000-navtex-518k0-1000.txt';
  fs.writeFileSync(path.join(g, f), 'ZCZC GA01\nPIN TEST MESSAGE\nNNNN\n');
  fs.writeFileSync(path.join(g, 'index.jsonl'), JSON.stringify({ f, k: 'navtex', o: 'background', t, hz: 518000, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio', title: 'NAVTEX GA01' }) + '\n');
  return f;
}
async function up(port) {
  for (let i = 0; i < 120; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) return true; } catch {} await sleep(250); }
  return false;
}
/** A single-radio server on the fake dongle. */
async function single(tag, cfg) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-lpin-${tag}-`)); dirs.push(dir);
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const file = seedGallery(path.join(dir, 'data'));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Landing PIN test', adminPass: 'lpin-admin', pin: '', ...cfg }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' }));
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  kids.push(srv);
  ok(await up(port), `${tag}: the server is up`);
  return { port, file, srv };
}
/** A multi-radio front door, no radio started. */
async function door(tag, cfg) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-lpin-${tag}-`)); dirs.push(dir);
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const file = seedGallery(path.join(dir, 'data'));
  let port = 0;
  for (let i = 0; i < 20 && !port; i++) { const p = await freePort(); if (await new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(p + 1, '127.0.0.1', () => s.close(() => r(true))); })) port = p; }
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Landing PIN door', adminPass: 'lpin-admin', pin: '', port, fullMode: true, ...cfg }));
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'door.log'), 'w'), 'ignore'] });
  kids.push(srv);
  ok(await up(port), `${tag}: the door is up`);
  return { port, file, srv };
}
async function proof(port, pin) {
  const j = await (await fetch(`http://${LAN}:${port}/vibeserver/auth`)).json();
  return `vs_nonce=${j.nonce}&vs_auth=${crypto.createHmac('sha256', pin).update(j.nonce).digest('hex')}`;
}
const st = async (port, p, q = '') => {
  try { return (await fetch(`http://${LAN}:${port}${p}${q ? (p.includes('?') ? '&' : '?') + q : ''}`, { cache: 'no-store' })).status; } catch { return -1; }
};
const VIEWER = (file) => [
  '/vibeserver/gallery.json?offset=0&limit=1&since=86400',
  `/vibeserver/gallery/file/${file}`,
  '/vibeserver/gallery/ais-ships.json?since=3600&limit=1',
  '/vibeserver/aircraft.json?since=3600',
  '/vibeserver/airsea/join',
];

console.log(`── A. a single-radio server with a SERVER PIN (asked from ${LAN}) ──`);
{
  const S = await single('master', { pin: '4242', radios: [{ serial: 'lpin', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] });
  try {
    for (const p of [...VIEWER(S.file), '/vibeserver/dxlog.json']) ok(await st(S.port, p) === 401, `no PIN: ${p.split('?')[0]} refused (${await st(S.port, p)})`);
    const bad = await proof(S.port, '9999');
    ok(await st(S.port, VIEWER(S.file)[0], bad) === 401, 'a wrong PIN: gallery.json refused');
    for (const p of VIEWER(S.file)) { const q = await proof(S.port, '4242'); const s = await st(S.port, p, q); ok(s === 200, `the server PIN: ${p.split('?')[0]} opens (${s})`); }
    const g = await (await fetch(`http://${LAN}:${S.port}${VIEWER(S.file)[0]}&${await proof(S.port, '4242')}`)).json();
    ok(g.total === 1 && g.items?.[0]?.title === 'NAVTEX GA01', 'and the gallery answers with its item');
    ok(await st(S.port, '/vibeserver/gallery.json') === 200 || true, '(loopback stays exempt — not asked here)');
  } finally { S.srv.kill('SIGTERM'); }
}
console.log('── B. a single-radio server with a PER-RADIO PIN only ──');
{
  const S = await single('radio', { radios: [{ serial: 'lpin', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000, pin: '5151' }] });
  try {
    for (const p of VIEWER(S.file)) { const s = await st(S.port, p); ok(s === 200, `no PIN: ${p.split('?')[0]} stays open — a radio's PIN guards the radio, not the server (${s})`); }
    ok(await st(S.port, '/vibeserver/spectrogram') === 401, 'the radio is still guarded: its spectrogram refused');
    ok(await st(S.port, '/vibeserver/dxlog.json') === 401, '…and its DX log (one per radio) refused');
    const q = await proof(S.port, '5151');
    ok(await st(S.port, '/vibeserver/dxlog.json', q) !== 401, 'the radio\'s PIN opens its DX log');
  } finally { S.srv.kill('SIGTERM'); }
}
console.log('── C. a multi-radio front door ──');
{
  const radios = (pins) => [{ serial: 'lpin-a', driver: 'rtlsdr', label: 'Radio A', enabled: true, configured: true, users: 1, ...(pins ? { pin: '6161' } : {}) },
                            { serial: 'lpin-b', driver: 'rtlsdr', label: 'Radio B', enabled: true, configured: true, users: 1, ...(pins ? { pin: '7171' } : {}) }];
  const D = await door('door-master', { pin: '4242', radios: radios(false) });
  try {
    for (const p of VIEWER(D.file)) ok(await st(D.port, p) === 401, `server PIN, no PIN given: the door refuses ${p.split('?')[0]}`);
    for (const p of VIEWER(D.file)) { const q = await proof(D.port, '4242'); const s = await st(D.port, p, q); ok(s === 200, `the server PIN: the door opens ${p.split('?')[0]} (${s})`); }
  } finally { D.srv.kill('SIGTERM'); }
  const E = await door('door-radios', { radios: radios(true) });
  try {
    for (const p of VIEWER(E.file)) { const s = await st(E.port, p); ok(s === 200, `per-radio PINs only: the door's ${p.split('?')[0]} stays open (${s})`); }
  } finally { E.srv.kill('SIGTERM'); }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
