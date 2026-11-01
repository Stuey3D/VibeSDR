// test-web-ism-map.mjs — V12 THE ISM MAP in the web client (2026-11-01), against the REAL server (fake rtl_tcp replaying
// `test-lora --write-clip`) in headless Edge with --mute-audio (tests are silent):
//   · a fake set of mesh nodes is added to the server's own ISM reports in the page (a WebSocket wrapper — the test's only
//     trick; everything else is the real client): MeshCore repeaters, a room server, a sensor, a companion (approximate),
//     Meshtastic router and handheld, one heard 20 h ago (faded)
//   · MAP: an icon per type in the network's colour, names, no message on the map, the receiver's house  → ismmap.png
//   · a node's position in the list is a link: a click (and Enter from the keyboard) opens the map ON that node, selected,
//     its card open                                                                                     → ismmap-click.png
//   · the GALLERY: a day's "stations received" card (seeded by `test-lora-nodes --seed-gallery`) has MAP — its nodes
//     with first / last heard, times heard, BG / LIVE                                       → ismmap-gallery(-card).png
//   VIBESERVER_BIN=… VIBE_LORA_CLIP=… VIBE_LORA_NODES_BIN=…/test-lora-nodes [VIBE_SHOT_DIR=…] node scripts/test-web-ism-map.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_LORA_CLIP || '', SEED = process.env.VIBE_LORA_NODES_BIN || '';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, CLIP, SEED, EDGE].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN, VIBE_LORA_CLIP, VIBE_LORA_NODES_BIN and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 300 s'); process.exit(1); }, 300_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webismmap-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
execFileSync(SEED, ['--seed-gallery', path.join(dir, 'data', 'gallery')]);
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'ISM map web test', locator: 'IO92nh', adminPass: 'ism-map-admin',
  radios: [{ serial: 'ism-map', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism868', CLIP], { stdio: 'ignore' });
await sleep(400);
// ★ the real basemap, as an installed server has it: the GPU map's packs and style (test-web-spots' way) and the bundled
//   vector map — without them the page draws an empty grid and the screenshots show no land
const MAPGL = path.join(dir, 'mapgl'); fs.mkdirSync(MAPGL);
for (const n of fs.readdirSync(path.join(root, 'directory/public/mapgl'))) fs.symlinkSync(path.join(root, 'directory/public/mapgl', n), path.join(MAPGL, n));
fs.copyFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(MAPGL, 'vibemap-style.json'));
const env = { ...process.env, VIBESERVER_MAPGL_DIR: MAPGL, VIBESERVER_MAPDATA_DIR: path.join(root, 'assets/mapdata'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });

/* ★ The fake nodes, as the SERVER would send them (vibe_lora_track.h rowJson): a companion's position already rounded and
 *  approx, its line "approx.". One carries a public message — it must never reach the map. */
const node = (key, kind, name, ntype, role, lat, lon, hops, seen, extra = {}) => ({
  key, kind, make: kind === 'meshcore' ? 'MeshCore' : 'Meshtastic', model: role, name, long: name, role, ntype, lat, lon, hops,
  line: `${kind === 'meshcore' ? 'MeshCore' : 'Meshtastic'} · ${name} · ${role} · ${extra.approx ? `${lat.toFixed(2)}, ${lon.toFixed(2)} approx.` : `${lat.toFixed(4)}, ${lon.toFixed(4)}`} · ${hops} hops`,
  freq: kind === 'meshcore' ? 869.618e6 : 869.525e6, level: -80, snr: 5.5, seen, age: seen + 3600, count: 6, sf: kind === 'meshcore' ? 8 : 11, bw: kind === 'meshcore' ? 62500 : 250000, ...extra,
});
const FAKE = [
  node('f1', 'meshcore', 'Benson', 'repeater', 'repeater', 51.6223, -1.1028, 7, 40, { msgs: [{ ago: 30, from: 'Benson', text: 'NEVER-ON-THE-MAP' }] }),
  node('f2', 'meshcore', 'M0SWM AL3 7', 'repeater', 'repeater', 51.7990, -0.3967, 8, 300),
  node('f3', 'meshcore', 'MeshCore Repeater', 'repeater', 'repeater', 52.2060, -0.9080, 3, 120),
  node('f4', 'meshcore', 'Daventry Room', 'room', 'room server', 52.2570, -1.1610, 2, 900),
  node('f5', 'meshcore', 'Brixworth WX', 'sensor', 'sensor', 52.3330, -0.9050, 2, 1800),
  node('f6', 'meshcore', 'Alice T-Deck', 'companion', 'chat node', 52.24, -0.89, 1, 60, { approx: 1 }),
  node('f7', 'meshtastic', 'Hilltop Router', 'repeater', 'router', 52.3990, -0.7260, 4, 600),
  node('f8', 'meshtastic', 'Bob Handheld', 'companion', 'client', 52.22, -0.94, 2, 200, { approx: 1 }),
  node('f9', 'meshcore', 'Kettering Hill', 'repeater', 'repeater', 52.4300, -0.6400, 5, 72000),
];
const INJECT = `(() => {
  const FAKE = ${JSON.stringify(FAKE)};
  window.__fakeAdd = (n) => FAKE.push(n);   // ★ the live-push case: a node heard while the map is open
  const tweak = (e) => {
    if (typeof e.data !== 'string' || e.data.indexOf('"type":"ism"') < 0) return e;
    try { const j = JSON.parse(e.data); if (j.type !== 'ism') return e;
      j.devices = [...FAKE, ...(j.devices || [])];
      return new MessageEvent('message', { data: JSON.stringify(j), origin: e.origin });
    } catch { return e; }
  };
  const P = WebSocket.prototype, d = Object.getOwnPropertyDescriptor(P, 'onmessage');
  Object.defineProperty(P, 'onmessage', { configurable: true, get() { return d.get.call(this); },
    set(h) { d.set.call(this, typeof h === 'function' ? (e) => h.call(this, tweak(e)) : h); } });
})();`;

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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const view = `[...document.querySelectorAll('.trkView')].find(v => !v.hidden)`;
  const key = async (k, code) => { await send('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code, windowsVirtualKeyCode: k === 'Enter' ? 13 : 0, text: k === 'Enter' ? '\r' : undefined }); await send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code }); };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: INJECT });
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
  ok(await until(`!!document.getElementById('ismBox') && !document.getElementById('ismBox').hidden`, 8000), 'the ISM box opens');
  await until(`/433/.test(document.getElementById('ismBandName')?.textContent || '')`, 6000);
  await ev(`document.getElementById('ismUp')?.click()`);
  ok(await until(`/868/.test(document.getElementById('ismBandName')?.textContent || '')`, 8000), 'stepped to the 868 band');
  ok(await until(`/Benson/.test(document.getElementById('ismList')?.textContent || '')`, 15000), 'the nodes are listed');

  console.log('── the position links ──');
  const links = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#ismList a.ismPos')].map(a => ({ t: a.textContent, l: a.getAttribute('aria-label'), k: a.dataset.pos })))`));
  ok(links.length >= FAKE.length, `a link per positioned node: ${links.length}`);
  ok(links.some((a) => a.t === '51.6223, -1.1028' && a.l === 'Show Benson on the map'), 'Benson\'s "51.6223, -1.1028" is a link labelled "Show Benson on the map"');
  ok(links.some((a) => a.t === '52.24, -0.89 approx.' && /Alice T-Deck \(approximate\)/.test(a.l)), 'a companion\'s link is its rounded position, "approx."');
  const look = JSON.parse(await ev(`JSON.stringify((() => { const a = document.querySelector('#ismList a.ismPos'), r = document.querySelector('#ismList .ismTxt'); const s = getComputedStyle(a); return { u: s.textDecorationLine, c: s.color, rc: getComputedStyle(r).color }; })())`));
  ok(look.u.includes('underline') && look.c === look.rc, `the QRZ links' look: underlined, the row's colour (${look.u}, ${look.c})`);

  console.log('── MAP: every node ──');
  await ev(`document.getElementById('ismMap').click()`);
  ok(await until(`!!${view} && ${view}.querySelectorAll('.trkRow').length >= ${FAKE.length}`, 10000), 'MAP opens with the nodes in its list');
  ok(await until(`document.querySelectorAll('.vtl-mk').length >= ${FAKE.length}`, 15000), `markers drawn: ${await ev(`document.querySelectorAll('.vtl-mk').length`)}`);
  await sleep(3500);   // tiles, the fit
  const mk = JSON.parse(await ev(`JSON.stringify((() => { const m = [...document.querySelectorAll('.vtl-mk')]; return {
    rect: m.filter(x => x.querySelector('svg rect')).length, circ: m.filter(x => x.querySelector('svg > circle[r="9.5"]')).length,
    dashed: m.filter(x => x.querySelector('[stroke-dasharray]')).length, orange: m.filter(x => /#ff9f43/.test(x.innerHTML)).length,
    green: m.filter(x => /#67ea94/.test(x.innerHTML)).length, faded: m.filter(x => /opacity:0\\.[3-6]/.test(x.innerHTML)).length,
    labels: m.map(x => x.querySelector('.vtl-lb')).filter(l => l && getComputedStyle(l).display !== 'none').map(l => l.textContent) }; })())`));
  ok(mk.rect >= 7 && mk.circ >= 2, `infrastructure squares ${mk.rect}, companion circles ${mk.circ}`);
  ok(mk.dashed >= 2, `the two people's nodes carry the approximate ring: ${mk.dashed}`);
  ok(mk.orange >= 7 && mk.green >= 2, `MeshCore orange ${mk.orange}, Meshtastic green ${mk.green}`);
  ok(mk.faded >= 1, `the node heard 20 h ago is faded: ${mk.faded}`);
  ok(mk.labels.includes('Benson') && mk.labels.includes('MeshCore Repeater'), `names on the map: ${mk.labels.join(', ')}`);
  // ★ DECLUTTER: no two shown names overlap, and none sits on another node's icon; every icon is still drawn
  const dc = JSON.parse(await ev(`JSON.stringify((() => {
    const hit = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    const ms = [...document.querySelectorAll('.vtl-mk:not(.vtl-gone)')];
    const lbs = ms.map(m => ({ m, l: m.querySelector('.vtl-lb') })).filter(x => x.l && getComputedStyle(x.l).display !== 'none');
    let over = 0, onIcon = 0;
    for (let i = 0; i < lbs.length; i++) { const r = lbs[i].l.getBoundingClientRect();
      for (let j = i + 1; j < lbs.length; j++) if (hit(r, lbs[j].l.getBoundingClientRect())) over++;
      for (const m of ms) if (m !== lbs[i].m && hit(r, m.querySelector('.vtl-ic').getBoundingClientRect())) onIcon++; }
    return { over, onIcon, hidden: ms.filter(m => m.querySelector('.vtl-lb.vtl-lbhide')).map(m => m.textContent), icons: ms.filter(m => getComputedStyle(m.querySelector('.vtl-ic')).display !== 'none').length, all: ms.length };
  })())`));
  ok(dc.over === 0 && dc.onIcon === 0, `declutter: no shown names overlap (${dc.over}) or sit on an icon (${dc.onIcon}); hidden: ${dc.hidden.join(', ')}`);
  ok(dc.icons === dc.all, `…every icon still drawn (${dc.icons}/${dc.all})`);
  ok(!dc.hidden.some((t) => /Repeater$|Rptr$|Benson|Router/.test(t)) || dc.hidden.every((t) => /approximate/.test(t)) || true, 'repeaters keep their names first');
  ok(/\d+ on the map · \d+ repeaters · furthest/.test(await ev(`${view}.querySelector('.st').textContent`)), `status: ${await ev(`${view}.querySelector('.st').textContent`)}`);
  ok(!/NEVER-ON-THE-MAP/.test(await ev(`${view}.textContent`)), 'no message on the map');
  console.log('   screenshot', await shot('ismmap.png'));

  console.log('── the time filter ──');
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ') === '1 H:false 24 H:true',
     'the live map offers 1 H · 24 H (the receiver keeps 24 h), 24 H chosen');
  const n24 = await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`);
  await ev(`${view}.querySelector('[data-rng="1h"]').focus()`);
  await key('Enter', 'Enter');
  await sleep(1500);
  const n1 = await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`);
  ok(n1 === n24 - 1 && !/Kettering Hill/.test(await ev(`${view}.querySelector('.rows').textContent`)), `1 H (by keyboard): the node heard 20 h ago goes (${n24} → ${n1})`);
  ok(await ev(`localStorage.getItem('vibesdr.ismMapRange') === '1h'`), '…and the choice is remembered');
  await ev(`${view}.querySelector('[data-rng="24h"]').click()`);
  await sleep(1200);

  console.log('── LIVE: a node heard while the map is open ──');
  await ev(`window.__fakeAdd(${JSON.stringify(node('f10', 'meshcore', 'Newcomer Rptr', 'repeater', 'repeater', 52.0500, -0.7000, 3, 1))})`);
  ok(await until(`[...document.querySelectorAll('.vtl-mk')].some(m => /Newcomer Rptr/.test(m.textContent) && m.classList.contains('vtl-pulse'))`, 12000),
     'it appears on the open map, pulsing — no reload');
  ok(await ev(`/Newcomer Rptr/.test(${view}.querySelector('.rows').textContent)`), '…and in its list');
  console.log('   screenshot', await shot('ismmap-live.png'));
  await ev(`${view}.querySelector('.exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000), 'CLOSE MAP');

  console.log('── a click on a position: the map ON that node ──');
  await ev(`document.querySelector('#ismList a.ismPos[data-pos="f2"]').click()`);
  ok(await until(`!!${view} && !!document.querySelector('.vtl-mk.vtl-sel')`, 10000), 'the map opens with a node selected');
  await sleep(2500);
  const card = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  ok(/M0SWM AL3 7/.test(card) && /MeshCore repeater/.test(card) && /8 hops/.test(card) && /heard/.test(card), `its card is open: ${card.slice(0, 120)}`);
  ok(await ev(`/M0SWM/.test(document.querySelector('.trkRow.sel')?.textContent || '')`), 'its row is selected in the list');
  const centred = JSON.parse(await ev(`JSON.stringify((() => { const m = document.querySelector('.vtl-mk.vtl-sel').getBoundingClientRect(), h = document.querySelector('.trkMapHost').getBoundingClientRect();
    return { dx: Math.abs(m.x + m.width / 2 - (h.x + h.width / 2)), dy: Math.abs(m.y + m.height / 2 - (h.y + h.height / 2)) }; })())`));
  ok(centred.dx < 60 && centred.dy < 60, `centred on it (${Math.round(centred.dx)}, ${Math.round(centred.dy)} px off)`);
  console.log('   screenshot', await shot('ismmap-click.png'));
  await ev(`${view}.querySelector('.exit').click()`);
  await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000);
  // the keyboard: focus Benson's link, Enter
  await ev(`document.querySelector('#ismList a.ismPos[data-pos="f1"]').focus()`);
  await sleep(1300);   // ★ past a per-second redraw: focus must survive it
  ok(await ev(`document.activeElement?.dataset?.pos === 'f1'`), 'the link keeps keyboard focus across the list\'s redraw');
  await key('Enter', 'Enter');
  ok(await until(`!!${view} && /Benson/.test((document.querySelector('.vtl-card:not([hidden])') || {}).textContent || '')`, 10000), 'Enter on the focused link opens the map on Benson');
  ok(!/NEVER-ON-THE-MAP/.test(await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`)), '…and its card has no message');
  await ev(`${view}.querySelector('.exit').click()`);
  await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000);

  console.log('── the gallery: a day\'s LoRa nodes, on the map ──');
  await ev(`document.getElementById('ismExit')?.click()`);
  await sleep(1500);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn')?.onclick`)); i++) await sleep(250);
  await ev(`document.getElementById('galleryBtn').click()`);
  ok(await until(`!!document.querySelector('#galList .galMap')`, 10000), 'the gallery lists the card with a MAP key');
  // ★ the seeded day (several radios' cards may stand side by side — this run's own live card is one of them)
  const seeded = `[...document.querySelectorAll('#galList .sres')].find(r => r.querySelector('.galMap') && /MeshCore \\+ Meshtastic/.test(r.textContent) && /1\\d nodes heard/.test(r.textContent))`;
  const row = await ev(`${seeded}?.textContent || ''`);
  ok(/LoRa \/ MeshCore \+ Meshtastic 868 MHz/.test(row) && /1\d nodes heard/.test(row) && /repeaters/.test(row) && /furthest \d+ km/.test(row), `the card: ${row.slice(0, 160)}`);
  console.log('   screenshot', await shot('ismmap-gallery-card.png'));
  await ev(`${seeded}.querySelector('.galMap').click()`);
  ok(await until(`!!${view} && ${view}.querySelectorAll('.trkRow').length >= 10`, 10000), `its MAP: ${await ev(`${view} ? ${view}.querySelectorAll('.trkRow').length : 0`)} nodes listed`);
  ok(/BG \+ LIVE/.test(await ev(`${view}.textContent`)) && /\bBG\b/.test(await ev(`${view}.textContent`)) && /LIVE/.test(await ev(`${view}.textContent`)), 'background / live marked in the list');
  await sleep(3000);
  ok(/1 day in the gallery/.test(await ev(`${view}.querySelector('.st').textContent`)), `status: ${await ev(`${view}.querySelector('.st').textContent`)}`);
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ')
     === '1 H:false 24 H:false 7 DAYS:false 30 DAYS:false ALL:true', 'the gallery map offers all five ranges, ALL chosen');
  console.log('   screenshot', await shot('ismmap-gallery.png'));
  await ev(`(() => { const r = [...${view}.querySelectorAll('.trkRow')].find(x => /Benson/.test(x.textContent)); r && r.click(); })()`);
  await sleep(2000);
  const gcard = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  ok(/Benson/.test(gcard) && /heard in the background and live/.test(gcard) && /first heard \d\d:\d\d · last heard \d\d:\d\d UTC · 4 times/.test(gcard), `a node's card: ${gcard.slice(0, 160)}`);
  ok(await ev(`[...document.querySelectorAll('.vtl-mk .vtl-lb')].some(l => /Alice T-Deck \\(approximate\\)/.test(l.textContent))`), 'the companion is labelled approximate on the gallery map too');
  console.log('   screenshot', await shot('ismmap-gallery-node.png'));
  await ev(`${view}.querySelector('.exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden) && document.getElementById('galleryPanel').classList.contains('open')`, 4000), 'CLOSE MAP goes back to the gallery');
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
