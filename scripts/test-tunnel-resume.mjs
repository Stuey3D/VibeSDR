// test-tunnel-resume.mjs — "Start the tunnel again after a reboot" on the REAL server (Stuart, 2026-10-16).
//
// ★★★ WHAT IT PROVES. A never-chosen setting is settled at start and WRITTEN to the file (one rule, two readers):
//     ON on an ordinary server (what it always did), OFF on a Portable, and a Portable owner's old "Resume" (dirResume)
//     carries over. A listed server with the option OFF comes back unlisted after a REBOOT (a new boot id) — and
//     stays listed across a plain restart in the same boot (every settings save is one). With it ON, a reboot keeps
//     the listing.
// ★ Local only: temp configs, a loopback stand-in directory, a stand-in cloudflared beside a COPY of the binary,
//   VIBESERVER_TEST_BOOT_ID for the reboots. SILENT: no audio.
// usage: VIBESERVER_BIN=… node scripts/test-tunnel-resume.mjs
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

let fails = 0;
const ok = (c, m) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${m}`); if (!c) fails++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((r) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });

const dirSrv = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    res.setHeader('Content-Type', 'application/json');
    if (req.url === '/api/directory/register')
      res.end(JSON.stringify({ id: 't-id', key: 'k'.repeat(64), slug: 't', address: 't.example', verified: true, pingSec: 900 }));
    else if (req.url === '/api/directory/ping') res.end(JSON.stringify({ ok: true, verified: true, address: 't.example', pingSec: 900 }));
    else { res.statusCode = 404; res.end('{}'); }
  });
});
const dirPort = await freePort();
await new Promise((r) => dirSrv.listen(dirPort, '127.0.0.1', r));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);

/** One box: its own temp dir, config, binary copy and stand-in cloudflared. */
function box(tag, { pocket = false, extra = {} } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-resume-${tag}-`));
  for (const d of ['data', 'run', 'bin']) fs.mkdirSync(path.join(dir, d));
  const bin = path.join(dir, 'bin', 'vibeserver');
  fs.copyFileSync(BIN, bin); fs.chmodSync(bin, 0o755);
  fs.writeFileSync(path.join(dir, 'bin', 'cloudflared'), "#!/bin/sh\necho 'INF |  https://resume-test.trycloudflare.com  |'\ntrap 'exit 0' TERM INT\nwhile :; do sleep 1; done\n");
  fs.chmodSync(path.join(dir, 'bin', 'cloudflared'), 0o755);
  let PD = '';
  if (pocket) { PD = path.join(dir, 'pocket'); fs.mkdirSync(PD); fs.writeFileSync(path.join(PD, 'enabled'), '1\n'); }
  const cfgPath = path.join(dir, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify(Object.assign({
    configured: true, name: 'Resume Test', locator: 'IO92nh', adminPass: 'resume-admin-pass', dirList: true,
    radios: [{ label: 'Resume', enabled: true, configured: true }],
  }, extra)));
  const cfg = () => JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
  /** Start, wait until the front door has settled the file, stop. */
  async function run(bootId) {
    const port = await freePort();
    const env = { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                  VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBESERVER_DIRECTORY_URL: `http://127.0.0.1:${dirPort}`,
                  VIBESERVER_TEST_BOOT_ID: bootId, HOME: dir };
    if (PD) env.VIBESERVER_POCKET_DIR = PD;
    const log = fs.openSync(path.join(dir, 'server.log'), 'a');
    const srv = spawn(bin, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'], { env, stdio: ['ignore', log, log] });
    for (let i = 0; i < 60; i++) {
      await sleep(200);
      try { if ((await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).ok) break; } catch {}
    }
    srv.kill('SIGTERM');
    await new Promise((r) => { srv.once('exit', r); setTimeout(r, 5000); });
    return cfg();
  }
  return { run, cfg, dir };
}

console.log('── an ordinary server ──');
{
  const b = box('plain');
  let c = await b.run('boot-A');
  ok(c.tunnelResume === true, 'never chosen → settled ON and written to the file (what it always did)');
  c = await b.run('boot-B');
  ok(c.dirList === true, 'ON: still listed after a reboot');
  const raw = b.cfg(); raw.tunnelResume = false; fs.writeFileSync(path.join(b.dir, 'config.json'), JSON.stringify(raw));
  c = await b.run('boot-B');
  ok(c.dirList === true && c.tunnelResume === false, 'OFF, restarted in the same boot (a settings save): still listed');
  c = await b.run('boot-C');
  ok(c.dirList === false, 'OFF, after a reboot: comes back unlisted');
  ok(/restarted — not listed, tunnel stopped/.test(fs.readFileSync(path.join(b.dir, 'server.log'), 'utf8')), '…and says why in its log');
}

console.log('── a Portable ──');
{
  const b = box('pocket', { pocket: true });
  let c = await b.run('boot-A');
  ok(c.tunnelResume === false, 'never chosen → settled OFF on a Portable');
  c = await b.run('boot-B');
  ok(c.dirList === false, 'after a reboot: comes back personal (unlisted)');
  const b2 = box('pocket-kept', { pocket: true, extra: { dirResume: true } });
  c = await b2.run('boot-A');
  ok(c.tunnelResume === true, "a Portable owner's old 'Resume the tunnel after a restart' carries over as ON");
  c = await b2.run('boot-B');
  ok(c.dirList === true, '…so it stays listed after a reboot');
}

try { rtl.kill('SIGTERM'); } catch {}
dirSrv.close();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\nall good');
process.exit(fails ? 1 : 0);
