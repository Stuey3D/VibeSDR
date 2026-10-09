// ★★ BULK BOOKMARK IMPORT, END TO END (2026-10-09) — the real vibeserver's POST /bookmarks/import. Stuart imported the
// 1,478-row UK ATC list one POST per row and it took minutes: every row rewrote the whole bookmark file.
//   1. admin-only (401 without the password);
//   2. a large file in ≤60 KB batches lands every row, with mode and passband, and saves once per batch;
//   3. re-importing the same rows adds nothing (keyed by frequency, as the one-row write is);
//   4. a refusal from THIS route always has a JSON body — an EMPTY 400 is how the app recognises an older server
//      (which treats the path as a one-row POST with no frequency) and falls back to one POST per row;
//   5. oversized and non-array bodies are refused.
// ★ SILENT: no audio. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-bookmark-import.mjs
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }

const PASS = 'bm-import-admin-pass';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bmimp-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Bookmark Import Test', locator: 'IO92nh', adminPass: PASS,
  radios: [{ serial: 'bmimp-test', label: 'Test radio', enabled: true, configured: true, users: 3 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
                  { env, stdio: ['ignore', log, log] });
const base = `http://127.0.0.1:${port}`;
const auth = async () => {
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
// ★ The host IS the operator (vsAdminHttpOk lets loopback through, as for the one-row write), so the refusal is checked
//   as a LAN visitor: this machine's own LAN address is not loopback to the server. No LAN address → that check is skipped.
const lanIp = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
const post = async (body, q = '', visitor = false) => {
  const host = visitor ? lanIp : '127.0.0.1';
  const r = await fetch(`http://${host}:${port}/bookmarks/import${q ? `?${q}` : ''}`, { method: 'POST', body, headers: { 'Content-Type': 'application/json' } });
  const text = await r.text();
  return { status: r.status, text, json: (() => { try { return JSON.parse(text); } catch { return null; } })() };
};

try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }

  // The UK ATC list's shape: AM, ±8.5 kHz, 25 kHz raster from 118 MHz and 225 MHz.
  const rows = [];
  for (let i = 0; i < 760; i++) rows.push({ name: `CIVIL ${i} / APPROACH`, frequency: 118_000_000 + i * 25_000, mode: 'am', bandwidth_low: -8500, bandwidth_high: 8500 });
  for (let i = 0; i < 718; i++) rows.push({ name: `MIL ${i} TOWER`, frequency: 225_025_000 + i * 25_000, mode: 'am', bandwidth_low: -8500, bandwidth_high: 8500 });

  if (lanIp) {
    const unauth = await post(JSON.stringify(rows.slice(0, 2)), '', true);
    ok(unauth.status === 401, `admin-only: a LAN visitor without the password gets ${unauth.status}`);
    const withPw = await post(JSON.stringify([]), await auth(), true);
    ok(withPw.status === 200 && withPw.json?.imported === 0, `…and the same visitor WITH it is let in (${withPw.status})`);
  } else console.log('   (no LAN address — the admin refusal is not checked)');

  // Batch exactly as the app does: as many rows as fit in 60 KB.
  const batches = [];
  for (let cur = []; rows.length || cur.length;) {
    const r = rows.shift();
    if (r && JSON.stringify([...cur, r]).length <= 60_000) { cur.push(r); continue; }
    batches.push(cur); cur = r ? [r] : [];
    if (!r) break;
  }
  const t0 = Date.now();
  let imported = 0, last = null;
  for (const b of batches) { const r = await post(JSON.stringify(b), await auth()); imported += r.json?.imported ?? 0; last = r; }
  const ms = Date.now() - t0;
  ok(imported === 1478, `1,478 rows in ${batches.length} batches, ${ms} ms: ${imported} imported`);
  ok(Array.isArray(last?.json?.bookmarks) && last.json.bookmarks.length === 1478, `the reply carries the whole list (${last?.json?.bookmarks?.length})`);
  const list = await fetch(`${base}/bookmarks`).then((r) => r.json());
  const any = list.find((b) => b.name === 'MIL 0 TOWER');
  ok(list.length === 1478, `GET /bookmarks has them all (${list.length})`);
  ok(any && any.frequency === 225_025_000 && any.mode === 'am', `exact frequency and mode kept: ${JSON.stringify(any)}`);
  ok(any && (any.bandwidth_low === -8500 && any.bandwidth_high === 8500), `passband kept: ${any?.bandwidth_low}..${any?.bandwidth_high}`);
  const saved = JSON.parse(fs.readFileSync(fs.readdirSync(path.join(dir, 'data'), { recursive: true })
    .map((f) => path.join(dir, 'data', f)).find((f) => /bookmark/i.test(f) && f.endsWith('.json')) ?? '/dev/null', 'utf8') || '[]');
  ok(saved.length === 1478, `persisted to disk (${saved.length})`);

  const again = await post(JSON.stringify([{ name: 'MIL 0 TOWER', frequency: 225_025_000, mode: 'am' }]), await auth());
  const after = await fetch(`${base}/bookmarks`).then((r) => r.json());
  ok(again.json?.imported === 1 && after.length === 1478, `re-importing a row replaces it, adds nothing (${after.length})`);

  const bad = await post('{"not":"an array"}', await auth());
  ok(bad.status === 400 && bad.text.length > 0 && bad.json?.error, `a non-array body: 400 WITH a JSON body (${bad.text})`);
  const big = await post('[' + ' '.repeat(70_000) + ']', await auth());
  ok(big.status === 413 && big.json?.error, `over 60 KB: ${big.status}`);
  const skip = await post(JSON.stringify([{ name: '', frequency: 1 }, { name: 'x', frequency: -5 }, 'junk', { name: 'OK ROW', frequency: 7_100_000, mode: 'LSB!' }]), await auth());
  ok(skip.json?.imported === 1, `unusable rows are skipped, the rest land (${skip.json?.imported})`);

  // What an OLDER server says to this request: the one-row route, no frequency → an EMPTY 400. Same server, same route.
  const legacy = await fetch(`${base}/bookmarks?${await auth()}`, { method: 'POST' });
  const legacyText = await legacy.text();
  ok(legacy.status === 400 && legacyText === '', `the older-server signal is an EMPTY 400 (${legacy.status}, ${legacyText.length} bytes)`);
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-bookmark-import: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
