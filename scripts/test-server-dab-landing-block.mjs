// test-server-dab-landing-block.mjs — WHICH MULTIPLEX DAB OPENS ON WHEN NOBODY NAMED ONE, end to end (V12, 2026-11-12).
// Stuart: "ISM remembers the last frequency it was tuned to, DAB should also follow suit as right now it defaults to 12B
// which is fine for the UK as the BBC is there, but for other countries 12B may be dead". The rule (vibe_dab_landing.h):
// the LAST block a listener chose → the STRONGEST of the saved scan → the COUNTRY's national multiplex → 12B. The server
// decides; every client just sends {type:'dab',on:1} with no channel.
//   A. Germany by POSITION (lat/lon, no country), nothing remembered: opens on 5C; `unscanned` is false;
//   B. a NAMED block wins (10D); leave DAB and come back with none named: 10D (the last block);
//   C. a band scan (the server's own moves — the same path as the DX sweep: handleControl with no socket) walks several
//      blocks: the stored block is still 10D, on disk and on re-entry;
//   D. a restart: still 10D;
//   E. a saved scan and nothing remembered (a fresh radio, scan file written first): its strongest block (11D);
//   F. Vienna (DAB on air, but Austria's national mux is on four blocks — no table entry), no scan: 12B, and `unscanned`
//      is true (the box's "no scan yet" line). ★ Not New York: an ITU Region 2 server has DAB off until its owner opts in.
// ★ The bench clip (VIBE_DAB_CLIP, or vibeserver/bench-clip/dab-bench-clean.vbu8) is played on every Band III block by
//   fake-rtl-tcp. Without it or VIBESERVER_BIN: NOT RUN (exit 3). SILENT (no audio socket), loopback only.
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
const CLIP = process.env.VIBE_DAB_CLIP || path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; VIBE_DAB_CLIP= or vibeserver/bench-clip/make-clip.sh)'); process.exit(3); }

const until = async (f, ms = 20000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };
const dirs = [];
const procs = [];

/** One radio: its own data dir and config; started (and restarted) on demand. */
async function radio(tag, cfgExtra, prepare) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dabland-${tag}-`));
  dirs.push(dir);
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: `DAB landing ${tag}`, adminPass: 'dab-land-admin',
    ...cfgExtra, radios: [{ serial: `dabland${tag}`, label: 'T', enabled: true, configured: true, users: 1 }] }));
  if (prepare) prepare(path.join(dir, 'data'));
  const raw = path.join(dir, 'clip.u8');
  fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  const r = { dir, port, srv: null };
  r.start = async () => {
    r.srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', log, log] });
    procs.push(r.srv);
    for (let i = 0; i < 100; i++) {
      if (await new Promise((res) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); res(true); }); s.on('error', () => res(false)); })) return;
      await sleep(100);
    }
  };
  r.stop = async () => {
    if (!r.srv) return;
    const done = new Promise((res) => r.srv.once('exit', res));
    r.srv.kill('SIGTERM');
    await Promise.race([done, sleep(8000)]);
    r.srv = null;
  };
  r.lastFile = () => { const f = fs.readdirSync(path.join(dir, 'data')).find((x) => x.startsWith('dab-last')); return f ? fs.readFileSync(path.join(dir, 'data', f), 'utf8') : ''; };
  await r.start();
  return r;
}
function client(port, tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=dl${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1`);
  const c = { ws, msgs: [], opened: null };
  c.opened = new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (e) => { if (typeof e.data === 'string') { try { c.msgs.push(JSON.parse(e.data)); } catch {} } };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.dab = (from = 0) => c.msgs.slice(from).filter((m) => m.type === 'dab' && m.channel !== undefined);
  return c;
}
/** Enter DAB (optionally on a named block) and return the block the server reports. */
async function enter(c, channel) {
  const mark = c.msgs.length;
  c.send(channel ? { type: 'dab', on: 1, channel } : { type: 'dab', on: 1 });
  await until(() => c.dab(mark).length > 0, 10000);
  return c.dab(mark)[0];
}
async function leave(c) {
  const mark = c.msgs.length;
  c.send({ type: 'dab', on: 0 });
  await until(() => c.msgs.slice(mark).some((m) => m.type === 'dab_off'), 8000);
  await sleep(500);
}

try {
  console.log('── A. Germany by position, nothing remembered ──');
  const de = await radio('de', { lat: '52.520', lon: '13.405' });
  let c = client(de.port, 'a'); await c.opened; await sleep(800);
  let r = await enter(c);
  ok(r?.channel === '5C', `opens on 5C, the 1. Bundesmux (got ${r?.channel})`);
  ok(r?.unscanned === false, `\`unscanned\` is false — the country is known (got ${r?.unscanned})`);

  console.log('── B. a named block wins, and is remembered ──');
  r = await enter(c, '10D');
  ok(r?.channel === '10D', `a named block: 10D (got ${r?.channel})`);
  await sleep(500);
  ok(/^B 10D$/m.test(de.lastFile()), 'the block is on disk (dab-last: "B 10D")');
  await leave(c);
  r = await enter(c);
  ok(r?.channel === '10D', `left and came back with none named: 10D, the last block (got ${r?.channel})`);

  console.log('── C. a band scan (the server\'s own moves, as the DX sweep) never changes it ──');
  const mark = c.msgs.length;
  c.send({ type: 'dab_scan', on: 1 });
  const walked = () => new Set(c.msgs.slice(mark).filter((m) => m.type === 'dab_scan' && m.current).map((m) => m.current));
  const moved = await until(() => walked().size >= 3, 60000);
  ok(moved, `the scan walked several blocks (${[...walked()].join(' ')})`);
  ok(/^B 10D$/m.test(de.lastFile()), 'mid-scan: the stored block is still 10D');
  c.send({ type: 'dab_scan', on: 0 });
  await until(() => c.msgs.slice(mark).some((m) => m.type === 'dab_scan' && m.running === false), 15000);
  await sleep(1500);
  ok(/^B 10D$/m.test(de.lastFile()), 'after the scan: still 10D on disk');
  await leave(c);
  r = await enter(c);
  ok(r?.channel === '10D', `re-entry after the scan: 10D (got ${r?.channel})`);

  console.log('── D. a restart ──');
  await leave(c);
  c.ws.close();
  await de.stop();
  await de.start();
  c = client(de.port, 'd'); await c.opened; await sleep(800);
  r = await enter(c);
  ok(r?.channel === '10D', `after a restart: 10D (got ${r?.channel})`);
  c.ws.close();
  await de.stop();

  console.log('── E. a saved scan, nothing remembered: its strongest block ──');
  const sc = await radio('scan', { lat: '52.520', lon: '13.405' }, (data) => {
    fs.writeFileSync(path.join(data, 'dab-scan-dablandscan.txt'),
      'VIBE-DAB-SCAN 1\nat 1762900000 secs 120 cancelled 0 by listener\n'
      + 'H 11C 1 0.90 12.0 Weak mux\nS 49665 Weak One\n'
      + 'H 11D 1 0.99 21.5 Strong mux\nS 49666 Strong One\nS 49667 Strong Two\n'
      + 'H 12A 1 1.00 25.0 No stations\n');
  });
  c = client(sc.port, 'e'); await c.opened; await sleep(800);
  r = await enter(c);
  ok(r?.channel === '11D', `the strongest block with stations: 11D, not 5C (got ${r?.channel})`);
  ok(r?.unscanned === false, 'a scan exists: `unscanned` is false');
  c.ws.close();
  await sc.stop();

  console.log('── F. Vienna, no scan: 12B and "no scan yet" ──');
  const us = await radio('at', { lat: '48.21', lon: '16.37' });
  c = client(us.port, 'f'); await c.opened; await sleep(800);
  r = await enter(c);
  ok(r?.channel === '12B', `the last resort: 12B (got ${r?.channel})`);
  ok(r?.unscanned === true, `\`unscanned\` is true (got ${r?.unscanned})`);
  c.ws.close();
  await us.stop();
} finally {
  for (const p of procs) { try { p.kill('SIGTERM'); } catch {} }
}
console.log(`test-server-dab-landing-block: ${pass} passed, ${fail} failed${fail ? ` (logs: ${dirs.join(' ')})` : ''}`);
if (!fail) for (const d of dirs) fs.rmSync(d, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
