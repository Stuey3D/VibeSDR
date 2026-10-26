// test-server-dsc.mjs — V12 DSC through the REAL server: a fake rtl_tcp replaying a synthetic clip (`dsc_bench clip`: the
// distress alert of tools/dsc-bench/dsc_synth.h, then an all-ships safety call), and the contract of vibe_dsc_run.h:
//   • vibeserver.json says "dsc": true;
//   • MF/HF: a listener on 2185.8 kHz USB attaches "dsc" — each call arrives as a dsc_call frame (its line, RF 2187.5 kHz,
//     the distress position) and as a 0x01 text line; the gallery then holds the day's log and the distress item, pinned;
//   • VHF channel 70: the same calls as an FM subcarrier on 156.525 MHz, a listener in NFM;
//   • BACKGROUND DECODING: a profile with one DSC VFO (2185.8 kHz USB) and nobody listening — the calls reach the gallery
//     on their own, labelled background.
// Silent: no audio is played (an audio socket is opened only so the listener has audio; its frames are dropped unread).
//   VIBESERVER_BIN=…/vibeserver VIBE_DSC_CLIP=…/hf.cu8 VIBE_DSC_CLIP_VHF=…/vhf.cu8 node scripts/test-server-dsc.mjs
//   (make the clips: scripts/test-dsc.sh clip hf.cu8 ; scripts/test-dsc.sh clip vhf.cu8 vhf)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DSC_CLIP || '', CLIP_VHF = process.env.VIBE_DSC_CLIP_VHF || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !CLIP_VHF || !fs.existsSync(CLIP_VHF)) {
  console.log('   not run — set VIBESERVER_BIN, VIBE_DSC_CLIP and VIBE_DSC_CLIP_VHF (scripts/test-dsc.sh clip …)');
  process.exit(3);
}
const RATE = 960000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false };
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
      if (op === 0x2 && !st.dropBin) st.bin.push(Buffer.from(p));
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
  return st;
}
const calls = (st) => st.txt.filter((t) => t.includes('"type":"dsc_call"')).map((t) => JSON.parse(t).data);
const text = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');

async function server(args, tag, clip, centre, extra = {}, envExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dsc-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DSC test', locator: 'IO92nh', adminPass: 'dsc-test-admin', galleryMb: 20,
    radios: [{ serial: 'dsc-test', label: 'Test radio', enabled: true, configured: true, rate: RATE, ...extra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...envExtra };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const gallery = async (kind) => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json?kind=${kind}&limit=50`)).json(); } catch { return null; } };
  const cleanup = () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); setTimeout(() => { try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} }, 1500); };
  return { port, dir, status, gallery, stop: cleanup, log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8') };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  return L;
}
const isAlert = (c) => /^DISTRESS · MMSI 235012345 \(United Kingdom\) · Sinking · 50°12'N 004°10'W · 14:32 UTC/.test(c.line);
const isSafety = (c) => /^SAFETY · all ships · from coast station 002570000 \(Norway\)/.test(c.line);

// ── 1. MF/HF: a listener on 2185.8 kHz USB ─────────────────────────────────────────────────────────────────────────────
{
  console.log('── MF/HF: a listener on 2185.8 kHz USB ──');
  const S = await server(['--freq', '2185800', '--mode', 'usb', '--users', '1'], 'hf', CLIP, 2187500);
  try {
    const st = await S.status();
    ok(st && st.dsc === true, `vibeserver.json: dsc ${JSON.stringify(st && st.dsc)}`);
    const L = await listener(S.port, 'dschf001', 2185800, 'usb');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'dsc', band: 'hf', center_frequency: 1700 });
    ok(await until(() => calls(L.dx).some(isAlert) && calls(L.dx).some(isSafety), 45000),
       `the distress alert and the safety call arrive within 45 s (${calls(L.dx).map((c) => c.line.slice(0, 40)).join(' | ')})`);
    const a = calls(L.dx).find(isAlert);
    ok(a && a.rf === 2187500 && a.level === 3 && a.check === 'ok', `the alert: RF ${a && a.rf}, level ${a && a.level}, check ${a && a.check}`);
    ok(a && Math.abs(a.lat - 50.2) < 0.01 && Math.abs(a.lon + 4.1667) < 0.01, `…its position for the map (${a && a.lat}, ${a && a.lon})`);
    ok(/2187\.5 kHz  DISTRESS · MMSI 235012345/.test(text(L.dx)), 'the 0x01 text line names the frequency and the call');
    const wrong = calls(L.dx).filter((c) => !isAlert(c) && !isSafety(c));
    ok(wrong.length === 0, `no other call decoded (${wrong.map((c) => c.line).join(' | ')})`);
    const states = L.dx.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
    ok(states.includes(1) && states.includes(0), `state 1 (a call coming in) and 0 (listening) were sent (${[...new Set(states)].join(',')})`);
    // the gallery: the listener's decodes are kept
    let g = null;
    const alertItem = (x) => x.lv === 3 && /^DISTRESS /.test(x.title);
    ok(await until(async () => { g = await S.gallery('dsc'); return g && g.items && g.items.some(alertItem); }, 15000), 'the gallery holds the distress item');
    const di = g && g.items.find(alertItem);
    ok(di && Math.abs(di.plat - 50.2) < 0.01 && Math.abs(di.plon + 4.1667) < 0.01 && di.o === 'live', `…pinned at its position, labelled live (${di && JSON.stringify([di.plat, di.plon, di.o])})`);
    const day = g && g.items.find((x) => /^DSC 2187\.5 kHz · /.test(x.title));
    ok(!!day && /distress/.test(day.title) && /^DSC 2187\.5 /.test(day.dn || ''), `…and the day's log ("${day && day.title}", downloads as "${day && day.dn}")`);
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(300);
    for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

// ── 2. VHF channel 70: a listener on 156.525 MHz NFM ───────────────────────────────────────────────────────────────────
{
  console.log('── VHF channel 70: a listener on 156.525 MHz NFM ──');
  const S = await server(['--freq', '156525000', '--mode', 'nfm', '--users', '1'], 'vhf', CLIP_VHF, 156525000);
  try {
    const L = await listener(S.port, 'dscvh001', 156525000, 'nfm');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'dsc', band: 'vhf' });
    ok(await until(() => calls(L.dx).some(isAlert) && calls(L.dx).some(isSafety), 45000),
       `the distress alert and the safety call arrive within 45 s (${calls(L.dx).map((c) => c.line.slice(0, 40)).join(' | ')})`);
    const a = calls(L.dx).find(isAlert);
    ok(a && a.rf === 156525000 && a.band === 'vhf', `the alert's RF is channel 70 (${a && a.rf}, ${a && a.band})`);
    ok(/156\.525 MHz  DISTRESS/.test(text(L.dx)), 'the text line says 156.525 MHz');
    for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

// ── 3. Background decoding: one DSC VFO, nobody listening ─────────────────────────────────────────────────────────────
{
  console.log('── background decoding: a DSC VFO, nobody listening ──');
  const BG = { on: true, waitMin: 5, profiles: [{ centre: 2187500, from: '00:00', to: '00:00', vfos: [{ hz: 2185800, mode: 'usb', dec: 'dsc' }] }] };
  const S = await server(['--freq', '7074000', '--mode', 'usb', '--users', '1'], 'bg', CLIP, 2187500,
                         { bgDecode: JSON.stringify(BG) }, { VIBESERVER_BG_WAIT_SEC: '1' });
  try {
    let g = null;
    const alertItem = (x) => x.lv === 3 && /^DISTRESS /.test(x.title) && x.o === 'background';
    ok(await until(async () => { g = await S.gallery('dsc'); return g && g.items && g.items.some(alertItem); }, 60000),
       `the distress call reaches the gallery by itself, labelled background (${g && g.items ? g.items.map((x) => x.title.slice(0, 30)).join(' | ') : 'none'})`);
    const it = g && g.items.find(alertItem);
    ok(it && Math.abs(it.plat - 50.2) < 0.01, `…pinned at its position (${it && it.plat})`);
    ok(g.items.some((x) => /^DSC 2187\.5 kHz \u00b7 /.test(x.title) && x.o === 'background'), 'the day\'s log is named for 2187.5 kHz, not the dial');
    ok(it && it.d === 'dsc' && it.p === 'Profile 1' && Math.abs(it.hz - 2185800) < 1, `…from the profile's VFO (${it && JSON.stringify([it.d, it.p, it.hz])})`);
    ok(/background decoding: .*VFO 1/.test(S.log()) || /background/.test(S.log()), 'the log says background decoding ran');
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-dsc: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
