// test-web-shipping-map.mjs — V12 2026-11-02 THE SHIPPING MAP in the REAL web page from a REAL vibeserver (headless Edge,
// --mute-audio): a gallery holding the NAVTEX fixtures, a SHIP STORE (written by `test-ais-store --write`, as an AIS radio
// on the same machine would) and a pinned DSC DISTRESS call (as vibe_dsc_gallery.h writes one). Checks:
//   · the ship store over HTTP (/vibeserver/gallery/ais-ships.json) and the met JSON's wind arrows
//   · the map's title, its layers (SHIPS · DSC · STATIONS · WARNINGS · WEATHER — only those with data), each layer's
//     markers, the weather drawn as arrows, the KEY with ship / DSC / arrow rows, a ship's card
//   · the gallery's DSC row opens the map with DSC FIRST, on that call; an empty layer is never a toggle
//   VIBESERVER_BIN=… VIBE_AIS_STORE_BIN=…/test-ais-store [VIBE_SHOT_DIR=…] node scripts/test-web-shipping-map.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', STORE = process.env.VIBE_AIS_STORE_BIN || '', SET = path.join(root, 'scripts/navtex-fixtures');
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, STORE, EDGE].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN, VIBE_AIS_STORE_BIN and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 240 s'); process.exit(1); }, 240_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webship-'));
for (const d of ['data', 'run', 'edge', 'data/gallery']) fs.mkdirSync(path.join(dir, d), { recursive: true });
const gal = path.join(dir, 'data/gallery');
const lines = fs.readFileSync(path.join(SET, 'index.jsonl'), 'utf8').split('\n').filter((l) => l.startsWith('{'));
const keep = [];
for (const l of lines) { const j = JSON.parse(l); const src = path.join(SET, 'files', j.f); if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(gal, j.f)); keep.push(l); } }
keep.reverse();
// ★ the fixtures were heard on 2026-11-01: moved to end an hour ago, so their forecasts are CURRENT (a forecast over a day
//   old is never drawn — latest wins) and the arrows show
const now = Math.floor(Date.now() / 1000);
{
  const tMax = Math.max(...keep.map((l) => JSON.parse(l).t));
  for (let i = 0; i < keep.length; i++) { const j = JSON.parse(keep[i]); j.t = j.t - tMax + now - 3600; keep[i] = JSON.stringify(j); }
}
// ★ a DSC DISTRESS alert with its position, as vibe_dsc_gallery.h saveAlert writes it (newest)
const dscF = '20261102-120000-dsc-2187k5-a1b2.txt';
fs.writeFileSync(path.join(gal, dscF), 'DSC DISTRESS · heard now on 2187.5 kHz\n\nDISTRESS 232001234 sinking 50°10′N 001°10′W\n');
keep.push(JSON.stringify({ f: dscF, k: 'dsc', o: 'background', t: now - 120, hz: 2187500, m: 'usb', d: 'dsc', p: 'Profile 1', r: 'Test radio',
  title: 'DSC DISTRESS 232001234 sinking', plat: 50.1667, plon: -1.1667, lv: 3 }));
fs.writeFileSync(path.join(gal, 'index.jsonl'), keep.join('\n') + '\n');
execFileSync(STORE, ['--write', gal], { stdio: 'ignore' });
console.log(`   a gallery of ${keep.length - 1} NAVTEX messages, one DSC distress call and a ship store`);
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Shipping map web test', locator: 'IO90ir', adminPass: 'ship-map-admin',
  galleryMb: 100, radios: [{ serial: 'ship-map', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(400);
const MAPGL = path.join(dir, 'mapgl'); fs.mkdirSync(MAPGL);
for (const n of fs.readdirSync(path.join(root, 'directory/public/mapgl'))) fs.symlinkSync(path.join(root, 'directory/public/mapgl', n), path.join(MAPGL, n));
fs.copyFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(MAPGL, 'vibemap-style.json'));
const env = { ...process.env, VIBESERVER_MAPGL_DIR: MAPGL, VIBESERVER_MAPDATA_DIR: path.join(root, 'assets/mapdata'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '518000', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
// ★★ TESTS MUST BE SILENT: --mute-audio on every headless browser
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,900', 'about:blank'], { stdio: 'ignore' });
const H = `http://127.0.0.1:${port}`;
let wsC;
try {
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(1500);
  console.log('── the server ──');
  const S = await fetch(`${H}/vibeserver/gallery/ais-ships.json?tracks=1`).then((r) => r.json());
  ok(S.count === 3 && S.vessels.map((v) => v.name).join(',') === 'SOLENT TRADER,ISLAND FERRY,NAB PILOT', `the ship store: ${S.vessels.map((v) => v.name).join(', ')}`);
  ok(S.vessels.every((v) => Array.isArray(v.pts) && v.pts.length === 3 && v.stored === 1), 'each with its short track');
  const S2 = await fetch(`${H}/vibeserver/gallery/ais-ships.json?range=1`).then((r) => r.json());
  ok(S2.count === 0, `?range=1 km from the receiver (IO90ir, 25 km away): none (${S2.count})`);
  await fetch(`${H}/vibeserver/gallery/navtex-warnings.json`);   // ★ builds the index the met request reads
  const M = await fetch(`${H}/vibeserver/gallery/navtex-met.json`).then((r) => r.json());
  const withWind = M.areas.filter((a) => a.fc[0].wind);
  ok(withWind.length > 0 && withWind.every((a) => ['a', 'v', 'c', 'n'].includes(a.fc[0].wind.k)), `the met JSON carries the arrows (${withWind.length} of ${M.areas.length} areas)`);
  const a0 = withWind.find((a) => a.fc[0].wind.k === 'a');
  ok(!!a0 && Math.abs(((a0.fc[0].wind.from + 180) % 360) - a0.fc[0].wind.to) < 0.2, `an arrow points downwind: ${a0 && a0.name} from ${a0 && a0.fc[0].wind.from} → to ${a0 && a0.fc[0].wind.to}`);

  console.log('── the page ──');
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms2) => { for (let i = 0; i < ms2 / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const view = `[...document.querySelectorAll('.trkView')].find(v => !v.hidden)`;
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `${H}/` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 900, deviceScaleFactor: 1, mobile: false });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await sleep(3000);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn')?.onclick`)); i++) await sleep(250);
  await ev(`document.getElementById('galleryBtn').click()`);
  ok(await until(`!!document.querySelector('#galList .galNav')`, 10000), 'the gallery\'s standing SHIPPING MAP row');
  ok(/^SHIPPING MAP · NAVTEX stations received/.test(await ev(`document.querySelector('#galList .galNav .n')?.textContent || ''`)), '…named SHIPPING MAP');
  await ev(`document.querySelector('#galList .galNav .galMap').click()`);
  ok(await until(`!!${view} && ${view}.querySelector('.ttl').textContent === 'SHIPPING MAP'`, 8000), `the title: ${await ev(`${view}?.querySelector('.ttl')?.textContent`)}`);
  const tg = `JSON.stringify([...${view}.querySelectorAll('[data-tgl]')].map(b => b.textContent))`;
  ok(await until(`${tg} === JSON.stringify(['STATIONS','WARNINGS','WEATHER','SHIPS','DSC'])`, 15000), `the layers, NAVTEX first (opened from NAVTEX's row): ${await ev(tg)}`);
  ok(await until(`[...document.querySelectorAll('.vtl-mk')].filter(e => /^(SOLENT TRADER|ISLAND FERRY|NAB PILOT):/.test(e.getAttribute('aria-label') || '')).length === 3`, 10000), 'the three stored ships on the map');
  ok(await until(`[...document.querySelectorAll('.vtl-mk')].some(e => /^DSC DISTRESS from MMSI 232001234/.test(e.getAttribute('aria-label') || '') && e.innerHTML.includes('SOS'))`, 5000), 'the DSC distress pin, with SOS');
  const arrows = Number(await ev(`[...document.querySelectorAll('.vtl-mk')].filter(e => /^Sea area/.test(e.getAttribute('aria-label') || '') && e.querySelector('.nvWind svg g[transform^="rotate("]')).length`));
  ok(arrows > 0, `the weather drawn as arrows (${arrows} sea areas)`);
  const rows = JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('.trkList .rows .sec')].map(s => s.textContent))`));
  ok(rows.includes('SHIPS') && rows.includes('DSC') && rows.includes('NAVTEX STATIONS'), `the RECEIVED list covers what is shown: ${rows.join(' · ')}`);
  const key = await ev(`${view}.querySelector('.trkKey .kb').textContent`);
  ok(/Ship under way/.test(key) && /DSC DISTRESS position/.test(key) && /points DOWNWIND/.test(key) && /Gale force 8 or more: red/.test(key), 'the KEY: ships, DSC and the arrows beside the hazards');
  console.log('   screenshot', await shot('shipping-map.png'));
  // a stored ship's card
  await ev(`(() => { const r = [...${view}.querySelectorAll('.trkRow')].find(x => /ISLAND FERRY/.test(x.textContent)); r && r.click(); })()`);
  await sleep(2000);
  const card = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  ok(/ISLAND FERRY/.test(card) && /MMSI235100002|MMSI 235100002/.test(card.replace(/\s+/g, ' ')) && /passenger/.test(card), `a ship's card: ${card.slice(0, 160)}`);
  // SHIPS off: the ships go, the toggle stays (it has data)
  await ev(`${view}.querySelector('[data-tgl="ships"]').click()`);
  ok(await until(`[...document.querySelectorAll('.vtl-mk')].filter(e => /^(SOLENT TRADER|ISLAND FERRY|NAB PILOT):/.test(e.getAttribute('aria-label') || '')).length === 0`, 5000), 'SHIPS off: the ships go');
  ok(await ev(`localStorage.getItem('vibesdr.shipMapLayers').includes('"ships":false')`), '…remembered');
  await ev(`${view}.querySelector('[data-tgl="ships"]').click()`);
  await ev(`${view}.querySelector('.exit').click()`);
  await sleep(800);
  // the gallery's DSC row: DSC FIRST, on that call
  await ev(`document.getElementById('galleryBtn').click()`);
  ok(await until(`[...document.querySelectorAll('#galList .sres')].some(r => /DSC DISTRESS 232001234/.test(r.textContent) && r.querySelector('.galMap'))`, 8000), 'the gallery\'s DSC call has MAP');
  await ev(`[...document.querySelectorAll('#galList .sres')].find(r => /DSC DISTRESS 232001234/.test(r.textContent)).querySelector('.galMap').click()`);
  ok(await until(`!!${view} && JSON.parse(${tg})[0] === 'DSC'`, 10000), `opened from the DSC call: DSC first (${await ev(`${view} ? ${tg} : ''`)})`);
  ok(await until(`!!document.querySelector('.vtl-card:not([hidden])') && /DSC DISTRESS/.test(document.querySelector('.vtl-card:not([hidden])').textContent)`, 8000), '…on that call (its card open)');
  console.log('   screenshot', await shot('shipping-map-dsc.png'));
  await ev(`${view}.querySelector('.exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000), 'CLOSE MAP');
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
