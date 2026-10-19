// ★★★ A TUNNEL A PREVIOUS PROCESS LEFT BEHIND IS RETIRED, NOT JOINED (2026-10-19) — vibeserver/directory.cpp startTunnel().
//
// Kiko (N270): after "Restart server" on the web panel there were FOUR cloudflared processes for two radios; the old
// ones held the address, so the server could not register and dropped off the directory until a reboot. stopTunnel()
// only kills what THIS process started, and a fresh process has started nothing — so whatever a previous one left (the
// self-restart's execv runs no teardown; a crash or a kill runs none) lived on beside the new tunnel.
//
// Here: a stand-in cloudflared beside a copy of the server, a stand-in directory on loopback. Start, kill the server
// HARD so its tunnel is orphaned, start it again on the same port: exactly one tunnel must be left. A second server on
// another port keeps its own tunnel throughout — the sweep is by OUR port only.
// ★ The real directory and the real cloudflared are never contacted. SILENT: no audio.
// usage: VIBESERVER_BIN=… node scripts/test-tunnel-orphan.mjs   (~30 s)
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (good, what) => { console.log(`   ${good ? 'ok' : 'FAIL'}   ${what}`); if (!good) fails++; };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-orphan-'));
for (const d of ['data', 'data2', 'run', 'run2', 'bin']) fs.mkdirSync(path.join(dir, d));
// cloudflared is looked for beside the executable first (cloudflaredPath), so the server runs from a copy with the
// stand-in next to it.
const bin = path.join(dir, 'bin', 'vibeserver');
fs.copyFileSync(BIN, bin); fs.chmodSync(bin, 0o755);
const standIn = path.join(dir, 'bin', 'cloudflared');
fs.writeFileSync(standIn, `#!/bin/sh
echo "INF |  https://orphan-$(date +%s)$$.trycloudflare.com  |"
trap 'exit 0' TERM INT
while :; do sleep 1; done
`);
fs.chmodSync(standIn, 0o755);

/** The stand-in tunnels running for a port — the script itself, not popen's `sh -c` wrapper around it. */
const tunnels = (p) => {
  let out = '';
  try { out = execFileSync('ps', ['-Ao', 'pid,args'], { encoding: 'utf8' }); } catch { return []; }
  return out.split('\n').filter((l) => l.includes(`${standIn} tunnel`) && l.includes(`127.0.0.1:${p} `) && !/ -c /.test(l));
};

const registered = [];
const dirSrv = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    let j = {}; try { j = JSON.parse(body); } catch {}
    // ★ Any call carrying an address: a restarted server with a saved listing re-publishes by ping, not register.
    if (j.url) registered.push(j.url);
    res.setHeader('Content-Type', 'application/json');
    if (req.url === '/api/directory/register')
      res.end(JSON.stringify({ id: 'orphan-id', key: 'k'.repeat(64), slug: 'orphan', address: 'orphan.example', verified: true, pingSec: 900 }));
    else if (req.url === '/api/directory/ping')
      res.end(JSON.stringify({ ok: true, verified: true, address: 'orphan.example', pingSec: 900 }));
    else { res.statusCode = 404; res.end('{}'); }
  });
});
const dirPort = await freePort();
await new Promise((r) => dirSrv.listen(dirPort, '127.0.0.1', r));

const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const cfg = (n) => {
  const f = path.join(dir, `config${n}.json`);
  fs.writeFileSync(f, JSON.stringify({ configured: true, name: `Orphan Test ${n}`, locator: 'IO92nh', adminPass: 'orphan-admin-pass', dirList: true,
                                       radios: [{ label: `Orphan ${n}`, enabled: true, configured: true }] }));
  return f;
};
const envFor = (n) => ({ ...process.env, VIBESERVER_CONFIG: cfg(n), VIBESERVER_DATA_DIR: path.join(dir, n === 1 ? 'data' : 'data2'),
                         VIBESERVER_RUNTIME_DIR: path.join(dir, n === 1 ? 'run' : 'run2'), VIBESERVER_DIRECTORY_URL: `http://127.0.0.1:${dirPort}` });
const log = fs.openSync(path.join(dir, 'server.log'), 'a');
const start = (n, p) => spawn(bin, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(p), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm'],
                              { env: envFor(n), stdio: ['ignore', log, log] });
const waitFor = async (cond, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms && !cond()) await sleep(250); return cond(); };

const port = await freePort(), other = await freePort();
let srv = null, srv2 = null;
try {
  srv2 = start(2, other);
  srv = start(1, port);
  ok(await waitFor(() => tunnels(port).length === 1 && tunnels(other).length === 1, 20000),
     `each server starts one tunnel (${tunnels(port).length} on ours, ${tunnels(other).length} on the other)`);

  // A teardown that never runs — the shape of an execv restart, a crash or a kill.
  srv.kill('SIGKILL');
  await sleep(1500);
  ok(tunnels(port).length === 1, `killed hard, the server leaves its tunnel behind (${tunnels(port).length} orphaned)`);

  const before = registered.length;
  srv = start(1, port);
  ok(await waitFor(() => registered.length > before, 20000), 'the restarted server publishes its address again');
  await sleep(1500);
  ok(tunnels(port).length === 1, `★ exactly ONE tunnel on its port afterwards — the orphan retired (${tunnels(port).length})`);
  ok(tunnels(other).length === 1, `★ the other server's tunnel was never touched (${tunnels(other).length})`);
} finally {
  for (const s of [srv, srv2]) { try { s?.kill('SIGTERM'); } catch {} }
  await sleep(1500);
  // Anything still standing — the test's own stand-ins only, matched by their unique path.
  for (const l of [...tunnels(port), ...tunnels(other)]) { try { process.kill(Number(l.trim().split(/\s+/)[0]), 'SIGTERM'); } catch {} }
  try { rtl.kill('SIGTERM'); } catch {}
  dirSrv.close();
}
if (fails) { console.log(`\n${fails} FAILED — server log: ${path.join(dir, 'server.log')}`); process.exit(1); }
fs.rmSync(dir, { recursive: true, force: true });
console.log('\nall good');
