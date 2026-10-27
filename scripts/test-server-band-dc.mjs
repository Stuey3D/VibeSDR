// test-server-band-dc.mjs — V12 BAND SPOTS AND THE TUNER'S DC through the REAL server (2026-10-27, branch v12-band-dc).
//
// Stuart's Pi 2 (RTL-SDR Blog V4, 2.048 MS/s): 40 m in Band put the dial on 7.0606 and the tuner 15 kHz above it — the
// dongle's DC at 7.0756, in the middle of FT8. A fake rtl_tcp given the dongle's own offset tuning (VIBE_TCP_DC_OFFSET_HZ,
// test only) replays a UTC-anchored 40 m clip (`test-digi-spots clip40dc`: six FT8 CQs spread over 7.0744–7.0769) mixed to
// wherever the server tunes it, and adds a DONGLE'S DC there: a constant spike plus a dirty ±1.5 kHz skirt (--dc-skirt — the
// stress model; Stuart's V4 measures a clean one-bin spike, which costs FT8 little: see test-band-slice real dc=). Then:
//   1. BEFORE (VIBE_BAND_DC=0 — the placement exactly as it was): the DC the server tunes to is ON FT8 and Band decodes
//      almost nothing — the test catches the fault;
//   2. AFTER: Band moves the tuner so the DC is outside every slice (the server's own tune command, checked against the
//      rule by `test-band-dc where`), all six stations are spotted at their RF (±10 Hz) as FT8 on 40 m;
//   3. A SECOND LISTENER on the shared dial (USB 7.0606, a carrier at 7.0616 → a 1000 Hz tone) hears that tone within 1 Hz
//      of where it was, before Band, during it and after it stops; its readout (config vfo / centre) never moves;
//   4. Band off → the tuner goes back to the ordinary placement (the DC at dial + 15 kHz again).
// Silent: audio frames are analysed in memory, never played. Every child is killed in a finally.
//   VIBESERVER_BIN=…/vibeserver VIBE_DIGI_BIN=…/test-digi-spots VIBE_DC_BIN=…/test-band-dc node scripts/test-server-band-dc.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', DIGI = process.env.VIBE_DIGI_BIN || '', DCB = process.env.VIBE_DC_BIN || '';
if (![BIN, DIGI, DCB].every((b) => b && fs.existsSync(b))) {
  console.log('   not run — set VIBESERVER_BIN, VIBE_DIGI_BIN (test-digi-spots, writes the clip) and VIBE_DC_BIN (test-band-dc)');
  process.exit(3);
}
const RATE = 960000, DIAL = 7060600, OFF = 15000, CARRIER = DIAL + 1000;
const CALLS = [['G4AAA', 7074400], ['DL1AAB', 7074900], ['K1AAC', 7075400], ['F5AAD', 7075900], ['EA4AAE', 7076400], ['JA1AAF', 7076900]];
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-banddc-'));
const CLIP = path.join(tmp, '40m-dc.cu8');
execFileSync(DIGI, ['clip40dc', CLIP, String(RATE), String(DIAL), process.env.CLIP_AMP || '0.7'], { stdio: 'ignore' });   // ★ ≈ +4 dB in 2.5 kHz: a real band's middling station
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };
const where = (dc) => execFileSync(DCB, ['where', String(RATE), String(DIAL), String(dc)]).toString().trim();

/** A minimal WebSocket client: text frames kept (bounded), binary frames kept only while `keepBin`. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, keepBin: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
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
      if (op === 0x2 && st.keepBin) st.bin.push(Buffer.from(p));
      if (op === 0x1) { const t = p.toString(); if (t.includes('"type":"config"') || t.includes('digital_spot')) { st.txt.push(t); if (st.txt.length > 4000) st.txt.splice(0, 1000); } }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.destroy(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));

/** Audio frames → mono Float32 (the left channel). [0] channels, [1] codec (0 PCM, 3 Opus — a shared dial always sends
 *  Opus), [2..5] rate; Opus decoded with the web client's own decoder package. */
let opusDec = null;
async function pcmOf(bins) {
  const out = [];
  for (const b of bins) {
    if (b.length < 7) continue;
    if (b[1] === 0) { const ch = b[0] || 1, n = (b.length - 6) / 2; for (let i = 0; i < n; i += ch) out.push(b.readInt16LE(6 + 2 * i) / 32768); continue; }
    if (b[1] !== 3) continue;
    if (!opusDec) { const { OpusDecoder } = await import('opus-decoder'); opusDec = new OpusDecoder({ channels: 2 }); await opusDec.ready; }
    let r; try { r = opusDec.decodeFrame(new Uint8Array(b.subarray(6))); } catch { continue; }
    if (r && r.samplesDecoded) for (let i = 0; i < r.samplesDecoded; i++) out.push(r.channelData[0][i]);
  }
  return Float32Array.from(out);
}
/** The tone's frequency to a fraction of a hertz: the phase of a 1000 Hz Goertzel over successive 0.25 s windows; its slope is
 *  the offset from 1000 Hz. Returns NaN without a second of steady audio. */
function toneHz(x, fs = 48000, f0 = 1000) {
  const N = fs / 4, ph = [];
  for (let k = 0; k + N <= x.length; k += N) {
    let re = 0, im = 0;
    for (let i = 0; i < N; i++) { const a = 2 * Math.PI * f0 * (k + i) / fs; re += x[k + i] * Math.cos(a); im -= x[k + i] * Math.sin(a); }
    if (Math.hypot(re, im) / N < 1e-4) return NaN;   // no tone there
    ph.push(Math.atan2(im, re));
  }
  if (ph.length < 4) return NaN;
  let tot = 0;
  for (let i = 1; i < ph.length; i++) { let d = ph[i] - ph[i - 1]; while (d > Math.PI) d -= 2 * Math.PI; while (d < -Math.PI) d += 2 * Math.PI; tot += d; }
  return f0 + tot / (ph.length - 1) / (2 * Math.PI * (N / fs));
}
async function tone(L, ms = 3000) {
  L.aud.bin.length = 0; L.aud.keepBin = true;
  await sleep(ms);
  L.aud.keepBin = false;
  const x = await pcmOf(L.aud.bin);
  if (process.env.DEBUG) { const f = {}; for (const b of L.aud.bin) f[b[0] + '/' + b[1]] = (f[b[0] + '/' + b[1]] || 0) + 1;
    let e = 0; for (const v of x) e += v * v; console.log('     [debug] frames', JSON.stringify(f), 'samples', x.length, 'rms', Math.sqrt(e / Math.max(1, x.length)).toExponential(2)); }
  L.aud.bin.length = 0;
  return toneHz(x.subarray(Math.min(x.length, 24000)));   // ★ skip the first half second (a retune blip settles)
}

const children = [];
async function server(tag, extraEnv) {
  const dir = fs.mkdtempSync(path.join(tmp, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Band DC test', locator: 'IO92nh', adminPass: 'banddc-test-admin',
    radios: [{ serial: 'banddc-test', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const tunes = [];
  const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(DIAL), '--hf-utc', '--dc', '6', '--dc-skirt', '1500', '--dc-skirt-amp', '30',
                                       '--carrier', String(CARRIER), '--carrier-amp', '10'], { stdio: ['ignore', 'pipe', 'ignore'] });
  children.push(rtl);
  rtl.stdout.on('data', (d) => { for (const m of d.toString().matchAll(/tune ([0-9.]+) MHz/g)) { tunes.push(Math.round(Number(m[1]) * 1e6)); if (tunes.length > 200) tunes.shift(); } });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBE_TCP_DC_OFFSET_HZ: String(OFF), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'usb', '--users', '2'],
                    { env, stdio: ['ignore', log, log] });
  children.push(srv);
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  // ★ the fake prints the tune command (the tuner's frequency — where the DC is); with the fake's own 0.1 kHz rounding
  const dcNow = () => tunes.length ? tunes[tunes.length - 1] : NaN;
  return { port, dir, tunes, dcNow, log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8'),
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, spots) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(300);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  if (spots) L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: DIAL, mode: 'usb' });
  await sleep(800);
  L.close = () => { L.spec.close(); L.aud.close(); if (L.dx) L.dx.close(); };
  return L;
}
const spotted = (L) => new Set(jsons(L.dx, 'digital_spot').filter((x) => x.data.mode === 'FT8').map((x) => x.data.callsign));

try {
  {
    console.log('── BEFORE (VIBE_BAND_DC=0, the placement as it was): 40 m Band with the DC on FT8 ──');
    const S = await server('before', { VIBE_BAND_DC: '0' });
    try {
      const A = await listener(S.port, 'bdcbefore01', true);
      A.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      await sleep(2500);
      const dc = S.dcNow();
      ok(where(dc).startsWith('2 FT8'), `the tuner (the DC) at ${(dc / 1e3).toFixed(1)} kHz: ${where(dc)} — on FT8, as on the Pi 2`);
      await sleep(48000);   // ★ three FT8 slots
      const got = spotted(A);
      ok(got.size <= 1, `the fault reproduces: ${got.size} of 6 FT8 stations spotted with the DC on FT8 (${[...got].join(' ') || 'none'})`);
      A.close();
    } finally { S.stop(); }
  }
  {
    console.log('── AFTER: Band moves the tuner so the DC is outside the slices; a second listener on the shared dial ──');
    const S = await server('after', {});
    try {
      const B = await listener(S.port, 'bdclisten02', false);
      const dc0 = S.dcNow();
      ok(dc0 === DIAL + OFF, `no Band: the tuner at dial + 15 kHz (${(dc0 / 1e3).toFixed(1)} kHz), as ever`);
      const f0 = await tone(B);
      ok(Math.abs(f0 - 1000) < 1, `listener B (USB ${DIAL / 1e3} kHz, carrier ${CARRIER / 1e3}): ${f0.toFixed(3)} Hz before Band`);
      const cfgN = jsons(B.spec, 'config').length;
      const A = await listener(S.port, 'bdcband0001', true);
      A.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      ok(await until(() => S.dcNow() !== DIAL + OFF, 8000), `Band on: the tuner moved (${(S.dcNow() / 1e3).toFixed(1)} kHz)`);
      const dc = S.dcNow();
      ok(where(dc).startsWith('0'), `the DC at ${(dc / 1e3).toFixed(1)} kHz is outside every slice of 40 m (test-band-dc: ${where(dc)})`);
      ok(/Band spots: the tuner's DC moved off the band's slices/.test(S.log()), 'the server says why it moved the tuner');
      const f1 = await tone(B);
      ok(Math.abs(f1 - f0) < 1, `listener B through Band on: ${f1.toFixed(3)} Hz (${(f1 - f0).toFixed(3)} Hz from before)`);
      ok(await until(() => spotted(A).size === 6, 65000), `all six FT8 stations spotted with the DC moved (${[...spotted(A)].join(' ')})`);
      const sp = jsons(A.dx, 'digital_spot').filter((x) => x.data.mode === 'FT8');
      for (const [call, rf] of CALLS) {
        const s = sp.find((x) => x.data.callsign === call);
        ok(!!s && Math.abs(s.data.frequency - rf) <= 10 && s.data.band === '40m', `${call} at ${rf} (got ${s ? s.data.frequency + ' ' + s.data.band : 'nothing'})`);
      }
      const f2 = await tone(B);
      ok(Math.abs(f2 - f0) < 1, `listener B a minute into Band: ${f2.toFixed(3)} Hz`);
      ok(S.tunes.filter((t) => t !== DIAL + OFF).length <= 2, `one retune for Band, not a stream of them (${S.tunes.length} tunes in all: ${S.tunes.map((t) => (t / 1e3).toFixed(1)).join(', ')})`);
      ok(await until(() => /Band slice: dial 7\.0606 MHz, tuner centre [0-9.]+ MHz, DC [0-9.]+ MHz \(outside every slice\)/.test(S.log()), 20000),
         'the tap\'s minute line says where the DC is (outside every slice)');
      ok(await until(() => /Band decode, last [0-9]+ s of capture: main slice [0-9.]+ s fed/.test(S.log()), 75000),
         `the decode thread's minute line: ${(S.log().match(/Band decode, last[^\n]*/) || ['none'])[0].replace(/^.*?Band decode/, 'Band decode')}`);
      A.dx.send({ type: 'unsubscribe_digital_spots' });
      ok(await until(() => S.dcNow() === DIAL + OFF, 8000), `Band off: the tuner back at dial + 15 kHz (${(S.dcNow() / 1e3).toFixed(1)} kHz)`);
      const f3 = await tone(B);
      ok(Math.abs(f3 - f0) < 1, `listener B after Band: ${f3.toFixed(3)} Hz`);
      const cfgs = jsons(B.spec, 'config').slice(cfgN);
      ok(cfgs.every((c) => c.vfo === DIAL), `listener B's readout never moved (${cfgs.length} config frame(s), vfo ${[...new Set(cfgs.map((c) => c.vfo))].join(', ') || DIAL})`);
      A.close(); B.close();
    } finally { S.stop(); }
  }
} catch (e) {
  fail++; console.log(`   FAIL ${e && e.stack || e}`);
} finally {
  for (const c of children) { try { c.kill('SIGKILL'); } catch {} }
  if (opusDec) try { opusDec.free(); } catch {}
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
