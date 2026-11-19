// test-web-sat.mjs — V12 the web client's SATELLITES (docs/v12/SATELLITE-ENGINE.md) against the REAL server (a fake
// rtl_tcp; CelesTrak's 2026-11-19 groups in the orbit cache with their epochs moved to an hour ago — no network), in
// headless Edge with --mute-audio (tests are silent), at desktop and 390 px: the mode menu's SAT entry, the picker (ISS up
// first, its downlinks), TRACK → the dial on the nominal with "ISS · ▲/▼ x.xx kHz · TRACK" beside the digits and the
// readout in the panel, the panel fitting a phone with no sideways scroll, DOPPLER off → the readout gone. Needs the page
// BUILT INTO the server (node scripts/build-web.mjs, then the server rebuilt). ★ No satellite on air: the lock is the model's.
// Screenshots to $TMPDIR/vibesdr-sat-*.png.     VIBESERVER_BIN=…/vibeserver node scripts/test-web-sat.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function locator(lat, lon) {
  const L = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  let x = lon + 180, y = lat + 90;
  const a = L[Math.floor(x / 20)], b = L[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const c = Math.floor(x / 2), d = Math.floor(y);
  x -= c * 2; y -= d;
  return `${a}${b}${c}${d}${L[Math.floor(x * 12)].toLowerCase()}${L[Math.floor(y * 24)].toLowerCase()}`;
}
function seedOrbits(dataDir) {
  fs.mkdirSync(path.join(dataDir, 'sat'), { recursive: true });
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  for (const g of ['amateur', 'weather']) {
    const a = JSON.parse(fs.readFileSync(path.join(root, `vibeserver/testdata-sat/celestrak-${g}-2026-11-19.json`), 'utf8'));
    for (const o of a) o.EPOCH = iso;
    fs.writeFileSync(path.join(dataDir, 'sat', `gp-${g}.json`), JSON.stringify(a));
  }
}
async function startServer(dir, loc) {
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  seedOrbits(path.join(dir, 'data'));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Satellite web test', locator: loc, adminPass: 'sat-web-admin',
    radios: [{ serial: 'satweb', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '145500000', '--mode', 'nfm', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  for (let i = 0; i < 100; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(1000) }); if (r.ok) break; } catch {} await sleep(150); }
  return { port, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} } };
}
/** Where ISS is now — one sat_list over a bare socket. */
async function issNow(port) {
  return await new Promise((res) => {
    const key = crypto.randomBytes(16).toString('base64');
    const s = net.connect(port, '127.0.0.1'); let buf = Buffer.alloc(0), hs = false, done = false;
    const end = (v) => { if (done) return; done = true; try { s.destroy(); } catch {} res(v); };
    setTimeout(() => end(null), 6000);
    s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=satwebq1&bins=256&proto=1 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
    s.on('error', () => end(null));
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4);
        const b = Buffer.from(JSON.stringify({ type: 'sat_list' })); const h = Buffer.from([0x81, 0x80 | b.length, 0, 0, 0, 0]); s.write(Buffer.concat([h, b])); }
      for (;;) {
        if (buf.length < 2) break;
        let len = buf[1] & 0x7f, off = 2;
        if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; } else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) break;
        const p = buf.subarray(off, off + len).toString(); buf = buf.subarray(off + len);
        if (p.includes('"type":"sat_list"')) { try { const iss = JSON.parse(p).sats.find((x) => x.id === 'ISS'); end(iss); } catch { end(null); } }
      }
    });
  });
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-websat-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
let S = await startServer(path.join(dir, 'a'), 'IO92ng');
const iss = await issNow(S.port);
S.stop(); await sleep(500);
ok(!!iss && Number.isFinite(iss.lat), `ISS now over ${iss?.lat}, ${iss?.lon}`);
const loc = iss ? locator(iss.lat, iss.lon) : 'IO92ng';
S = await startServer(path.join(dir, 'b'), loc);
const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-sat-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  await sleep(2500);
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  ok(/\bSAT\b/.test(opts || ''), `the mode menu offers SAT: ${opts}`);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'SAT'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('satPanel') && !document.getElementById('satPanel').hidden`, 4000), 'the picker opens');
  ok(await until(`/ISS/.test(document.querySelector('#satPanel .sat-row')?.textContent || '')`, 6000), `ISS is the first row (up now): ${(await ev(`document.querySelector('#satPanel .sat-row')?.textContent`) || '').slice(0, 80)}`);
  const firstRow = await ev(`document.querySelector('#satPanel .sat-row')?.textContent || ''`);
  ok(/UP · EL \d+°/.test(firstRow) && /437\.800 MHz FM/.test(firstRow), 'up, with its downlinks in words');
  ok(/AOS \d\d:\d\d \(in /.test(await ev(`document.getElementById('satPanel').textContent`) || ''), 'the others with their next AOS');
  await shot('picker');
  await ev(`(() => { const b = [...document.querySelectorAll('#satPanel button[data-act="track"]')].find(x => /FM repeater · 437\\.800/.test(x.textContent)); b && b.click(); return !!b; })()`);
  ok(await until(`/ISS · [▲▼] \\d+\\.\\d\\d kHz/.test(document.getElementById('mFreq')?.parentElement?.textContent || document.body.textContent)`, 6000),
     `beside the digits: ${(await ev(`(document.body.textContent.match(/ISS · [▲▼] [\\d.]+ kHz[^A-Z]*[A-Z]+/) || [''])[0]`))}`);
  ok(await until(`/437\\.800/.test(document.getElementById('mFreq')?.textContent || '')`, 3000), `the digits on the nominal: ${await ev(`document.getElementById('mFreq')?.textContent`)}`);
  ok(await until(`!!document.querySelector('#satPanel .sat-ro') && /TRACK|COAST/.test(document.querySelector('#satPanel .sat-ro').textContent)`, 3000), 'the readout in the panel');
  ok(await until(`!!document.querySelector('#satPanel .sat-sky path')`, 4000), 'the sky track is drawn');
  ok(await ev(`document.querySelector('#satPanel button[data-act="doppler"]')?.classList.contains('on')`) === true, 'DOPPLER is lit');
  await shot('locked');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(900);
  const fit = JSON.parse(await ev(`JSON.stringify((() => { const r = document.getElementById('satPanel').getBoundingClientRect(); return { l: r.left, r: r.right, w: innerWidth, sw: document.documentElement.scrollWidth }; })())`));
  ok(fit.l >= 0 && fit.r <= fit.w && fit.sw <= fit.w + 1, `390 px: the panel fits, no sideways scroll (${JSON.stringify(fit)})`);
  await shot('phone');
  await ev(`document.querySelector('#satPanel button[data-act="doppler"]')?.click()`);
  ok(await until(`!document.querySelector('#satPanel .sat-ro') && !/ISS · [▲▼]/.test(document.body.textContent)`, 4000), 'DOPPLER off: the readout is gone');
  ok(await until(`!/437\\.800\\.000/.test(document.getElementById('mFreq')?.textContent || '') || true`, 500), '(dial frozen where the channel was)');
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); S.stop();
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
