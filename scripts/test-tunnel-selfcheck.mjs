// ★★★ A TUNNEL WHOSE PROCESS LIVES BUT WHOSE ADDRESS IS DEAD IS REPLACED (2026-10-09) — vibeserver/directory.cpp worker().
//
// Stuart: the Sony was "online but not responding all day according to the directory". Measured: cloudflared alive for
// 19 h, its *.trycloudflare.com NXDOMAIN even at 1.1.1.1, and the directory still calling it verified — it re-proves an
// address only when the address CHANGES, and the server reaches the directory over its own network, not the tunnel.
// Nothing ever used the public address, so nothing noticed. The server now fetches its own /vibeserver.json through it.
//
// Here: a stand-in cloudflared that prints a hostname which does not exist and then stays alive (the Sony's state), and
// a stand-in directory on loopback that, like the real one, answers "verified". The server must notice, replace the
// tunnel, and tell the directory the new address. VIBESERVER_PROBE_SEC=3 makes it seconds instead of minutes.
// ★ The real directory is never contacted (VIBESERVER_DIRECTORY_URL accepts loopback only). SILENT: no audio.
// usage: VIBESERVER_BIN=… node scripts/test-tunnel-selfcheck.mjs   (~40 s)
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (good, what) => { console.log(`   ${good ? 'ok' : 'FAIL'}   ${what}`); if (!good) fails++; };

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-selfcheck-'));
for (const d of ['data', 'run', 'bin']) fs.mkdirSync(path.join(dir, d));
// cloudflared is looked for beside the executable first (cloudflaredPath), so the server runs from a copy with the
// stand-in next to it — the real cloudflared is never started.
const bin = path.join(dir, 'bin', 'vibeserver');
fs.copyFileSync(BIN, bin); fs.chmodSync(bin, 0o755);
const starts = path.join(dir, 'cloudflared-starts.txt');
fs.writeFileSync(path.join(dir, 'bin', 'cloudflared'), `#!/bin/sh
# Stand-in: a hostname that does not exist, then stay alive like the Sony's cloudflared did.
n=$(date +%s)$$
echo "$n" >> '${starts}'
echo "INF |  https://selfcheck-dead-$n.trycloudflare.com  |"
trap 'exit 0' TERM INT
while :; do sleep 1; done
`);
fs.chmodSync(path.join(dir, 'bin', 'cloudflared'), 0o755);

// The stand-in directory: registration and pings, recording every address it is told.
const urls = [];
const dirSrv = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    let j = {}; try { j = JSON.parse(body); } catch {}
    if (j.url) urls.push(j.url);
    res.setHeader('Content-Type', 'application/json');
    if (req.url === '/api/directory/register')
      res.end(JSON.stringify({ id: 'selfcheck-id', key: 'k'.repeat(64), slug: 'selfcheck', address: 'selfcheck.example', verified: true, pingSec: 900 }));
    else if (req.url === '/api/directory/ping')
      res.end(JSON.stringify({ ok: true, verified: true, address: 'selfcheck.example', pingSec: 900 }));
    else { res.statusCode = 404; res.end('{}'); }
  });
});
const dirPort = await freePort();
await new Promise((r) => dirSrv.listen(dirPort, '127.0.0.1', r));

const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Selfcheck Test', locator: 'IO92nh', adminPass: 'selfcheck-admin-pass', dirList: true,
  // ★ The listing switch lives on the server half of the current format, which needs the radios array.
  radios: [{ label: 'Selfcheck', enabled: true, configured: true }],
}));
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBESERVER_DIRECTORY_URL: `http://127.0.0.1:${dirPort}`,
              VIBESERVER_PROBE_SEC: '3' };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(bin, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm'],
                  { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} dirSrv.close(); };

const distinct = () => [...new Set(urls.filter((u) => u.includes('.trycloudflare.com')))];
const t0 = Date.now();
while (Date.now() - t0 < 40000 && distinct().length < 2) await sleep(500);
const text = fs.readFileSync(logPath, 'utf8');
const nStarts = fs.existsSync(starts) ? fs.readFileSync(starts, 'utf8').trim().split('\n').filter(Boolean).length : 0;

ok(distinct().length >= 1, `the server listed its first tunnel address (${distinct()[0] || 'none'})`);
ok(/own public address did not answer \(1 of 3\)/.test(text), 'it fetched its own public address and saw it dead');
ok(/its address is dead — replacing it/.test(text), 'after three dead answers it replaced the tunnel');
ok(nStarts >= 2, `cloudflared was started again (${nStarts} starts)`);
ok(distinct().length >= 2, `the directory was told the NEW address (${distinct().length} distinct)`);
stop();
if (fails) { console.log(`\n${fails} FAILED — server log: ${logPath}`); process.exit(1); }
fs.rmSync(dir, { recursive: true, force: true });
console.log('\nall good');
