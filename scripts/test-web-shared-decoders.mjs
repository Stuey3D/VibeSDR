// test-web-shared-decoders.mjs — ★★★ SHARED DECODERS in the WEB PAGE against the REAL server (Stuart 2026-11-24), headless Edge
// with --mute-audio (tests are silent). A shared dial (users 4) on a fake rtl_tcp playing keyed APRS on 144.800:
//   · listener 1 (raw sockets, as test-server-shared-decoders.mjs) opens APRS;
//   · the page ARRIVES: its APRS box opens by itself, stations fill in (it JOINED the run — the server's key unchanged);
//   · the page closes the box: listener 1 still decodes; a later broadcast (a third listener joining) does NOT reopen it;
//   · listener 1 opens NAVTEX (a different decoder): the page follows it.
//   VIBESERVER_BIN=…/vibeserver VIBE_APRS_ONAIR_BIN=…/test-aprs-onair node scripts/test-web-shared-decoders.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', GEN = process.env.VIBE_APRS_ONAIR_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !GEN || !fs.existsSync(GEN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_APRS_ONAIR_BIN and Edge needed'); process.exit(3); }
const RATE = 1200000, DIAL = 144800000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const waitFor = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
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
      if (op === 0x1 && st.txt.length < 20000) st.txt.push(p.toString());
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const packets = (dx) => jsons(dx, 'aprs_packet').filter((p) => /on-air test #\d+/.test(p.data?.raw || '')).length;
const roomDec = (sp) => { const l = jsons(sp, 'dec_shared'); return l.length ? (l[l.length - 1].list.find((e) => e.slot === 'decoder') || null) : null; };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webshdec-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
const clip = path.join(dir, 'clean.cu8');
{ const r = spawnSync(GEN, ['--write-clip', clip, String(RATE), '15', '10', '0', '0', '3000', '0', '5'], { encoding: 'utf8' }); if (r.status !== 0) throw new Error(r.stderr); }
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Shared decoders web test', locator: 'IO92nh', adminPass: 'shdec-web-admin',
  radios: [{ serial: 'shdec-web', label: 'Test radio', enabled: true, configured: true, users: 4 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), '--hf', clip, '--hf-centre', String(DIAL)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), 'ignore'] });
await waitFor(async () => { try { return (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(2000) })).ok; } catch { return false; } }, 15000);
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC, L1, L3;
try {
  // ★ listener 1: raw sockets, opens APRS on the shared dial
  const sid = 'shdecweb01';
  L1 = { sp: ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`) };
  await sleep(300);
  L1.au = ws(port, `/ws/audio?user_session_id=${sid}&proto=1`);
  L1.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(600);
  L1.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
  L1.dx.send({ type: 'audio_extension_attach', extension_name: 'aprs', baud_rate: 1200, packet: 'auto' });
  ok(await waitFor(() => roomDec(L1.sp)?.ext === 'aprs', 4000), 'listener 1 opened APRS on the shared dial');
  const key1 = roomDec(L1.sp)?.key;

  // ★ the page arrives
  await sleep(2500);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  const boxIs = (t) => `document.getElementById('decBox')?.classList.contains('open') && document.getElementById('decTitle')?.textContent === '${t}'`;
  ok(await until(boxIs('APRS'), 10000), 'the page ARRIVES: its APRS box opens by itself');
  ok(await until(`document.querySelectorAll('#aprsRows .ar').length >= 2`, 40000), `stations fill in: ${await ev(`[...document.querySelectorAll('#aprsRows .ar .c')].map(x => x.textContent).join(' | ')`)}`);
  ok(await waitFor(() => roomDec(L1.sp)?.n === 2, 4000) && roomDec(L1.sp)?.key === key1, 'the page JOINED the run (two hold it, the same key — nothing restarted)');
  ok(await ev(`!!document.querySelector('#decodersPanel [data-dec="aprs"].on')`), 'the APRS key is lit');

  // ★ the page closes its box — the decoder keeps running for listener 1
  await ev(`document.querySelector('#decodersPanel [data-dec="aprs"]').click()`);
  ok(await until(`!document.getElementById('decBox')?.classList.contains('open')`, 4000), 'the page closes its box');
  ok(await waitFor(() => roomDec(L1.sp)?.n === 1, 4000), 'the server let go for the page only (one holds it)');
  const p1 = packets(L1.dx);
  ok(await waitFor(() => packets(L1.dx) >= p1 + 2, 30000), `listener 1 still decoding (+${packets(L1.dx) - p1})`);
  // ★ a broadcast (a third listener joins it) must not reopen the page's box
  const sid3 = 'shdecweb03';
  L3 = { sp: ws(port, `/ws/user-spectrum?user_session_id=${sid3}&mode=binary8&bins=256&proto=1`) };
  await sleep(600);
  L3.dx = ws(port, `/ws/dxcluster?user_session_id=${sid3}`);
  await sleep(400);
  const e3 = roomDec(L3.sp);
  L3.dx.send({ type: 'audio_extension_attach', extension_name: e3.ext, params: e3.params });
  ok(await waitFor(() => roomDec(L1.sp)?.n === 2, 4000), 'a third listener joined (the room re-told)');
  await sleep(3000);
  ok(await ev(`!document.getElementById('decBox')?.classList.contains('open')`), 'the page\'s box stays CLOSED (it closed that run)');

  // ★ a different decoder in the room: followed
  L1.dx.send({ type: 'audio_extension_attach', extension_name: 'navtex', center_frequency: 500, shift: 170, baud_rate: 100, inverted: false, framing: '4/7', encoding: 'CCIR476' });
  ok(await until(boxIs('NAVTEX'), 8000), `listener 1 opens NAVTEX: the page follows it (${await ev(`document.getElementById('decTitle')?.textContent`)})`);
  ok(await waitFor(() => roomDec(L1.sp)?.ext === 'navtex' && roomDec(L1.sp)?.n >= 2, 6000), `…and joined it (${roomDec(L1.sp)?.n} hold it)`);

  // ★★★ one set of settings: the page's RTTY panel shows the room's run, and follows a change made by somebody else
  const RT = (shift) => ({ type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1000, shift, baud_rate: 50, inverted: true, framing: '5N1', encoding: 'ITA2' });
  L1.dx.send(RT(425));
  ok(await until(boxIs('RTTY'), 8000), 'listener 1 opens RTTY (425 Hz, 50 Bd, inverted): the page follows it');
  const on = (id, attr, v) => ev(`!!document.querySelector('#${id} [data-${attr}="${v}"].on')`);
  ok(await until(`!!document.querySelector('#rttyShift [data-shift="425"].on') && !!document.querySelector('#rttyBaud [data-baud="50"].on')`, 4000)
     && await ev(`document.getElementById('rttyInv')?.classList.contains('on')`), 'the page\'s panel shows the ROOM\'s settings (425 Hz, 50 Bd, inverted)');
  ok(await waitFor(() => roomDec(L1.sp)?.ext === 'fsk' && roomDec(L1.sp)?.n >= 2, 6000), 'the page joined that run');
  const k0 = roomDec(L1.sp).key;
  L1.dx.send(RT(850));
  ok(await until(`!!document.querySelector('#rttyShift [data-shift="850"].on')`, 6000), 'listener 1 changes the shift to 850: the page\'s panel follows');
  ok(await waitFor(() => roomDec(L1.sp)?.key !== k0 && roomDec(L1.sp)?.n >= 2, 4000) && await ev(boxIs('RTTY')), 'the one run, still held by both, the box still open');
  void on;
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  for (const L of [L1, L3]) for (const k of ['sp', 'au', 'dx']) try { L?.[k]?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`\ntest-web-shared-decoders: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
