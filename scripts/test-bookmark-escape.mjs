// ★★★ A BOOKMARK NAME SURVIVES A RESTART UNCHANGED (2026-10-19). The loader took a name RAW up to the next quote while
// the writer escapes every backslash and quote, so a name with a `\` doubled its backslashes on every save — and the list
// saves on every change. On the Sony (Lite 77) the file grew until loading it at start took longer than anyone waited:
// the server "refused to start" with a brand-new dongle, spinning inside setBookmarksPath before the radio was touched.
//   1. names with `\` and `"` come back EXACTLY, across two restarts, and the file does not grow;
//   2. a file the old loader had already bloated loads at once, every name cut back to 64 bytes;
//   3. a file past 16 MB is set aside (kept, renamed .oversize) and the server starts with none.
// ★ SILENT: no audio. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-bookmark-escape.mjs
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

const PASS = 'bm-escape-admin-pass';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bmesc-'));
const dataDir = path.join(dir, 'data');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Bookmark Escape Test', locator: 'IO92nh', adminPass: PASS,
  radios: [{ serial: 'bmesc-test', label: 'Test radio', enabled: true, configured: true, users: 3 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: dataDir, VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const base = `http://127.0.0.1:${port}`;
let srv = null;

const up = () => new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
/** Start the server; resolves with how long it took to answer, or -1 if it never did. */
async function start() {
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
              { env, stdio: ['ignore', log, log] });
  const t0 = Date.now();
  for (let i = 0; i < 150; i++) { if (await up()) return Date.now() - t0; await sleep(100); }
  return -1;
}
async function stop() {
  if (!srv) return;
  const s = srv; srv = null;
  const gone = new Promise((r) => s.once('exit', r));
  s.kill('SIGTERM');
  await Promise.race([gone, sleep(10_000)]);
  for (let i = 0; i < 50 && await up(); i++) await sleep(100);
}
const auth = async () => {
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
const list = () => fetch(`${base}/bookmarks`).then((r) => r.json());
/** The file the server keeps them in — one per radio process, so find it rather than assume its name. */
const bmFile = () => fs.readdirSync(dataDir).filter((f) => /^bookmarks.*\.json$/.test(f)).map((f) => path.join(dataDir, f))
  .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];

const NAMES = [
  { name: 'BACK\\SLASH FM', frequency: 88_100_000 },
  { name: 'SAY "HELLO"', frequency: 89_200_000 },
  { name: 'BOTH \\" AT ONCE', frequency: 90_300_000 },
  { name: 'PLAIN NAME', frequency: 91_400_000 },
];

try {
  // ── 1. round trip ──
  ok(await start() >= 0, 'the server starts');
  const imp = await fetch(`${base}/bookmarks/import?${await auth()}`, { method: 'POST', body: JSON.stringify(NAMES.map((n) => ({ ...n, mode: 'wfm' }))), headers: { 'Content-Type': 'application/json' } }).then((r) => r.json());
  ok(imp.imported === NAMES.length, `the four names are imported (${imp.imported})`);
  const file = bmFile();
  ok(!!file, `they are saved to a file (${file ? path.basename(file) : 'none'})`);
  const size0 = file ? fs.statSync(file).size : 0;
  for (const round of [1, 2]) {
    await stop();
    ok(await start() >= 0, `restart ${round}: the server starts`);
    const got = await list();
    for (const n of NAMES) {
      const row = got.find((r) => Math.abs(r.frequency - n.frequency) < 1000);
      ok(row && row.name === n.name, `restart ${round}: ${JSON.stringify(n.name)} comes back exactly (${JSON.stringify(row?.name)})`);
    }
    const size = fs.statSync(file).size;
    ok(size === size0, `restart ${round}: the file has not grown (${size0} → ${size} bytes)`);
  }
  await stop();

  // ── 2. a file the OLD loader bloated: the writer was always right, so it is valid JSON with very long names ──
  const bloated = [{ frequency: 94_500_000, name: 'GARBLED' + '\\'.repeat(200_000) + 'RDS', pi: 4322, lastHeard: Math.floor(Date.now() / 1000), manual: false, mode: 'wfm', source: 'server' }];
  for (let i = 0; i < 2000; i++) bloated.push({ frequency: 100_000_000 + i * 100_000, name: `STATION ${i}`, pi: -1, lastHeard: Math.floor(Date.now() / 1000), manual: true, mode: 'wfm', source: 'server' });
  fs.writeFileSync(file, JSON.stringify(bloated));
  const tBloat = await start();
  ok(tBloat >= 0 && tBloat < 10_000, `a bloated file (${(fs.statSync(file).size / 1e6).toFixed(1)} MB as written) loads promptly (${tBloat} ms)`);
  const got2 = await list();
  ok(got2.length === bloated.length, `every row is there (${got2.length} of ${bloated.length})`);
  const g = got2.find((r) => Math.abs(r.frequency - 94_500_000) < 1000);
  ok(g && Buffer.byteLength(g.name) <= 64, `the bloated name is cut back to 64 bytes (${g ? Buffer.byteLength(g.name) : 'missing'})`);
  await stop();
  ok(fs.statSync(file).size < 300_000, `…and the file is written back small (${fs.statSync(file).size} bytes)`);

  // ── 3. past 16 MB: set aside, never deleted, and the server starts with none ──
  const huge = '[{"frequency":95600000,"name":"' + 'X'.repeat(17 * 1024 * 1024) + '","lastHeard":1,"manual":true,"mode":"wfm"}]';
  fs.writeFileSync(file, huge);
  const tHuge = await start();
  ok(tHuge >= 0 && tHuge < 10_000, `a 17 MB file does not hold up the start (${tHuge} ms)`);
  ok(fs.existsSync(file + '.oversize') && fs.statSync(file + '.oversize').size === huge.length, 'it is kept beside, renamed .oversize');
  const got3 = await list();
  ok(Array.isArray(got3) && got3.length === 0, `the server starts with no bookmarks (${got3.length})`);
} finally {
  await stop();
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-bookmark-escape: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
