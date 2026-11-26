// test-server-bg-split.mjs — b96-bgsplit: BACKGROUND DECODING and WHERE NEW LISTENERS LAND as two choices, on a REAL
// vibeserver (fake rtl_tcp, synthetic IQ — this pins the radio's life cycle, not a decoder's output).
//
// ★★★ STUART (11-26): "for ADS-B, VDL2, AIS etc to work the user needs to have the map as the landing page. Listen-Amateur
//     uses FM radio as his landing zone, so if he enabled background AIS that would break that."
//   1. bg AIS + landing FM 96.1 WFM: AIS from the start with nobody here; a listener arrives → AIS pauses, they land on
//      96.1 WFM with no AIS report; they leave → AIS comes back (and again, a second round).
//   2. bg AIS + landing on the map in ADS-B: a listener arrives → AIS pauses, they land in ADS-B; the ship store stays served
//      (ais-ships.json answers, never "off"); they leave → ADS-B leaves, AIS comes back.
//   3. bg ADS-B + landing FM: ADS-B kept from the start, paused for the listener (no adsb report), back after.
//   4. bg = the landing's own mode (bg VDL2 + map in VDL2, the b96-keeprun shape written in the NEW words): joined, not paused.
//   5. the RATE GATE: bg ACARS + VDL2 at 2.4 MS/s cannot run — said why, the radio idles; the FM landing is untouched.
//   6. SINGLE MODE overrides both: singleMode vdl2 over bg AIS + landing FM → VDL2, never AIS, a listener joins VDL2.
//   7. manual (no bgMode, no keep words): nothing whole-radio enters.
// ★ SILENT: no audio socket is opened.
//   usage: VIBESERVER_BIN=vibeserver/build/vibeserver node --max-old-space-size=512 scripts/test-server-bg-split.mjs
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
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const LAND = 96100000, VDL2C = 136862500;

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
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-bgsplit-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'local', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: LAND, rate, demodMode: 'wfm',
               landingFreq: LAND, users: 1, ...radio }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort),
                                       '--rate', String(rate)], { stdio: 'ignore' });
  await sleep(800);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '19', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate),
                             '--idle-grace', '0', '--admin-pass', 'bg-split-test'], { env, stdio: ['ignore', log, log] });
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



const quiet = (st, type, fromT) => parsed(st, type).filter((m) => m.t >= fromT).length === 0;
const lastCfg = (st) => { const c = parsed(st, 'config'); return c[c.length - 1] || {}; };
const vfoOf = (c) => c.vfo ?? c.frequency ?? 0;

// ── 1. bg AIS + landing FM ──────────────────────────────────────────────────────────────────────────────────────────
console.log('\nBackground AIS, landing 96.1 MHz WFM (Listen-Amateur\'s case)');
{
  const S = await server({ bgMode: 'ais' }, 'aisfm');
  try {
    await waitFor(() => /\[AIS\] mode ON/.test(S.log()), 8000);
    ok(/\[MAP\] keep running: nobody is connected \(the server has started\) — entering AIS/.test(S.log()), '★ background AIS entered at the start, with nobody connected');
    ok(/\[LANDING\] new listeners land on the plain landing .*; background decoding: AIS whenever nobody is connected/.test(S.log()), 'the server says both choices');
    for (const round of [1, 2]) {
      const A = spectrum(S.port, crypto.randomUUID());
      await waitFor(() => /AIS \(the background mode\) pauses/.test(S.log()) && parsed(A, 'config').length > 0, 4000);
      await sleep(1500);
      ok((S.log().match(/AIS \(the background mode\) pauses/g) || []).length === round, `round ${round}: ★ the arrival paused background AIS`);
      const c = lastCfg(A);
      ok(Math.abs(vfoOf(c) - LAND) < 5000 && Math.abs(centreOf(c) - LAND) < 1.5e6, `round ${round}: ★ the listener is on the FM landing (vfo ${vfoOf(c)}, centre ${centreOf(c)})`);
      ok(parsed(A, 'ais').length === 0, `round ${round}: no AIS report reaches them`);
      ok(!/\[AIS\] last listener left/.test(S.log().split('pauses').slice(-1)[0]), `round ${round}: (AIS is not "left" under them a second time)`);
      const before = (S.log().match(/entering AIS/g) || []).length;
      A.close();
      await waitFor(() => (S.log().match(/entering AIS/g) || []).length > before, 9000);
      ok((S.log().match(/entering AIS/g) || []).length > before && /\(the last listener left\) — entering AIS/.test(S.log()), `round ${round}: ★ the last listener leaving brings background AIS back`);
    }
  } finally { S.stop(); }
}

// ── 2. bg AIS + landing on the map in ADS-B ────────────────────────────────────────────────────────────────────────
console.log('\nBackground AIS, landing on the Aviation & Maritime map in ADS-B');
{
  const S = await server({ bgMode: 'ais', landingMap: 'avmar', landingModes: 'adsb' }, 'aisadsb');
  try {
    await waitFor(() => /\[AIS\] mode ON/.test(S.log()), 8000);
    ok(/entering AIS/.test(S.log()) && !/\[ADS-B\] mode ON/.test(S.log()), 'background AIS at the start (ADS-B is the landing only)');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'adsb').length > 0, 6000);
    ok(/AIS \(the background mode\) pauses/.test(S.log()), '★ the arrival paused AIS');
    ok(parsed(A, 'adsb').length > 0 && /\[ADS-B\] new session — landing in ADS-B/.test(S.log()), '★ and landed them in ADS-B (the map)');
    const ships = await new Promise((res) => {
      const c = net.connect(S.port, '127.0.0.1', () => c.write('GET /vibeserver/gallery/ais-ships.json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'));
      let b = ''; c.on('data', (d) => { b += d; }); c.on('end', () => res(b)); c.on('error', () => res(''));
      setTimeout(() => { c.destroy(); res(b); }, 3000);
    });
    const body = ships.slice(ships.indexOf('\r\n\r\n') + 4);
    ok(/"vessels"/.test(body) && !/"off":1/.test(body), '★ the ship store stays served while ADS-B has the radio (the map shows stored ships beside live aircraft)');
    const before = (S.log().match(/entering AIS/g) || []).length;
    A.close();
    await waitFor(() => (S.log().match(/entering AIS/g) || []).length > before, 12000);
    ok(/\[ADS-B\] last listener left — the receiver goes back/.test(S.log()), 'the last listener leaving ends ADS-B (it is not the background)');
    ok((S.log().match(/entering AIS/g) || []).length > before, '★ and background AIS comes back');
  } finally { S.stop(); }
}

// ── 3. bg ADS-B + landing FM ───────────────────────────────────────────────────────────────────────────────────────
console.log('\nBackground ADS-B, landing FM');
{
  const S = await server({ bgMode: 'adsb' }, 'adsbfm');
  try {
    await waitFor(() => /\[ADS-B\] keep running: nobody is connected \(the server has started\)/.test(S.log()), 8000);
    ok(/\[ADS-B\] keep running: nobody is connected \(the server has started\)/.test(S.log()), '★ background ADS-B from the start (no ADS-B landing needed any more)');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => /ADS-B \(the background mode\) pauses/.test(S.log()), 4000);
    await sleep(1500);
    const t0 = parsed(A, 'config')[0]?.t ?? 0;
    ok(/ADS-B \(the background mode\) pauses/.test(S.log()), '★ the arrival paused ADS-B');
    ok(Math.abs(vfoOf(lastCfg(A)) - LAND) < 5000, `they are on the FM landing (vfo ${vfoOf(lastCfg(A))})`);
    ok(quiet(A, 'adsb', t0 + 500), 'no ADS-B report after their landing');
    A.close();
    await waitFor(() => /\[ADS-B\] keep running: nobody is connected \(the last listener left\)/.test(S.log()), 9000);
    ok(/\[ADS-B\] keep running: nobody is connected \(the last listener left\)/.test(S.log()), '★ ADS-B comes back when they leave');
  } finally { S.stop(); }
}

// ── 4. bg = the landing's own mode, in the new words ────────────────────────────────────────────────────────────────
console.log('\nBackground VDL2 + land on the map in VDL2 (joined, never paused)');
{
  const S = await server({ bgMode: 'vdl2', landingMap: 'avmar', landingModes: 'vdl2' }, 'same');
  try {
    await waitFor(() => /\[AVDATA\] mode ON: VDL2/.test(S.log()), 8000);
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'avdata').length > 0, 3000);
    ok(parsed(A, 'avdata').some((m) => m.band === 'vdl2') && !/the background mode\) pauses/.test(S.log()), '★ the listener joins the warm VDL2 map, nothing paused');
    A.close();
    await sleep(6000);
    ok(!/\[AVDATA\] mode OFF/.test(S.log()) && (S.log().match(/entering VDL2/g) || []).length === 1, 'and it stays when they leave (no leave, no re-entry)');
  } finally { S.stop(); }
}

// ── 5. THE RATE GATE ──────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nBackground ACARS + VDL2 on a 2.4 MS/s radio');
{
  const S = await server({ bgMode: 'acars,vdl2' }, 'gate');
  try {
    await sleep(5000);
    ok(/\[MAP\] keep running ACARS \+ VDL2 is set, but it cannot run on this receiver now \(ACARS \+ VDL2 needs a capture of 6\.1 MS\/s/.test(S.log()), '★ the server says why (the wide band needs 6.1 MS/s)');
    ok(!/\[AVDATA\] mode ON/.test(S.log()), 'the datalink is not entered');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'config').length > 0, 3000);
    ok(Math.abs(vfoOf(lastCfg(A)) - LAND) < 5000, 'a listener lands on FM as ever');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

// ── 6. SINGLE MODE OVERRIDES BOTH ─────────────────────────────────────────────────────────────────────────────────
console.log('\nSingle mode VDL2 over background AIS + landing FM');
{
  const S = await server({ singleMode: 'vdl2', bgMode: 'ais', blockedModes: 'dab' }, 'single');
  try {
    await waitFor(() => /\[AVDATA\] mode ON: VDL2/.test(S.log()), 8000);
    ok(/\[AVDATA\] mode ON: VDL2/.test(S.log()) && !/\[AIS\] mode ON/.test(S.log()), '★ the single mode runs (VDL2), the background AIS does not');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'avdata').length > 0, 3000);
    ok(parsed(A, 'avdata').some((m) => m.band === 'vdl2') && !/the background mode\) pauses/.test(S.log()), 'a listener joins VDL2 (it is the landing too)');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

// ── 7. MANUAL ─────────────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nManual background decoding (no bgMode, no keep words)');
{
  const S = await server({ bgMode: '' }, 'manual');
  try {
    await sleep(4000);
    ok(!/\[(AIS|ADS-B|AVDATA|ISM|VORMAP)\] mode ON/.test(S.log()) && !/keep running[: ]/.test(S.log().replace(/keep running either way/g, '')), 'no whole-radio mode enters with nobody here');
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
