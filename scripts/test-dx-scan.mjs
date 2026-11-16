#!/usr/bin/env node
// test-dx-scan.mjs — THE IDLE DX SCANNER end to end through a real vibeserver (V12, 2026-11-04). docs/v12/DX-SCANNER.md.
//
// What the unit tests (vibeserver/test-dxscan.cpp, test-dxscan-fm.cpp) cannot show — the brief's §13 acceptance tests that
// need a server and a radio:
//   A. FM. With nobody here past the owner's wait, the scan STARTS and walks 87.5–108 MHz hop by hop; three RDS stations in
//      the fake dongle's band (--fm-rds) land in the DX log (provisional — a new server is in its bootstrap) with PI + PS.
//      hwinfo offers `dxLog`; GET /vibeserver/dxlog.json lists them; the admin line says what it is doing.
//   B. A LISTENER ARRIVES MID-HOP: the scan stops on the connect path, the newcomer gets the dial the radio was left on
//      (never a hop centre), and the INTERRUPTED HOP IS NOT LOGGED; the next idle period carries on from that hop (cursor).
//   C. AN OLD CLIENT (one that knows nothing of the DX log) connects while the scanner is running and works normally:
//      config, spectrum, a tune — nothing in the core protocol changed (V11 core lock).
//   D. PROMOTE (owner): copies the station into the bookmarks; refused without the password. CLEAR keeps promoted.
//   E. DAB (when the 12D reference capture is in VIBE_TEST_AUDIO): the owner's band scan in DX MODE finds the 12D
//      multiplex — EId, label, services — logs it, adds NO bookmarks, and reports its dwell / pull-in times.
// ★ TEST-ONLY knobs: VIBESERVER_DX_WAIT_SEC shortens the owner's wait, VIBESERVER_DX_DWELL_SEC the FM dwell.
// ★ SILENT: no browser, no audio device.
// usage: VIBESERVER_BIN=… node scripts/test-dx-scan.mjs   (~2–4 min; exit 3 = not run)
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
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, open: false, closed: false, hw: null };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) { st.bin++; continue; }
      if (op === 0x1) { const t = p.toString(); st.txt.push(t); if (t.includes('"type":"hwinfo"')) { try { st.hw = JSON.parse(t); } catch {} } }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes(`"type":"${type}"`)) return JSON.parse(st.txt[i]); return null; };
  return st;
}

const ADMIN = 'dx-admin-pass';
const procs = [];
const stopAll = () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stopAll);

async function startServer(tag, dxScan, rtlArgs, opts = {}) {
  // ★ opts.dir: start again on the same data (a restart); opts.seed(dir): write files into data/ before it starts
  const dir = opts.dir || fs.mkdtempSync(path.join(os.tmpdir(), `vs-dx-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (opts.seed) opts.seed(dir);
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DX Scanner Test', locator: 'IO92nh', adminPass: ADMIN,
    radios: [{ serial: 'dx-test', label: 'Test radio', enabled: true, configured: true, users: 1, rate: 2400000, dxScan: JSON.stringify(dxScan) }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), ...rtlArgs], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(400);
  const env = { ...process.env, VIBESERVER_DX_WAIT_SEC: '3', VIBESERVER_DX_DWELL_SEC: '2.5',
                VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const rate = rtlArgs.includes('2048000') ? '2048000' : '2400000';
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', rate, '--freq', '93200000', '--mode', 'wfm', '--users', '1'],
                    { env, stdio: ['ignore', log, log] });
  procs.push(srv);
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const logText = () => fs.readFileSync(logPath, 'utf8');
  return {
    dir, port, H, srv, rtl, logPath, logText,
    count: (re) => (logText().match(new RegExp(re.source, 'g')) || []).length,
    until: async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); },
    adminQ: async () => {
      const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
      return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
    },
    stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} },
  };
}
const vfo = (st) => { const c = st.last('config'); return c ? Number(c.vfo) : NaN; };

const STATIONS = '89.3:C201:BBC_R4,98.8:5201:RAI_R1,104.6:F201:FR_INTER';

/* ★★ F. THE LOCALS INTO THE DISCOVERED BOOKMARKS (2026-11-16). A week of scanning cannot be run here, so the server starts
 *  on a DX log a week old (written as the server writes it): FM and DAB locals, a DX catch, a suspect, a provisional, a local
 *  handed over before whose bookmark the owner has since deleted — and a bookmarks file holding a learned RDS row for one of
 *  the locals and the owner's own bookmark on another local's frequency. */
async function testLocals() {
  console.log('── F. the locals the DX log established go into the discovered bookmarks (FM + DAB) ──');
  const now = Math.floor(Date.now() / 1000), DAY = 86400;
  const fmE = (id, pi, khz, ps, cls, extra = {}) => ({ id, band: 'FM', key: `FM:${pi.toString(16).toUpperCase().padStart(4, '0')}:${khz}`, class: cls,
    khz, piN: pi, ps, psVariants: ps ? [ps] : [], firstHeard: now - 10 * DAY, lastHeard: now - 300, timesHeard: 400, presence: 0.99, lastSweep: 50, ...extra });
  const dabE = (id, eid, blk, block, cls, services) => ({ id, band: 'DAB', key: `DAB:${eid.toString(16).toUpperCase()}:${block}`, class: cls,
    blk, block, eidN: eid, ecc: 0xE1, ensemble: 'Test Mux', services, khz: 0, firstHeard: now - 10 * DAY, lastHeard: now - 300, timesHeard: 300, presence: 0.98, lastSweep: 50 });
  const entries = [
    fmE(1, 0xC201, 89300, 'BBC R4', 'local'),                       // a learned RDS row is already there → merged, one row
    fmE(2, 0xC202, 90500, 'BBC R2', 'local'),                       // new → added with its PI and PS
    fmE(3, 0, 95000, '', 'local'),                                  // no RDS → by its frequency
    fmE(4, 0xC999, 101000, 'OTHER', 'local'),                       // the owner's bookmark at 101.0 → untouched
    fmE(5, 0x5201, 98800, 'RAI R1', 'dx'),                          // ★ a Sporadic-E catch → NOT bookmarked
    fmE(6, 0xCAAA, 102000, 'GONE', 'local', { inBookmarks: true }), // handed over before, deleted since → stays deleted
    fmE(7, 0xF201, 104600, 'FR INTER', 'suspect'),                  // suspect → not
    fmE(8, 0xC203, 91000, 'BBC R3', 'provisional'),                 // not yet judged → not
    dabE(9, 0xC1CE, 32, '12B', 'local', [{ sid: 0xC0CE, label: 'Classic FM' }, { sid: 0xC0D2, label: 'talkSPORT' }]),
    dabE(10, 0xE0F1, 33, '12C', 'dx', [{ sid: 0xF1, label: 'Far Radio' }]),   // ★ a DX multiplex → NOT bookmarked
  ];
  const seed = (dir) => {
    fs.writeFileSync(path.join(dir, 'data', 'dxlog-dx-test.json'), JSON.stringify({ v: 1, created: now - 10 * DAY, nextId: 20, sweepsFm: 50,
      sweepsDab: 50, alertSeq: 0, fmCursorKhz: 87500, dabCursor: 0, nextDab: false, bootDone: true, entries, alerts: [] }));
    fs.writeFileSync(path.join(dir, 'data', 'bookmarks-dx-test.json'), JSON.stringify([
      { frequency: 89300000, name: 'BBC Radio 4', pi: 0xC201, lastHeard: now - 3600, manual: false, mode: 'wfm', source: 'server' },
      { frequency: 101000000, name: 'My Station', pi: -1, lastHeard: now - 3600, manual: true, mode: 'wfm', source: 'server' },
    ]));
  };
  const L = await startServer('locals', { on: true, bands: 'both', waitMin: 5 }, ['--rate', '2400000'], { seed });
  const hold = ws(L.port, '/ws/user-spectrum?user_session_id=dxff0001&bins=256');   // ★ a listener: the scanner never sweeps
  await L.until(() => hold.hw);
  ok(await L.until(() => L.count(/DX log: \d+ local\(s\) handed to the discovered bookmarks/) >= 1, 15000),
     `★ the server hands its locals over (${(L.logText().match(/DX log: \d+ local\(s\) handed[^\n]*/) || ['nothing'])[0]})`);
  const getBm = async (S) => { const r = await fetch(`${S.H}/bookmarks`).then((x) => x.json()).catch(() => []); return Array.isArray(r) ? r : (r.bookmarks || []); };
  const at = (arr, hz) => arr.filter((x) => Math.abs(Number(x.frequency) - hz) < 1000);
  let bm = await getBm(L);
  ok(at(bm, 89300000).length === 1 && at(bm, 89300000)[0].name === 'BBC Radio 4' && at(bm, 89300000)[0].pi === 0xC201,
     `★ a local already learned from RDS: ONE row, its settled name kept (${at(bm, 89300000).map((x) => x.name).join(' | ')})`);
  const r2 = at(bm, 90500000)[0];
  ok(r2 && r2.name === 'BBC R2' && r2.pi === 0xC202 && /wfm/.test(r2.mode) && r2.manual === false && r2.scanned === true,
     `★ a new FM local: frequency, WFM, its PS and PI, discovered (not manual) — ${r2 ? `"${r2.name}" PI ${r2.pi.toString(16)} ${r2.mode}` : 'missing'}`);
  ok(at(bm, 95000000).length === 1 && /95\.0MHz \(no RDS\)/.test(at(bm, 95000000)[0].name), `an FM local with no RDS, by its frequency (${at(bm, 95000000)[0]?.name})`);
  ok(at(bm, 101000000).length === 1 && at(bm, 101000000)[0].name === 'My Station' && at(bm, 101000000)[0].manual === true,
     '★ the owner’s own bookmark on a local’s frequency is untouched');
  ok(at(bm, 98800000).length === 0, '★★ the DX catch (Sporadic-E, RAI R1 98.8) is NOT bookmarked — only locals');
  ok(at(bm, 104600000).length === 0 && at(bm, 91000000).length === 0, 'nor the suspect, nor the provisional');
  ok(at(bm, 102000000).length === 0, '★ a local handed over before, whose bookmark was deleted, does not come back');
  const dab = bm.filter((x) => /dab/i.test(x.mode || ''));
  ok(dab.length === 2 && dab.every((x) => Math.abs(x.frequency - 225648000) < 1000 && x.eid === 0xC1CE)
     && dab.some((x) => x.sid === 0xC0CE && x.name === 'Classic FM') && dab.some((x) => x.sid === 0xC0D2 && x.name === 'talkSPORT'),
     `★ the DAB local multiplex: every service on 12B with its SId (${dab.map((x) => `${x.name}/${x.sid?.toString(16)}`).join(', ')})`);
  ok(!bm.some((x) => Math.abs(x.frequency - 227360000) < 1000), '★★ the DX multiplex on 12C is NOT bookmarked');
  const dl = await fetch(`${L.H}/vibeserver/dxlog.json?local=1&limit=100`).then((r) => r.json());
  ok(dl.entries.find((e) => e.id === 2)?.inBookmarks === true && !dl.entries.find((e) => e.id === 5)?.inBookmarks,
     'the DX log says which locals are in the bookmarks');
  const total1 = bm.length;
  // the owner deletes one of the new ones
  const del = await fetch(`${L.H}/bookmarks?frequency=90500000&${await L.adminQ()}`, { method: 'DELETE' });
  ok(del.ok, `the owner deletes the BBC R2 bookmark (${del.status})`);
  await sleep(65000);   // ★ the next minute's pass
  bm = await getBm(L);
  ok(at(bm, 90500000).length === 0, '★ the next pass does not put the deleted local back');
  hold.close(); L.stop();
  await sleep(1500);
  // ★ a restart on the same files: nothing twice, the deletion holds
  const L2 = await startServer('locals', { on: true, bands: 'both', waitMin: 5 }, ['--rate', '2400000'], { dir: L.dir });
  const hold2 = ws(L2.port, '/ws/user-spectrum?user_session_id=dxff0002&bins=256');
  await L2.until(() => hold2.hw);
  await L2.until(() => /DX log: \d+ entries from/.test(L2.logText()), 8000);
  await sleep(5000);
  bm = await getBm(L2);
  ok(L2.count(/handed to the discovered bookmarks/) === 0, 'after a restart nothing is handed over twice');
  ok(bm.length === total1 - 1 && at(bm, 89300000).length === 1 && bm.filter((x) => /dab/i.test(x.mode || '')).length === 2 && at(bm, 90500000).length === 0,
     `★ no duplicates after a restart, and the deleted one stays deleted (${bm.length} rows, was ${total1} before the deletion)`);
  hold2.close(); L2.stop();
  await sleep(1000);
}

try {
  await testLocals();
  if (process.env.DX_TEST_ONLY !== 'locals') {
  // ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── A. FM: nobody here → the scan starts and finds the RDS stations ──');
  const S = await startServer('fm', { on: true, bands: 'fm', waitMin: 5, raster: 100 }, ['--rate', '2400000', '--fm-rds', STATIONS]);
  const a = ws(S.port, '/ws/user-spectrum?user_session_id=dxaa0001&bins=256');
  ok(await S.until(() => a.hw), 'hwinfo arrives');
  ok(a.hw?.dxLog === true, `hwinfo offers the DX Log (dxLog=${a.hw?.dxLog})`);
  a.send({ type: 'tune', frequency: 93200000, mode: 'wfm' });
  await sleep(2500);
  ok(S.count(/DX scanner: nobody here for/) === 0, 'nothing swept while somebody was listening');
  a.send({ type: 'leave' }); a.close();
  const leftAt = Date.now();
  ok(await S.until(() => S.count(/DX scanner: nobody here for [\d.]+ s/) >= 1, 15000), '★ with the radio idle past the wait, the scan starts');
  ok((Date.now() - leftAt) / 1000 >= 2.5, `…not before the wait (${((Date.now() - leftAt) / 1000).toFixed(1)} s)`);
  // two whole sweeps (12 hops each at 2.4 MS/s): with the test's short 2.5 s dwell a station can give its PI in one visit
  // and finish its PS in the next — the entry keeps the last PS it heard, as on air
  ok(await S.until(() => S.count(/DX scanner: FM [\d.]+.[\d.]+ MHz: /) >= 24, 180000), `two whole FM sweeps (${S.count(/DX scanner: FM [\d.]+.[\d.]+ MHz: /)} hops)`);
  const hopCpu = [...S.logText().matchAll(/DX scanner: FM ([\d.]+).([\d.]+) MHz: (\d+) peak\(s\) \+ \d+ weak, (\d+) identified.* in ([\d.]+) s .*?(\d+) worker\(s\), ([\d.]+) s CPU/g)];
  if (hopCpu.length) {
    const cpu = hopCpu.map((m) => Number(m[7])), wall = hopCpu.map((m) => Number(m[5]));
    console.log(`      hop wall ${Math.min(...wall).toFixed(1)}–${Math.max(...wall).toFixed(1)} s, process CPU per hop ${Math.min(...cpu).toFixed(2)}–${Math.max(...cpu).toFixed(2)} s`);
  }
  const list = await fetch(`${S.H}/vibeserver/dxlog.json?limit=100`).then((r) => r.json());
  const byPi = (pi) => list.entries.find((e) => e.pi === pi);
  for (const [pi, mhz, ps] of [['C201', 89.3, 'BBC R4'], ['5201', 98.8, 'RAI R1'], ['F201', 104.6, 'FR INTER']]) {
    const e = byPi(pi);
    // ★ 2026-11-04: the name is the WHOLE PS or none yet — never a half-built one ("FR INT") from an interrupted dwell.
    ok(e && e.freqKhz === Math.round(mhz * 1000) && (e.ps === ps || e.ps === '') && e.class === 'provisional',
       `${mhz} MHz PI ${pi} "${ps}" (or not named yet, never part of it) in the DX log, provisional (bootstrap) — ${e ? `${e.freqKhz} "${e.ps}" ${e.class}` : 'missing'}`);
  }
  ok(list.entries.filter((e) => e.ps).length >= 2, `the names arrive: ${list.entries.filter((e) => e.ps).map((e) => e.ps).join(', ')}`);
  ok(list.entries.filter((e) => e.band === 'FM').length === 3, `exactly the three RDS stations in the default view, no phantom PIs (${list.entries.length})`);
  const withSus = await fetch(`${S.H}/vibeserver/dxlog.json?limit=100&suspect=1`).then((r) => r.json());
  const sus = withSus.entries.filter((e) => e.class === 'suspect');
  ok(sus.every((e) => /image of/.test(e.suspectWhy || '')),
     `★ the dongle's IQ images of the strong stations (same PI, mirrored about the hop's DC) are Suspect, hidden by default: ${sus.map((e) => `${e.freqKhz / 1000} ${e.pi}`).join(', ') || 'none'}`);
  ok(list.bootstrap === true && list.scanner?.on === true, 'the page says the bootstrap is on, and what the scanner is set to');
  const q = await S.adminQ();
  const adm = await fetch(`${S.H}/vibeserver/admin/status?${q}`).then((r) => r.ok ? r.json() : null).catch(() => null);
  ok(!adm || (adm.dxScan && /scanning|waiting/.test(adm.dxScan.state)), `the admin line: ${adm?.dxScan ? `${adm.dxScan.state} — ${adm.dxScan.now || adm.dxScan.last}` : '(no admin status endpoint)'}`);

  // ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── B. a listener arrives MID-HOP: the radio back, the hop not logged ──');
  await S.until(() => /DX scanner: FM [\d.]+.[\d.]+ MHz: /.test(S.logText().split('\n').slice(-3).join('\n')), 20000);
  await sleep(700);   // ★ inside the next hop's dwell (2.5 s)
  const hopsBefore = S.count(/DX scanner: FM [\d.]+.[\d.]+ MHz: /);
  const b = ws(S.port, '/ws/user-spectrum?user_session_id=dxbb0002&bins=256');
  ok(await S.until(() => S.count(/DX scanner: stopped — a listener arrived/) >= 1, 6000), '★ a listener arriving stops the scan at once');
  ok(/DX scanner: stopped — a listener arrived \(FM hop \d+ of \d+/.test(S.logText()), '…in the middle of a hop, which is DISCARDED');
  ok(await S.until(() => vfo(b) === 93200000, 6000), `★ the newcomer lands on 93.200 MHz, where the radio was left — never a hop centre (${vfo(b)})`);
  ok(/DX scanner: the radio is back on 93200\.000 kHz/.test(S.logText()), 'the log says the radio went back');
  await sleep(3000);
  ok(S.count(/DX scanner: FM [\d.]+.[\d.]+ MHz: /) === hopsBefore, `★ the interrupted hop is NOT logged (${hopsBefore} hops before, ${S.count(/DX scanner: FM [\d.]+.[\d.]+ MHz: /)} after)`);

  // ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── C. an OLD client works normally ──');
  b.send({ type: 'tune', frequency: 98800000, mode: 'wfm' });
  ok(await S.until(() => vfo(b) === 98800000, 5000), `it tunes (98.8 MHz: ${vfo(b)})`);
  const frames0 = b.bin; await sleep(1500);
  ok(b.bin > frames0, `spectrum frames keep coming (${b.bin - frames0} in 1.5 s)`);
  ok(!b.txt.some((t) => /"type":"dx/.test(t)), 'no DX message type is ever pushed down the socket (old clients never see the DX log)');
  b.send({ type: 'leave' }); b.close();
  // ★ the next idle period carries on from the hop that was cut short — not from 87.5
  const startedAgain = await S.until(() => S.count(/DX scanner: nobody here for/) >= 2, 15000);
  ok(startedAgain, 'idle again: the scan restarts');
  const lines = S.logText().split('\n');
  const restartAt = lines.findLastIndex((l) => /DX scanner: nobody here for/.test(l));
  await S.until(() => S.logText().split('\n').slice(restartAt).some((l) => /DX scanner: FM [\d.]+.[\d.]+ MHz: /.test(l)), 20000);
  const firstAfter = S.logText().split('\n').slice(restartAt).find((l) => /DX scanner: FM [\d.]+.[\d.]+ MHz: /.test(l)) || '';
  ok(!/DX scanner: FM 87\.5/.test(firstAfter) || hopsBefore % 12 === 0, `★ the cursor: it resumes at the hop it was on, not the band's start (${firstAfter.replace(/.*DX scanner: /, '').slice(0, 40)})`);

  // ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── D. promote and clear ──');
  const id = byPi('5201')?.id;
  // ★ (Loopback is the owner here — the phone's own app — so a refusal cannot be shown from 127.0.0.1; promote is behind
  //   vsAdminHttpOk exactly as POST /bookmarks is.)
  const pr = await fetch(`${S.H}/vibeserver/dxlog/promote?id=${id}&${await S.adminQ()}`, { method: 'POST' }).then((r) => r.json());
  ok(pr.ok === true, `the owner promotes RAI R1 (${JSON.stringify(pr)})`);
  const bms = await fetch(`${S.H}/bookmarks`).then((r) => r.json()).catch(() => []);
  const arr = Array.isArray(bms) ? bms : (bms.bookmarks || []);
  ok(arr.some((x) => Math.abs(Number(x.frequency) - 98800000) < 1000 && /RAI R1/.test(x.name || '') && /wfm/i.test(x.mode || '')),
     '★ promote copies frequency, name and mode into the normal bookmarks');
  ok(!arr.some((x) => Math.abs(Number(x.frequency) - 89300000) < 1000), 'nothing else from the DX log went into the bookmarks');
  const cl = await fetch(`${S.H}/vibeserver/dxlog/clear?${await S.adminQ()}`, { method: 'POST' }).then((r) => r.json());
  const after = await fetch(`${S.H}/vibeserver/dxlog.json`).then((r) => r.json());
  ok(cl.ok && after.entries.length === 1 && after.entries[0].pi === '5201', `clear keeps the promoted entry (${after.entries.length} left)`);
  const wrote = fs.existsSync(path.join(S.dir, 'data', 'dxlog-dx-test.json'));
  ok(wrote, 'the log is written beside the gallery (data/dxlog-<serial>.json)');
  S.stop();
  await sleep(1500);

  // ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── E. DAB: the band scan in DX mode on the weak 12D reference capture ──');
  const TA = process.env.VIBE_TEST_AUDIO || path.join(os.homedir(), 'VibeSDR-test-audio');
  const clip = path.join(TA, 'dab', 'bench', '12D-v4l-2048.vbu8');
  if (!fs.existsSync(clip)) console.log(`   (DAB part not run — no ${clip})`);
  else {
    const raw = path.join(os.tmpdir(), `dx-12d-${process.pid}.u8`);
    fs.writeFileSync(raw, fs.readFileSync(clip).subarray(28));
    const D = await startServer('dab', { on: true, bands: 'dab', waitMin: 5 }, ['--rate', '2048000', '--dab', raw, '--dab-at', '229072000']);
    const t0 = Date.now();
    const done = await D.until(() => D.count(/\[DAB\] band scan: finished after/) >= 1, 400000);
    ok(done, `a whole DAB sweep (5A → 13F) in ${((Date.now() - t0) / 1000).toFixed(0)} s`);
    const line = D.logText().split('\n').find((l) => /DX scanner: DAB 12D: /.test(l)) || '';
    const band = D.logText().split('\n').find((l) => /\[DAB\] band scan: 12D /.test(l)) || '';
    console.log(`      ${line.replace(/.*DX scanner: /, '')}`);
    console.log(`      ${band.replace(/.*\[DAB\] /, '')}`);
    const empties = [...D.logText().matchAll(/\[DAB\] band scan: (\w+) nothing in ([\d.]+) s/g)].map((m) => Number(m[2]));
    if (empties.length) console.log(`      empty blocks: ${empties.length}, ${Math.min(...empties).toFixed(1)}–${Math.max(...empties).toFixed(1)} s each (mean ${(empties.reduce((x, y) => x + y, 0) / empties.length).toFixed(1)})`);
    const dl = await fetch(`${D.H}/vibeserver/dxlog.json?band=DAB`).then((r) => r.json());
    const e = dl.entries.find((x) => x.block === '12D');
    ok(e && e.eid && e.services.length > 0, `★ the 12D multiplex is in the DX log: EId ${e?.eid}, "${e?.ensemble}", ${e?.services?.length} services${e?.tii ? `, TII ${e.tii.main}/${e.tii.sub}` : ', no TII'}`);
    ok(dl.entries.length === 1, `nothing else in Band III (${dl.entries.length})`);
    const bm = await fetch(`${D.H}/bookmarks`).then((r) => r.json()).catch(() => []);
    const ba = Array.isArray(bm) ? bm : (bm.bookmarks || []);
    ok(!ba.some((x) => /dab/i.test(x.mode || '')), `★ DX mode adds NO bookmarks (${ba.filter((x) => /dab/i.test(x.mode || '')).length} DAB bookmarks)`);
    D.stop();
    try { fs.unlinkSync(raw); } catch {}
  }
  }   // ★ DX_TEST_ONLY=locals runs F alone
} catch (e) {
  console.log('   FAIL (exception)', e);
  fail++;
}
stopAll();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
