// test-usrp-images-frontdoor.mjs — V12 2026-11-02: a USRP clone's FPGA image uploaded through the Setup page while
// that radio is NOT running (the stock FPGA will not load on a clone, so its process never starts). The page sends
// /r/<serial>/vibeserver/usrp-images; the front door must take it itself (the folder is the machine's), not answer
// "That radio is not answering" (the tester's "Refused (503)"). Every other /r/<serial> path still gets the 503.
//   VIBESERVER_BIN=… node scripts/test-usrp-images-frontdoor.mjs
import crypto from 'node:crypto'; import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import net from 'node:net';
import { spawn } from 'node:child_process';
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((r) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok   ' + m); } else { fail++; console.log('   FAIL ' + m); } };
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-usrpfd-'));
for (const d of ['data', 'run', 'imgs']) fs.mkdirSync(path.join(dir, d));
const P = await freePort(), ADMIN = 'fd-admin-pass', SER = '31A0B7C';
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'USRP door test', adminPass: ADMIN, port: P,
  radios: [{ serial: SER, driver: 'usrp', label: 'USRP', enabled: true, configured: true, users: 4, rate: 4000000 }] }));
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBE_UHD_OVERRIDE_DIR: path.join(dir, 'imgs'),
              VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = path.join(dir, 'door.log');
const door = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(log, 'w'), fs.openSync(log, 'a')] });
const H = `http://127.0.0.1:${P}`;
try {
  for (let i = 0; i < 60; i++) { try { await fetch(`${H}/vibeserver.json`); break; } catch { await sleep(100); } }
  const auth = async () => { const n = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce; return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n).digest('hex')}`; };
  // ★ A synthetic 7-series bitstream: FF padding, the bus-width detect, the sync word, then filler (never a real image).
  const img = Buffer.alloc(2 * 1024 * 1024, 0x5a);
  img.fill(0xff, 0, 0x20); Buffer.from([0, 0, 0, 0xbb, 0x11, 0x22, 0, 0x44]).copy(img, 0x20); img.fill(0xff, 0x28, 0x30); Buffer.from([0xaa, 0x99, 0x55, 0x66]).copy(img, 0x30);
  const r0 = await fetch(`${H}/r/${SER}/vibeserver/usrp-images`);
  ok(r0.status === 401, `no admin password: refused (${r0.status})`);
  const r1 = await fetch(`${H}/r/${SER}/vibeserver/usrp-images?${await auth()}`);
  ok(r1.status === 200, `the listing through /r/${SER} while the radio is down (${r1.status})`);
  const r2 = await fetch(`${H}/r/${SER}/vibeserver/usrp-images?op=put&name=usrp_b210_fpga.bin&${await auth()}`, { method: 'POST', body: img });
  const j2 = await r2.json().catch(() => ({}));
  ok(r2.status === 200 && (j2.files || []).some((f) => f.name === 'usrp_b210_fpga.bin' && /7-series/.test(f.kind)), `the upload is taken by the front door and saved (${r2.status} ${JSON.stringify(j2).slice(0, 120)})`);
  const saved = path.join(dir, 'imgs', 'usrp_b210_fpga.bin');
  ok(fs.existsSync(saved) && fs.readFileSync(saved).equals(img), 'the file is in the owner folder, byte for byte');
  ok(/front door takes the request itself/.test(fs.readFileSync(log, 'utf8')), 'the door logs that it answered for the radio');
  const r3 = await fetch(`${H}/r/${SER}/vibeserver.json`);
  ok(r3.status === 503, `every other path of the down radio still says it is not answering (${r3.status})`);
  const r4 = await fetch(`${H}/r/${SER}/vibeserver/usrp-imagesX?${await auth()}`);
  ok(r4.status === 503, `a look-alike path is not let through (${r4.status})`);
  const r5 = await fetch(`${H}/r/${SER}/vibeserver/usrp-images?op=remove&name=usrp_b210_fpga.bin&${await auth()}`, { method: 'POST' });
  ok(r5.status === 200 && !fs.existsSync(saved), `…and removed the same way (${r5.status})`);
} finally { try { door.kill('SIGTERM'); } catch {} }
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
