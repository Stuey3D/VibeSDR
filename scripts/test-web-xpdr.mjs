// test-web-xpdr.mjs — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER on the REAL web page (docs/v12/TRANSPONDER-FINDER.md), headless
// Edge with --mute-audio (tests are silent), against the real server where RS-44 is up now: the fake rtl_tcp replays
// VIBE_XPDR_CLIP (vibeserver/test-xpdr --write-clip — RS-44's beacon, G4ABC's CQ, an SSB voice). SATELLITE → RS-44's
// transponder → the box lists the rows (VOICE with LISTEN, the BEACON and G4ABC without), the waterfall draws the slice
// titled "TRANSPONDER · every signal found here" with an arrow per signal (ORBCOMM's drawing), LISTEN snaps VFO A to the
// voice and the box says VFO A follows it; 390 px with no sideways scroll. Needs the page BUILT INTO the server
// (node scripts/build-web.mjs, then the server rebuilt). Screenshots to $TMPDIR/vibesdr-xpdr-*.png.
//   VIBESERVER_BIN=… VIBE_XPDR_CLIP=… node scripts/test-web-xpdr.mjs
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

const CLIP = process.env.VIBE_XPDR_CLIP || '';
if (!CLIP || !fs.existsSync(CLIP)) { console.log('   not run — VIBE_XPDR_CLIP needed (vibeserver/test-xpdr --write-clip c.u8 2400000 435640000 12)'); process.exit(3); }
async function startServerX(dir, loc) {
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  seedOrbits(path.join(dir, 'data'));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Transponder web test', locator: loc, adminPass: 'xpdr-web-admin',
    radios: [{ serial: 'xpdrweb', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '435640000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '435640000', '--mode', 'usb', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), fs.openSync(path.join(dir, 'server.log'), 'a')] });
  for (let i = 0; i < 100; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(1000) }); if (r.ok) break; } catch {} await sleep(150); }
  return { port, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } }, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} } };
}
/** Where a bird is now — one sat_list over a bare socket. */
async function birdNow(port, norad) {
  return await new Promise((res) => {
    const key = crypto.randomBytes(16).toString('base64');
    const s = net.connect(port, '127.0.0.1'); let buf = Buffer.alloc(0), hs = false, done = false;
    const end = (v) => { if (done) return; done = true; try { s.destroy(); } catch {} res(v); };
    setTimeout(() => end(null), 6000);
    s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=xpwebq01&bins=256&proto=1 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
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
        if (p.includes('"type":"sat_list"')) { try { end(JSON.parse(p).sats.find((x) => x.norad === norad)); } catch { end(null); } }
      }
    });
  });
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webxpdr-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
let S = await startServerX(path.join(dir, 'a'), 'IO92ng');
const rs = await birdNow(S.port, 44909);
S.stop(); await sleep(500);
ok(!!rs && Number.isFinite(rs.lat), `RS-44 now over ${rs?.lat}, ${rs?.lon}`);
const loc = rs ? locator(Math.max(-80, Math.min(80, rs.lat + (rs.lat > 0 ? -8 : 8))), ((rs.lon + 8 + 540) % 360) - 180) : 'IO92ng';
S = await startServerX(path.join(dir, 'b'), loc);
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-xpdr-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  // ★ every canvas fillText recorded (the waterfall's overlay words), bounded
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => { window.__ft = []; const f = CanvasRenderingContext2D.prototype.fillText;
    CanvasRenderingContext2D.prototype.fillText = function (t) { if (window.__ft.length > 4000) window.__ft.splice(0, 2000); window.__ft.push(String(t)); return f.apply(this, arguments); }; })();` });
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
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'SATELLITE'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('satPanel') && !document.getElementById('satPanel').hidden`, 4000), 'the SATELLITE window opens');
  ok(await until(`!![...document.querySelectorAll('#satPanel button.sat-row')].find(x => /RS-44/.test(x.textContent) && /435\\.6[0-9]+/.test(x.textContent) && /Transponder/i.test(x.textContent))`, 6000), 'RS-44\'s transponder is listed');
  await ev(`(() => { const b = [...document.querySelectorAll('#satPanel button.sat-row')].find(x => /RS-44/.test(x.textContent) && /Transponder/i.test(x.textContent)); b && b.click(); return !!b; })()`);
  ok(await until(`!document.querySelector('#satPanel .sat-xp').hidden`, 8000), "★ the finder's panel opens in the SATELLITE box");
  ok(await until(`/VOICE/.test(document.querySelector('#satPanel .sat-xp').textContent) && /BEACON/.test(document.querySelector('#satPanel .sat-xp').textContent) && /G4ABC/.test(document.querySelector('#satPanel .sat-xp').textContent)`, 30000),
     `★ rows: a VOICE, the BEACON, G4ABC's CQ (${(await ev(`document.querySelector('#satPanel .sat-xp').textContent`) || '').slice(0, 200)})`);
  ok(await ev(`!!document.querySelector('#satPanel .sat-xp button[data-act="xlisten"]')`) === true, 'a voice row has LISTEN');
  ok(await ev(`[...document.querySelectorAll('#satPanel .sat-xp .sat-xr')].filter(r => /CW|BEACON/.test(r.textContent)).every(r => !r.querySelector('button'))`) === true, 'CW rows have none (CW simply decodes)');
  ok(await until(`window.__ft.includes('TRANSPONDER · every signal found here')`, 6000), "★ the waterfall: the slice shaded and titled (ORBCOMM's drawing)");
  ok(await until(`window.__ft.includes('VOICE') && window.__ft.includes('G4ABC')`, 6000), '…with an arrow per signal: VOICE, G4ABC');
  await shot('rows');
  // LISTEN
  const want = Number(await ev(`(() => { const r = [...document.querySelectorAll('#satPanel .sat-xp .sat-xr')].find(x => x.querySelector('button[data-act="xlisten"]')); return r ? r.querySelector('.sat-xf').textContent : '0'; })()`));
  await ev(`document.querySelector('#satPanel .sat-xp button[data-act="xlisten"]').click()`);
  ok(await until(`/VFO A follows/.test(document.querySelector('#satPanel .sat-xp').textContent)`, 10000), `★ LISTEN: "${(await ev(`document.querySelector('#satPanel .sat-xp .sat-note')?.textContent || ''`))}"`);
  ok(await until(`/LISTENING/.test(document.querySelector('#satPanel .sat-xp').textContent)`, 3000), 'the row says LISTENING');
  const dial = await ev(`document.getElementById('mFreq')?.textContent || ''`);
  ok(want > 0 && dial.replace(/[^0-9.]/g, '').startsWith(want.toFixed(3)), `VFO A snapped to the voice (${dial} for ${want})`);
  ok(/TRANSPONDER: VFO A follows/.test(S.log()), 'the server says so in its log');
  await shot('listen');
  // a phone
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  const fit = JSON.parse(await ev(`JSON.stringify({ sw: document.documentElement.scrollWidth, w: innerWidth })`));
  ok(fit.sw <= fit.w + 1, `390 px: no sideways scroll (${fit.sw} / ${fit.w})`);
  await shot('phone');
  await ev(`document.querySelector('#satPanel button[data-act="exit"]')?.click()`);
  ok(await until(`document.getElementById('satPanel').hidden`, 3000), 'EXIT leaves the mode');
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); S.stop();
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
