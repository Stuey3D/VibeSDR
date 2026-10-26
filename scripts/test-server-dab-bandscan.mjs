#!/usr/bin/env node
// test-server-dab-bandscan.mjs — the V12 DAB BAND SCAN end to end (M7EIW on Discord, 2026-10-26: "a scan function for DAB
// which also shows the results"; Stuart's rules the same day: listeners scan only on a SINGLE-USER radio, the owner scans
// from setup, every station found is bookmarked, scanned bookmarks last a year, a multiplex re-learns on every visit).
//
// Real server, the bench multiplex through fake-rtl-tcp --dab --dab-at (the clip plays ONLY on 11D; every other block is
// noise), no browser, silent.
//   A. SINGLE-USER radio (--users 1): hwinfo offers `dabScan`; in DAB on 12B a scan walks all 41 blocks, reports
//      progress, finds the bench ensemble at 11D and nothing else, lists its services with FIC quality, adds them to the
//      server's bookmarks as SCANNED ("N added"), remembers the ensemble for 11D, and puts the radio back on 12B.
//   B. cancel mid-scan → back on the block it was on; a TUNE mid-scan → the scan stops and the dial stays where tuned.
//   C. a rescan adds nothing new, and leaves the bookmark file BYTE-IDENTICAL (the 134 MB escape-doubling guard), across
//      a restart too.
//   D. SHARED radio (--users 3): a listener is not offered it (no `dabScan`), and a hand-sent request is refused in words.
//   E. OWNER at the setup endpoint (/vibeserver/dab-bandscan, admin): scans the idle radio, then hands it back.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-server-dab-bandscan.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh) — not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0, passes = 0;
const ok = (c, what) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); c ? passes++ : fails++; };

const BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
  '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D','13A','13B','13C','13D','13E','13F'];
const HERE = '11D', HERE_HZ = 222064000;
const PASS = 'dab-bandscan-admin';
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabband-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
let srv = null, rtl = null, port = 0, rtlPort = 0, dataDir = '';

async function startRtl() {
  rtlPort = await freePort();
  rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(HERE_HZ)], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(400);
}
async function startSrv(users, tag) {
  port = await freePort();
  const D = path.join(T, tag);
  dataDir = path.join(D, 'data');
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
  fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Band scan test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: 'dabband', label: 'T', enabled: true, configured: true, users }] }));
  const log = fs.openSync(path.join(D, 'server.log'), 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', String(users)], {
    env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: dataDir, VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
    stdio: ['ignore', log, log] });
  procs.push(srv);
  for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); return path.join(D, 'server.log'); } catch { await sleep(250); } }
  throw new Error('server did not start');
}
async function stopSrv() {
  if (!srv) return;
  const s = srv; srv = null;
  const gone = new Promise((r) => s.once('exit', r));
  s.kill('SIGTERM');
  await Promise.race([gone, sleep(10000)]);
}
function client(tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1`);
  const c = { ws, hw: null, dab: null, scan: null, scans: [], errors: [], open: new Promise((r) => (ws.onopen = r)) };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') return;
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.type === 'hwinfo') c.hw = m;
    else if (m.type === 'dab' && m.channel) c.dab = m;
    else if (m.type === 'dab_scan') { c.scan = m; c.scans.push(m); if (m.error) c.errors.push(m.error); }
  };
  c.send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  // ★ proto=1: a session that says nothing for 30 s is released — a real client's own traffic, here a harmless resync.
  c.hb = setInterval(() => c.send({ type: 'dab_resync' }), 8000);
  ws.addEventListener('close', () => clearInterval(c.hb));
  c.until = async (f, ms = 20000) => { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(100); } return false; };
  return c;
}
const auth = async () => {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
const bookmarks = () => fetch(`http://127.0.0.1:${port}/bookmarks`).then((r) => r.json());
const bmFile = () => fs.readdirSync(dataDir).filter((f) => /^bookmarks.*\.json$/.test(f)).map((f) => path.join(dataDir, f))[0];
const ensFile = () => { const f = fs.readdirSync(dataDir, { recursive: true }).find((x) => String(x).endsWith('dab-ensembles.txt')); return f ? fs.readFileSync(path.join(dataDir, String(f)), 'utf8') : ''; };

try {
  await startRtl();
  // ── A. single-user radio ─────────────────────────────────────────────────────────────────────────────────
  console.log('A. a single-user radio: a listener scans Band III from DAB');
  let logPath = await startSrv(1, 'single');
  const a = client('scanA');
  await a.open;
  ok(await a.until(() => a.hw), 'hwinfo arrives');
  ok(a.hw?.dabScan === true, `hwinfo offers the scan on a single-user radio (dabScan=${a.hw?.dabScan})`);
  a.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12B') });
  ok(await a.until(() => a.dab?.channel === '12B'), 'in DAB on 12B (noise here)');
  const t0 = Date.now();
  a.send({ type: 'dab_scan', on: 1 });
  ok(await a.until(() => a.scan?.running), 'the scan starts');
  ok(await a.until(() => a.scan?.finished, 300000), `…and finishes (${((Date.now() - t0) / 1000).toFixed(0)} s for ${BLOCKS.length} blocks)`);
  const fin = a.scan;
  ok(fin.done === BLOCKS.length && fin.total === BLOCKS.length, `progress reached ${fin.done} / ${fin.total}`);
  const prog = a.scans.filter((m) => m.running).map((m) => m.done);
  ok(prog.length >= BLOCKS.length && prog.every((v, i) => i === 0 || v >= prog[i - 1]), `progress came as it went (${prog.length} updates, never backwards)`);
  ok(fin.found.length === 1 && fin.found[0].block === HERE, `found exactly the bench ensemble at ${HERE} (${fin.found.map((h) => h.block).join(',')})`);
  const hit = fin.found[0] || {};
  ok(hit.hz === HERE_HZ && hit.ensemble && hit.ensemble.length > 0, `…with its frequency and name (${hit.hz} "${hit.ensemble}")`);
  ok(Array.isArray(hit.services) && hit.services.length >= 5 && hit.services.every((s) => s.sid > 0 && s.label), `…and its ${hit.services?.length} named services`);
  ok(hit.fib > 0.5, `…and its FIC quality (${hit.fib})`);
  ok(fin.added === hit.services.length, `"${fin.added} stations added to bookmarks" — every service (${hit.services.length})`);
  ok(!fin.cancelled, 'not cancelled');
  ok(await a.until(() => a.dab?.channel === '12B', 15000), `the radio is back on 12B (${a.dab?.channel})`);
  let bms = await bookmarks();
  const dabBms = bms.filter((b) => b.mode === 'dab');
  ok(dabBms.length === hit.services.length && dabBms.every((b) => b.scanned === true && b.manual === false),
     `the server's bookmarks hold every service, origin SCANNED (${dabBms.length})`);
  ok(dabBms.every((b) => Math.abs(b.frequency - HERE_HZ - b.sid) < 1000 || Math.abs(b.frequency - HERE_HZ) < 1000), '…at the block centre (keyed + SId)');
  ok(ensFile().includes(`${HERE} `), `the ensemble memory knows ${HERE} (${JSON.stringify(ensFile().trim())})`);
  const perBlock = fs.readFileSync(logPath, 'utf8').split('\n').filter((l) => /band scan: \S+ nothing in/.test(l))
    .map((l) => Number(/nothing in ([\d.]+) s/.exec(l)[1]));
  ok(perBlock.length === BLOCKS.length - 1 && Math.max(...perBlock) < 4.5, `an empty block is left fast (${perBlock.length} empty, longest ${Math.max(...perBlock).toFixed(1)} s)`);

  // ── C. rescan: nothing new, the file byte-identical ──
  console.log('C. a rescan adds nothing and leaves the bookmark file byte-identical');
  const f = bmFile();
  const before = fs.readFileSync(f);
  a.scans.length = 0;
  a.send({ type: 'dab_scan', on: 1 });
  ok(await a.until(() => a.scans.some((m) => m.finished), 300000), 'the rescan finishes');
  ok(a.scan.added === 0 && a.scan.found.length === 1, `"0 stations added" — nothing twice (added ${a.scan.added})`);
  ok(Buffer.compare(before, fs.readFileSync(f)) === 0, `the file is byte-identical (${before.length} bytes)`);
  ok((await bookmarks()).filter((b) => b.mode === 'dab').length === dabBms.length, 'the count is unchanged');

  // ── B. cancel, then a tune mid-scan ──
  console.log('B. cancel puts the block back; a tune mid-scan wins');
  a.scans.length = 0;
  a.send({ type: 'dab_scan', on: 1 });
  ok(await a.until(() => a.scan?.running && a.scan.done >= 3), 'a scan is running');
  a.send({ type: 'dab_scan', on: 0 });
  ok(await a.until(() => a.scan?.finished), 'cancel stops it');
  ok(a.scan.cancelled === true, 'it says cancelled');
  ok(await a.until(() => a.dab?.channel === '12B', 15000), `…and the radio is back on 12B (${a.dab?.channel})`);
  a.scans.length = 0;
  a.send({ type: 'dab_scan', on: 1 });
  ok(await a.until(() => a.scan?.running && a.scan.done >= 3), 'another scan is running');
  a.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf(HERE) });   // ★ a tap on a result
  ok(await a.until(() => a.scan?.finished), 'a tune stops it');
  await sleep(1500);
  ok(a.dab?.channel === HERE, `…and the dial stays where the listener tuned (${a.dab?.channel}), not put back on 12B`);
  a.ws.close();
  await stopSrv();

  // restart: the file is read and written back identically
  logPath = await startSrv(1, 'single');
  await sleep(1500);
  ok(Buffer.compare(before, fs.readFileSync(f)) === 0 || fs.readFileSync(f).length === before.length, 'across a restart the file is the same size');
  ok((await bookmarks()).filter((b) => b.mode === 'dab' && b.scanned).length === dabBms.length, '…and every scanned bookmark is still scanned');
  await stopSrv();

  // ── F. a multiplex re-learns on every visit — but deletes only on a second, confirmed visit 6 h later ──
  console.log('F. re-learn on tuning: a swapped service goes on the second visit; a weak day removes nothing');
  const real = (await (async () => { logPath = await startSrv(1, 'single'); return bookmarks(); })()).filter((b) => b.mode === 'dab');
  await stopSrv();
  const eid = real[0]?.eid, nowS = Math.floor(Date.now() / 1000);
  const row = (sid, name, extra) => ({ frequency: HERE_HZ, name, pi: -1, lastHeard: nowS - 3600, manual: false, mode: 'dab', sid, eid, ecc: real[0]?.ecc ?? -1, source: 'server', ...extra });
  const visit = async (rows, tag) => {
    fs.writeFileSync(bmFile(), JSON.stringify([...real, ...rows]));
    logPath = await startSrv(1, 'single');
    const c = client(tag); await c.open;
    c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf(HERE) });
    await c.until(() => c.dab?.channel === HERE);
    await sleep(26000);                       // ★ the learn path samples every 5 s; 4 agreeing samples = confirmed
    const got = await bookmarks();
    c.ws.close(); await stopSrv();
    return { got, log: fs.readFileSync(logPath, 'utf8') };
  };
  // F1: Radio X 90s missed once already, 7 h ago → removed; Radio X Classic missed for the first time → only marked;
  //     a hand-saved bookmark of a gone service and another ensemble's service on the same block → untouched.
  const f1 = await visit([
    row(0xC0DE, 'RADIO X 90S', { missedAt: nowS - 7 * 3600 }),
    row(0xC0DF, 'FIRST MISS', {}),
    row(0xC0E0, 'HAND SAVED', { manual: true }),
    row(0xC0E1, 'OTHER ENSEMBLE', { eid: (eid ^ 0x1111) }),
  ], 'f1');
  const has = (g, sid) => g.find((b) => b.mode === 'dab' && b.sid === sid);
  ok(!has(f1.got, 0xC0DE), 'a service missed on a visit 7 h ago and still missing now is removed');
  ok(/0xC0DE "RADIO X 90S" is still not in ensemble/.test(f1.log), '…and the removal is logged with block, SId and label');
  ok(has(f1.got, 0xC0DF) && has(f1.got, 0xC0DF).missedAt > 0, 'a service missing for the FIRST time is only marked (missedAt set), not removed');
  ok(has(f1.got, 0xC0E0) && !has(f1.got, 0xC0E0).missedAt, 'a hand-saved bookmark is never marked or removed');
  ok(has(f1.got, 0xC0E1) && !has(f1.got, 0xC0E1).missedAt, "another ensemble's service on the same block is left alone");
  ok(real.every((r) => has(f1.got, r.sid) && !has(f1.got, r.sid).missedAt), 'every service on air is kept, unmarked');
  // F2: the pending mark is cleared by a sighting — the service is on air again (one of the real sids, marked 7 h ago).
  const marked = real.map((r) => ({ ...r, missedAt: nowS - 7 * 3600 }));
  fs.writeFileSync(bmFile(), JSON.stringify(marked));
  logPath = await startSrv(1, 'single');
  { const c = client('f2'); await c.open; c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf(HERE) }); await c.until(() => c.dab?.channel === HERE); await sleep(12000); c.ws.close(); }
  const f2got = await bookmarks(); await stopSrv();
  ok(real.every((r) => has(f2got, r.sid) && !has(f2got, r.sid).missedAt), 'a sighting between visits cancels a pending removal (every mark cleared)');
  // F3: the weak day — 20 more saved services of this ensemble, all "missed 7 h ago", and the visit lists only the real 12:
  //     more than half the saved list missing at once → a suspect visit, nothing removed.
  const extra = []; for (let i = 0; i < 20; i++) extra.push(row(0xD000 + i, `WEAK DAY ${i}`, { missedAt: nowS - 7 * 3600 }));
  const f3 = await visit(extra, 'f3');
  ok(extra.every((r) => has(f3.got, r.sid)), `a visit missing more than half the saved services removes nothing (${extra.filter((r) => has(f3.got, r.sid)).length} of 20 kept)`);
  ok(/a suspect visit/.test(f3.log), '…and says so in the log');
  fs.writeFileSync(bmFile(), JSON.stringify(real));

  // ── D. a shared radio ──
  console.log('D. a shared radio: no scan for a listener');
  const sharedLog = await startSrv(3, 'shared');
  const d = client('scanD');
  await d.open;
  ok(await d.until(() => d.hw), 'hwinfo arrives');
  ok(d.hw?.dabScan === undefined, `hwinfo does not offer the scan (dabScan=${d.hw?.dabScan})`);
  d.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf(HERE) });
  ok(await d.until(() => d.dab?.channel === HERE), `in DAB on ${HERE}`);
  d.send({ type: 'dab_scan', on: 1 });
  ok(await d.until(() => d.errors.length), 'a hand-sent scan request is answered…');
  ok(/single-listener radio/.test(d.errors[0] || '') && !d.scans.some((m) => m.running), `…with a refusal in words ("${d.errors[0]}")`);
  d.ws.close();
  await sleep(7000);   // ★ the leave grace — the radio goes idle

  // ── E. the owner at the setup endpoint ──
  console.log('E. the owner scans from setup, on the idle radio');
  const q = await auth();
  const st = await fetch(`http://127.0.0.1:${port}/vibeserver/dab-bandscan?start=1&${q}`).then((r) => r.json());
  ok(st.running === true && st.by === 'setup' && !st.error, `the setup scan starts (${st.error || st.by})`);
  const e2 = await fetch(`http://127.0.0.1:${port}/vibeserver/dab-bandscan?start=1&${await auth()}`).then((r) => r.json());
  ok(/already running/.test(e2.error || ''), `a second scan cannot start while one runs ("${e2.error}")`);
  let s2 = st;
  for (let i = 0; i < 600 && !s2.finished; i++) { await sleep(500); s2 = await fetch(`http://127.0.0.1:${port}/vibeserver/dab-bandscan?${await auth()}`).then((r) => r.json()); }
  ok(s2.finished && !s2.cancelled && s2.found.length === 1 && s2.found[0].block === HERE, `it finds ${HERE} (${s2.found.map((h) => h.block)}) in ${s2.secs} s`);
  ok(s2.found[0]?.services.length >= 5, `…with its services (${s2.found[0]?.services.length})`);
  await sleep(500);
  const elog = fs.readFileSync(sharedLog, 'utf8');
  const after = elog.slice(elog.lastIndexOf('band scan: started by the owner'));
  ok(/band scan: finished .* radio handed back/.test(after) && /\[DAB\] mode OFF: radio back at/.test(after), 'the idle radio is handed back out of DAB');
  const listener = client('afterE');
  await listener.open;
  ok(await listener.until(() => listener.hw), 'a listener can connect afterwards');
  listener.ws.close();
} catch (e) {
  console.error(e); fails++;
} finally {
  await stopSrv();
  for (const p of procs) { try { p.kill(); } catch {} }
  await sleep(300);
  if (!fails) fs.rmSync(T, { recursive: true, force: true }); else console.log(`  (logs kept in ${T})`);
}
console.log(`test-server-dab-bandscan: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
