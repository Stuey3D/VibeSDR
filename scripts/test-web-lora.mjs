// test-web-lora.mjs — the web client's LoRa in ISM mode against the REAL server (fake rtl_tcp replaying
// `test-lora --write-clip`: Meshtastic, MeshCore and LoRaWAN on the 868 band), in headless Edge with --mute-audio (tests
// are silent): the band stepped to 868, the MESHTASTIC / MESHCORE / LORAWAN / MESSAGES chips, a node's detail on a tap,
// the public messages, the box's MAP key (drawn only once something has a position), the map with the nodes and back,
// and phone width. Screenshots to $TMPDIR/vibesdr-lora-*.png. docs/v12/LORA-MESH.md.
//   VIBESERVER_BIN=…/vibeserver VIBE_LORA_CLIP=…/clip.u8 node scripts/test-web-lora.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_LORA_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_LORA_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 240 s'); process.exit(1); }, 240_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-weblora-'));
// ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
//   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'LoRa web test', locator: 'IO92nh', adminPass: 'lora-web-admin',
  radios: [{ serial: 'lora-web', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism868', CLIP], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-lora-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const chip = (label) => ev(`(() => { const b = [...document.querySelectorAll('#ismChips button')].find(x => x.textContent.trim() === '${label}'); b && b.click(); return !!b; })()`);
  const rowsText = () => ev(`[...document.querySelectorAll('#ismList .ismTxt')].map(x => x.textContent).join(' | ')`);
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
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
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'ISM'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('ismBox') && !document.getElementById('ismBox').hidden`, 6000), 'the ISM box opens');
  ok(await ev(`document.getElementById('ismMap')?.hidden === true`), 'no MAP key while nothing has a position (no dead key)');
  // the UK landing is 433: step up to 868
  await until(`/433/.test(document.getElementById('ismBandName')?.textContent || '')`, 6000);
  await ev(`document.getElementById('ismUp')?.click()`);
  ok(await until(`/868/.test(document.getElementById('ismBandName')?.textContent || '')`, 6000), 'stepped to the 868 band');
  for (const c of ['MESHTASTIC', 'MESHCORE', 'LORAWAN', 'MESSAGES'])
    ok(await ev(`[...document.querySelectorAll('#ismChips button')].some(x => x.textContent.trim() === '${c}')`), `the ${c} chip`);
  ok(await until(`/Northampton Base/.test([...document.querySelectorAll('#ismList .ismTxt')].map(x => x.textContent).join('|'))
                 && /Moulton Rptr/.test([...document.querySelectorAll('#ismList .ismTxt')].map(x => x.textContent).join('|'))`, 60000),
     `mesh nodes listed: ${await rowsText()}`);
  ok(/LoRa \d+ frames · \d+ of \d+ channels/.test(await ev(`document.getElementById('ismStat')?.textContent`) || ''), `the status counts LoRa: ${await ev(`document.getElementById('ismStat')?.textContent`)}`);
  ok(await until(`document.getElementById('ismMap') && !document.getElementById('ismMap').hidden`, 5000), 'MAP appears once a node has a position');
  await chip('LORAWAN');
  await sleep(400);
  const lw = await rowsText();
  ok(/TTN 260B1234 · Unconfirmed up · FCnt 4211 · port 2 · 12 bytes encrypted/.test(lw) && !/Northampton/.test(lw), `LORAWAN: ${lw}`);
  await chip('MESHTASTIC');
  await sleep(400);
  // a tap opens a node's detail
  await ev(`(() => { const r = [...document.querySelectorAll('#ismList .ismRow')].find(x => /Northampton Base/.test(x.textContent)); r && r.click(); return !!r; })()`);
  await sleep(300);
  const det = await ev(`document.querySelector('#ismList .ismDetail')?.textContent || ''`);
  ok(/SF11/.test(det) && /Heltec V3/.test(det) && /52\.2400, -0\.8950/.test(det), `a node's detail: ${det}`);
  await chip('MESSAGES');
  await sleep(400);
  const msgs = await rowsText();
  ok(/Alice: anyone on the mesh tonight\?/.test(msgs) && /Hello from the Racecourse/.test(msgs), `MESSAGES: ${msgs}`);
  const all = await ev(`document.body.innerText`);
  ok(!/SECRET/.test(all), 'no private or direct text anywhere on the page');
  console.log('   screenshot', await shot('list'));
  await chip('ALL');
  // MAP → the shared map view with the nodes, CLOSE MAP → back to the box
  await ev(`document.getElementById('ismMap').click()`);
  ok(await until(`!![...document.querySelectorAll('.trkView')].find(v => !v.hidden && /ISM/.test(v.textContent))`, 8000), 'MAP opens the ISM map view');
  ok(await until(`[...document.querySelectorAll('.trkView')].filter(v => !v.hidden).some(v => v.querySelectorAll('.trkRow').length >= 2)`, 8000),
     `the nodes in the map's list: ${await ev(`[...document.querySelectorAll('.trkView')].filter(v => !v.hidden).map(v => [...v.querySelectorAll('.trkRow')].map(r => r.textContent).join(' | ')).join('')`)}`);
  ok(/\d+ on the map/.test(await ev(`[...document.querySelectorAll('.trkView')].filter(v => !v.hidden).map(v => v.querySelector('.st')?.textContent || '').join('')`) || ''), 'the map status counts them');
  await sleep(2500);
  console.log('   screenshot', await shot('map'));
  await ev(`(() => { const v = [...document.querySelectorAll('.trkView')].find(v => !v.hidden); v && v.querySelector('.exit').click(); return !!v; })()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden) && !document.getElementById('ismBox').hidden`, 4000), 'CLOSE MAP goes back to the box, ISM still on');
  // phone width
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const fit = JSON.parse(await ev(`JSON.stringify(document.getElementById('ismBox').getBoundingClientRect())`));
  ok(fit.left >= 15 && fit.right <= 390 - 15, `the box fits a phone: ${Math.round(fit.left)}–${Math.round(fit.right)} of 390`);
  ok(await ev(`(() => { const m = document.getElementById('ismMap').getBoundingClientRect(); return m.width > 0 && m.right <= 390; })()`), 'MAP reachable at 390 px');
  console.log('   screenshot', await shot('phone'));
  await ev(`document.getElementById('ismExit').click()`);
  ok(await until(`document.getElementById('ismBox').hidden`, 5000), 'EXIT ISM closes the box');
} catch (e) { fail++; console.log('   FAIL', e?.message || e); }
finally {
  clearTimeout(watchdog);
  try { wsC?.close(); } catch { /* gone */ }
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
