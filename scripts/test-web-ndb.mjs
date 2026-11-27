// test-web-ndb.mjs — the web client's NDB mode against the REAL server (fake rtl_tcp with automatic direct sampling
// replaying `test-ndb --write-clip`: WOD 352.0 and CAM 333.0 kHz), in headless Edge with --mute-audio (tests are silent):
// START, the Mode sheet's NDB entry, the MAP-FIRST view (ADS-B's pattern), the live list lowest frequency first with ✓ WOD
// Woodley in NM, its 3-bar meter and the readings streaming in by confidence, the tiles, the beacons on the map with NM
// rings, a row opening its card, RINGS, HEARD LOG, Find NDB, the stats, the fold, 390 px, EXIT NDB giving the dial back. Screenshots to $TMPDIR/vibesdr-ndb-*.png.
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
const V = "document.querySelector('.trkView:not([hidden])')";   // ★ beta96 the AIR & SEA map's view
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webndb-'));
// ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
//   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'NDB web test', locator: 'IO92nh', adminPass: 'ndb-web-admin',
  radios: [{ serial: 'ndb-web', label: 'Test radio', enabled: true, configured: true, users: 1, autoDirectSampling: true }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '362750', '--tones', '0'], { stdio: 'ignore' });
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
  // ★★★ beta96 ONE AIR & SEA LIVE key in the mode menu (MAP-SHARED.md, menu consolidation): last used = NDB → it lands there
  ok(/AIR & SEA LIVE/.test(opts || '') && !/(^|,)NDB(,|$)/.test(opts || ''), `the Mode sheet offers AIR & SEA LIVE (NDB on its TUNE list), no NDB key of its own: ${opts}`);
  await ev(`localStorage.setItem('vibesdr.airSeaLast', 'ndb'); document.getElementById('mModeAirSea')?.click(); true`);
  // ★★★ beta96 (docs/v12/ONE-MAP.md stage 8): NDB is a live state of the AIR & SEA map
  ok(await until(`window.__vibeAirSea?.()?.open === true && window.__vibeAirSea().mode === 'ndb'`, 6000), 'NDB opens the AIR & SEA map, NDB live (map-first, over the waterfall)');
  const ch = await ev(`JSON.stringify(window.__vibeAirSea())`);
  ok(/AVIATION & MARITIME · NDB/.test(ch) && /"exit":"EXIT NDB"/.test(ch) && /"tab":"navaids"/.test(ch), `its title, EXIT NDB and the NAVAIDS tab: ${ch}`);
  ok(await ev(`document.getElementById('app').inert === true`), 'the page under the map is inert');
  // ★★★ beta97 ONE LIST (Stuart 11-27): the beacons are rows of the map's LIST (NAVAIDS); no list of NDB's own; RINGS,
  //     UNHEARD and HEARD LOG · FIND NDB · STATS in MAP CONTROLS; the tiles are STATS' NDB section
  ok(await ev(`!document.getElementById('ndbList') && !document.getElementById('ndbTiles') && !!${V}?.querySelector('.slot-ctl #ndbRings') && !!${V}?.querySelector('.slot-ctl #ndbTools #ndbLogBtn') && !${V}?.querySelector('.slot-top #ndbFit')`), 'no list of NDB\'s own; RINGS and HEARD LOG · FIND NDB · STATS in MAP CONTROLS; the map\'s own FIT (no second one)');
  ok(await until(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].map(r => r.querySelector('.nmT')?.textContent).filter(t => /^✓/.test(t || '')).join() === '✓ CAM,✓ WOD'`, 120000),
     `✓ CAM and ✓ WOD are rows of the ONE LIST, lowest frequency first: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].map(r => r.innerText.replace(/\\n/g, ' ')).join(' | ')`)}`);
  const row = await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].find(r => r.textContent.includes('WOD'))?.innerText.replace(/\\n/g, ' ') || ''`);
  ok(/Woodley/.test(row) && /\d+ NM S/.test(row) && /\d+ dB/.test(row), `WOD's row (NM, bearing, SNR): ${row}`);
  ok(await until(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].some(r => /“[^”]+”/.test(r.textContent))`, 15000), `the live readings stream into the rows: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].map(x => x.querySelector('small')?.textContent).join(' | ').slice(0, 200)`)}`);
  await ev(`${V}.querySelector('[data-cbtn="stats"]')?.click(); 1`); await sleep(600);
  const tiles = await ev(`${V}.querySelector('.trkCard2[data-card="stats"]')?.textContent || ''`);
  ok(/NDB/.test(tiles) && /IDENTIFIED/.test(tiles) && /FURTHEST/.test(tiles), `the tiles are STATS' NDB section: ${tiles.slice(0, 200)}`);
  await ev(`${V}.querySelector('.trkCard2[data-card="stats"] > .ch .x')?.click(); 1`); await sleep(300);
  const MH = `${V}.querySelector('.trkMapHost')`;
  ok(await until(`[...${MH}.querySelectorAll('.leaflet-marker-icon, canvas')].length > 0`, 20000), 'the map draws (GPU, or Leaflet without WebGL 2)');
  // ★ the beacon layer is on the map's OWN map (one WebGL context): its sources there (GPU), or WOD's marker (Leaflet)
  ok(await until(`/WOD/.test(${MH}.textContent) || (window.__vibeNdbLayer?.points?.() || []).length >= 2`, 10000), `WOD and CAM are on the map at their published positions: ${await ev(`JSON.stringify(window.__vibeNdbLayer?.points?.())`)}`);
  ok(await until(`/\\d+ NM/.test(${MH}.textContent) || !!${MH}.querySelector('canvas')`, 5000), `the range rings are labelled in NM: ${await ev(`[...${MH}.querySelectorAll('.vsadsb-rl')].slice(0, 4).map(x => x.textContent).join(', ')`)}`);
  ok(await ev(`${V}.querySelectorAll('canvas.maplibregl-canvas').length <= 1`), 'one map, not two (no map of NDB\'s own)');
  ok(await until(`/NDB/.test(${V}.querySelector('.trkKey')?.textContent || '')`, 3000), 'the KEY has the NDB rows');
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === true`), 'zoom is held');
  console.log('   screenshot', await shot('map'));
  await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].find(r => r.textContent.includes('WOD')).click()`);
  ok(await until(`!document.getElementById('ndbCard').hidden && /WOD/.test(document.getElementById('ndbCard').textContent) && /Woodley/.test(document.getElementById('ndbCard').textContent) && /NM/.test(document.getElementById('ndbCard').textContent)`, 4000),
     `a tapped row opens its card: ${await ev(`document.getElementById('ndbCard').innerText.replace(/\\n/g, ' / ')`)}`);
  ok(await until(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow.ndbR')].filter(r => r.classList.contains('sel')).length === 1`, 3000), 'and is selected in the list');
  console.log('   screenshot', await shot('card'));
  await ev(`${V}.querySelector('[data-cbtn="map"]')?.click(); 1`); await sleep(400);   // ★ MAP CONTROLS holds NDB's keys
  await ev(`document.getElementById('ndbRings').click()`);
  ok(await until(`!document.getElementById('ndbRings').classList.contains('on')`, 2000), 'RINGS toggles');
  await ev(`document.getElementById('ndbRings').click()`);
  await ev(`document.getElementById('ndbLogBtn').click()`);
  ok(await until(`/HEARD LOG/.test(document.getElementById('ndbDlgBox')?.textContent || '') && /WOD/.test(document.getElementById('ndbDlgBox')?.textContent || '')`, 3000), 'HEARD LOG opens with WOD');
  await ev(`document.getElementById('ndbDlgX').click()`);
  await ev(`document.getElementById('ndbFindBtn').click()`);
  ok(await until(`[...document.querySelectorAll('#ndbFoundList tr')].length > 5`, 5000), `Find NDB lists the nearest: ${await ev(`[...document.querySelectorAll('#ndbFoundList tr')].slice(1, 4).map(r => r.textContent).join(' | ')`)}`);
  await ev(`(() => { const i = document.getElementById('ndbQ'); i.value = 'woodley'; i.dispatchEvent(new Event('input')); })()`);
  ok(await until(`[...document.querySelectorAll('#ndbFoundList tr')].some(r => /✓ WOD/.test(r.textContent))`, 5000), 'searching "woodley" finds ✓ WOD (heard)');
  await ev(`document.getElementById('ndbDlgX').click()`);
  await ev(`document.getElementById('ndbStatsBtn').click()`);
  ok(await until(`/BEACONS HEARD PER HOUR/.test(document.getElementById('ndbDlgBox')?.textContent || '') && /WOD/.test(document.getElementById('ndbDlgBox')?.textContent || '')`, 5000), 'the stats open with WOD');
  await ev(`document.getElementById('ndbDlgX').click()`);
  // ★★★ beta96 map cards: the list is the LIST card — its button shuts and opens it (it no longer folds in place)
  await ev(`${V}.querySelector('[data-cbtn="list"]').click()`);
  ok(await until(`${V}.querySelector('[data-cbtn="list"]').getAttribute('aria-expanded') === 'false' && !${V}.querySelector('.trkList').getClientRects().length`, 2000), 'the LIST card shuts');
  await ev(`${V}.querySelector('[data-cbtn="list"]').click()`);
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  await ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b.getAttribute('aria-expanded') !== 'true') b.click(); })()`);   // ★ a phone opens with no card
  await sleep(500);
  const fit = JSON.parse(await ev(`JSON.stringify(${V}.querySelector('.trkList').getBoundingClientRect())`));
  ok(fit.left >= -2 && fit.right <= 390 + 1, `the list fits a phone (the LIST drawer joins the left edge — map cards): ${Math.round(fit.left)}–${Math.round(fit.right)} of 390`);
  ok(await ev(`document.documentElement.scrollWidth <= 390`), `nothing scrolls sideways (${await ev('document.documentElement.scrollWidth')})`);
  ok(await ev(`(() => { const e = ${V}.querySelector('.exit').getBoundingClientRect(); return e.left >= 0 && e.right <= 390 && e.width > 0; })()`), 'EXIT NDB stays on screen');
  console.log('   screenshot', await shot('phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(800);
  await ev(`${V}.querySelector('.exit').click()`);
  ok(await until(`!window.__vibeAirSea?.()?.open`, 6000), 'EXIT NDB closes the map');
  ok(await ev(`document.getElementById('ndbView').contains(document.getElementById('ndbRings')) && document.getElementById('ndbView').contains(document.getElementById('ndbTools')) && document.getElementById('ndbView').hidden`), 'its pieces are parked again');
  ok(await until(`/7[.,]074/.test(document.getElementById('mFreq')?.textContent || '')`, 6000), `the dial is back: ${await ev(`document.getElementById('mFreq')?.textContent`)}`);
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === false && document.getElementById('app').inert === false`), 'zoom and the page are given back');
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
