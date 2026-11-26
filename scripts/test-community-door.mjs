#!/usr/bin/env node
// test-community-door.mjs — ★★ beta95 THE FRONT DOOR'S ONE MERGED COMMUNITY FEED (Stuart's Pi 500, 11-25: the door's
// /vibeserver/community.json answered only what the FIRST radio (the RSP1B, DSC only) allowed; the V4Ls' layers were only at
// /r/<id>/vibeserver/community.json, so the directory had to read every radio). A multi-radio machine's front door (it owns no
// radio, so it took the first radio's blocked modes) now answers for EVERY radio: a layer is shared when any radio may share
// it. The rows are the machine's stores (gallery, ship store, aircraft table, remembered rows) — one per item:
//   A. radio 1 blocks the aircraft modes, DSC, NDB; radio 2 blocks nothing → DSC and APRS are at the door.
//   B. two radios filed the SAME DSC call (same title + position, 40 s apart) → one row; a different call stays.
//   C. two radios remember the SAME APRS station → one row, the newest.
//   D. the owner then blocks DSC on radio 2 too (a save through the door) → DSC leaves the door's feed; APRS stays.
// ★ SILENT: no radio, no audio (VIBESERVER_NO_RADIO_SPAWN). usage: VIBESERVER_BIN=… node scripts/test-community-door.mjs
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-commdoor-'));
let door = null;
const cleanup = () => { try { door && door.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} };
process.on('exit', cleanup);
const deadline = setTimeout(() => { console.log('   FAIL deadline (90 s)'); process.exit(1); }, 90000);

const ADMIN = 'cd-admin';
const now = Math.floor(Date.now() / 1000);
for (const d of ['data/gallery/.community', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
let P = 0;
for (let i = 0; i < 20 && !P; i++) {
  const p = await freePort();
  const free = await Promise.all([p + 1, p + 2].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
  if (free.every(Boolean)) P = p;
}
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, name: 'Door feed test', adminPass: ADMIN, port: P, communityMap: true, galleryPublic: true, galleryMb: 50,
  radios: [
    { serial: 'cd-rsp', driver: 'sdrplay', label: 'RSP1B', enabled: true, configured: true, users: 4, rate: 2048000,
      blockedModes: 'adsb,avdata,hfdl,dsc,ndb' },
    { serial: 'cd-v4l', driver: 'rtlsdr', label: 'V4L', enabled: true, configured: true, users: 4, rate: 2400000, blockedModes: '' },
  ],
}));
// ── the machine's gallery, as two radios wrote it ──
const gdir = path.join(dir, 'data', 'gallery');
const stamp = (t) => new Date(t * 1000).toISOString().replace(/[-:]/g, '').slice(0, 15) + 'Z';
let index = '';
const add = (it, body) => { fs.writeFileSync(path.join(gdir, it.f), body); index += JSON.stringify(it) + '\n'; };
const TITLE = 'DSC DISTRESS from 235012345 · fire, explosion · 50°45.00\'N 001°15.00\'W';
add({ f: `${stamp(now - 640)}-dsc-2187k5-a.txt`, k: 'dsc', o: 'background', t: now - 640, hz: 2187500, m: 'usb', d: 'dsc', r: 'RSP1B', title: TITLE, lv: 3, plat: 50.75, plon: -1.25 }, 'a\n');
add({ f: `${stamp(now - 600)}-dsc-2187k5-b.txt`, k: 'dsc', o: 'background', t: now - 600, hz: 2187500, m: 'usb', d: 'dsc', r: 'V4L', title: TITLE, lv: 3, plat: 50.75, plon: -1.25 }, 'b\n');
add({ f: `${stamp(now - 300)}-dsc-2187k5-c.txt`, k: 'dsc', o: 'background', t: now - 300, hz: 2187500, m: 'usb', d: 'dsc', r: 'V4L', title: 'DSC ROUTINE from 232001234', lv: 1, plat: 51.0, plon: 1.5 }, 'c\n');
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);
// ── the remembered APRS rows of two radios: the same station from both ──
fs.writeFileSync(path.join(gdir, '.community', 'live-p-radioA.json'),
  JSON.stringify([['G4ABC-9', 'G4ABC-9', 52.2, -1.3, now - 900, '/>', 'position', 'older copy']]));
fs.writeFileSync(path.join(gdir, '.community', 'live-p-radioB.json'),
  JSON.stringify([['G4ABC-9', 'G4ABC-9', 52.21, -1.31, now - 120, '/>', 'position', 'newer copy'], ['M0XYZ', 'M0XYZ', 51.5, -0.1, now - 200, '/-', 'position', 'home']]));

const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const doorLog = path.join(dir, 'door.log');
door = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(doorLog, 'w'), fs.openSync(doorLog, 'a')] });
const H = `http://127.0.0.1:${P}`;
const getC = async () => { try { const r = await fetch(`${H}/vibeserver/community.json`, { headers: { 'cache-control': 'no-cache' } }); return r.status === 200 ? await r.json() : { status: r.status }; } catch { return null; } };
const until = async (f, ms = 15000) => { for (let t = 0; t < ms; t += 250) { const v = await f(); if (v) return v; await sleep(250); } return f(); };

try {
  const j = await until(async () => { const x = await getC(); return x && x.v ? x : null; });
  ok(!!j && /front door on port/.test(fs.readFileSync(doorLog, 'utf8')), `the door answers community.json (${JSON.stringify(j && j.layers)})`);
  ok(/the front door answers for 2 radios/.test(fs.readFileSync(doorLog, 'utf8')), 'the door knows both radios');
  console.log('── A. a layer any radio allows ──');
  ok(j && Array.isArray(j.d) && (j.layers || []).includes('dsc'), 'DSC is at the door though the first radio blocks it (the V4L allows it)');
  ok(j && Array.isArray(j.p) && (j.layers || []).includes('aprs'), 'APRS is at the door');
  console.log('── B. the same DSC call from two radios: one row ──');
  ok(j && j.d && j.d.length === 2, `two calls, not three (${j && j.d && j.d.length}: ${JSON.stringify(j && j.d && j.d.map((r) => r[4]))})`);
  ok(j && j.d && j.d.some((r) => String(r[5]).endsWith('-b.txt')) && !j.d.some((r) => String(r[5]).endsWith('-a.txt')), 'the newer copy is the one kept');
  console.log('── C. the same APRS station from two radios: one row, the newest ──');
  const g = (j && j.p || []).filter((r) => r[0] === 'G4ABC-9');
  ok(g.length === 1 && g[0][7] === 'newer copy', `G4ABC-9 once, the newer copy (${JSON.stringify(g)})`);
  ok((j && j.p || []).some((r) => r[0] === 'M0XYZ'), 'the other station stays');

  console.log('── D. DSC blocked on every radio: it leaves the door\'s feed ──');
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
  const got = await fetch(`${H}/vibeserver/config?${q}`).then((r) => r.json());
  const body = { ...got, radios: got.radios.map((r) => (r.serial === 'cd-v4l' ? { ...r, blockedModes: 'dsc' } : r)) };
  const nonce2 = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q2 = `vs_admin_nonce=${nonce2}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce2).digest('hex')}`;
  const st = await fetch(`${H}/vibeserver/config?${q2}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }).then((r) => r.status);
  ok(st === 200, `the owner's save through the door (${st})`);
  const j2 = await until(async () => { const x = await getC(); return x && x.v && !x.d ? x : null; }, 10000);
  ok(!!j2 && !j2.d && !(j2.layers || []).includes('dsc'), `no DSC once both radios block it (${JSON.stringify(j2 && j2.layers)})`);
  ok(!!j2 && Array.isArray(j2.p), 'APRS stays');
} finally {
  clearTimeout(deadline);
  try { door.kill('SIGTERM'); } catch {}
  await sleep(300);
}
console.log(`\n${pass}/${pass + fail} checks passed`);
process.exit(fail ? 1 : 0);
