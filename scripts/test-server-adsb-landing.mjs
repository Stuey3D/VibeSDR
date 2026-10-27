// test-server-adsb-landing.mjs — ADS-B as a radio's landing, and "keep it running when nobody is connected" (V12).
//
// ★★★ STUART (2026-10-24): "can we set ADSB as a landing page for an SDR and when it is set that way have a tick box to
//     keep it running when nobody is connected that way a new connection instantly sees a list of planes and a full map".
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp replaying a 1090 MHz recording at 2.4 MS/s):
//     1. KEEP RUNNING: with landingAdsb + adsbKeepRunning the server is in ADS-B, decoding aircraft, BEFORE anybody has
//        connected (the log, and the radio's own status) — and nothing parks it, though the idle grace is 0;
//     2. a client connecting then gets a NON-EMPTY aircraft list within ~1 s, with no request of its own (the full
//        state from the server — the shared-dial contract), and its first config is already on 1090 MHz;
//     3. EXIT ADS-B puts the radio on the owner's PLAIN landing (landingFreq/demodMode are the fallback);
//     4. the listener leaving puts it back into ADS-B (the last listener left);
//     5. KEEP RUNNING OFF: nothing enters ADS-B before anybody connects, the first client LANDS in ADS-B and gets
//        reports, and the last listener leaving gives the receiver back;
//     6. ★ NEGATIVE: no ADS-B landing → no `adsb` report at all, so the positives cannot pass by accident;
//     7. ★★ PRECEDENCE: a Background Decoding profile due on the same idle radio does NOT run — kept-running ADS-B owns it;
//     8. ★ landingAdsb on a radio running at 2.048 MS/s without the rate switch: ADS-B cannot run, so the plain landing
//        applies and keep-running does not hold the radio.
// ★ SILENT: no audio socket is opened.
//
//   usage: VIBESERVER_BIN=… VIBE_ADSB_CLIP=~/VibeSDR-test-audio/adsb/flightaware-2026-10-24-2.4msps.u8 \
//          node scripts/test-server-adsb-landing.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ADSB_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree and VIBE_ADSB_CLIP to a 2.4 MS/s 1090 MHz recording');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const LAND = 96100000, ADSB = 1090000000;

/** A minimal WebSocket client recording every text frame with its arrival time (binary frames counted only). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, open: false, closed: false, t0: Date.now() };
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
      if (op === 0x2) st.bin++;
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
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.s.includes(`"type":"${type}"`)).map((x) => { try { return { t: x.t, ...JSON.parse(x.s) }; } catch { return { t: x.t }; } });
const centreOf = (c) => c.center_freq ?? c.centerFreq ?? 0;

async function server(radio, tag, rate = 2400000, extraEnv = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-adsbland-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'local', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: LAND, rate, demodMode: 'wfm',
               landingFreq: LAND, users: 1, ...radio }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort),
                                       '--rate', String(rate), '--adsb', CLIP], { stdio: 'ignore' });
  await sleep(800);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '19', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate),
                             '--idle-grace', '0', '--admin-pass', 'adsb-landing-test'], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
const spectrum = (port, sid) => ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(100); } return fn(); };

// ── 1-4. KEEP RUNNING ─────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nKeep ADS-B running when nobody is connected');
{
  const S = await server({ landingAdsb: true, adsbKeepRunning: true }, 'keep');
  try {
    // ★ Nobody connects for 8 s: the table must fill on its own.
    await sleep(8000);
    const log0 = S.log();
    ok(/\[ADS-B\] keep running: nobody is connected \(the server has started\)/.test(log0), 'the server entered ADS-B at its start, with nobody connected');
    ok(/\[ADS-B\] mode ON: 1090 MHz/.test(log0), '…and the decoder is running on 1090 MHz');
    ok(!/idle park in|dongle capture paused|RELEASED/i.test(log0), 'nothing parked or released the radio (idle grace 0)');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'adsb').some((m) => (m.aircraft || []).length > 0), 1500);
    const firstRep = parsed(A, 'adsb').find((m) => (m.aircraft || []).length > 0);
    ok(!!firstRep && firstRep.t <= 1200, `★ a new client has a non-empty aircraft list within ~1 s (${firstRep ? firstRep.aircraft.length + ' aircraft at ' + firstRep.t + ' ms' : 'none'})`);
    const cfg0 = parsed(A, 'config')[0] || {};
    ok(Math.abs(centreOf(cfg0) - ADSB) < 2e6, `its first config is already on 1090 MHz (${centreOf(cfg0)})`);
    ok(!/\[ADS-B\] new session — landing in ADS-B/.test(S.log()), 'the arrival did not re-enter (it joined the warm table)');
    // ── 3. EXIT ADS-B → the plain landing ──
    A.send({ type: 'adsb', on: 0 });
    await waitFor(() => parsed(A, 'adsb_off').length > 0, 3000);
    await sleep(800);
    const after = parsed(A, 'config').filter((c) => c.t >= (parsed(A, 'adsb_off')[0]?.t ?? 1e9));
    const last = after[after.length - 1] || {};
    ok(parsed(A, 'adsb_off').length > 0, 'EXIT ADS-B: adsb_off');
    const vfo = last.vfo ?? last.frequency ?? 0;
    ok(Math.abs(vfo - LAND) < 5000 && centreOf(last) < 500e6, `★ EXIT goes to the owner's landing ${LAND} (vfo ${vfo}, centre ${centreOf(last)})`);
    // ── 4. the listener leaves → back into ADS-B ──
    A.close();
    await waitFor(() => /keep running: nobody is connected \(the last listener left\)/.test(S.log()), 8000);
    ok(/keep running: nobody is connected \(the last listener left\)/.test(S.log()), '★ the listener leaving puts the radio back into ADS-B');
    const B = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(B, 'adsb').length > 0, 2500);
    ok(parsed(B, 'adsb').length > 0, 'and the next client is in ADS-B at once');
    B.close();
    await sleep(500);
  } finally { S.stop(); }
}

// ── 5. KEEP RUNNING OFF ───────────────────────────────────────────────────────────────────────────────────────────
console.log('\nADS-B as the landing, keep running OFF');
{
  const S = await server({ landingAdsb: true }, 'land');
  try {
    await sleep(4000);
    ok(!/\[ADS-B\] mode ON/.test(S.log()), 'nothing enters ADS-B before anybody connects');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'adsb').length > 0, 4000);
    ok(/\[ADS-B\] new session — landing in ADS-B/.test(S.log()), '★ the first client lands in ADS-B (logged with the reason)');
    ok(parsed(A, 'adsb').length > 0, 'and gets aircraft reports without asking');
    A.close();
    // ★ the server holds ADS-B up for 5 s after the last listener (a reconnect keeps it) — wait past that grace
    await waitFor(() => /\[ADS-B\] last listener left — the receiver goes back/.test(S.log()), 9000);
    ok(/\[ADS-B\] last listener left — the receiver goes back/.test(S.log()), 'the last listener leaving gives the receiver back');
    await sleep(3000);
    ok(!/\[ADS-B\] keep running/.test(S.log()), 'and nothing keeps it running');
  } finally { S.stop(); }
}

// ── 6. NEGATIVE ───────────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nNo ADS-B landing (negative)');
{
  const S = await server({}, 'none');
  try {
    const A = spectrum(S.port, crypto.randomUUID());
    await sleep(3500);
    ok(parsed(A, 'adsb').length === 0 && !/\[ADS-B\] mode ON/.test(S.log()), 'no adsb report, ADS-B never entered');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

// ── 7. PRECEDENCE AGAINST BACKGROUND DECODING ─────────────────────────────────────────────────────────────────────
console.log('\nKept-running ADS-B and a Background Decoding profile on the same radio');
{
  const bg = JSON.stringify({ on: true, waitMin: 1, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
                              vfos: [{ hz: 517500, mode: 'usb', dec: 'navtex' }] }] });
  const S = await server({ landingAdsb: true, adsbKeepRunning: true, bgDecode: bg }, 'bg', 2400000, { VIBESERVER_BG_WAIT_SEC: '0' });
  try {
    await sleep(7000);
    ok(/\[ADS-B\] mode ON: 1090 MHz/.test(S.log()), 'ADS-B is running with nobody connected');
    ok(!/background decoding: (nobody here for|moving the radio)/.test(S.log()), '★ the background profile never moved the radio (ADS-B owns the idle radio)');
  } finally { S.stop(); }
}

// ── 8. A RADIO THAT CANNOT RUN ADS-B NOW ──────────────────────────────────────────────────────────────────────────
console.log('\nADS-B landing on a 2.048 MS/s radio without the rate switch');
{
  const S = await server({ landingAdsb: true, adsbKeepRunning: true }, 'slow', 2048000);
  try {
    await sleep(4000);
    ok(/keep running is set, but ADS-B cannot run on this receiver now/.test(S.log()), 'keep-running says why it does not hold the radio');
    ok(!/\[ADS-B\] mode ON/.test(S.log()), 'ADS-B is not entered');
    const A = spectrum(S.port, crypto.randomUUID());
    await sleep(2500);
    ok(parsed(A, 'adsb').length === 0, 'a client gets the plain landing, no adsb reports');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
