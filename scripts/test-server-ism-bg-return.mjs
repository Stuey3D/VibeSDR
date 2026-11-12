// test-server-ism-bg-return.mjs — an ISM listener the phone suspends, on a radio whose idle default is ADS-B (V12, 2026-11-12).
//
// ★★★ STUART (iPhone, the Pi 500's V4L, idle default ADS-B kept running): "when using the ISM band and backgrounding the app I
//     get the notification that it will be put into powersave by the iPhone etc but when I switch back to the app I stay in
//     ISM mode but for some reason it tunes to 1089.590MHz which is obviously the adsb frequency but nudged off slightly and
//     that radio defaults back to ADSB when it is idle so it looks like the keep awake is not working."
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp, noise only — no clip needed), --idle-grace 20 (a 20 s hold):
//     1. THE HOLD: back inside it, ISM carries on (nothing re-entered, ADS-B never entered);
//     2. ★ ADS-B KEEP WAITS FOR ISM'S OWN LEAVE: past the hold the ISM leave comes first (recorded for the resume), never
//        kept-running ADS-B entering over a mode that is still on;
//     3. ★★ BACK PAST THE HOLD, alone, to a radio nobody else used (only the server's own idle ADS-B ran): the SAME session
//        is put back in ISM on the band it left, BEFORE its first config — it never sees an ADS-B report or 1090 MHz;
//     4. ★ the in-socket `mode_resume` is granted over kept-running ADS-B too (the idle default is not somebody's use);
//     5. ★ NEGATIVE — a different session arriving at the idle radio gets ADS-B (the idle default), never ISM;
//     6. ★ NEGATIVE — somebody else listening when the ISM listener comes back: NOT moved, the returning session is told
//        `ism_off` and gets ADS-B — the truth, never "ISM" over a radio somewhere else;
//     7. ★ an exact ISM frequency outside every ISM allocation (1089.59 MHz) is refused — in ISM (the band stays) and from
//        ADS-B (ism_error, ADS-B stays): the frequency box can never put "ISM" on 1090.
// ★ SILENT: no audio socket is opened.
//
//   usage: VIBESERVER_BIN=…/vibeserver node scripts/test-server-ism-bg-return.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
const ONLY = (process.env.PARTS || '1234567').split('');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const LAND = 96100000, GRACE = 20;
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 8 minutes'); killAll(); process.exit(1); }, 8 * 60_000);
deadline.unref();

/** A minimal WebSocket client recording text frames. `drop` = a suspended phone (no close frame). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, closed: false, t0: Date.now() };
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
      if (op === 0x1) st.txt.push({ t: Date.now() - st.t0, s: p.toString() });
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.drop = () => { s.destroy(); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.s.includes(`"type":"${type}"`)).map((x) => { try { return { t: x.t, ...JSON.parse(x.s) }; } catch { return { t: x.t }; } });
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(200); } return fn(); };
const spectrum = (port, sid) => ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);

async function server(tag, users = 1) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-ismbg-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'local', port: front, locator: 'IO92nh', adminPass: 'ismbg-test-admin',
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: LAND, rate: 2400000, demodMode: 'wfm',
               landingFreq: LAND, users, landingAdsb: true, adsbKeepRunning: true }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  children.add(rtl);
  for (let i = 0; i < 50; i++) {   // ★ wait for the fake radio's port, not a fixed sleep
    const up = await new Promise((r) => { const c = net.connect(rtlPort, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '19', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000',
                             '--idle-grace', String(GRACE)], { env, stdio: ['ignore', log, log] });
  children.add(srv);
  for (let i = 0; i < 150; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const S = { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
              stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); children.delete(srv); children.delete(rtl); } };
  ok(await waitFor(() => /\[ADS-B\] mode ON: 1090 MHz/.test(S.log()), 10000), `[${tag}] the idle radio is in ADS-B (kept running) before anybody connects`);
  return S;
}
/** A listener (session `sid`) who goes into ISM 433 and is then suspended (the socket just goes). */
async function ismThenSuspend(S, sid) {
  const A = spectrum(S.port, sid);
  await waitFor(() => parsed(A, 'adsb').length > 0, 4000);
  A.send({ type: 'ism', on: 1, band: '433' });
  ok(await waitFor(() => parsed(A, 'ism').some((m) => m.band === '433'), 5000), 'the listener is in ISM 433.92');
  A.drop();
  ok(await waitFor(() => /session is HELD for 20 s/.test(S.log()), 5000), 'suspended: the session is held for 20 s');
  return A;
}
const pastTheHold = (S) => waitFor(() => /\[ADS-B\] keep running: nobody is connected \(the last listener left\)/.test(S.log()), (GRACE + 15) * 1000);
const lastCfg = (st) => parsed(st, 'config').at(-1) || {};

try {
  if (ONLY.includes('1')) {
    console.log('\n1. Back inside the hold');
    const S = await server('inside');
    try {
      const SID = crypto.randomUUID();
      await ismThenSuspend(S, SID);
      await sleep(8000);
      const B = spectrum(S.port, SID);
      ok(await waitFor(() => parsed(B, 'ism').some((m) => m.band === '433'), 4000), 'back after 8 s: ISM 433 reports carry on');
      ok(parsed(B, 'adsb').length === 0, 'and no ADS-B report');
      ok(/held session [0-9a-f]{8} returned/.test(S.log()), 'the server logged the held session returning');
      B.close(); await sleep(300);
    } finally { S.stop(); }
  }

  if (ONLY.includes('2') || ONLY.includes('3') || ONLY.includes('4') || ONLY.includes('7')) {
    console.log('\n2-4, 7. Back past the hold, alone, to the idle radio');
    const S = await server('past');
    try {
      const SID = crypto.randomUUID();
      await ismThenSuspend(S, SID);
      ok(await pastTheHold(S), 'past the hold: kept-running ADS-B takes the idle radio');
      const log = S.log();
      const iLeave = log.indexOf('[ISM] mode OFF'), iKeep = log.indexOf('keep running: nobody is connected (the last listener left)');
      ok(iLeave >= 0 && iKeep > iLeave, `★ 2. ISM left through its own door BEFORE kept ADS-B entered (ISM off at ${iLeave}, keep at ${iKeep})`);
      ok(/\[ISM\] last listener left — the receiver goes back/.test(log), '   …the leave recorded as ours (for the resume)');
      await sleep(1500);
      // ── 3. the same session comes back ──
      const B = spectrum(S.port, SID);
      ok(await waitFor(() => parsed(B, 'ism').some((m) => m.band === '433' && !m.custom), 5000),
         `★★ 3. the same session is back in ISM on 433.92 (${(parsed(B, 'ism').at(-1) || {}).bandName || 'no ism report'})`);
      const cfg0 = parsed(B, 'config')[0] || {};
      ok(Math.abs((cfg0.vfo || 0) - 433.92e6) < 1, `★ its FIRST config is already ISM's (vfo ${cfg0.vfo}) — never 1090 MHz`);
      ok(parsed(B, 'adsb').length === 0, `★ it never saw an ADS-B report (${parsed(B, 'adsb').length})`);
      // ── 7. an exact frequency outside every ISM allocation, inside ISM ──
      B.send({ type: 'tune', frequency: 1089590000 });
      await sleep(1500);
      const r7 = parsed(B, 'ism').at(-1) || {};
      ok(r7.band === '433' && !r7.custom && Math.abs((lastCfg(B).vfo || 0) - 433.92e6) < 1,
         `★ 7. a tune to 1089.590 MHz inside ISM is refused — still 433 (${r7.band}, custom ${r7.custom}, vfo ${lastCfg(B).vfo})`);
      B.send({ type: 'tune', frequency: 433420000 });
      ok(await waitFor(() => { const r = parsed(B, 'ism').at(-1) || {}; return r.custom === 1 && Math.abs(r.centre - 433.42e6) < 1; }, 5000),
         '   …and an exact frequency INSIDE the 433 allocation still works (433.420)');
      // ── 4. the in-socket ask, over kept ADS-B ──
      B.drop();
      ok(await waitFor(() => (S.log().match(/keep running: nobody is connected \(the last listener left\)/g) || []).length >= 2, (GRACE + 15) * 1000),
         'suspended again past the hold: kept ADS-B again');
      await sleep(1500);
      const C = spectrum(S.port, crypto.randomUUID());   // ★ a different session: the in-socket ask is the old app's path
      await waitFor(() => parsed(C, 'adsb').length > 0, 4000);
      ok(parsed(C, 'adsb').length > 0 && parsed(C, 'ism').length === 0, '★ 5. a DIFFERENT session arriving gets the idle default (ADS-B), not ISM');
      // ── 7b. an out-of-allocation exact frequency from ADS-B ──
      C.send({ type: 'ism', on: 1, hz: 1089590000 });
      ok(await waitFor(() => parsed(C, 'ism_error').length > 0, 3000), `★ 7. ISM at 1089.590 MHz from ADS-B: ism_error (${(parsed(C, 'ism_error')[0] || {}).why})`);
      await sleep(1200);
      ok(parsed(C, 'ism').length === 0 && !/\[ADS-B\] mode OFF/.test(S.log().slice(S.log().lastIndexOf('keep running: nobody'))), '   …and ADS-B stays');
      C.close();
      await sleep(1500);
      const D = spectrum(S.port, SID);
      D.send({ type: 'mode_resume', mode: 'ism' });   // ★ the original session asks (an app on a socket that lapsed)
      ok(await waitFor(() => parsed(D, 'ism').some((m) => m.band === '433'), 6000), `★ 4. the ISM listener back: in ISM again (${(parsed(D, 'mode_resume')[0] || {}).ok ?? 'no ask needed'})`);
      ok(parsed(D, 'mode_resume').every((m) => m.ok === 1), '   …any mode_resume answer is a yes');
      D.close(); await sleep(300);
    } finally { S.stop(); }
  }

  if (ONLY.includes('6')) {
    console.log('\n6. Somebody else listening when the ISM listener comes back (two-listener radio)');
    const S = await server('other', 2);
    try {
      const SID = crypto.randomUUID();
      await ismThenSuspend(S, SID);
      ok(await pastTheHold(S), 'past the hold: kept ADS-B');
      const X = spectrum(S.port, crypto.randomUUID());
      await waitFor(() => parsed(X, 'adsb').length > 0, 4000);
      const B = spectrum(S.port, SID);
      await waitFor(() => parsed(B, 'adsb').length > 0, 5000);
      await sleep(1500);
      ok(parsed(X, 'ism').length === 0 && parsed(X, 'adsb_off').length === 0, '★ the other listener was NOT moved (no adsb_off, no ism)');
      ok(parsed(B, 'adsb').length > 0 && parsed(B, 'ism').length === 0, 'the returning session gets ADS-B — where the radio is');
      ok(parsed(B, 'ism_off').length > 0, `★ …and is TOLD its ISM has ended (ism_off ${parsed(B, 'ism_off').length})`);
      X.close(); B.close(); await sleep(300);
    } finally { S.stop(); }
  }
} finally { killAll(); }
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
