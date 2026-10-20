// test-admin-diagnostics.mjs — the REAL server: Download diagnostics on the admin API (2026-10-20).
//
// ★★★ Admin only; a plain GET answered with a FILE (Content-Disposition: attachment, text/plain) — what makes a phone's
//     browser, iOS Safari included, save it; and the daemon's own scrub: the admin password, the listener PIN, a radio's
//     PIN and the directory key planted in the file the root helper left are gone from what is downloaded.
// ★ The root helper is not run here (it is root's, and builds the file from the real journal): the test plants its
//   output where VIBESERVER_DIAG_FILE says. vibeserver-diagnostics' own scrub has its own test.
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=…; without one, "not run" (exit 3). SILENT.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const ADMIN = 'diag-Admin-Pass-91', PIN = '583921', RPIN = '660417', DKEY = 'dk_test_9f8e7d6c5b4a';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-diag-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Diag Test', adminPass: ADMIN, pin: PIN,
  radios: [{ serial: 'diag-test', label: 'Test radio', enabled: true, configured: true, users: 2, pin: RPIN }] }));
fs.writeFileSync(path.join(dir, 'data', 'directory.json'), JSON.stringify({ id: 'x', key: DKEY, slug: 'diag', until: 0 }) + '\n');
const DIAG = path.join(dir, 'diagnostics.txt');
fs.writeFileSync(DIAG, `VibeServer diagnostics — test\nconfig adminPass=${ADMIN} pin ${PIN}\nradio pin ${RPIN}\ndirectory ${DKEY}\nordinary line\n`);
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBESERVER_DIAG_FILE: DIAG };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm'],
                  { env, stdio: ['ignore', log, log] });
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  // ★ The admin handlers are registered a few seconds after the port opens (the door's start-up); the page only
  //   offers the button once it has loaded, so this waits the same way.
  await sleep(3000);
  // ★ An unlisted server clears its directory key at start; a LISTED one holds it — written now, as a listed one would.
  fs.writeFileSync(path.join(dir, 'data', 'directory.json'), JSON.stringify({ id: 'x', key: DKEY, slug: 'diag', until: 0 }) + '\n');
  console.log('── Download diagnostics (admin API) ──');
  const base = `http://127.0.0.1:${port}`;
  const proof = async () => {
    const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
    return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
  };
  const r0 = await fetch(`${base}/vibeserver/admin/diagnostics`);
  ok(r0.status === 401, `without the admin password: ${r0.status}, no file`);
  const r1 = await fetch(`${base}/vibeserver/admin/diagnostics?${await proof()}`);
  const body = await r1.text();
  const cd = r1.headers.get('content-disposition') || '';
  ok(r1.status === 200 && /^text\/plain/.test(r1.headers.get('content-type') || ''), `as the owner: 200, text/plain (${r1.status})`);
  ok(/^attachment; filename="vibeserver-diagnostics-[A-Za-z0-9-]+-\d{8}-\d{6}\.txt"$/.test(cd),
     `…as an ATTACHMENT named for the machine and the time — what a phone saves (${cd})`);
  for (const [name, v] of [['the admin password', ADMIN], ['the listener PIN', PIN], ['a radio\'s PIN', RPIN], ['the directory key', DKEY]])
    ok(!body.includes(v), `★★★ ${name} planted in the helper's file is scrubbed by the daemon`);
  ok(body.includes('ordinary line') && body.includes('[redacted]'), '…and the rest of the file is there');
  const st = await fetch(`${base}/vibeserver/admin/status?${await proof()}`).then((r) => r.json());
  if (process.platform === 'linux') ok(String(st.maintenance || '').split(',').includes('diagnostics'), 'Linux: the admin page is told it may offer the button');
  else ok(!String(st.maintenance || '').includes('diagnostics'), `${process.platform}: no maintenance helper, so no button (AGENTS.md: never a dead control)`);
  fs.unlinkSync(DIAG);
  const r2 = await fetch(`${base}/vibeserver/admin/diagnostics?${await proof()}`);
  ok(r2.status === 409, `no file built yet: a sentence, not an empty download (${r2.status})`);
} finally { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); }
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
