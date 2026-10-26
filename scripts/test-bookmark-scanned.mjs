// ★★★ THE THIRD BOOKMARK ORIGIN: SCANNED (V12, 2026-10-26). Stuart: "scanned stations have a much longer timeout before
// they drop off the bookmarks" — a station a DAB band scan found lasts 365 days unheard, a learned one 30, a hand-saved one
// for ever; past the 1,000 learned rows the plain learned go first, scanned only after every one of them.
//   1. expiry by origin: learned 31 d → gone; scanned 31 d → kept; scanned 366 d → gone; manual 10 y → kept;
//   2. eviction: 1,100 learned (newer) + 100 scanned (older) → all 100 scanned kept, the learned cut to 900;
//   3. an OLD file (no `scanned`, no `missedAt`) loads as before — every row, none scanned;
//   4. the file is written back and survives two restarts BYTE-IDENTICAL (the 134 MB escape-doubling guard),
//      `scanned` and `missedAt` intact;
//   5. "Clear learned" (POST /vibeserver/admin/clear?what=learned, the owner's button) takes scanned rows too, never manual ones.
// ★ SILENT, loopback only. usage: VIBESERVER_BIN=… node scripts/test-bookmark-scanned.mjs
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

const PASS = 'bm-scanned-admin-pass';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bmscan-'));
const dataDir = path.join(dir, 'data');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Bookmark Scanned Test', locator: 'IO92nh', adminPass: PASS,
  radios: [{ serial: 'bmscan-test', label: 'Test radio', enabled: true, configured: true, users: 3 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: dataDir, VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const base = `http://127.0.0.1:${port}`;
let srv = null;
const up = () => new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
async function start() {
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
              { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 150; i++) { if (await up()) return true; await sleep(100); }
  return false;
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
const bmFile = () => fs.readdirSync(dataDir).filter((f) => /^bookmarks.*\.json$/.test(f)).map((f) => path.join(dataDir, f))[0];
const now = Math.floor(Date.now() / 1000), D = 24 * 3600;
const dab = (blockHz, sid, name, lastHeard, extra = {}) => ({ frequency: blockHz, name, pi: -1, lastHeard, manual: false, mode: 'dab', sid, eid: 0xC1CE, ecc: 0xE1, source: 'server', ...extra });

try {
  // A first start writes the (empty) file, so we know its name.
  ok(await start(), 'the server starts');
  await fetch(`${base}/bookmarks/import?${await auth()}`, { method: 'POST', body: JSON.stringify([{ name: 'SEED', frequency: 99_000_000, mode: 'wfm' }]), headers: { 'Content-Type': 'application/json' } });
  await stop();
  const file = bmFile();
  ok(!!file, `the bookmark file exists (${file && path.basename(file)})`);

  // ── 1. expiry by origin ──
  fs.writeFileSync(file, JSON.stringify([
    dab(222_064_000, 0xC201, 'LEARNED 31 DAYS', now - 31 * D),
    dab(222_064_000, 0xC202, 'SCANNED 31 DAYS', now - 31 * D, { scanned: true }),
    dab(222_064_000, 0xC203, 'SCANNED 366 DAYS', now - 366 * D, { scanned: true }),
    dab(222_064_000, 0xC204, 'SCANNED 300 DAYS MISSED', now - 300 * D, { scanned: true, missedAt: now - 3600 }),
    { frequency: 120_000_000, name: 'HAND 10 YEARS', pi: -1, lastHeard: now - 3650 * D, manual: true, mode: 'am', source: 'server' },
  ]));
  ok(await start(), 'restart with rows of every origin and age');
  const g1 = await list();
  const by = (n) => g1.find((r) => r.name === n);
  ok(!by('LEARNED 31 DAYS'), 'a LEARNED row unheard 31 days is gone (30-day rule)');
  ok(by('SCANNED 31 DAYS')?.scanned === true, 'a SCANNED row unheard 31 days is kept, still scanned');
  ok(!by('SCANNED 366 DAYS'), 'a SCANNED row unheard 366 days is gone (365-day rule)');
  ok(by('SCANNED 300 DAYS MISSED')?.missedAt === now - 3600, 'a pending removal mark (missedAt) survives the load');
  ok(by('HAND 10 YEARS')?.manual === true, 'a hand-saved row never expires');
  await stop();

  // ── 2. eviction order ──
  const many = [];
  for (let i = 0; i < 1100; i++) many.push({ frequency: 87_600_000 + i * 10_000, name: `LEARNED ${i}`, pi: 0x1000 + i, lastHeard: now - 1000 + i, manual: false, mode: 'wfm', source: 'server' });
  for (let i = 0; i < 100; i++) many.push(dab(174_928_000 + Math.floor(i / 20) * 1_712_000, 0xC300 + i, `SCANNED ${i}`, now - 20 * D, { scanned: true }));
  fs.writeFileSync(file, JSON.stringify(many));
  ok(await start(), 'a list of 1,100 learned + 100 older scanned rows loads');
  const g2 = await list();
  const l2 = g2.filter((r) => r.name.startsWith('LEARNED ')), s2 = g2.filter((r) => r.name.startsWith('SCANNED '));
  ok(s2.length === 100, `every scanned row stays, though older than every learned one (${s2.length} of 100)`);
  ok(l2.length === 900 && !l2.some((r) => Number(r.name.slice(8)) < 200), `the learned rows take the cut, longest unheard first (${l2.length} left)`);
  await stop();

  // ── 3. an OLD file loads as before ──
  const old = [];
  for (let i = 0; i < 12; i++) old.push({ frequency: 222_064_000, name: `OLD DAB ${i}`, pi: -1, lastHeard: now - D, manual: false, mode: 'dab', sid: 0xC400 + i, eid: 0xC1CE, ecc: 0xE1, source: 'server' });
  old.push({ frequency: 96_100_000, name: 'OLD FM', pi: 0xC201, lastHeard: now - D, manual: false, mode: 'wfm', source: 'server' });
  fs.writeFileSync(file, JSON.stringify(old));
  ok(await start(), 'a file written before the scanned origin loads');
  const g3 = await list();
  ok(g3.length === old.length && g3.every((r) => !r.scanned && !r.missedAt), `every row is there, none scanned or marked (${g3.length} of ${old.length})`);
  await stop();

  // ── 4. byte-identical round trip ──
  fs.writeFileSync(file, JSON.stringify([
    dab(222_064_000, 0xC501, 'BACK\\SLASH "DAB"', now - D, { scanned: true }),
    dab(222_064_000, 0xC502, 'MARKED', now - D, { scanned: true, missedAt: now - 7200 }),
    dab(225_648_000, 0xC503, 'LEARNED', now - D),
  ]));
  ok(await start(), 'load a list with scanned, marked and escaped names');
  await stop();
  const first = fs.readFileSync(file);
  for (const r of [1, 2]) {
    ok(await start(), `restart ${r}`);
    await stop();
    ok(Buffer.compare(first, fs.readFileSync(file)) === 0, `restart ${r}: the file is byte-identical (${first.length} bytes)`);
  }
  const g4 = JSON.parse(first.toString());
  ok(g4.find((r) => r.sid === 0xC501)?.name === 'BACK\\SLASH "DAB"' && g4.find((r) => r.sid === 0xC501)?.scanned === true, 'the escaped name and `scanned` come back exactly');
  ok(g4.find((r) => r.sid === 0xC502)?.missedAt === now - 7200, '`missedAt` comes back exactly');
  ok(!('scanned' in g4.find((r) => r.sid === 0xC503)) && !('missedAt' in g4.find((r) => r.sid === 0xC503)), 'a plain learned row carries neither key (the common row does not grow)');

  // ── 5. Clear learned takes scanned rows, never manual ──
  ok(await start(), 'start for the clear');
  await fetch(`${base}/bookmarks/import?${await auth()}`, { method: 'POST', body: JSON.stringify([{ name: 'HAND KEEP', frequency: 121_500_000, mode: 'am' }]), headers: { 'Content-Type': 'application/json' } });
  const cl = await fetch(`${base}/vibeserver/admin/clear?what=learned&${await auth()}`, { method: "POST", body: "{}" });
  const g5 = await list();
  ok(cl.ok && !g5.some((r) => r.scanned) && !g5.some((r) => r.name === 'LEARNED'), `"Clear learned" removes learned AND scanned rows (${g5.length} left)`);
  ok(g5.some((r) => r.name === 'HAND KEEP'), '…and keeps the hand-saved one');
} finally {
  await stop();
  try { rtl.kill('SIGTERM'); } catch {}
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`test-bookmark-scanned: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
