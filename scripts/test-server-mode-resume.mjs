// test-server-mode-resume.mjs — a listener coming back to the whole-radio mode they left in (V12, 2026-10-27).
//
// ★★★ STUART (iPhone, V12 498/499): "a couple of times I was in the map view for NDB or ADS-B and when I moved to another
//     app leaving VibeSDR running in the background when I switched back the map dropped back to a waterfall view on the
//     previous frequency, this may have been poor cellular connection though."
//     ADS-B / NDB / ISM / AIS send no audio, so the app lets go of the audio session and iOS suspends it seconds after it
//     goes to the background; its sockets go with it. The server waited 5 s and gave the radio back, telling nobody.
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp replaying a 1090 MHz recording at 2.4 MS/s), ADS-B standing for
//     every whole-radio mode (one grace, one record, one handler — local_sdr_shim.cpp modeLeaveGraceS / modeResume):
//     1. THE GRACE FOLLOWS THE OWNER'S IDLE GRACE (capped at 60 s): with --idle-grace 30 the mode is still up 10 s after
//        the last socket closed, and the same session reconnecting finds it running (reports at once, nothing re-entered);
//     2. --idle-grace 0 keeps the old 5 s (a socket swap), and the radio is then given back;
//     3. ★ mode_resume after that leave, from the returning session, alone on an untouched radio: ok:1 and ADS-B again;
//     4. ★ NEGATIVE — somebody else on the radio: refused in words, ADS-B NOT entered (the shared-dial contract);
//     5. ★ NEGATIVE — the radio tuned since the leave: refused, not entered;
//     6. ★ NEGATIVE — no leave of ours to undo (a fresh server): refused, not entered;
//     7. still in the mode: a plain ok, nothing re-entered.
// ★ SILENT: no audio socket is opened.
//
//   usage: VIBESERVER_BIN=… VIBE_ADSB_CLIP=~/VibeSDR-test-audio/adsb/flightaware-2026-10-24-2.4msps.u8 \
//          node scripts/test-server-mode-resume.mjs
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
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
// ★ A deadline on the whole run: a hung server must never leave this waiting (or its children running).
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 4 minutes'); killAll(); process.exit(1); }, 240_000);
deadline.unref();

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
  // ★ Like a suspended phone: the socket just goes (no close frame) — the server must notice on its own.
  st.drop = () => { s.destroy(); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.s.includes(`"type":"${type}"`)).map((x) => { try { return { t: x.t, ...JSON.parse(x.s) }; } catch { return { t: x.t }; } });
const centreOf = (c) => c.center_freq ?? c.centerFreq ?? 0;

async function server(tag, idleGrace) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-moderesume-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'local', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: LAND, rate: 2400000, demodMode: 'wfm',
               landingFreq: LAND, users: 3 }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort),
                                       '--rate', '2400000', '--adsb', CLIP], { stdio: 'ignore' });
  children.add(rtl);
  await sleep(800);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '19', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000',
                             '--idle-grace', String(idleGrace), '--admin-pass', 'mode-resume-test'], { env, stdio: ['ignore', log, log] });
  children.add(srv);
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); children.delete(srv); children.delete(rtl); } };
}
const spectrum = (port, sid) => ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(100); } return fn(); };
const gaveBack = (S) => /\[ADS-B\] last listener left — the receiver goes back/.test(S.log());
const entries = (S) => (S.log().match(/\[ADS-B\] mode ON: 1090 MHz/g) || []).length;

/** Into ADS-B with session `sid`, then the socket simply goes (a suspended phone). */
async function enterAndVanish(S, sid) {
  const A = spectrum(S.port, sid);
  await waitFor(() => parsed(A, 'config').length > 0, 3000);
  A.send({ type: 'adsb', on: 1 });
  await waitFor(() => parsed(A, 'adsb').length > 0, 6000);
  const inAdsb = parsed(A, 'adsb').length > 0;
  A.drop();
  return inAdsb;
}

try {
  // ── 1. the grace follows the owner's idle grace ────────────────────────────────────────────────────────────────
  console.log('\nThe whole-radio grace follows the idle grace (--idle-grace 30)');
  {
    const S = await server('grace', 30);
    try {
      const sid = crypto.randomUUID();
      ok(await enterAndVanish(S, sid), 'in ADS-B, then the socket goes');
      await waitFor(() => /last listener left — ADS-B stays up for 30 s/.test(S.log()), 4000);
      ok(/last listener left — ADS-B stays up for 30 s/.test(S.log()), '★ "stays up for 30 s" — not 5');
      await sleep(10_000);
      ok(!gaveBack(S), '★ still in ADS-B 10 s after the last socket went (the old grace gave it back at 5 s)');
      const A2 = spectrum(S.port, sid);
      await waitFor(() => parsed(A2, 'adsb').length > 0, 2500);
      ok(parsed(A2, 'adsb').length > 0, 'the same session back: reports at once');
      ok(/listener returned within 30 s — ADS-B kept/.test(S.log()), '…the leave cancelled, logged');
      ok(entries(S) === 1, `…and nothing was re-entered (${entries(S)} entry)`);
      A2.close(); await sleep(300);
    } finally { S.stop(); }
  }

  // ── 2-3. the old 5 s, then mode_resume ─────────────────────────────────────────────────────────────────────────
  console.log('\nGrace 0 → the old 5 s; then the returning listener asks for ADS-B back');
  {
    const S = await server('resume', 0);
    try {
      const sid = crypto.randomUUID();
      ok(await enterAndVanish(S, sid), 'in ADS-B, then the socket goes');
      await waitFor(() => gaveBack(S), 12_000);
      ok(gaveBack(S) && /ADS-B stays up for 5 s/.test(S.log()), 'idle grace 0: the radio is given back after 5 s, as before');
      const A2 = spectrum(S.port, sid);
      await waitFor(() => parsed(A2, 'config').length > 0, 3000);
      await sleep(1500);
      const cfg = parsed(A2, 'config').at(-1) || {};
      ok(parsed(A2, 'adsb').length === 0 && centreOf(cfg) < 500e6, `the returning socket finds the waterfall on the old dial (centre ${centreOf(cfg)}) — Stuart's report`);
      A2.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A2, 'adsb').length > 0, 6000);
      const ans = parsed(A2, 'mode_resume')[0] || {};
      ok(ans.ok === 1 && ans.mode === 'adsb', `★ mode_resume: ok (${JSON.stringify(ans)})`);
      ok(parsed(A2, 'adsb').length > 0, '★ …and ADS-B reports again');
      const cfg2 = parsed(A2, 'config').at(-1) || {};
      ok(Math.abs(centreOf(cfg2) - ADSB) < 2e6, `…the radio on 1090 MHz (${centreOf(cfg2)})`);
      ok(/a returning listener asked for it back .* so it goes back in/.test(S.log()), '…logged with its reason');

      // ── 7. still in the mode: a plain ok ──
      const n = entries(S);
      A2.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A2, 'mode_resume').length > 1, 3000);
      ok((parsed(A2, 'mode_resume')[1] || {}).ok === 1 && entries(S) === n, 'asked while still in ADS-B: ok, nothing re-entered');
      A2.close(); await sleep(300);
    } finally { S.stop(); }
  }

  // ── 4. NEGATIVE: somebody else is on the radio ─────────────────────────────────────────────────────────────────
  console.log('\nRefused: somebody else is listening now');
  {
    const S = await server('other', 0);
    try {
      const sid = crypto.randomUUID();
      ok(await enterAndVanish(S, sid), 'in ADS-B, then the socket goes');
      await waitFor(() => gaveBack(S), 12_000);
      const B = spectrum(S.port, crypto.randomUUID());       // somebody else arrives, listening to the old dial
      await waitFor(() => parsed(B, 'config').length > 0, 3000);
      const A2 = spectrum(S.port, sid);
      await waitFor(() => parsed(A2, 'config').length > 0, 3000);
      const n = entries(S);
      A2.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A2, 'mode_resume').length > 0, 3000);
      await sleep(1500);
      const ans = parsed(A2, 'mode_resume')[0] || {};
      ok(ans.ok === 0 && /somebody else is listening/.test(ans.why || ''), `★ refused in words (${ans.why})`);
      ok(entries(S) === n && parsed(B, 'adsb').length === 0, '★ ADS-B NOT entered — the other listener keeps their dial');
      A2.close(); B.close(); await sleep(300);
    } finally { S.stop(); }
  }

  // ── 5. NEGATIVE: tuned since ───────────────────────────────────────────────────────────────────────────────────
  console.log('\nRefused: the radio has been tuned since');
  {
    const S = await server('tuned', 0);
    try {
      const sid = crypto.randomUUID();
      ok(await enterAndVanish(S, sid), 'in ADS-B, then the socket goes');
      await waitFor(() => gaveBack(S), 12_000);
      const C = spectrum(S.port, crypto.randomUUID());       // somebody comes, retunes, goes
      await waitFor(() => parsed(C, 'config').length > 0, 3000);
      C.send({ type: 'tune', frequency: 98500000, mode: 'wfm' });
      await sleep(1200);
      C.close();
      await sleep(800);
      const A2 = spectrum(S.port, sid);
      await waitFor(() => parsed(A2, 'config').length > 0, 3000);
      const n = entries(S);
      A2.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A2, 'mode_resume').length > 0, 3000);
      await sleep(1500);
      const ans = parsed(A2, 'mode_resume')[0] || {};
      ok(ans.ok === 0 && /tuned it since/.test(ans.why || ''), `★ refused in words (${ans.why})`);
      ok(entries(S) === n && parsed(A2, 'adsb').length === 0, '★ ADS-B NOT entered');
      A2.close(); await sleep(300);
    } finally { S.stop(); }
  }

  // ── 6. NEGATIVE: no leave of ours to undo ──────────────────────────────────────────────────────────────────────
  console.log('\nRefused: nothing to resume (a fresh server)');
  {
    const S = await server('fresh', 0);
    try {
      const A = spectrum(S.port, crypto.randomUUID());
      await waitFor(() => parsed(A, 'config').length > 0, 3000);
      A.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A, 'mode_resume').length > 0, 3000);
      await sleep(1500);
      const ans = parsed(A, 'mode_resume')[0] || {};
      ok(ans.ok === 0 && !!ans.why, `★ refused in words (${ans.why})`);
      ok(entries(S) === 0 && parsed(A, 'adsb').length === 0, '★ ADS-B NOT entered');
      A.send({ type: 'mode_resume', mode: 'bogus' });
      await sleep(800);
      ok(parsed(A, 'mode_resume').length === 1, 'an unknown mode name is ignored');
      A.close(); await sleep(300);
    } finally { S.stop(); }
  }
} finally {
  killAll();
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
