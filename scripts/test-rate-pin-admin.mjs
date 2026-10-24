#!/usr/bin/env node
// test-rate-pin-admin.mjs — the owner's rate pin and the shared-dial rate rule bind LISTENERS, never the signed-in admin
// (Stuart, 2026-10-24, the Pi 500 V4L in admin mode: "sample rate change isnt working … I am in admin mode so controls
// are available"). The page shows an unlocked admin the picker; the server refused them anyway — a control whose every
// use was a no-op. Real server, fake rtl_tcp, two sockets on a shared dial with the rate pinned:
//   • a LISTENER's sampleRate is refused (the room's rate), and the radio stays where it was;
//   • the ADMIN's goes through, and the radio moves.
// Usage: VIBESERVER_BIN=vibeserver/build/vibeserver node scripts/test-rate-pin-admin.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import fs from 'node:fs'; import net from 'node:net'; import os from 'node:os'; import path from 'node:path';
const root = path.resolve(new URL('..', import.meta.url).pathname);
const BIN = process.env.VIBESERVER_BIN || path.join(root, 'vibeserver/build/vibeserver');
if (!fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const until = async (f, ms) => { const end = Date.now() + ms; while (Date.now() < end) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ratepin-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(T, d));
const rtlPort = await freePort(), port = await freePort(), front = await freePort();
fs.writeFileSync(path.join(T, 'config.json'), JSON.stringify({ configured: true, fullMode: true, sharing: 'local', port: front, rateLock: true,
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: 96100000, rate: 2400000, demodMode: 'wfm', users: 2, rateLock: true }] }));
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(800);
const log = fs.openSync(path.join(T, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--idle-grace', '0'],
  { env: { ...process.env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'), VIBESERVER_RUNTIME_DIR: path.join(T, 'run') }, stdio: ['ignore', log, log] });
const slog = () => { try { return fs.readFileSync(path.join(T, 'server.log'), 'utf8'); } catch { return ''; } };
async function cleanup() { srv.kill(); rtl.kill(); await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }
try {
  await until(async () => { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); return true; } catch { return false; } }, 15000);
  const open = (sid) => { const w = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
    w.txt = []; w.onmessage = (e) => { if (typeof e.data === 'string') w.txt.push(e.data); }; return w; };
  const lis = open(crypto.randomUUID()), adm = open(crypto.randomUUID());
  await Promise.all([lis, adm].map((w) => new Promise((r) => (w.onopen = r))));
  await sleep(1500);
  // ★ the rate the RADIO is at: the server's own "sample rate: N (actual …)" line, the last one
  const rateNow = () => { const m = [...slog().matchAll(/sample rate: (\d+) \(actual|RTL-TCP started: \S+ center=\d+ rate=(\d+)/g)].pop(); return m ? Number(m[1] || m[2]) : 0; };
  const r0 = rateNow();
  ok(r0 > 0, `the radio reports its rate (${r0})`);
  ok(/sample rate: PINNED/.test(slog()), 'the rate is pinned in the owner\'s config');
  lis.send(JSON.stringify({ type: 'sampleRate', value: 1200000 }));
  await sleep(1500);
  ok(/sampleRate ignored — the owner has pinned the rate|sampleRate ignored — the dial is shared/.test(slog()), '★ a LISTENER\'s rate change is refused');
  ok(rateNow() === r0, `  ...and the radio stayed at ${r0}`);
  adm.send(JSON.stringify({ type: 'admin_unlock', nonce: '', token: '' }));
  ok(await until(() => adm.txt.some((t) => t.includes('"type":"admin","ok":true')), 3000), 'the admin signs in (no password set: granted)');
  adm.send(JSON.stringify({ type: 'sampleRate', value: 1200000 }));
  ok(await until(() => rateNow() === 1200000, 6000), `★ the ADMIN's rate change goes through (now ${rateNow()})`);
  lis.close(); adm.close();
} catch (e) { ok(false, 'threw: ' + e.message); }
await cleanup();
console.log(`test-rate-pin-admin: ${fail ? 'FAIL' : 'ok'} — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
