// test-web-navtex-map.mjs — V12 THE NAVTEX MAP, stage 1: the STATIONS RECEIVED (2026-11-01), in the REAL web page from a
// REAL vibeserver whose gallery holds a NAVTEX set (headless Edge, --mute-audio):
//   · GET /vibeserver/gallery/navtex-stations.json folds the gallery per station (the label every client shows)
//   · the gallery's standing "NAVTEX · stations received" row and its MAP; the NAVTEX box's MAP
//   · a marker per station with its letter, great-circle lines from the receiver, distances, decluttered names
//   · a station's card (first / last heard, messages, kinds, newest ids); the time filter; ALL by default
//   · LIVE: a message added to the gallery while the map is open appears without a reload
//   VIBESERVER_BIN=… VIBE_NAVTEX_SET=<dir with index.jsonl + files/> [VIBE_SHOT_DIR=…] node scripts/test-web-navtex-map.mjs
// ★ The set is NOT in git (a real server's gallery, ~4 500 messages); scripts/navtex-fixtures/ holds a small curated one.
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', SET = process.env.VIBE_NAVTEX_SET || path.join(root, 'scripts/navtex-fixtures');
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, EDGE, path.join(SET, 'index.jsonl')].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN, a NAVTEX set and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 300 s'); process.exit(1); }, 300_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webnavmap-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge', 'data/gallery']) fs.mkdirSync(path.join(dir, d), { recursive: true });
// ★ the set becomes this server's gallery: its index lines (kept as the gallery wrote them) and the files they name
const gal = path.join(dir, 'data/gallery');
const lines = fs.readFileSync(path.join(SET, 'index.jsonl'), 'utf8').split('\n').filter((l) => l.startsWith('{'));
const keep = [];
for (const l of lines) { const j = JSON.parse(l); const src = path.join(SET, 'files', j.f); if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(gal, j.f)); keep.push(l); } }
keep.reverse();   // ★ the gallery's index is oldest first; gallery.json (where the set came from) is newest first
fs.writeFileSync(path.join(gal, 'index.jsonl'), keep.join('\n') + '\n');
console.log(`   a gallery of ${keep.length} NAVTEX messages`);
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'NAVTEX map web test', locator: 'IO92nh', adminPass: 'nav-map-admin',
  galleryMb: 100, radios: [{ serial: 'nav-map', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(400);
// ★ the real basemap, as an installed server has it: the GPU map's packs and style (test-web-spots' way) and the bundled
//   vector map — without them the page draws an empty grid and the screenshots show no land
const MAPGL = path.join(dir, 'mapgl'); fs.mkdirSync(MAPGL);
for (const n of fs.readdirSync(path.join(root, 'directory/public/mapgl'))) fs.symlinkSync(path.join(root, 'directory/public/mapgl', n), path.join(MAPGL, n));
fs.copyFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(MAPGL, 'vibemap-style.json'));
const env = { ...process.env, VIBESERVER_MAPGL_DIR: MAPGL, VIBESERVER_MAPDATA_DIR: path.join(root, 'assets/mapdata'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '518000', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,900', 'about:blank'], { stdio: 'ignore' });
const H = `http://127.0.0.1:${port}`;
let wsC;
try {
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(1500);
  console.log('── the server: the stations received ──');
  const t0 = Date.now();
  const all = await fetch(`${H}/vibeserver/gallery/navtex-stations.json`).then((r) => r.json());
  const ms = Date.now() - t0;
  const names = all.stations.map((s) => `${s.b1} ${s.name}${s.conf === 'confident' ? '' : ' (' + s.conf + ')'} ${s.km} km ×${s.n}`);
  console.log(`   ${all.stations.length} stations, ${all.messages} messages, ${all.unplaced} not placed (${ms} ms):\n     ` + names.join('\n     '));
  ok(all.stations.length >= 5 && all.messages > 0, `stations folded from the gallery: ${all.stations.length}`);
  ok(all.rx && Math.abs(all.rx.lat - 52.3) < 0.2, 'the receiver is the server\'s own position (IO92nh)');
  ok(all.stations.every((s) => s.first <= s.last && s.n >= s.uniq && s.recent.length > 0 && s.recent.length <= 12), 'each: first ≤ last, received ≥ different, newest ids (≤ 12)');
  // ★ co-sited letters are ONE marker (src/services/navtexMap.ts navSites): the same name and place, a "?" on its own
  const sitesOf = (j) => { const g = []; for (const x of j.stations) { if (x.conf !== 'ambiguous' && g.some((y) => !y.q && y.name === x.name && Math.abs(y.lat - x.lat) < 0.5 && Math.abs(y.lon - x.lon) < 0.8)) continue; g.push({ q: x.conf === 'ambiguous', name: x.name, lat: x.lat, lon: x.lon }); } return g.length; };
  const nSites = sitesOf(all);
  ok(nSites < all.stations.length, `co-sited letters grouped: ${all.stations.length} stations at ${nSites} sites`);
  const day = await fetch(`${H}/vibeserver/gallery/navtex-stations.json?since=3600`).then((r) => r.json());
  ok(day.messages <= all.messages, `the time filter: 1 h holds ${day.messages} of ${all.messages}`);

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
  ok(await until(`!!document.querySelector('#galList .galNav')`, 10000), 'the gallery has a standing "NAVTEX · stations received" row');
  const row = await ev(`document.querySelector('#galList .galNav')?.textContent || ''`);
  ok(new RegExp(`${nSites} sites · ${all.stations.length} stations · ${all.messages} messages`).test(row), `…which counts them: ${row.slice(0, 140)}`);
  console.log('   screenshot', await shot('navtex-gallery-row.png'));
  await ev(`document.querySelector('#galList .galNav .galMap').click()`);
  ok(await until(`!!${view} && ${view}.querySelectorAll('.trkRow').length === ${nSites}`, 10000), `MAP: the sites in its list (${await ev(`${view} ? ${view}.querySelectorAll('.trkRow').length : 0`)})`);
  ok(await until(`document.querySelectorAll('.vtl-mk').length >= ${nSites}`, 15000), `a marker per site: ${await ev(`document.querySelectorAll('.vtl-mk').length`)}`);
  await sleep(4000);
  const arcs = Number(await ev(`document.querySelector('[data-vtl-arcs]')?.getAttribute('data-vtl-arcs') || 0`));
  ok(arcs === nSites, `one great-circle line from the receiver to each site: ${arcs}`);
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ')
     === '1 H:false 24 H:false 7 DAYS:false 30 DAYS:false ALL:true', 'the time filter: five ranges, ALL by default (a received-stations view)');
  const st = await ev(`${view}.querySelector('.st').textContent`);
  ok(/\d+ sites · \d+ stations · \d+ messages · furthest/.test(st), `status: ${st}`);
  const dc = JSON.parse(await ev(`JSON.stringify((() => { const hit = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    const l = [...document.querySelectorAll('.vtl-mk .vtl-lb')].filter(x => getComputedStyle(x).display !== 'none').map(x => x.getBoundingClientRect());
    let o = 0; for (let i = 0; i < l.length; i++) for (let j = i + 1; j < l.length; j++) if (hit(l[i], l[j])) o++; return { o, n: l.length }; })())`));
  ok(dc.o === 0, `no two shown names overlap (${dc.n} shown)`);
  console.log('   screenshot', await shot('navtex-map.png'));
  // a station's card
  await ev(`(() => { const r = [...${view}.querySelectorAll('.trkRow')].find(x => /Oostende|Cullercoats|Niton/.test(x.textContent)); r && r.click(); })()`);
  await sleep(2500);
  const card = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  ok(/Named by the message itself or its station label: \d+ of \d+ messages/.test(card) && /Nearest [A-Z] station on \d+(\.\d)? kHz/.test(card) && /schedule slot/.test(card) && !/\d+ %/.test(card), `the why, in plain words: ${(card.match(/Named by[^·]*|Ended inside[^·]*/g) || []).join(' | ')}`);
  ok(/first heard \d{4}-\d\d-\d\d/.test(card) && /[A-Z] · \d+(\.\d)? kHz · \d+ received, \d+ different/.test(card) && /Newest: [A-Z]{2}\d\d/.test(card), `a station's card: ${card.slice(0, 220)}`);
  ok(/Niton/.test(card) ? (card.match(/ · \d+(\.\d)? kHz · \d+ received/g) || []).length >= 2 : true, 'a co-sited card lists each of its letters');
  console.log('   screenshot', await shot('navtex-map-station.png'));
  // 7 DAYS by keyboard
  await ev(`${view}.querySelector('[data-rng="7d"]').focus()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter' });
  const wk = await fetch(`${H}/vibeserver/gallery/navtex-stations.json?since=${7 * 86400}`).then((r) => r.json());
  ok(await until(`${view}.querySelectorAll('.trkRow').length === ${sitesOf(wk)}`, 6000), `7 DAYS (keyboard): ${sitesOf(wk)} sites`);
  ok(await ev(`localStorage.getItem('vibesdr.navMapRange') === '7d'`), '…remembered');
  await ev(`${view}.querySelector('[data-rng="all"]').click()`);
  await sleep(1500);
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
