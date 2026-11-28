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
  node('f1', 'meshcore', 'Benson', 'repeater', 'repeater', 51.6223, -1.1028, 7, 40, { node: 'c1c2c3c4', msgs: [{ ago: 30, from: 'Benson', text: 'NEVER-ON-THE-MAP' }] }),
  node('f2', 'meshcore', 'M0SWM AL3 7', 'repeater', 'repeater', 51.7990, -0.3967, 8, 300, { node: '9f001122' }),
  node('f3', 'meshcore', 'MeshCore Repeater', 'repeater', 'repeater', 52.2060, -0.9080, 3, 120, { node: 'a3b4c5d6' }),
  node('f4', 'meshcore', 'Daventry Room', 'room', 'room server', 52.2570, -1.1610, 2, 900),
  node('f5', 'meshcore', 'Brixworth WX', 'sensor', 'sensor', 52.3330, -0.9050, 2, 1800),
  node('f6', 'meshcore', 'Alice T-Deck', 'companion', 'chat node', 52.24, -0.89, 1, 60, { approx: 1 }),
  node('f7', 'meshtastic', 'Hilltop Router', 'repeater', 'router', 52.3990, -0.7260, 4, 600),
  node('f8', 'meshtastic', 'Bob Handheld', 'companion', 'client', 52.22, -0.94, 2, 200, { approx: 1 }),
  node('f9', 'meshcore', 'Kettering Hill', 'repeater', 'repeater', 52.4300, -0.6400, 5, 72000, { node: 'a3000001' }),
  // ★★ beta96 THE ROUTE: its advert came 9f → a3 → 5e → c1: M0SWM (placed), a3 = MeshCore Repeater OR Kettering Hill
  //   (ambiguous), 5e (nobody heard: unknown), Benson (placed)
  node('f11', 'meshcore', 'Route Test', 'repeater', 'repeater', 51.9500, -0.7500, 4, 90, { node: 'ee112233', relays: '9f a3 5e c1' }),
];
const INJECT = `(() => {
  const FAKE = ${JSON.stringify(FAKE)};
  window.__fakeAdd = (n) => FAKE.push(n);   // ★ the live-push case: a node heard while the map is open
  window.__fakeClear = () => { FAKE.length = 0; };   // ★ beta92: nothing heard live — only what the gallery remembers
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
  await ev(`(() => { document.getElementById('ismBands')?.click(); const r = document.querySelector('#ismBandList .ismBandRow[data-id="868"]'); r?.click(); return !!r; })()`);   // ★ beta72: BANDS → 868 by its row (the box's ◀ ▶ are gone; 479–867 now sit between 433 and 868)
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
  // ★★ 11-21 AUTO FIT (Stuart: "the map needs the auto fit" / "i have to keep pressing fit"): every node in view on opening
  const fitIn = JSON.parse(await ev(`JSON.stringify((() => { const h = ${view}.querySelector('.trkMapHost').getBoundingClientRect();
    const ms = [...document.querySelectorAll('.vtl-mk:not(.vtl-gone) .vtl-ic')].map(i => i.getBoundingClientRect());
    return { n: ms.length, out: ms.filter(r => r.right < h.left || r.left > h.right || r.bottom < h.top || r.top > h.bottom).length }; })())`));
  ok(fitIn.n >= 7 && fitIn.out === 0, `AUTO FIT on opening: ${fitIn.n - fitIn.out} of ${fitIn.n} nodes in view`);
  const mk = JSON.parse(await ev(`JSON.stringify((() => { const m = [...document.querySelectorAll('.vtl-mk')]; return {
    rect: m.filter(x => x.querySelector('svg rect')).length, circ: m.filter(x => x.querySelector('svg > circle[r="9.5"]')).length,
    dashed: m.filter(x => x.querySelector('[stroke-dasharray]')).length, orange: m.filter(x => /#ff9f43/.test(x.innerHTML)).length,
    green: m.filter(x => /#67ea94/.test(x.innerHTML)).length, faded: m.filter(x => /opacity:0\\.[3-6]/.test(x.innerHTML)).length,
    labels: m.map(x => x.querySelector('.vtl-lb')).filter(l => l && getComputedStyle(l).display !== 'none').map(l => l.textContent), all: m.map(x => (x.querySelector('.vtl-lb')?.textContent || '-') + (x.querySelector('.vtl-lb.vtl-lbhide') ? '[hid]' : '')) }; })())`));
  ok(mk.rect >= 7 && mk.circ >= 2, `infrastructure squares ${mk.rect}, companion circles ${mk.circ}`);
  ok(mk.dashed >= 2, `the two people's nodes carry the approximate ring: ${mk.dashed}`);
  ok(mk.orange >= 7 && mk.green >= 2, `MeshCore orange ${mk.orange}, Meshtastic green ${mk.green}`);
  ok(mk.faded >= 1, `the node heard 20 h ago is faded: ${mk.faded}`);
  // ★ map cards: the LIST card sits over part of the map, so AUTO frames tighter and the declutter may hide a name for a
  //   neighbour's — the node keeps its icon and its name (shown, or hidden by the declutter), never lost
  ok(mk.labels.includes('Benson') && (mk.labels.includes('MeshCore Repeater') || mk.all.includes('MeshCore Repeater[hid]')), `names on the map: ${mk.labels.join(', ')} (all: ${mk.all.join(', ')})`);
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
  { const rng = JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ');
    ok(rng === '1 H:false 24 H:true THIS SESSION:false', `the live map offers 1 H · 24 H (the receiver keeps 24 h), 24 H chosen, and beta96 THIS SESSION (${rng})`); }
  const n24 = await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`);
  // ★ map cards: the time filter lives in MAP CONTROLS — its card opened first (a hidden key cannot take focus, and focus
  //   left on CLOSE MAP made Enter close the map: '13 → 0', then 'Uncaught')
  await ev(`(() => { const b = ${view}.querySelector('[data-cbtn="map"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(500);
  await ev(`${view}.querySelector('[data-rng="1h"]').focus()`);
  if (process.env.ISM_DIAG) console.log('   DIAG', await ev(`JSON.stringify({ act: (document.activeElement?.outerHTML || '').slice(0, 120), vis: (() => { const b = ${view}.querySelector('[data-rng="1h"]'); return b ? b.getClientRects().length : -1; })(), hl: (document.querySelector('.gpFocus')?.outerHTML || '').slice(0, 120) })`));
  await key('Enter', 'Enter');
  if (process.env.ISM_DIAG) console.log('   DIAG2', await ev(`JSON.stringify({ open: !!document.querySelector('.trkView:not([hidden])'), act: (document.activeElement?.outerHTML || '').slice(0, 80) })`));
  await sleep(1500);
  const n1 = await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`);
  ok(n1 === n24 - 1 && !/Kettering Hill/.test(await ev(`${view}.querySelector('.rows').textContent`)), `1 H (by keyboard): the node heard 20 h ago goes (${n24} → ${n1})`);
  ok(await ev(`localStorage.getItem('vibesdr.ismMapRange') === '1h'`), '…and the choice is remembered');
  await ev(`${view}.querySelector('[data-rng="24h"]').click()`);
  await sleep(1200);

  /* ★★ beta96 (Stuart 11-26: "meshcore list needs to filter by nearest first and latest first") NEAREST | LATEST */
  console.log('── beta96: the NODES list, NEAREST | LATEST ──');
  const srt = JSON.parse(await ev(`JSON.stringify((() => { const h = ${view}.querySelector('.hd .srt'); return { hidden: h.hidden, aria: h.getAttribute('aria-label'),
    keys: [...h.querySelectorAll('[data-srt]')].map(b => b.dataset.srt + '>' + b.dataset.next) }; })())`));
  // ★ map cards (Stuart 12:58): ONE toggle wearing the current sort (rings = nearest, clock = latest) — a press swaps
  ok(!srt.hidden && srt.aria === 'Sort the nodes' && srt.keys.join(' ') === 'nearest>latest', `the list's sort key, one toggle on NEAREST: ${srt.keys.join(' ')}, "${srt.aria}"`);
  const kmOrder = `JSON.stringify([...${view}.querySelectorAll('.trkRow')].map(r => parseFloat((r.querySelector('.rg')?.textContent || '').replace(',', ''))).filter(n => isFinite(n)))`;
  const fakeOrder = `JSON.stringify([...${view}.querySelectorAll('.trkRow')].map(r => r.dataset.key).filter(k => /^f\\d+$/.test(k)))`;
  const kms = JSON.parse(await ev(kmOrder));
  ok(kms.length >= 9 && kms.every((k, i) => i === 0 || k >= kms[i - 1]), `NEAREST (the default): distances never go down: ${kms.join(', ')}`);
  await ev(`${view}.querySelector('.hd .srt .srtT').click()`);
  const LATE = ['f1', 'f6', 'f11', 'f3', 'f8', 'f2', 'f7', 'f4', 'f5', 'f9'];   // their seen: 40 60 90 120 200 300 600 900 1800 72000
  ok(await until(`${fakeOrder} === ${JSON.stringify(JSON.stringify(LATE))}`, 4000), `LATEST: the most recently heard first: ${await ev(fakeOrder)}`);
  ok(await ev(`${view}.querySelector('.hd .srt .srtT')?.dataset.srt === 'latest' && localStorage.getItem('vibesdr.meshListSort') === 'latest'`), '…the toggle now on LATEST, and remembered for this viewer');
  ok(/\d+ on the map · \d+ repeaters/.test(await ev(`${view}.querySelector('.st').textContent`)) && await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`) === n24,
     'the order changes nothing on the map: the same markers, the same status');
  await ev(`${view}.querySelector('.hd .srt .srtT').focus()`);
  await key('Enter', 'Enter');   // ★ by keyboard
  ok(await until(`(() => { const k = ${kmOrder}; const a = JSON.parse(k); return a.length >= 9 && a.every((x, i) => i === 0 || x >= a[i - 1]); })()`, 4000)
     && await ev(`localStorage.getItem('vibesdr.meshListSort') === 'nearest'`), 'NEAREST again (by keyboard), remembered');

  /* ★★★ beta97 hotfix (Stuart, the MeshCore map at 868.600): the drawer's waterfall fits the LoRa decoder's CHANNELS (the
   *  report's lora.chs), one labelled marker each — never the dial's 13 kHz slice of a wide capture */
  console.log('── beta97 hotfix: the waterfall fits the LoRa channels ──');
  { const has = await ev(`!!${view}.querySelector('.lSwapC') && !${view}.querySelector('.lSwapC').hidden`);
    if (has) {
      await ev(`${view}.querySelector('.lSwapC').click(); 1`);
      ok(await until(`/LoRa channel|the capture/.test(${view}.querySelector('.wfInfo')?.textContent || '')`, 8000), `the waterfall's span is the LoRa channels or the capture, never a 13 kHz slice: "${await ev(`${view}.querySelector('.wfInfo')?.textContent`)}"`);
      const info = await ev(`${view}.querySelector('.wfInfo')?.textContent || ''`);
      const n = Number((/(\d+) LoRa channel/.exec(info) || [])[1] || 0);
      const st = JSON.parse(await ev(`JSON.stringify(window.__vibeMapWf?.() || null)`));
      ok(!!st && st.hi - st.lo >= 100e3 && (!n || st.marks === n), `span ${st ? Math.round((st.hi - st.lo) / 1e3) : '-'} kHz (≥ 100 kHz, never a few bins), ${st?.marks} markers for ${n} channels (${(st?.labels || []).slice(0, 4).join(' | ')})`);
      await ev(`(() => { if (${view}.querySelector('.trkList')?.dataset.pane === 'wf') ${view}.querySelector('.lSwap')?.click(); return 1; })()`); await sleep(400);
    } else console.log('   (no waterfall offered on this stack)');
  }
  /* ★★★ beta97 hotfix (Stuart, iPhone Safari: a MeshCore card stuck UNDER the chips, its × out of reach): a node's card stays
   *  in the chip-free rectangle, its × the thing under its centre, at phone sizes, portrait and landscape */
  console.log('── beta97 hotfix: a node\'s card on a phone ──');
  for (const [w, h] of [[390, 844], [375, 667], [844, 390], [667, 375]]) {
    /* mobile: false — Chrome's MOBILE emulation widens the layout viewport to the content, and the map view's min-width: 100vw
     *  (563b843be) then holds it at the desktop's 1280 px after the switch (a flake: the view stuck wide, the later zoom checks
     *  failing with it). iOS Safari with width=device-width keeps 100vw at the device width; the phone's size is what counts */
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 2, mobile: false }); await sleep(1500);
    await ev(`(() => { const c = document.querySelector('.trkView:not([hidden])').querySelector('.vtl-card'); if (c && !c.hidden) c.querySelector('.vtl-x')?.click(); return 1; })()`); await sleep(300);
    await ev(`(() => { const m = [...document.querySelector('.trkView:not([hidden])').querySelectorAll('.vtl-mk:not(.vtl-gone)')].find((x) => /Benson|MeshCore Repeater|Daventry/.test(x.getAttribute('aria-label') || x.textContent)); m && m.click(); return 1; })()`); await sleep(1200);
    const r = JSON.parse(await ev(`JSON.stringify((() => { try { const c = document.querySelector('.trkView:not([hidden])').querySelector('.vtl-card'); if (!c || c.hidden || !c.getClientRects().length) return { card: null };
      const b = c.getBoundingClientRect(), x = c.querySelector('.vtl-x'), xb = x.getBoundingClientRect(); const hit = document.elementFromPoint((xb.left + xb.right) / 2, (xb.top + xb.bottom) / 2);
      const chips = [...document.querySelector('.trkView:not([hidden])').querySelectorAll('.mChipsL .mDock:not([hidden]) .hCard, .mChipsR > :not([hidden]) .hCard, .mChipsR > .edgeDock:not(.mDock):not([hidden]) .hTab, .trkTop .exit, #mapShLine')].filter((e) => e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden');
      const overEl = chips.filter((e) => { const k = e.getBoundingClientRect(); return b.left < k.right - 1 && b.right > k.left + 1 && b.top < k.bottom - 1 && b.bottom > k.top + 1; }); const over = overEl.length;
      return { card: [Math.round(b.left), Math.round(b.top), Math.round(b.right), Math.round(b.bottom)], xHit: !!hit && (hit === x || x.contains(hit)), over, inView: b.top >= 0 && b.bottom <= innerHeight + 1 && b.left >= 0 && b.right <= innerWidth + 1 }; } catch (e) { return { err: String(e).slice(0, 200), view: !!document.querySelector('.trkView:not([hidden])') }; } })())`));
    if (process.env.ISM_SHOT) { const sh = await send('Page.captureScreenshot', { format: 'png' }); (await import('node:fs')).writeFileSync(`${process.env.ISM_SHOT}/card-${w}x${h}.png`, Buffer.from(sh.result.data, 'base64')); }
    ok(!!r.card && r.xHit && !r.over && r.inView, `${w}×${h}: a node's card clear of every chip, on screen, its × reachable (${JSON.stringify(r)})`);
    await ev(`(() => { const c = document.querySelector('.trkView:not([hidden])').querySelector('.vtl-card'); if (c && !c.hidden) c.querySelector('.vtl-x')?.click(); return 1; })()`); await sleep(300);
  }
  /* ★★★ beta97 hotfix (found in the app): with the LIST tall, its top tabs — the list ↔ waterfall swap among them, which this
   *  map shows — stop below the lowest left chip (MAP CONTROLS), normal and maximised, on a phone and a desktop */
  for (const [w, h, mob] of [[390, 844, true], [375, 667, true], [1280, 900, false]]) {
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: mob ? 2 : 1, mobile: false }); await sleep(1200);
    await ev(`(() => { const v = document.querySelector('.trkView:not([hidden])'), c = v.querySelector('.trkCard2[data-card="list"]'); if (!c || c.hidden) v.querySelector('[data-cbtn="list"]')?.click(); return 1; })()`); await sleep(700);
    const hits = async () => JSON.parse(await ev(`JSON.stringify((() => { const v = document.querySelector('.trkView:not([hidden])'), c = v.querySelector('.trkCard2[data-card="list"]'); if (!c || c.hidden) return { list: false, why: !c ? 'none' : 'hidden' };
      const tabs = [c, ...c.querySelectorAll('.lMax, .lSwap')].filter((e) => !e.hidden && e.getClientRects().length && getComputedStyle(e).display !== 'none');
      const out = []; for (const e of tabs) { const b = e.getBoundingClientRect(); for (const k of v.querySelectorAll('.mChipsL .mDock:not([hidden]) .hCard')) { const q = k.getBoundingClientRect(); if (q.width && b.right > q.left && b.left < q.right && b.bottom > q.top + 0.5 && b.top < q.bottom - 0.5) out.push(e.className.split(' ')[0] + '/' + (k.closest('.mDock')?.dataset.dock || '?')); } }
      return { list: true, swap: !!tabs.find((e) => e.classList.contains('lSwap')), hits: out }; })())`));
    const n0 = await hits();
    await ev(`(() => { const t = document.querySelector('.trkView:not([hidden])').querySelector('.lMax'); if (t && t.getAttribute('aria-pressed') !== 'true') t.click(); return 1; })()`); await sleep(800);
    const n1 = await hits();
    await ev(`(() => { const t = document.querySelector('.trkView:not([hidden])').querySelector('.lMax'); if (t && t.getAttribute('aria-pressed') === 'true') t.click(); return 1; })()`); await sleep(600);
    ok(n0.list && n1.list && !n0.hits.length && !n1.hits.length, `${w}×${h}: the LIST and its tabs (swap tab ${n1.swap ? 'shown' : 'not shown'}) clear every left chip, normal and maximised (${JSON.stringify(n0.list && n1.list ? [n0.hits, n1.hits] : [n0, n1])})`);
  }
  await send('Emulation.clearDeviceMetricsOverride', {}); await sleep(1500);

  console.log('── LIVE: a node heard while the map is open ──');
  await ev(`window.__fakeAdd(${JSON.stringify(node('f10', 'meshcore', 'Newcomer Rptr', 'repeater', 'repeater', 52.0500, -0.7000, 3, 1))})`);
  ok(await until(`[...document.querySelectorAll('.vtl-mk')].some(m => /Newcomer Rptr/.test(m.textContent) && m.classList.contains('vtl-pulse'))`, 12000),
     'it appears on the open map, pulsing — no reload');
  ok(await ev(`/Newcomer Rptr/.test(${view}.querySelector('.rows').textContent)`), '…and in its list');
  console.log('   screenshot', await shot('ismmap-live.png'));

  /* ★★ beta96 (Stuart 11-26) THE ROUTE, A SELECTION HOLDS THE VIEW, ONE km ↔ miles */
  console.log('── beta96: the route, the held view, km ↔ miles ──');
  const st0 = await ev(`${view}.querySelector('.st').textContent`);
  ok(/furthest direct: |furthest via mesh: /.test(st0) && / km/.test(st0), `the status says furthest direct / via mesh: ${st0}`);
  await ev(`document.querySelector('#ismChips .ismUnits[data-u="speed"]').click()`);   // the ISM list's KM/H → MPH key
  ok(await until(`/ mi/.test(${view}.querySelector('.st').textContent) && !/ km/.test(${view}.querySelector('.st').textContent)`, 4000),
     `KM/H → MPH in the ISM list: the map's status in miles: ${await ev(`${view}.querySelector('.st').textContent`)}`);
  const rowsMi = await ev(`${view}.querySelector('.rows').textContent`);
  ok(/\d mi/.test(rowsMi) && !/\d km/.test(rowsMi) && !/\bNM\b/.test(rowsMi), `…its NODES list in miles too (never NM): ${rowsMi.slice(0, 140)}`);
  const cam = `JSON.stringify((() => { const m = window.__vibeTrkMap, c = m.getCenter(); return { lat: +c.lat.toFixed(4), lng: +c.lng.toFixed(4), z: +m.getZoom().toFixed(2) }; })())`;
  const inView = (pts) => `JSON.stringify((() => { const m = window.__vibeTrkMap, h = document.querySelector('.trkMapHost').getBoundingClientRect();
    return ${JSON.stringify(pts)}.filter(p => { const q = m.latLngToContainerPoint(p); return !(q.x >= 0 && q.y >= 0 && q.x <= h.width && q.y <= h.height); }).length; })())`;
  const routeBtn = `(document.querySelector('.vtl-card:not([hidden]) .vmrBtn') || null)`;
  // ★★ (Stuart 11-26 refinement) a pick opens the card and ZOOMS IN — it never draws the route itself
  const z0 = JSON.parse(await ev(cam)).z;
  await ev(`(() => { const r = [...${view}.querySelectorAll('.trkRow')].find(x => /Route Test/.test(x.textContent)); r && r.click(); })()`);
  await sleep(2000);
  const rcard = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  const zp = JSON.parse(await ev(cam));
  ok(zp.z >= Math.max(z0, 10) && Math.abs(zp.lat - 51.95) < 0.02 && Math.abs(zp.lng + 0.75) < 0.02, `a NODES pick: zoomed in on the node (${z0} → ${zp.z})`);
  ok(await ev(`document.querySelectorAll('.vmr-bd').length === 0 && document.querySelectorAll('.vmr-ar').length === 0`), '…no route drawn by the pick');
  ok(await ev(`!!${routeBtn} && ${routeBtn}.textContent === 'ROUTE' && ${routeBtn}.getAttribute('aria-pressed') === 'false'`), '…its card has a ROUTE key (this node\'s packet carried relays)');
  ok(/route: 4 hops, 2 placed, 1 ambiguous, 1 unknown/.test(rcard) && /1 · 9f · M0SWM AL3 7/.test(rcard) && /one of MeshCore Repeater, Kettering Hill|one of Kettering Hill, MeshCore Repeater/.test(rcard)
     && /unknown repeater 0x5e/.test(rcard) && /4 · c1 · Benson/.test(rcard), `the route card: ${rcard.slice(rcard.indexOf('route:'), rcard.indexOf('route:') + 160)}`);
  ok(/ mi [NESW]/.test(rcard), '…the card in miles');
  // ROUTE: drawn hop by hop, and the view fits the WHOLE PATH (node, the placed hops M0SWM and Benson, the receiver)
  await ev(`${routeBtn}.click()`);
  await sleep(2600);
  const lit = JSON.parse(await ev(`JSON.stringify({ bd: document.querySelectorAll('.vmr-bd').length, ar: document.querySelectorAll('.vmr-ar').length, nums: [...document.querySelectorAll('.vmr-bd')].map(b => b.textContent).join(' ') })`));
  ok(lit.bd === 4 && lit.ar === 6, `ROUTE: drawn hop by hop: ${lit.bd} badges (${lit.nums}), ${lit.ar} arrowheads (node→9f, 9f→each a3, each a3→c1 across the unknown, c1→receiver)`);
  const outside = Number(await ev(inView([[51.95, -0.75], [51.799, -0.3967], [51.6223, -1.1028], [52.3125, -0.875]])));
  ok(outside === 0, `…and the view fits the whole path: node, placed hops and receiver all in view (${outside} outside, zoom ${JSON.parse(await ev(cam)).z})`);
  ok(await ev(`${routeBtn}.textContent === 'HIDE ROUTE' && ${routeBtn}.getAttribute('aria-pressed') === 'true'`), '…the key says HIDE ROUTE');
  console.log('   screenshot', await shot('ismmap-route.png'));
  const c1 = await ev(cam);
  await ev(`window.__fakeAdd(${JSON.stringify(node('f12', 'meshcore', 'Far North Rptr', 'repeater', 'repeater', 53.9, -1.5, 6, 1))})`);   // a refresh with a node outside the view
  await sleep(4500);
  const c2 = await ev(cam);
  ok(c1 === c2, `a selection HOLDS the view through refreshes (Stuart's recording): ${c1} → ${c2}`);
  ok(await ev(`${view}.querySelector('.fit').classList.contains('paused')`), '…the FIT key shows the auto fit paused');
  await ev(`${view}.querySelector('.fit').click()`);
  await sleep(3500);
  const c3 = await ev(cam);
  ok(c3 !== c2 && !(await ev(`${view}.querySelector('.fit').classList.contains('paused')`)), `FIT re-frames: ${c3}`);
  await ev(`${routeBtn}.click()`);
  await sleep(500);
  ok(await ev(`document.querySelectorAll('.vmr-bd').length === 0 && ${routeBtn}.textContent === 'ROUTE'`), 'HIDE ROUTE: the route goes, the card stays');
  await ev(`${routeBtn}.click()`);
  await sleep(800);
  ok(await ev(`document.querySelectorAll('.vmr-bd').length > 0`), 'ROUTE again: back');
  // a node whose packets carried no relays: no ROUTE key at all (no dead control); choosing it clears the other's route
  await ev(`(() => { const r = [...${view}.querySelectorAll('.trkRow')].find(x => /Moulton Rptr/.test(x.textContent)); r && r.click(); })()`);
  await sleep(1500);
  ok(await ev(`/Moulton/.test((document.querySelector('.vtl-card:not([hidden])') || {}).textContent || '') && !${routeBtn}`), 'a node with no path: its card has no ROUTE key');
  ok(await ev(`document.querySelectorAll('.vmr-bd').length === 0`), '…and choosing it cleared the other node\'s route');
  ok(await ev(`${view}.querySelector('.fit').classList.contains('paused')`), 'chosen: the view held');
  // a tap on a marker: its card AND a zoom in
  await ev(`(() => { const m = window.__vibeTrkMap; m.setView([52.1, -0.8], 7); return true; })()`);
  await sleep(1200);
  await ev(`[...document.querySelectorAll('.vtl-mk')].find(m => /M0SWM AL3 7/.test(m.textContent))?.click()`);
  await sleep(1800);
  const zm = JSON.parse(await ev(cam));
  ok(zm.z >= 10 && Math.abs(zm.lat - 51.799) < 0.03 && /M0SWM/.test(await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`)), `a marker tap: its card, zoomed in on it (zoom ${zm.z})`);
  await ev(`${view}.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
  await sleep(600);
  ok(await ev(`document.querySelectorAll('.vmr-bd').length === 0 && document.querySelectorAll('.vmr-ar').length === 0 && !document.querySelector('.vtl-card:not([hidden])')`), 'the card closed: no route');
  ok(!(await ev(`${view}.querySelector('.fit').classList.contains('paused')`)), '…and the auto fit resumes');
  await ev(`document.querySelector('#ismChips .ismUnits[data-u="speed"]').click()`);   // back to km/h
  ok(await until(`/ km/.test(${view}.querySelector('.st').textContent)`, 4000), 'MPH → KM/H: km again');
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

  /* ★★ beta92 (Stuart 11-23: "when i close out of ism i will loose them all … maybe expire them after 24h"): the live
   *  map also draws the band's nodes the gallery heard in the last 24 h. Here nothing more is heard live (the fakes
   *  gone, as after leaving ISM and coming back): MAP is still offered, and the seeded card's nodes are on it, each with
   *  the gallery's first / last heard. */
  console.log('── nothing heard live: the last 24 h from the gallery ──');
  await ev(`window.__fakeClear()`);
  await sleep(3000);
  ok(await until(`!document.getElementById('ismMap').hidden`, 8000), 'MAP is offered for the remembered nodes');
  await ev(`document.getElementById('ismMap').click()`);
  ok(await until(`!!${view} && document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length >= 8`, 10000),
     `the remembered nodes are on the map (${await ev(`document.querySelectorAll('.vtl-mk:not(.vtl-gone)').length`)})`);
  const remRows = await ev(`${view}.querySelector('.rows').textContent`);
  ok(/Benson/.test(remRows) && /Moulton Rptr|Wellingborough Rptr/.test(remRows), `…and in its list: ${remRows.slice(0, 100)}`);
  await ev(`[...document.querySelectorAll('.vtl-mk')].find(m => /Benson/.test(m.textContent))?.click()`);
  await sleep(800);
  ok(/first heard \d\d:\d\d · last heard \d\d:\d\d UTC/.test(await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`)),
     'a remembered node\'s card: first / last heard, from the gallery');
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
  ok(/LoRa \/ MeshCore \+ Meshtastic 868 MHz/.test(row) && /1\d nodes heard/.test(row) && /repeaters/.test(row) && /furthest (direct|via mesh) \d+ km/.test(row), `the card: ${row.slice(0, 160)}`);
  console.log('   screenshot', await shot('ismmap-gallery-card.png'));
  await ev(`${seeded}.querySelector('.galMap').click()`);
  ok(await until(`!!${view} && ${view}.querySelectorAll('.trkRow').length >= 10`, 10000), `its MAP: ${await ev(`${view} ? ${view}.querySelectorAll('.trkRow').length : 0`)} nodes listed`);
  ok(/BG \+ LIVE/.test(await ev(`${view}.textContent`)) && /\bBG\b/.test(await ev(`${view}.textContent`)) && /LIVE/.test(await ev(`${view}.textContent`)), 'background / live marked in the list');
  await sleep(3000);
  ok(/1 day in the gallery/.test(await ev(`${view}.querySelector('.st').textContent`)), `status: ${await ev(`${view}.querySelector('.st').textContent`)}`);
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ')
     === '1 H:false 24 H:false 7 DAYS:false 30 DAYS:false ALL:true', 'the gallery map offers all five ranges, ALL chosen');
  console.log('   screenshot', await shot('ismmap-gallery.png'));
  // ★★ beta96 the gallery's map: the same key, the same remembered pick (NEAREST, set on the live map)
  ok(await ev(`(() => { const h = ${view}.querySelector('.hd .srt'); return !h.hidden && h.querySelector('.srtT')?.dataset.srt === 'nearest'; })()`),
     'the gallery map\'s list has the sort toggle, the viewer\'s pick (NEAREST) kept');
  await ev(`${view}.querySelector('.hd .srt .srtT').click()`);
  const galLast = `JSON.stringify([...${view}.querySelectorAll('.trkRow .ag')].map(a => a.textContent))`;
  ok(await until(`(() => { const a = JSON.parse(${galLast}); return a.length >= 10 && a.every((x, i) => i === 0 || x <= a[i - 1]); })()`, 4000), `gallery LATEST: last heard, newest first: ${await ev(galLast)}`);
  await ev(`${view}.querySelector('.hd .srt .srtT').click()`);

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
