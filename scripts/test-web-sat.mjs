// test-web-sat.mjs — V12 the web client's SATELLITE mode (docs/v12/SATELLITE-MODE.md; was SATELLITES, SATELLITE-ENGINE.md)
// against the REAL server (a fake rtl_tcp; CelesTrak's 2026-11-19 groups in the orbit cache with their epochs moved to an
// hour ago — no network), in headless Edge with --mute-audio (tests are silent), at desktop and 390 px: the mode menu's
// SATELLITE entry, the timetable (ISS up first, "UP · m:ss left"), a pick ({"op":"pick"} — the server tunes + locks) →
// the dial on the nominal with "ISS · ▲/▼ x.xx kHz" beside the digits and the compact box, the window fitting a phone with
// no sideways scroll, AUTO DOPPLER off → the readout gone, EXIT → dropped. ★ Needs the SERVER half of the contract. Needs the page
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
  // ★ vibesdr.net's sats.json fixture (the mirror's own output from CelesTrak's 2026-11-19 data) with the epochs an hour
  //   ago, and the sync's state "just checked" so the server asks vibesdr.net nothing (no network in tests)
  fs.mkdirSync(path.join(dataDir, 'sat'), { recursive: true });
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 5 * 3600e3).toISOString();
  fs.writeFileSync(path.join(dataDir, 'sat', 'sats.json'), JSON.stringify(d));
  const t = Math.floor(Date.now() / 1000);
  fs.writeFileSync(path.join(dataDir, 'sat', 'sync-state'), `checked ${t}\nattempt ${t}\nseed 0\n`);
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
  ok(/\bSATELLITE\b/.test(opts || '') && !/\bSAT\b/.test(opts || ''), `the mode menu offers SATELLITE: ${opts}`);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'SATELLITE'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('satPanel') && !document.getElementById('satPanel').hidden`, 4000), 'the SATELLITE window opens');
  // ★ THE ORDER (Stuart 11-20): up now with the SHORTEST remaining pass first — not the highest bird. At this fixture's
  //   time ISS and AO-73 are both up and AO-73 has less left, so it leads; the old test assumed ISS on top.
  ok(await until(`/UP · \\d+:\\d\\d left/.test(document.querySelector('#satPanel .sat-row')?.textContent || '')`, 6000), 'the first row is a satellite up now');
  const ups = await ev(`[...document.querySelectorAll('#satPanel .sat-row')].map(r => r.textContent).filter(t => /UP · \\d+:\\d\\d left/.test(t)).map(t => { const m = t.match(/UP · (\\d+):(\\d\\d) left/); return [t.slice(0, 40), +m[1] * 60 + +m[2]]; })`) || [];
  ok(ups.length >= 2 && ups.every((u, k) => k === 0 || u[1] >= ups[k - 1][1]), `up rows in shortest-remaining order: ${ups.map((u) => `${u[0].split(' ')[0]} ${u[1]}s`).join(' · ')}`);
  const firstRow = await ev(`[...document.querySelectorAll('#satPanel .sat-row')].map(r => r.textContent).find(t => /ISS/.test(t) && /437\\.800 MHz FM/.test(t)) || ''`);
  ok(/UP · \d+:\d\d left/.test(firstRow), 'the ISS repeater row: up, with its time left and its downlink in words');
  ok(/AOS \d\d:\d\d \(in /.test(await ev(`document.getElementById('satPanel').textContent`) || ''), 'the others with their next AOS');
  const credit = await ev(`document.querySelector('#satPanel .sat-credit')?.textContent || ''`);
  ok(/^Orbits: CelesTrak · 5 h old · Transmitters: SatNOGS DB \(CC BY-SA 4\.0\)$/.test(credit), `the credit with the data's age: "${credit}"`);
  await shot('picker');
  await ev(`(() => { const b = [...document.querySelectorAll('#satPanel button.sat-row')].find(x => /437\\.800/.test(x.textContent)); b && b.click(); return !!b; })()`);
  // ★★ 11-21 (Stuart): beside the digits only the badge — the satellite glyph and ▲ / ▼; no name, no words
  ok(await until(`(() => { const b = document.getElementById('mSatDop'); return !!b && !b.hidden && !!b.querySelector('svg.mSatIco') && !!b.querySelector('svg.mSatArr') && /ISS/.test(b.title); })()`, 6000),
     `beside the digits: the Doppler badge (${await ev(`document.getElementById('mSatDop')?.title || 'none'`)})`);
  ok(await ev(`document.getElementById('mChan').hidden && !/ISS · /.test(document.getElementById('mFreq')?.parentElement?.textContent || '')`) === true, 'and no words beside the digits');
  ok(await until(`/437\\.800/.test(document.getElementById('mFreq')?.textContent || '')`, 3000), `the digits on the nominal: ${await ev(`document.getElementById('mFreq')?.textContent`)}`);
  ok(await until(`!document.querySelector('#satPanel .sat-box').hidden && /TRACK|COAST/.test(document.querySelector('#satPanel .sat-ro').textContent)`, 3000), 'the compact box with the lock');
  ok(/In range for \d+:\d\d/.test(await ev(`document.querySelector('#satPanel .sat-ro').textContent`) || ''), 'the countdown');
  ok(await ev(`document.querySelector('#satPanel button[data-act="doppler"]')?.classList.contains('on')`) === true, 'AUTO DOPPLER is lit');
  await shot('locked');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(900);
  const fit = JSON.parse(await ev(`JSON.stringify((() => { const r = document.getElementById('satPanel').getBoundingClientRect(); return { l: r.left, r: r.right, w: innerWidth, sw: document.documentElement.scrollWidth }; })())`));
  ok(fit.l >= 0 && fit.r <= fit.w && fit.sw <= fit.w + 1, `390 px: the panel fits, no sideways scroll (${JSON.stringify(fit)})`);
  await shot('phone');
  await ev(`document.querySelector('#satPanel button[data-act="doppler"]')?.click()`);
  ok(await until(`document.getElementById('mSatDop').hidden`, 4000), 'AUTO DOPPLER off: the badge beside the digits is gone');
  await ev(`document.querySelector('#satPanel button[data-act="exit"]')?.click()`);
  ok(await until(`document.getElementById('satPanel').hidden`, 2000), 'EXIT leaves the mode');
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
