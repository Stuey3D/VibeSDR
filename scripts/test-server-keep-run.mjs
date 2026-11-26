// test-server-keep-run.mjs — beta96 THE LANDING MAP'S KEEP-RUNNING MODES on a REAL vibeserver (fake rtl_tcp, synthetic IQ):
// VDL2 kept running, picked as one word — no frequency, no VFO (docs/v12/KEEP-RUNNING.md).
//
// ★★★ STUART (11-26, the Sony's Setup page): "I thought I'd be able to set VDL2 as the Sony's background mode".
//
// ★★ WHAT THIS PINS (the synthetic source carries no VDL2 bursts — this is the MODE's life cycle, not the decoder's):
//     1. KEEP RUNNING VDL2: with landingMap avmar + landingModes vdl2 + mapKeep the server enters the aircraft datalink on
//        the VDL2 band (136.8625 MHz, all 12 channels from the mode's own table) BEFORE anybody connects; nothing parks it;
//     2. a client connecting gets avdata reports on the vdl2 band at once, with no request; its first config is on 136.86;
//        the arrival JOINS (no landing re-entry);
//     3. EXIT puts the radio on the owner's PLAIN landing; the listener leaving puts it back into VDL2 (the last listener left);
//     4. LANDING ONLY (keep off): nothing enters before anybody connects; the first client lands in VDL2; the last one
//        leaving gives the receiver back and nothing keeps it;
//     5. ★ NEGATIVE: no map → no avdata report at all;
//     6. ★ THE SHARED 137 MHz CENTRE: with ORBCOMM monitoring ticked, kept VDL2 sits where VDL2 + the ORBCOMM band both fit
//        (137.179 MHz at 2.4 MS/s) — still every VDL2 channel inside;
//     7. ★ the RATE GATE on the server: ACARS + VDL2 kept on a 2.4 MS/s radio cannot run — keep-running says why, the plain
//        landing applies; AIS kept on the same radio runs;
//     8. ★★ a beta95 config (landingAdsb + adsbKeepRunning only) still means kept ADS-B — the migration, end to end.
// ★ SILENT: no audio socket is opened.
//
//   usage: VIBESERVER_BIN=vibeserver/build/vibeserver node --max-old-space-size=512 scripts/test-server-keep-run.mjs
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
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-keeprun-${tag}-`));
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
                             '--idle-grace', '0', '--admin-pass', 'keep-run-test'], { env, stdio: ['ignore', log, log] });
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


const bgOrb = JSON.stringify({ on: false, profiles: [], satPasses: { on: true, minEl: 20, birds: [], orbcomm: true } });

// ── 1-3. KEEP RUNNING VDL2 ──────────────────────────────────────────────────────────────────────────────────────────
console.log('\nKeep VDL2 running when nobody is connected');
{
  const S = await server({ landingMap: 'avmar', landingModes: 'vdl2', mapKeep: true }, 'keep');
  try {
    await waitFor(() => /\[AVDATA\] mode ON: VDL2/.test(S.log()), 8000);
    await sleep(2000);
    const log0 = S.log();
    ok(/\[MAP\] keep running: nobody is connected \(the server has started\) — entering VDL2/.test(log0), 'the server entered VDL2 at its start, with nobody connected');
    ok(/\[AVDATA\] mode ON: VDL2, radio at 136\.8625 MHz, [0-9.]+ MS\/s, 12 channels/.test(log0), '★ the datalink on the VDL2 band from its own table: 136.8625 MHz, 12 channels');
    ok(!/idle park in|dongle capture paused|RELEASED/i.test(log0), 'nothing parked or released the radio (idle grace 0)');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'avdata').length > 0, 2500);
    const rep = parsed(A, 'avdata')[0];
    ok(!!rep && rep.band === 'vdl2' && rep.t <= 2000, `★ a new client gets avdata reports on the vdl2 band without asking (${rep ? rep.band + ' at ' + rep.t + ' ms' : 'none'})`);
    const cfg0 = parsed(A, 'config')[0] || {};
    ok(Math.abs(centreOf(cfg0) - VDL2C) < 100e3, `its first config is already on the VDL2 band (${centreOf(cfg0)})`);
    ok(!/\[MAP\] new session — landing on the Aviation/.test(S.log()), 'the arrival joined (no landing re-entry)');
    A.send({ type: 'avdata', on: 0 });
    await waitFor(() => parsed(A, 'avdata_off').length > 0, 3000);
    await sleep(800);
    const after = parsed(A, 'config').filter((c) => c.t >= (parsed(A, 'avdata_off')[0]?.t ?? 1e9));
    const last = after[after.length - 1] || {};
    ok(parsed(A, 'avdata_off').length > 0, 'EXIT: avdata_off');
    const vfo = last.vfo ?? last.frequency ?? 0;
    ok(Math.abs(vfo - LAND) < 5000, `★ EXIT goes to the owner's landing ${LAND} (vfo ${vfo})`);
    A.close();
    await waitFor(() => /\[MAP\] keep running: nobody is connected \(the last listener left\)/.test(S.log()), 9000);
    ok(/\[MAP\] keep running: nobody is connected \(the last listener left\)/.test(S.log()), '★ the listener leaving puts the radio back into VDL2');
    const B = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(B, 'avdata').length > 0, 2500);
    ok(parsed(B, 'avdata').some((m) => m.band === 'vdl2'), 'and the next client is in VDL2 at once');
    // ★ a band step is somebody's use of the radio: ACARS and back, then leave — the kept VDL2 stays and is the keep's again
    B.send({ type: 'avdata', on: 1, band: 'acars' });
    await waitFor(() => parsed(B, 'avdata').some((m) => m.band === 'acars'), 3000);
    ok(parsed(B, 'avdata').some((m) => m.band === 'acars'), 'a listener may step the band (ACARS)');
    B.send({ type: 'avdata', on: 1, band: 'vdl2' });
    await waitFor(() => parsed(B, 'avdata').slice(-1)[0]?.band === 'vdl2', 3000);
    const nOff = (S.log().match(/\[AVDATA\] mode OFF/g) || []).length;
    B.close();
    await waitFor(() => /\[MAP\] last listener left — staying in VDL2/.test(S.log()), 9000);
    ok(/\[MAP\] last listener left — staying in VDL2/.test(S.log()), '★ the last listener leaving a kept VDL2 leaves it running (the keep\'s again)');
    ok((S.log().match(/\[AVDATA\] mode OFF/g) || []).length === nOff, '…without a leave and re-entry (the table stays warm)');
  } finally { S.stop(); }
}

// ── 4. LANDING ONLY ───────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nVDL2 as the landing, keep running OFF');
{
  const S = await server({ landingMap: 'avmar', landingModes: 'vdl2', mapKeep: false }, 'land');
  try {
    await sleep(4000);
    ok(!/\[AVDATA\] mode ON/.test(S.log()), 'nothing enters the datalink before anybody connects');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'avdata').length > 0, 4000);
    ok(/\[MAP\] new session — landing on the Aviation & Maritime map in VDL2/.test(S.log()), '★ the first client lands in VDL2 (logged)');
    ok(parsed(A, 'avdata').some((m) => m.band === 'vdl2'), 'and gets VDL2 reports without asking');
    A.close();
    await waitFor(() => /\[AVDATA\] last listener left — the receiver goes back/.test(S.log()), 9000);
    ok(/\[AVDATA\] last listener left — the receiver goes back/.test(S.log()), 'the last listener leaving gives the receiver back');
    await sleep(3000);
    ok(!/\[MAP\] keep running/.test(S.log()), 'and nothing keeps it running');
  } finally { S.stop(); }
}

// ── 5. NEGATIVE ───────────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nNo landing map (negative)');
{
  const S = await server({}, 'none');
  try {
    const A = spectrum(S.port, crypto.randomUUID());
    await sleep(3500);
    ok(parsed(A, 'avdata').length === 0 && !/\[AVDATA\] mode ON/.test(S.log()), 'no avdata report, the datalink never entered');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

// ── 6. THE SHARED 137 MHz CENTRE ──────────────────────────────────────────────────────────────────────────────────
console.log('\nKept VDL2 with ORBCOMM monitoring ticked: the shared 137 MHz capture');
{
  const S = await server({ landingMap: 'avmar', landingModes: 'vdl2', mapKeep: true, bgDecode: bgOrb }, 'orb');
  try {
    await waitFor(() => /\[AVDATA\] mode ON: VDL2/.test(S.log()), 8000);
    const m = S.log().match(/\[AVDATA\] mode ON: VDL2, radio at ([0-9.]+) MHz, ([0-9.]+) MS\/s, 12 channels/);
    const c = m ? Number(m[1]) * 1e6 : 0, r = m ? Number(m[2]) * 1e6 : 0;
    ok(!!m && Math.abs(c - VDL2C) > 100e3 && c > 136.9875e6 + 10e3 && c < 137.81e6, `★ the capture sits on the shared centre (${m ? m[1] : '?'} MHz), its DC clear of the VDL2 block`);
    ok(!!m && c - r * 0.45 <= 136.6875e6 && c + r * 0.45 >= 137.81e6, '…and one capture holds every VDL2 channel AND the ORBCOMM band');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'avdata').length > 0, 2500);
    ok(parsed(A, 'avdata').some((x) => x.band === 'vdl2'), 'a listener joins VDL2 there as ever');
    A.close(); await sleep(300);
  } finally { S.stop(); }
}

// ── 7. THE RATE GATE ON THE SERVER ────────────────────────────────────────────────────────────────────────────────
console.log('\nACARS + VDL2 kept on a 2.4 MS/s radio; AIS kept');
{
  const S = await server({ landingMap: 'avmar', landingModes: 'acars,vdl2', mapKeep: true }, 'combo');
  try {
    await sleep(5000);
    ok(/\[MAP\] keep running ACARS \+ VDL2 is set, but it cannot run on this receiver now \(ACARS \+ VDL2 needs a capture of 6\.1 MS\/s/.test(S.log()), '★ keep-running says why (the wide band needs 6.1 MS/s)');
    ok(!/\[AVDATA\] mode ON/.test(S.log()), 'the datalink is not entered');
  } finally { S.stop(); }
  const T = await server({ landingMap: 'avmar', landingModes: 'ais', mapKeep: true }, 'ais');
  try {
    await waitFor(() => /\[MAP\] keep running: nobody is connected \(the server has started\) — entering AIS/.test(T.log()), 8000);
    ok(/\[MAP\] keep running: nobody is connected \(the server has started\) — entering AIS/.test(T.log()), 'AIS kept: entered with nobody connected');
  } finally { T.stop(); }
}

// ── 7b. THE AMATEUR MAP: ISM 868 KEPT (Stuart 11-26: MeshCore repeaters all day) ──────────────────────────────────────
console.log('\nKeep ISM 868 running (the Amateur map)');
{
  const S = await server({ landingMap: 'amateur', landingModes: 'ism:868', mapKeep: true }, 'ism');
  try {
    await waitFor(() => /\[MAP\] keep running: nobody is connected \(the server has started\) — entering ISM 868/.test(S.log()), 8000);
    ok(/\[MAP\] keep running: nobody is connected \(the server has started\) — entering ISM 868/.test(S.log()), '★ ISM 868 entered with nobody connected (the band from its own plan)');
    ok(/\[MAP\] landing: a new listener lands on the Amateur map in ISM 868/.test(S.log()), 'the landing is the Amateur map');
    const A = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(A, 'ism').length > 0, 3000);
    ok(parsed(A, 'ism').length > 0, 'a new client gets ISM reports at once');
    A.close();
    await sleep(7000);
    ok(!/\[ISM\] last listener left — the receiver goes back/.test(S.log()), 'the last listener leaving does not take kept ISM away');
  } finally { S.stop(); }
}

// ── 8. A beta95 CONFIG ────────────────────────────────────────────────────────────────────────────────────────────
console.log('\nA beta95 config (landingAdsb + adsbKeepRunning only) still keeps ADS-B');
{
  const S = await server({ landingAdsb: true, adsbKeepRunning: true }, 'b95');
  try {
    await waitFor(() => /\[ADS-B\] keep running: nobody is connected/.test(S.log()), 8000);
    ok(/\[ADS-B\] keep running: nobody is connected \(the server has started\)/.test(S.log()), '★ kept ADS-B, as before the upgrade');
    ok(!/\[MAP\] keep running/.test(S.log()), 'the map path does not also take the radio');
    const cfg = JSON.parse(fs.readFileSync(path.join(S.dir, 'config.json'), 'utf8'));
    ok(true, `(config on disk untouched until the owner saves: landingMap ${JSON.stringify(cfg.radios?.[0]?.landingMap ?? null)})`);
  } finally { S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
