// test-server-lanonly.mjs — ★★★ beta96 VIBESERVER SIMPLE: HOME NETWORK ONLY, ENFORCED BY THE ENGINE (Stuart 11-26).
//
// The Android app's VibeServer Simple runs with `lanOnly` (src/utils/serverChoice.ts LAN_ONLY_OFF → VibeServerBoot →
// LocalSdrShim::setVibeServerLanOnly). The engine then refuses every client that is not on the home network with the
// RTL-TCP door's own rule (isPrivateIp: private IPv4, 169.254 link-local, IPv6 link-local / ULA, the box's own prefixes).
// The same shape as test-server-rtltcp.mjs §3/§4: a real vibeserver (here with --lan-only, the daemon's twin of the app's
// switch) and VIBE_TEST_LANONLY_PEER pretending where the client is:
//   · 8.8.8.8 (public) → 403 on the page AND on /vibeserver.json, with the plain reason; the log says it once;
//   · 169.254.10.20 (link-local) and 192.168.1.20 → served;
//   · fd00::5 (IPv6 ULA) → served;
//   · without --lan-only the public peer is served (Full / the daemon exactly as before);
//   · a request that came THROUGH the tunnel (a trusted proxy naming a visitor) is refused even from loopback.
// ★ SILENT: no audio. Loopback only.
// usage: VIBESERVER_BIN=… node scripts/test-server-lanonly.mjs   (~20 s)
import net from 'node:net';
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

async function server({ env = {}, args = [], label, extraCfg = {} }) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lanonly-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'LAN only test', adminPass: 'lan-only-admin',
    radios: [{ serial: 'lanonly', label: 'Test radio', enabled: true, configured: true, users: 1 }], ...extraCfg }));
  const fakePort = await freePort(), port = await freePort();
  const fake = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(fakePort), '--rate', '2400000',
                                        '--tones', '0'], { stdio: 'ignore' });
  await sleep(300);
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${fakePort}`, '--port', String(port), '--rate', '2400000', '--freq', '100000000',
                          '--mode', 'wfm', '--users', '1', ...args],
                    { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                             VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...env },
                      stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const get = async (p, headers = {}) => {
    try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { headers, signal: AbortSignal.timeout(3000) }); return { status: r.status, text: await r.text() }; }
    catch { return null; }
  };
  for (let i = 0; i < 100; i++) { const r = await get('/vibeserver.json'); if (r) break; await sleep(100); }
  await sleep(300);
  console.log(`\n── ${label}`);
  return { get, L: () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } },
           stop: () => { try { srv.kill('SIGTERM'); } catch {} try { fake.kill(); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}

// ── 1. A public client is refused (page and JSON), with the reason, logged ──
{
  const S = await server({ label: 'lan-only, a client from a public address (8.8.8.8)', args: ['--lan-only'], env: { VIBE_TEST_LANONLY_PEER: '8.8.8.8' } });
  try {
    const page = await S.get('/'), js = await S.get('/vibeserver.json');
    ok(page?.status === 403 && /local network only/.test(page.text), `the page: 403 with the reason (${page?.status})`);
    ok(js?.status === 403, `vibeserver.json: 403 too — nothing answers (${js?.status})`);
    ok(/local network only: refused 8\.8\.8\.8/.test(S.L()), 'the refusal is in the log');
  } finally { S.stop(); }
}
// ── 2. Home-network clients are served ──
for (const peer of ['192.168.1.20', '169.254.10.20', '10.1.2.3', 'fd00::5']) {
  const S = await server({ label: `lan-only, a client from ${peer}`, args: ['--lan-only'], env: { VIBE_TEST_LANONLY_PEER: peer } });
  try {
    const js = await S.get('/vibeserver.json');
    let j = null; try { j = JSON.parse(js?.text || ''); } catch {}
    ok(js?.status === 200 && !!j, `served (${js?.status})`);
    ok(j?.lanOnly && j.lanOnly.refused === 0, `vibeserver.json says local network only, 0 refused (${JSON.stringify(j?.lanOnly)})`);
  } finally { S.stop(); }
}
// ── 3. Without the switch: exactly as before ──
{
  const S = await server({ label: 'no --lan-only (Full / the daemon), a public client', env: { VIBE_TEST_LANONLY_PEER: '8.8.8.8' } });
  try {
    const js = await S.get('/vibeserver.json');
    let j = null; try { j = JSON.parse(js?.text || ''); } catch {}
    ok(js?.status === 200 && j && !('lanOnly' in j), `served, and no lanOnly in vibeserver.json (${js?.status})`);
  } finally { S.stop(); }
}
// ── 4. Through the tunnel (a trusted proxy on loopback naming a visitor) — refused even though the socket is loopback ──
{
  const S = await server({ label: 'lan-only, a request through the tunnel (X-Forwarded-For from a trusted loopback proxy)',
                           args: ['--lan-only', '--trusted-proxies', '127.0.0.1'] });
  try {
    const plain = await S.get('/vibeserver.json');
    ok(plain?.status === 200, `loopback itself is served (${plain?.status})`);
    const tun = await S.get('/vibeserver.json', { 'X-Forwarded-For': '81.2.69.160' });
    ok(tun?.status === 403, `through the tunnel: refused (${tun?.status})`);
  } finally { S.stop(); }
}

console.log(`\n${fail ? 'FAILED' : 'PASS'}: ${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
