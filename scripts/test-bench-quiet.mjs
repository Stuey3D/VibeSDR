#!/usr/bin/env node
// test-bench-quiet.mjs — THE WHOLE MACHINE HOLDS STILL FOR A BENCHMARK (Stuart, 2026-11-20: "everything should stop for
// benchmark runs otherwise they are invalid"). vibeserver/vibe_bench_quiet.h has the design.
//
// ★★★ THE FAULT: on the Pi 2 the quick run, started from the setup page, ran in the FRONT DOOR (no radio). It released
//     nothing, counted only its own listeners, and the radio process's idle DX scanner swept on through the run.
// The layout is the Pi 2's / the Lenovo's: a front door and one process per radio, started the way systemd starts
// vibeserver@<serial> (VIBESERVER_NO_RADIO_SPAWN=1, the radios on fake rtl_tcp). The measurement itself is replaced by an
// 8 s wait (VIBESERVER_TEST_BENCH_STUB_SEC) — what is tested is the QUIET around it, not the figures.
//   A. a radio that does not answer (configured, no process, nothing says it is not running) stops the run before it
//      starts, by name — and nothing anywhere was touched;
//   B. ★ a QUICK run from the FRONT DOOR: the busy radio (DX scanner sweeping FM) and the other radio are both held still —
//      no DX scanner hop is logged inside the run — and both come back after; the DX scanner sweeps again;
//   C. a listener on ANY radio refuses the run (summed across processes, the radio named); forced, it runs;
//   D. a WIDE run (in a radio's own process, reached through the door) holds the OTHER radio still too;
//   E. the lease: a radio held by a coordinator that never says "done" brings itself back.
// ★ SILENT: fake rtl_tcp, no audio device, no browser. usage: VIBESERVER_BIN=… node scripts/test-bench-quiet.mjs (~2 min)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const portFree = (q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); });
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

const ADMIN = 'bench-quiet-admin';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bq-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
// ★ The door's port and the three radios behind it (portForRadio: P+1, P+2, P+3) — four free ports in a row.
let P = 0;
for (let i = 0; i < 30 && !P; i++) {
  const p = await freePort();
  if ((await Promise.all([p + 1, p + 2, p + 3].map(portFree))).every(Boolean)) P = p;
}
const DX = JSON.stringify({ on: true, bands: 'fm', waitMin: 5, raster: 100, oirt: false, ext: false });
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Bench quiet test', adminPass: ADMIN, port: P,
  radios: [
    { serial: 'bq-rsp', driver: 'sdrplay', label: 'Test RSP', enabled: true, configured: true, users: 1, rate: 2048000 },
    { serial: 'bq-rtl', driver: 'rtlsdr', label: 'Test RTL', enabled: true, configured: true, users: 1, rate: 2400000, dxScan: DX },
    // ★ configured and enabled, but no process runs for it (A): a radio nobody can ask
    { serial: 'bq-gone', driver: 'rtlsdr', label: 'Gone RTL', enabled: true, configured: true, users: 1, rate: 2048000 },
  ],
}));
const fakes = [];
const fakePort = async () => { let q = 0; while (!q || (q >= P && q <= P + 3)) q = await freePort(); return q; };
const rspTcp = await fakePort(), rtlTcp = await fakePort();
fakes.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rspTcp), '--rate', '2048000'], { stdio: 'ignore' }));
fakes.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlTcp), '--rate', '2400000', '--tones', '0',
                                    '--fm-rds', '89.3:C201:BBC_R4,98.8:5201:RAI_R1,104.6:F201:FR_INTER'], { stdio: 'ignore' }));
await sleep(400);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_TEST_TCP_RELEASE: '1',
              VIBESERVER_TEST_BENCH_STUB_SEC: '8', VIBESERVER_TEST_BENCH_LEASE_SEC: '6',
              VIBESERVER_DX_WAIT_SEC: '3', VIBESERVER_DX_DWELL_SEC: '2.5',
              VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logs = { door: path.join(dir, 'door.log'), rsp: path.join(dir, 'rsp.log'), rtl: path.join(dir, 'rtl.log') };
const out = (f) => ({ env, stdio: ['ignore', fs.openSync(f, 'w'), fs.openSync(f, 'a')] });
const procs = [
  spawn(BIN, [], out(logs.door)),
  spawn(BIN, ['--radio-serial', 'bq-rsp', '--tcp', `127.0.0.1:${rspTcp}`, '--freq', '7074000', '--mode', 'usb'], out(logs.rsp)),
  spawn(BIN, ['--radio-serial', 'bq-rtl', '--tcp', `127.0.0.1:${rtlTcp}`, '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm'], out(logs.rtl)),
];
const stopAll = () => { for (const p of [...procs, ...fakes]) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stopAll);
const up = async (port) => { for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const T = (k) => fs.readFileSync(logs[k], 'utf8');
const count = (t, re) => (t.match(new RegExp(re.source, 'g')) || []).length;
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); };
const auth = async (port) => {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};
const H = `http://127.0.0.1:${P}`;
const HOP = /DX scanner: [^\n]*new, [^\n]*worker\(s\)/;        // ★ a finished FM hop
const SWEEP = /DX scanner: nobody here for [^\n]*sweeping/;     // ★ a sweep starting
/** Start a benchmark at `base` (door or a radio route), follow its progress there until it ends. */
async function runBench(base, query = '', progressPort = P) {
  const r = await fetch(`${base}/vibeserver/benchmark?${await auth(progressPort)}${query}`, { method: 'POST' });
  const body = await r.text();
  if (r.status !== 200) return { status: r.status, body };
  let seen = false, done = false;
  for (let i = 0; i < 240 && !done; i++) {
    await sleep(250);
    const p = await fetch(`${base}/vibeserver/benchmark?progress=1&${await auth(progressPort)}`).then((x) => x.json()).catch(() => ({}));
    if (p.running) seen = true; else if (seen) done = true;
  }
  return { status: 200, body, done };
}
/** The text of a log between the first `from` match at/after offset `at` and the next `to` match. */
const windowOf = (t, at, from, to) => {
  const s = t.slice(at); const a = s.search(from); if (a < 0) return null;
  const rest = s.slice(a); const b = rest.search(to); return b < 0 ? null : rest.slice(0, b);
};
// ★ A WebSocket listener (spectrum) straight onto a radio's own port — it counts as that radio's listener.
function listener(port) {
  const s = net.connect(port, '127.0.0.1');
  s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=bq000001&bins=256 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', () => {});
  return { close: () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); } };
}

try {
  ok(await up(P) && await up(P + 1) && await up(P + 2), `the front door (${P}) and both radio processes (${P + 1}, ${P + 2}) are up`);
  ok(await until(() => SWEEP.test(T('rtl')) && HOP.test(T('rtl')), 30000), 'the RTL is busy: its idle DX scanner is sweeping FM and logging hops');

  // ── A. a radio that does not answer ──
  console.log('── A. a configured radio that does not answer stops the run, by name ──');
  {
    const r = await runBench(H);
    ok(r.status === 409 && /Gone RTL did not answer/.test(r.body) && /Nothing was changed/.test(r.body), `refused, the radio named (${r.status} ${r.body.slice(0, 140)})`);
    ok(!/benchmark hold/.test(T('rtl')) && !/benchmark hold/.test(T('rsp')), '…and neither running radio was touched');
    ok(!/listener/.test(JSON.parse(r.body).error), '…and it is not a listener refusal (the page will not offer "Run it anyway?")');
  }
  // ★ Its process KNOWS it is not running (its device is missing — the radio-status file its restart loop writes): skipped.
  fs.writeFileSync(path.join(dir, 'data', 'radio-status-bq-gone.json'), JSON.stringify({ serial: 'bq-gone', driver: 'rtlsdr', state: 'notfound',
    detail: 'no RTL-SDR with that serial', since: Math.floor(Date.now() / 1000), last: Math.floor(Date.now() / 1000) }) + '\n');

  // ── B. the quick run from the front door ──
  console.log('── B. ★ a QUICK run from the FRONT DOOR holds every radio still ──');
  {
    const at = { rtl: T('rtl').length, rsp: T('rsp').length };
    const hopsBefore = count(T('rtl'), HOP);
    const r = await runBench(H);
    ok(r.status === 200 && r.done, `the run started at the door and finished (${r.status} ${r.body.slice(0, 80)})`);
    ok(/the machine is quiet \(2 other radio process\(es\) held\)/.test(T('door')), 'the door held both radio processes before measuring');
    ok(/Gone RTL is not running \(notfound\)/.test(T('door')), '…and skipped the one its own status file says is not running');
    const rtl = T('rtl').slice(at.rtl), rsp = T('rsp').slice(at.rsp);
    // ★ Stopped mid-sweep (logged) — or caught between sweeps, when there is nothing running to stop: either way it must not start.
    ok(/DX scanner: stopped — the benchmark is measuring this machine/.test(rtl) || !/DX scanner: stopped/.test(rtl.split(/benchmark hold ended/)[0]),
       `the RTL's DX scanner was stopped for it (${/DX scanner: stopped — the benchmark/.test(rtl) ? 'mid-sweep' : 'between sweeps'})`);
    ok(/benchmark hold: idle work stopped and the radio released/.test(rtl) && /benchmark hold: idle work stopped/.test(rsp),
       '★ both radios: idle work stopped, released (or already off the air)');
    const w = windowOf(rtl, 0, /benchmark hold: idle work stopped/, /benchmark hold ended/);
    ok(w !== null, 'the RTL\'s hold has a start and an end');
    ok(w !== null && !HOP.test(w) && !SWEEP.test(w), `★★ NO DX scanner hop and no sweep inside the run window (${w ? count(w, HOP) : '?'} hops)`);
    ok(await until(() => /benchmark hold: the radio is back on the air/.test(T('rtl').slice(at.rtl))
                         && /benchmark hold: the radio is back on the air/.test(T('rsp').slice(at.rsp)), 30000),
       'both radios come back after the run');
    ok(/radio taken back \(test: rtl_tcp\)/.test(T('rtl').slice(at.rtl)), '…the RTL taken back');
    ok(await until(() => { const t = T('rtl').slice(at.rtl); const e = t.search(/benchmark hold ended/); return e > 0 && SWEEP.test(t.slice(e)); }, 30000),
       '★ and its DX scanner sweeps again afterwards (the idle work is not left switched off)');
    ok(count(T('rtl'), HOP) >= hopsBefore, 'its hops carry on from where they were');
  }

  // ── C. a listener on ANY radio refuses the run ──
  console.log('── C. a listener on any radio refuses the run (summed across processes); forced, it runs ──');
  {
    const L = listener(P + 2);
    ok(await until(async () => { const j = await fetch(`http://127.0.0.1:${P + 2}/vibeserver/benchmark?hold=check&${await auth(P + 2)}`).then((x) => x.json()).catch(() => ({})); return j.listeners >= 1; }, 10000),
       'the RTL counts its listener (hold=check)');
    const r = await runBench(H);
    ok(r.status === 409 && /1 listener is connected \(Test RTL: 1\)/.test(r.body) && /every radio on this machine/.test(r.body),
       `★ refused at the DOOR for a listener on the RTL's process, the radio named (${r.body.slice(0, 160)})`);
    const holds = count(T('rtl'), /benchmark hold: idle work stopped/);
    const f = await runBench(H, '&force=1');
    ok(f.status === 200 && f.done, 'forced (the owner\'s call), it runs');
    ok(count(T('rtl'), /benchmark hold: idle work stopped/) === holds + 1, '…with the RTL held still all the same');
    L.close();
    await until(() => (T('rtl').match(/benchmark hold: the radio is back on the air/g) || []).length >= 2, 30000);
  }

  // ── D. the WIDE run, in the RSP's own process, through the door ──
  console.log('── D. a WIDE run (in a radio\'s own process) holds the OTHER radio still ──');
  {
    await sleep(1500);
    const radios = await fetch(`${H}/vibeserver/radios`).then((x) => x.json());
    const rsp = (radios.radios || []).find((x) => x.serial === 'bq-rsp');
    ok(!!rsp, `the door lists the RSP (id ${rsp && rsp.id})`);
    const at = T('rtl').length;
    const base = `${H}/r/${rsp.id}`;
    const r = await runBench(base, '&wide=8000000&radio=bq-rsp', P + 1);
    ok(r.status === 200 && /"wide":true/.test(r.body) && r.done, `the wide run started in the RSP's process and finished (${r.status} ${r.body.slice(0, 100)})`);
    ok(/the machine is quiet \(1 other radio process\(es\) held\)/.test(T('rsp')), '★ the RSP\'s process held the other radio (the door owns none, the gone one is skipped)');
    const rtl = T('rtl').slice(at);
    const w = windowOf(rtl, 0, /benchmark hold: idle work stopped/, /benchmark hold ended/);
    ok(w !== null && !HOP.test(w), '★ the RTL was held still for the wide run — no DX hop inside it');
    ok(await until(() => /benchmark hold: the radio is back on the air/.test(T('rtl').slice(at)), 30000), '…and came back');
  }

  // ── E. the lease ──
  console.log('── E. a radio held by a coordinator that never says "done" brings itself back (the lease) ──');
  {
    await sleep(1500);
    const at = T('rtl').length;
    const r = await fetch(`http://127.0.0.1:${P + 2}/vibeserver/benchmark?hold=on&${await auth(P + 2)}`, { method: 'POST' });
    const j = await r.json().catch(() => ({}));
    ok(r.status === 200 && j.held === true, `held directly (as a coordinator would) — ${r.status} ${JSON.stringify(j)}`);
    const bad = await fetch(`http://127.0.0.1:${P + 2}/vibeserver/benchmark?hold=on`, { method: 'POST' });
    ok(bad.status === 401, `★ never without the admin proof (${bad.status})`);
    ok(await until(() => /lease ran out/.test(T('rtl').slice(at)) && /benchmark hold: the radio is back on the air/.test(T('rtl').slice(at)), 20000),
       '★ nobody renewed it: after its lease (6 s here, 90 s for real) it brought itself back');
  }
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stopAll();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs: ${logs.door} ${logs.rsp} ${logs.rtl})`);
else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
