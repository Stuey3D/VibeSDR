#!/usr/bin/env node
// test-server-owner-scans.mjs — the V12 OWNER'S SCANS end to end (Stuart, 2026-11-04: "in here is where there should be a
// button for FM scan and DAB scan so a server admin can trigger a scan on a server"; "When a server owner has triggered
// a scan to store stations on a shared VFO it should say ADMIN Triggered SCAN In Progress, Please Wait and the controls
// get locked out"; "As server admin is king they do not have to worry about other users").
//
// Real server on a SHARED radio (--users 3), fake-rtl-tcp --fm-rds (three RDS stations), no browser, silent.
//   A. GET /vibeserver/admin/bandscan: admin-gated; says FM SCAN may start (fmWhyNot ""), shared:true.
//   B. With a listener on 93.2 MHz, POST band=fm&act=start STARTS (no "nobody listening" rule for the owner); the listener
//      is told (`fm_scan` lock:true); their TUNE is REFUSED by the server (notice scan:1) and the dial does not go there;
//      a second listener arriving mid-scan gets the lock at once and is not landed.
//   C. The scan finds the three stations (PI + PS), stores them as SCANNED wfm bookmarks, and puts the dial back on
//      93.2 MHz; the lock clears; the listener can tune again.
//   D. dab_scan_get / fm_scan_get answer anyone (read-only); a cancel mid-FM-scan ends it and restores the dial.
// ★ VIBESERVER_DX_DWELL_SEC shortens each hop's dwell (TEST-ONLY, the DX scanner's own knob).
// usage: VIBESERVER_BIN=… node scripts/test-server-owner-scans.mjs   (~2 min; exit 3 = not run)
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
  st.all = (type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
  return st;
}

const ADMIN = 'owner-scan-pass';
const STATIONS = '89.3:C201:BBC_R4,98.8:5201:RAI_R1,104.6:F201:FR_INTER';
const procs = [];
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ownerscan-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Owner scan test', locator: 'IO92nh', adminPass: ADMIN,
  radios: [{ serial: 'own-scan', label: 'Test radio', enabled: true, configured: true, users: 3, rate: 2400000 }],
}));
const rtlPort = await freePort(), port = await freePort();
procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--fm-rds', STATIONS], { stdio: 'ignore' }));
await sleep(400);
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm', '--users', '3'],
  { env: { ...process.env, VIBESERVER_DX_DWELL_SEC: '2.5', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
           VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') },
    stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
procs.push(srv);
const H = `http://127.0.0.1:${port}`;
for (let i = 0; i < 100; i++) { try { await fetch(`${H}/vibeserver.json`); break; } catch { await sleep(150); } }
const logText = () => fs.readFileSync(logPath, 'utf8');
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); };
const adminQ = async () => {
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};
const scans = async (extra = '', method = 'GET') =>
  fetch(`${H}/vibeserver/admin/bandscan?${await adminQ()}${extra ? '&' + extra : ''}`, { method, body: method === 'POST' ? '{}' : undefined }).then((r) => r.json());
const vfo = (st) => { const c = st.last('config'); return c ? Number(c.vfo) : NaN; };

try {
  console.log('── A. the admin page asks ──');
  const noAuth = await fetch(`${H}/vibeserver/admin/bandscan`);
  ok(noAuth.status === 401, `admin-gated (${noAuth.status} without the password)`);
  const a0 = await scans();
  ok(a0.fmWhyNot === '' && a0.shared === true && a0.fm?.type === 'fm_scan' && a0.dab?.type === 'dab_scan',
     `FM SCAN may start on the shared radio (fmWhyNot "${a0.fmWhyNot}", dabWhyNot "${a0.dabWhyNot}")`);

  console.log('── B. the owner scans FM with a listener on — the listener is locked out ──');
  const L = ws(port, '/ws/user-spectrum?user_session_id=ownl0001&bins=256');
  await until(() => L.hw);
  L.send({ type: 'tune', frequency: 93200000, mode: 'wfm' });
  ok(await until(() => vfo(L) === 93200000), `the listener is on 93.2 MHz (${vfo(L)})`);
  const st = await scans('band=fm&act=start', 'POST');
  ok(!st.error && st.fm?.running === true, `★ the scan starts though somebody is listening — admin is king (${st.error || 'running'})`);
  ok(await until(() => L.all('fm_scan').some((m) => m.running && m.lock === true)), '★ the listener is told: fm_scan running, lock:true');
  const again = await scans('band=dab&act=start', 'POST');
  ok(/already running/.test(again.error || ''), `a DAB scan cannot start beside it ("${again.error}")`);
  await sleep(1500);
  L.send({ type: 'tune', frequency: 98800000, mode: 'wfm' });
  ok(await until(() => L.txt.some((t) => t.includes('"type":"notice"') && t.includes('"scan":1') && /ADMIN TRIGGERED SCAN/.test(t)), 4000),
     '★ the listener’s tune is REFUSED by the server, in words');
  ok(/tune refused — the owner's band scan is running/.test(logText()), '…and the log says so');
  const M = ws(port, '/ws/user-spectrum?user_session_id=ownm0002&bins=256');
  ok(await until(() => M.all('fm_scan').some((m) => m.running && m.lock === true), 5000), '★ an arrival mid-scan gets the lock at once');
  ok(!/new session — landing on/.test(logText()), '…and is not landed (the dial stays the scan’s)');

  console.log('── C. the result, the bookmarks, the dial back ──');
  ok(await until(() => L.all('fm_scan').some((m) => m.finished), 150000), 'the scan finishes');
  const fin = L.last('fm_scan');
  const pis = (fin.found || []).map((h) => h.pi).sort().join(',');
  ok(['5201', 'C201', 'F201'].every((p) => pis.includes(p)), `★ the three RDS stations are found (${(fin.found || []).map((h) => `${h.khz / 1000} ${h.pi} "${h.ps}"`).join(', ')})`);
  ok((fin.found || []).every((h) => ['C201', '5201', 'F201'].includes(h.pi)), 'no phantom PI, no IQ image');
  ok(fin.added >= 3 && !fin.cancelled && fin.lock !== true, `${fin.added} new bookmarks, not cancelled, lock off`);
  const bm = await fetch(`${H}/bookmarks`).then((r) => r.json());
  const arr = Array.isArray(bm) ? bm : (bm.bookmarks || []);
  // ★ named by the PS when the dwell assembled it whole, else the provisional "PI: XXXX f MHz" (the test's 2.5 s dwell can
  //   end before a PS is complete — RDS learning renames it in place on the next listen)
  for (const [mhz, name, pi] of [[89.3, 'BBC R4', 'C201'], [98.8, 'RAI R1', '5201'], [104.6, 'FR INTER', 'F201']]) {
    const b = arr.find((x) => Math.abs(Number(x.frequency) - mhz * 1e6) < 1000 && /wfm/i.test(x.mode || ''));
    ok(b && (b.name === name || b.name === `PI: ${pi} ${mhz}MHz`), `bookmark ${mhz} MHz (wfm) "${b?.name}"`);
  }
  const bmf = fs.readdirSync(path.join(dir, 'data')).find((f) => /^bookmarks.*\.json$/.test(f));
  ok(bmf && (fs.readFileSync(path.join(dir, 'data', bmf), 'utf8').match(/"scanned":true/g) || []).length >= 3, '★ stored as SCANNED (the year-long expiry)');
  ok(await until(() => vfo(L) === 93200000, 6000), `★ the dial is back on 93.2 MHz (${vfo(L)})`);
  L.send({ type: 'tune', frequency: 98800000, mode: 'wfm' });
  ok(await until(() => vfo(L) === 98800000, 5000), `the listener can tune again (${vfo(L)})`);

  console.log('── D. read-only state for anyone; cancel ──');
  L.send({ type: 'dab_scan_get' }); L.send({ type: 'fm_scan_get' });
  ok(await until(() => L.last('dab_scan') && L.all('fm_scan').length > 0, 3000), 'dab_scan_get and fm_scan_get answer a listener');
  await scans('band=fm&act=start', 'POST');
  ok(await until(() => L.all('fm_scan').filter((m) => m.running).length > 0 && L.last('fm_scan').running, 5000), 'a second scan starts');
  await sleep(2000);
  await scans('band=fm&act=cancel', 'POST');
  ok(await until(() => L.last('fm_scan')?.finished && L.last('fm_scan')?.cancelled, 20000), 'CANCEL ends it');
  ok(await until(() => vfo(L) === 98800000, 6000), `…and the dial is back where the listener left it (${vfo(L)})`);
  L.close(); M.close();
} catch (e) {
  console.error(e); fail++;
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  await sleep(500);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (logs kept in ${dir})`);
}
console.log(`test-server-owner-scans: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
