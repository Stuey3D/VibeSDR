// test-server-synop.mjs — THE REAL SERVER TRANSLATES A SYNOP IT DECODES FROM THE AIR (V12, 2026-10-23).
//
// ★★★ End to end, the way a listener meets it: the fake dongle transmits DWD-shaped RTTY (50 Bd, 450 Hz, ITA2 with FIGS —
//     fake-rtl-tcp.mjs --rtty) carrying a SYNOP bulletin; a listener tunes USB 1 kHz below it, attaches RTTY on the decoder
//     socket exactly as the web client does, and must receive:
//       · the raw bulletin as 0x01 text, untouched;
//       · a 0x08 weather_report frame with the plain-English line (Tempelhof, the wind, the temperature, the pressure);
//       · that frame AFTER the text that ended its report;
//     and the gallery (a listener's decoder is kept too) must hold the RTTY run with the translation under the report.
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=…; without one: "not run" (exit 3), never a pass. SILENT: no audio is played.
// usage: VIBESERVER_BIN=… node scripts/test-server-synop.mjs   (~40 s)
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

/** A minimal WebSocket client (test-server-decoders.mjs's), recording every frame IN ORDER. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { frames: [], txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.frames.push(Buffer.from(p));
      if (op === 0x1) st.txt.push(p.toString());
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
  /** The frames as one trace: 0x01 text verbatim, a 0x08 report as ⟦json⟧. */
  st.trace = () => st.frames.map((f) => f[0] === 0x01 && f.length >= 13 ? f.subarray(13, 13 + f.readUInt32BE(9)).toString()
    : f[0] === 0x08 && f.length >= 5 ? '⟦' + f.subarray(5, 5 + f.readUInt32BE(1)).toString() + '⟧' : '').join('');
  st.reports = () => st.frames.filter((f) => f[0] === 0x08 && f.length >= 5).map((f) => JSON.parse(f.subarray(5, 5 + f.readUInt32BE(1)).toString()));
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-synop-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'SYNOP Test', locator: 'IO92nh', adminPass: 'synop-admin', galleryMb: 20, decoderMax: 2,
  radios: [{ serial: 'synop-test', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }],
}));
const RF = 145001000, DIAL = RF - 1000;
const BULLETIN = 'RYRYRYRYRYRY\\r\\nZCZC 123\\r\\nAAXX 23064\\r\\n10384 41/96 82508 10118 20072 30115 40206=\\r\\n10147 31/97 72315 10092 40213=\\r\\nNNNN\\r\\n';
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--tones', '0',
                                     '--rtty', String(RF / 1e6), '--rtty-text', BULLETIN], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', String(DIAL), '--mode', 'usb', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
const until = async (f, ms) => { for (let t = 0; t < ms; t += 200) { if (f()) return true; await sleep(200); } return f(); };
const H = `http://127.0.0.1:${port}`;

try {
  console.log('── a listener runs RTTY on a SYNOP broadcast ──');
  const sid = 'synop001';
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  const aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  const dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  spec.send({ type: 'tune', frequency: DIAL, mode: 'usb' });
  await sleep(500);
  // ★ What the web client sends for RTTY AUTO (decParams: the preset's values plus "auto").
  dx.send({ type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1000, shift: 450, baud_rate: 50,
            encoding: 'ITA2', inverted: false, framing: '5N1.5', auto: true });
  const got = await until(() => dx.reports().some((r) => r.line.includes('Tempelhof')), 60000);
  const tr = dx.trace();
  ok(tr.includes('AAXX 23064'), `the raw bulletin arrives as text (${JSON.stringify(tr.slice(0, 120))}…)`);
  ok(got, `a 0x08 weather_report frame arrives (${dx.reports().length} so far)`);
  const r = dx.reports().find((x) => x.line.includes('Tempelhof'));
  if (r) console.log(`      ${r.line}`);
  ok(!!r && r.type === 'weather_report' && r.kind === 'synop' && r.raw.startsWith('10384 41/96 82508'), '...a SYNOP, with its raw groups');
  ok(!!r && r.line === '10384 · Tempelhof, Germany · 06:00 UTC · wind 250° 8 kt (15 km/h) · 11.8 °C · 1020.6 hPa',
     '...and the line says exactly what the groups say');
  ok(!!r && r.details.includes('Visibility 4 km') && r.details.includes('Dew point 7.2 °C'), '...details: visibility 4 km, dew point 7.2 °C');
  const at = tr.indexOf('⟦'), endOfReport = tr.indexOf('40206;');
  ok(endOfReport >= 0 && at > endOfReport && !tr.slice(endOfReport, at).includes('10147'), 'the frame comes after its report\'s text, before the next report');
  ok(tr.includes('10384 41/96 82508 10118 20072 30115 40206;'), 'the raw text is untouched (\';\' is DWD\'s \'=\' in the figures table)');
  // The gallery: a listener's decoder is kept; the run is written when the decoder stops.
  dx.send({ type: 'audio_extension_detach' });
  await sleep(1500);
  const g = await fetch(`${H}/vibeserver/gallery.json?kind=rtty`).then((x) => x.json()).catch(() => ({}));
  const item = Array.isArray(g.items) ? g.items[0] : null;
  ok(!!item, `the gallery kept the RTTY run (${item ? item.f : 'none'})`);
  if (item) {
    const body = await fetch(`${H}/vibeserver/gallery/file/${item.f}`).then((x) => x.text());
    const raw = body.indexOf('40206;'), line = body.indexOf('    » 10384 · Tempelhof, Germany');
    ok(raw >= 0 && line > raw, '★ the gallery .txt has the translation INLINE, under the raw report');
    ok(body.includes('        Visibility 4 km'), '...with its details indented');
  }
  for (const s of [spec, aud, dx]) s.close();
} finally {
  stop();
}
console.log(`   ${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
