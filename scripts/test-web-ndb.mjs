// test-web-ndb.mjs — the web client's NDB mode against the REAL server (fake rtl_tcp with automatic direct sampling
// replaying `test-ndb --write-clip`: WOD 352.0 and CAM 333.0 kHz), in headless Edge with --mute-audio (tests are silent):
// START, the Mode sheet's NDB entry, the box, the tiles, the beacon table with ✓ WOD Woodley and its distance, the
// spectrum strip's idents, a row tapped tuning the VFO, the zoom held, the map / live copy / heard log tabs, Find NDB,
// the stats, 390 px with nothing sideways, EXIT NDB giving the dial back. Screenshots to $TMPDIR/vibesdr-ndb-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_NDB_CLIP=…/clip.u8 node scripts/test-web-ndb.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_NDB_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_NDB_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webndb-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'NDB web test', locator: 'IO92nh', adminPass: 'ndb-web-admin',
  radios: [{ serial: 'ndb-web', label: 'Test radio', enabled: true, configured: true, users: 1, autoDirectSampling: true }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '362750'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '7074000', '--mode', 'usb', '--users', '1'],
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-ndb-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  // the landing page's START (the radio list), then the receiver's START RADIO gate
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  {   // ★ a TRUSTED click (the gate starts audio, which a script's .click() may not)
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), 'START RADIO pressed');
  await sleep(2500);
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  ok(/\bNDB\b/.test(opts || ''), `the Mode sheet offers NDB: ${opts}`);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'NDB'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('ndbBox') && !document.getElementById('ndbBox').hidden`, 6000), 'the NDB box opens');
  ok(await until(`[...document.querySelectorAll('#ndbPane td.conf')].some(td => td.textContent.includes('WOD')) && [...document.querySelectorAll('#ndbPane td.conf')].some(td => td.textContent.includes('CAM'))`, 120000),
     `✓ WOD and ✓ CAM in the table: ${await ev(`[...document.querySelectorAll('#ndbPane tr')].map(r => r.textContent).slice(0, 4).join(' | ')`)}`);
  const row = await ev(`[...document.querySelectorAll('#ndbPane tr')].map(r => r.textContent).find(t => t.includes('WOD')) || ''`);
  ok(/Woodley/.test(row) && /\d+ km S/.test(row) && /400 Hz/.test(row), `WOD's row: ${row}`);
  const tiles = await ev(`[...document.querySelectorAll('.ndbTile')].map(t => t.textContent).join(' | ')`);
  ok(/2IDENTIFIED/.test(tiles || '') && /WOD \d+ kmFURTHEST/.test(tiles || ''), `the tiles: ${tiles}`);
  ok(await ev(`[...document.querySelectorAll('#ndbSpec text')].some(t => t.textContent === 'WOD')`), 'the spectrum strip names WOD over its carrier');
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === true`), 'zoom is held');
  console.log('   screenshot', await shot('table'));
  await ev(`[...document.querySelectorAll('#ndbPane tr[data-hz]')].find(r => r.textContent.includes('WOD')).click()`);
  ok(await until(`/0[.,]352|352[.,]0/.test(document.getElementById('mFreq')?.textContent || '')`, 5000), `a tap tunes the VFO to WOD: ${await ev(`document.getElementById('mFreq')?.textContent`)}`);
  for (const [t, sel] of [['copy', '#ndbPane tr'], ['log', '#ndbPane td.unl']]) {
    await ev(`document.querySelector('#ndbTabs [data-t="${t}"]').click()`);
    ok(await until(`document.querySelectorAll('${sel}').length >= 1`, 4000), `the ${t} tab draws: ${await ev(`document.querySelector('#ndbPane')?.innerText.slice(0, 80)`)}`);
  }
  await ev(`document.querySelector('#ndbTabs [data-t="map"]').click()`);
  ok(await until(`!!document.getElementById('ndbMapHost') && (!!document.querySelector('#ndbMapHost canvas') || /WebGL|map/.test(document.getElementById('ndbMapHost').textContent))`, 15000),
     `the map tab: ${await ev(`document.querySelector('#ndbMapHost canvas') ? 'a GL canvas' : document.getElementById('ndbMapHost')?.textContent`)}`);
  await sleep(3000);
  console.log('   screenshot', await shot('map'));
  await ev(`document.querySelector('#ndbTabs [data-t="beacons"]').click()`);
  await ev(`document.getElementById('ndbFind').click()`);
  ok(await until(`[...document.querySelectorAll('#ndbFoundList tr')].length > 5`, 5000), `Find NDB lists the nearest: ${await ev(`[...document.querySelectorAll('#ndbFoundList tr')].slice(1, 4).map(r => r.textContent).join(' | ')`)}`);
  await ev(`(() => { const i = document.getElementById('ndbQ'); i.value = 'woodley'; i.dispatchEvent(new Event('input')); })()`);
  ok(await until(`[...document.querySelectorAll('#ndbFoundList tr')].some(r => /✓ WOD/.test(r.textContent))`, 5000), 'searching "woodley" finds ✓ WOD (heard)');
  console.log('   screenshot', await shot('find'));
  await ev(`document.getElementById('ndbDlgX').click()`);
  await ev(`document.getElementById('ndbStats').click()`);
  ok(await until(`/BEACONS HEARD PER HOUR/.test(document.getElementById('ndbDlgBox')?.textContent || '') && /WOD/.test(document.getElementById('ndbDlgBox')?.textContent || '')`, 5000), 'the stats open with WOD');
  await ev(`document.getElementById('ndbDlgX').click()`);
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify(document.getElementById('ndbBox').getBoundingClientRect())`));
  ok(fit.left >= 15 && fit.right <= 390 - 15, `the box fits a phone: ${Math.round(fit.left)}–${Math.round(fit.right)} of 390`);
  const deckTop = await ev(`Math.min(...['mFreqBox','mVfoDown','mVfoUp','mZoomIn'].map(i => document.getElementById(i)?.getBoundingClientRect().top ?? 1e9))`);
  ok(fit.bottom <= deckTop, `…and clears the control deck: box bottom ${Math.round(fit.bottom)}, deck top ${Math.round(deckTop)}`);
  ok(await ev(`document.documentElement.scrollWidth <= 390`), `nothing scrolls sideways (${await ev('document.documentElement.scrollWidth')})`);
  console.log('   screenshot', await shot('phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(800);
  await ev(`document.getElementById('ndbExit').click()`);
  ok(await until(`document.getElementById('ndbBox')?.hidden === true`, 6000), 'EXIT NDB closes the box');
  ok(await until(`/7[.,]074/.test(document.getElementById('mFreq')?.textContent || '')`, 6000), `the dial is back: ${await ev(`document.getElementById('mFreq')?.textContent`)}`);
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === false`), 'zoom is given back');
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
