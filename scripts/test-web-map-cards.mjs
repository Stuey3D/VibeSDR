// test-web-map-cards.mjs — ★★★ beta96 THE MAP CONTROL CARDS (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md) in the REAL web page
// from REAL vibeservers, headless Edge with --mute-audio (tests are silent), at 390×844, 820×1180 and 1440×900:
//   A. a FRONT DOOR with two sources — a synthetic Beast feed (a local port, never a real receiver) and a fake-RTL radio
//      replaying a synthetic VDL2 capture in the DATALINK mode: the always-on items (EXIT, the SHARED TUNER line, CHAT, the
//      live rate top right "VDL2 … msg/min"), each card opens and shuts (LIST · TUNE · VIEW · MAP · KEY · SOURCES), a
//      phone-width window holds ONE card, wider ones several (never the whole map), SOURCES present (2 sources), Escape
//      shuts the top card, FIT never greyed, no sideways scroll, the map ≥ 75 % of the window with no card open, the
//      choice remembered (localStorage) per narrow / wide; the VIEWER from the door's landing: no TUNE, no rate.
//   B. a single radio, no feed, in the DATALINK mode: ONE source — no SOURCES card.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/v.cu8 node scripts/test-web-map-cards.mjs     (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_VDL2_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, CLIP, EDGE].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN, VIBE_VDL2_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const kids = [];
const dirs = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGKILL'); } catch {} } for (const d of dirs) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} } });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 420 s'); process.exit(1); }, 420_000);
const SIZES = [[390, 844, true], [820, 1180, true], [1440, 900, false]];

// ── a synthetic dump1090 (Beast): the Riddle's worked examples, re-sent with a rising counter (as test-server-aircraft-feeds) ──
const FRAMES = ['8D4840D6202CC371C32CE0576098', '8D40621D58C382D690C8AC2863A7', '8D40621D58C386435CC412692AD6'];
function beast(hex, ts) {
  const m = Buffer.from(hex, 'hex'); const raw = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((BigInt(ts) >> BigInt(8 * i)) & 0xffn));
  raw.push(0x40, ...m);
  const out = [0x1a, 0x33];
  for (const b of raw) { out.push(b); if (b === 0x1a) out.push(0x1a); }
  return Buffer.from(out);
}
function feeder(port) {
  let ts = 1000, k = 0;
  const socks = new Set();
  const srv = net.createServer((c) => {
    socks.add(c); c.on('error', () => {});
    const t = setInterval(() => { const b = []; for (let i = 0; i < 12; i++, k++) { ts += 12000 * 25; b.push(beast(FRAMES[k % FRAMES.length], ts)); } c.write(Buffer.concat(b)); }, 100);
    c.on('close', () => clearInterval(t));
  });
  srv.listen(port, '127.0.0.1');
  return { kill: () => { srv.close(); for (const c of socks) c.destroy(); } };
}
async function up(url) { for (let i = 0; i < 120; i++) { try { const r = await fetch(url); if (r.status < 500) return true; } catch {} await sleep(250); } return false; }
function mkdir(tag) { const d = fs.mkdtempSync(path.join(os.tmpdir(), `vs-mc-${tag}-`)); dirs.push(d); for (const x of ['data', 'run', 'edge']) fs.mkdirSync(path.join(d, x)); return d; }

// ── the page in headless Edge ──
async function browser(dir) {
  const dbg = await freePort();
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1440,1180', 'about:blank'], { stdio: 'ignore' });
  kids.push(edge);
  await sleep(2500);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  const ws = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description?.slice(0, 300) || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const size = async (w, h, mobile) => { await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile }); await sleep(1200); };
  const key = async (k) => { await send('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code: k, windowsVirtualKeyCode: k === 'Escape' ? 27 : 0 }); await send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k, windowsVirtualKeyCode: k === 'Escape' ? 27 : 0 }); };
  await send('Page.enable'); await send('Runtime.enable');
  return { send, ev, until, size, key, errs, close: () => { try { ws.close(); } catch {} edge.kill('SIGKILL'); } };
}
const V = `document.querySelector('.trkView:not([hidden])')`;
const BTN = (id) => `${V}?.querySelector('[data-cbtn="${id}"]')`;
const OPEN = `JSON.stringify([...(${V}?.querySelectorAll('[data-cbtn][aria-expanded="true"]') || [])].map(b => b.dataset.cbtn))`;
// ★★★ tweak 3: each chip is an edge dock (.mDock) — offered = its dock is not hidden
const SHOWN = `JSON.stringify([...(${V}?.querySelectorAll('.mDock') || [])].filter(d => !d.hidden).map(d => d.dataset.dock))`;
const VISIBLE_CARDS = `JSON.stringify([...(${V}?.querySelectorAll('.trkCards .trkCard2') || [])].filter(c => !c.hidden && c.getClientRects().length).map(c => c.dataset.card))`;
async function radioDatalink(p, url) {
  await p.send('Page.navigate', { url });
  await p.until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 20000);
  await p.ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await p.until(`!!document.querySelector('#audioGate button')`, 15000);
  await p.ev(`document.querySelector('#audioGate button')?.click(); 1`);
  await sleep(2500);
  await p.ev(`document.getElementById('mMode')?.click()`);
  await sleep(600);
  await p.ev(`localStorage.setItem('vibesdr.airSeaLast', 'av'); document.getElementById('mModeAirSea')?.click(); true`);
  return p.until(`!!${V}`, 10000);
}
/** The checks every size shares: the bar, each card, the window rule, Escape, scroll, the map's share. */
const TRANSIENT = ['servers', 'tune', 'layers', 'map', 'help'];
const SHOWN_T = `JSON.parse(${VISIBLE_CARDS}).filter(x => x !== 'list')`;
async function shutTransients(p) { await p.ev(`[...(${V}?.querySelectorAll('.trkCards .trkCard2') || [])].filter(c => c.dataset.card !== 'list').forEach(c => c.querySelector(':scope > .ch').click()); 1`); await sleep(300); }
/** The checks every size shares: the bar, each chip and its card, the window rule, Escape, scroll, the map's share. */
async function sizeChecks(p, tag, w, h, o) {
  const narrow = w < 600;
  // ── always on ──
  ok(await p.ev(`!!${V}?.querySelector('.trkTop .exit')?.getClientRects().length`), `${tag} ${w}: the way out is on screen (${await p.ev(`${V}?.querySelector('.trkTop .exit')?.textContent`)})`);
  if (o.radio) {
    ok(await p.until(`!!document.getElementById('mapShLine')?.getClientRects().length`, 6000), `${tag} ${w}: the shared tuner line (listeners / ask) is on screen: ${await p.ev(`document.getElementById('mapShLine')?.textContent`)}`);
    ok(await p.ev(`!!document.getElementById('mapChat')?.getClientRects().length`), `${tag} ${w}: CHAT is on screen`);
    const r = await p.ev(`${V}?.querySelector('.trkTop .rate')?.textContent || ''`);
    ok(/^VDL2 [\d.,]+ msg\/min$/.test(r), `${tag} ${w}: the incoming messages: "${r}"`);
    const order = await p.ev(`(() => { const bar = ${V}.querySelector('.trkTop'); const els = [bar.querySelector('.exit'), document.getElementById('mapShLine'), document.getElementById('mapChat'), bar.querySelector('.rate')];
      if (els.some(e => !e || !bar.contains(e) || !e.getClientRects().length)) return 'missing';
      for (let i = 1; i < els.length; i++) if (!(els[i - 1].compareDocumentPosition(els[i]) & Node.DOCUMENT_POSITION_FOLLOWING)) return 'order';
      const vis = [...bar.querySelectorAll('button, .rate, .ttl')].filter(e => e.getClientRects().length && !e.closest('[hidden]'));
      return vis.length === 4 ? 'ok' : 'extra: ' + vis.map(e => e.textContent.trim().slice(0, 20)).join(' | '); })()`);
    ok(order === 'ok', `${tag} ${w}: the bar is EXIT | the shared line | CHAT | the rate, nothing else (${order})`);
    ok(/^Shared VFO: .+ \d+\/\d+$/.test(await p.ev(`document.getElementById('mapShLine').textContent`)), `${tag} ${w}: the line reads "Shared VFO: … n/max" (${await p.ev(`document.getElementById('mapShLine').textContent`)})`);
  } else {
    ok(!(await p.ev(`${V}?.querySelector('.trkTop .rate')?.textContent || ''`)), `${tag} ${w}: the viewer shows no live rate (nothing live)`);
  }
  ok(await p.ev(`(() => { const s = ${V}.querySelector('.trkTop .st'); const r = s.getBoundingClientRect(); return r.width <= 1 && r.height <= 1; })()`), `${tag} ${w}: the status paragraph is not on screen`);
  ok(await p.ev(`!${V}.querySelector('.trkTop .ttl').getClientRects().length`), `${tag} ${w}: no title in the bar`);
  // ── the chips: edge docks (the health pill's), each its own glyph, on their edges ──
  const shown = JSON.parse(await p.ev(SHOWN));
  for (const id of o.want) ok(shown.includes(id), `${tag} ${w}: the ${id.toUpperCase()} chip is there`);
  for (const id of o.not) ok(!shown.includes(id), `${tag} ${w}: no ${id.toUpperCase()} chip (nothing to tune)`);
  ok(await p.ev(`!${V}.querySelector('.trkTop [data-cbtn]') && [...${V}.querySelectorAll('.trkMapHost .mDock.edgeDock [data-cbtn]')].every(b => !!b.querySelector('svg'))`), `${tag} ${w}: the chips are edge docks on the map, each with its own glyph`);
  const geo = JSON.parse(await p.ev(`JSON.stringify(Object.fromEntries([...${V}.querySelectorAll('.mDock:not([hidden])')].map(d => { const b = d.querySelector('.hCard').getBoundingClientRect(); return [d.dataset.dock, { l: Math.round(b.left), r: Math.round(b.right), t: Math.round(b.top), b: Math.round(b.bottom), w: Math.round(b.width) }]; })))`));
  const mapH = await p.ev(`${V}.querySelector('.trkMapHost').clientHeight`);
  ok(['servers', 'tune', 'layers'].filter((id) => geo[id]).every((id) => geo[id].l <= 1) && geo.list && geo.list.l <= 16, `${tag} ${w}: SERVERS, TUNE and LAYERS on the left edge, LIST at the bottom-left`);
  ok(geo.servers && Object.values(geo).every((g) => g.t >= geo.servers.t || g === geo.servers || g.l > w / 2), `${tag} ${w}: SERVERS is the top-left chip`);
  ok(geo.list && geo.list.b >= Object.values(geo).reduce((m, g) => Math.max(m, g.b), 0) - 1, `${tag} ${w}: LIST is the bottom-left chip`);
  const mapLeft = geo.map && geo.map.l <= 1;
  ok(mapLeft === (mapH >= 600), `${tag} ${w}: MAP CONTROLS ${mapLeft ? 'down the left (a tall map)' : 'top-right (a short map, flanking)'} — map ${mapH} px tall`);
  ok(Object.values(geo).every((g) => g.w <= 48), `${tag} ${w}: every chip is a slim side tab at rest (${Object.values(geo).map((g) => g.w).join(',')} px)`);
  const lay = JSON.parse(await p.ev(`JSON.stringify((() => { const es = [...${V}.querySelectorAll('.mDock[data-dock="layers"] .le')]; if (!es.length) return { n: 0 }; const a = es[0].getBoundingClientRect(), b = es[es.length - 1].getBoundingClientRect(); return { n: es.length, col: es.length < 2 || b.top > a.top + 4, glyph: !!es[0].querySelector('svg') }; })())`));
  ok(lay.n >= 1 && lay.glyph && lay.col, `${tag} ${w}: the LAYERS tab stacks a glyph over a count per family (${lay.n})`);
  // ── no card: the map's share of the window ──
  await shutAll(p);
  const host = JSON.parse(await p.ev(`JSON.stringify(${V}.querySelector('.trkMapHost').getBoundingClientRect())`));
  const share = (host.width * host.height) / (w * h);
  ok(share >= 0.75, `${tag} ${w}: with no card open the map is ${(share * 100).toFixed(0)} % of the window (≥ 75 %)`);
  // ── each chip: ONE press opens its card out of it; ONE press on its heading folds it back ──
  for (const id of [...shown, 'help']) {
    await p.ev(`${BTN(id)}.click(); 1`); await sleep(350);
    const vis = JSON.parse(await p.ev(VISIBLE_CARDS));
    ok(vis.includes(id) && await p.ev(`${BTN(id)}.getAttribute('aria-expanded') === 'true'`), `${tag} ${w}: one press on ${id.toUpperCase()} opens its card`);
    ok(JSON.parse(await p.ev(`JSON.stringify(${SHOWN_T})`)).length <= 1, `${tag} ${w}: one transient card at a time (${vis.join(',')})`);
    if (id === 'map') {
      ok(await p.ev(`!${V}.querySelector('.trkCard2[data-card="map"] .fitOnce').disabled && !${V}.querySelector('.trkCard2[data-card="map"] .fit').disabled`), `${tag} ${w}: MAP: AUTO and FIT are never greyed`);
      ok(/far outliers/.test(await p.ev(`${V}.querySelector('.fitOnce').title`) || ''), `${tag} ${w}: MAP: FIT says it leaves far outliers out`);
      ok(await p.ev(`!!${V}.querySelector('.trkCard2[data-card="map"] .keySect summary')`), `${tag} ${w}: the KEY is a fold in MAP CONTROLS`);
      if (o.multi) ok(await p.ev(`!${V}.querySelector('.srcSect').hidden && ${V}.querySelectorAll('.srcRow[data-src]').length >= 2`), `${tag} ${w}: MAP CONTROLS lists each source as a toggle (${await p.ev(`[...${V}.querySelectorAll('.srcRow b')].map(b => b.textContent).join(' · ')`)})`);
    }
    // ★ Stuart 11:59 ("the sources look like windows 95"): no visible control in a card wears the browser's default grey
    const plain = await p.ev(`[...${V}.querySelectorAll('.trkCard2[data-card="${id}"] button, .trkCard2[data-card="${id}"] select, .trkCard2[data-card="${id}"] input')].filter(b => b.getClientRects().length && !b.closest('[hidden]')).filter(b => { const m = getComputedStyle(b).backgroundColor.match(/\\d+/g) || []; const [r, g, bl] = m.map(Number); return r === g && g === bl && r >= 190; }).map(b => b.className || b.textContent.trim().slice(0, 12)).join(', ')`);
    ok(!plain, `${tag} ${w}: ${id.toUpperCase()}: no control in the browser's default grey${plain ? ` (${plain})` : ''}`);
    if (id === 'map' && o.multi) ok(await p.ev(`[...${V}.querySelectorAll('.srcBody [data-src]')].every(b => b.classList.contains('mRow'))`), `${tag} ${w}: the SOURCES rows are the cards' row (.mRow)`);
    if (id === 'tune') ok(await p.ev(`${V}.querySelectorAll('.tunePop [data-tune]').length`) >= 2, `${tag} ${w}: TUNE holds the TUNE rows`);
    if (id === 'servers') ok(await p.ev(`!!${V}.querySelector('.srvBody [data-srv="back"]')`), `${tag} ${w}: SERVERS has the way back (${await p.ev(`${V}.querySelector('.srvBody [data-srv="back"]')?.textContent`)})`);
    if (id === 'layers') ok(await p.ev(`${V}.querySelectorAll('.trkCard2[data-card="layers"] .tgl [data-tgl]').length`) >= 2, `${tag} ${w}: LAYERS lists the modes, a toggle row each`);
    if (id === 'help') ok(await p.ev(`${V}.querySelectorAll('.trkCard2[data-card="help"] .hRowH').length`) === 5, `${tag} ${w}: HELP has a line per chip`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="${id}"] > .ch').click(); 1`); await sleep(300);
    ok(!JSON.parse(await p.ev(VISIBLE_CARDS)).includes(id) && await p.ev(`${BTN(id)}.getAttribute('aria-expanded') === 'false'`), `${tag} ${w}: one press on its heading folds ${id.toUpperCase()} back to its tab`);
  }
  // ── the cards never cover the whole map; no sideways scroll ──
  await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(200); await p.ev(`${BTN('layers')}.click(); 1`); await sleep(400);
  const cov = await p.ev(`(() => { const xs = new Set(); for (const x of ${V}.querySelectorAll('.trkCards .trkCard2')) { const b = x.getBoundingClientRect(); for (let i = Math.max(0, Math.floor(b.left)); i < Math.min(${w}, b.right); i++) xs.add(i); } return xs.size; })()`);
  ok(cov <= w * (narrow ? 0.99 : 0.8), `${tag} ${w}: the cards never cover the map's width (${cov} of ${w} px)`);
  const sw = await p.ev(`document.documentElement.scrollWidth`);
  ok(sw <= w, `${tag} ${w}: no sideways scroll (${sw})`);
  // ── Escape folds the transient card; LIST (sticky) stays ──
  await p.ev(`${V}.querySelector('.trkCard2[data-card="layers"] .cb button')?.focus(); 1`);
  await p.key('Escape'); await sleep(350);
  const after = JSON.parse(await p.ev(VISIBLE_CARDS));
  ok(!after.includes('layers') && after.includes('list') && await p.ev(`!!${V}`), `${tag} ${w}: Escape folded LAYERS back; LIST stays (${after.join(',')})`);
  await shutAll(p);
}
async function shutAll(p) { await p.ev(`[...(${V}?.querySelectorAll('[data-cbtn][aria-expanded="true"]') || [])].forEach(b => b.click()); 1`); await sleep(300); }

console.log('── the MAP CONTROL CARDS (beta96, docs/v12/MAP-CONTROL-CARDS.md) ──');
// ═════ A. a front door: a Beast feed + a VDL2 radio ═════
{
  const dir = mkdir('door');
  const fport = await freePort();
  const feed = feeder(fport);
  let P = 0;
  for (let i = 0; i < 20 && !P; i++) { const p = await freePort(); const free = await new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(p + 1, '127.0.0.1', () => s.close(() => r(true))); }); if (free) P = p; }
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map cards test', locator: 'IO92nh', adminPass: 'map-cards-admin', port: P, fullMode: true,
    aircraftFeeds: JSON.stringify([{ fmt: 'beast', dir: 'connect', host: '127.0.0.1', port: fport, label: 'adsb', name: 'Test Beast' }]),
    radios: [{ serial: 'mc-test', driver: 'rtlsdr', label: 'VDL2 radio', enabled: true, configured: true, users: 4, rate: 2400000 }] }));
  let rtlPort = 0; while (!rtlPort || (rtlPort >= P && rtlPort <= P + 2)) rtlPort = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' }));
  // ★ a SHORT runtime dir: the radio's hand-off socket path must fit AF_UNIX's 104 bytes on macOS
  const run = fs.mkdtempSync('/tmp/vsmc-'); dirs.push(run);
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: run };
  kids.push(spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'door.log'), 'w'), 'ignore'] }));
  ok(await up(`http://127.0.0.1:${P}/vibeserver.json`), `the door is up (${P})`);
  const radios = await (await fetch(`http://127.0.0.1:${P}/vibeserver/radios`)).json();
  const rid = radios.radios?.[0]?.id;
  // ★ the radio process, as vibeserver@mc-test would start it; on a busy machine its first start can miss the door's
  //   hand-off (seen 2 runs in 4 on 11-27 with seven agents building) — one fresh start more before the test gives up
  let behind = false;
  for (let attempt = 0; attempt < 2 && !behind; attempt++) {
    const radio = spawn(BIN, ['--radio-serial', 'mc-test', '--tcp', `127.0.0.1:${rtlPort}`, '--freq', '96100000', '--mode', 'wfm'], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'radio.log'), 'w'), fs.openSync(path.join(dir, 'radio.log'), 'a')] });
    kids.push(radio);
    for (let i = 0; i < 3 && !behind; i++) behind = !!rid && await up(`http://127.0.0.1:${P}/r/${rid}/vibeserver.json`);
    if (!behind) { console.log(`   (radio start ${attempt + 1} not behind the door after 90 s — restarting it)`); radio.kill('SIGTERM'); await sleep(2000); }
  }
  ok(behind, `the radio is behind the door (${rid})`);
  if (!behind) console.log(fs.readFileSync(path.join(dir, 'radio.log'), 'utf8').split('\n').slice(-15).join('\n'));
  const p = await browser(dir);
  try {
    ok(await radioDatalink(p, `http://127.0.0.1:${P}/r/${rid}/`), 'the DATALINK map is up (the radio page, AIR & SEA LIVE)');
    ok(await p.until(`/msg\\/min/.test(${V}?.querySelector('.trkTop .rate')?.textContent || '')`, 30000), 'the datalink report came (the rate shows)');
    ok(await p.until(`!${V}?.querySelector('.srcSect')?.hidden`, 20000), 'two sources (the feed and the radio): SOURCES is offered in MAP CONTROLS');
    for (const [w, h, m] of SIZES) { await p.size(w, h, m); await sleep(800); await sizeChecks(p, 'DATALINK', w, h, { radio: true, multi: true, want: ['servers', 'tune', 'layers', 'map', 'list'], not: [] }); }
    // ── the listener's CONTROL COLOURS (the web's skins: background, button, font, transparency — controlColours.ts) never
    //    reach the map's own chrome: the bar and the cards keep their palette, solid or glass ──
    const look = `JSON.stringify([...${V}.querySelectorAll('.trkTop .cbtn, .trkTop .exit, .trkTop .rate')].slice(0, 4).map(e => { const c = getComputedStyle(e); return c.backgroundColor + '|' + c.color; }))`;
    const look0 = await p.ev(look);
    await p.ev(`(() => { const r = document.documentElement.style; for (const [k, v] of [['--btn-bg', 'rgb(250,250,250)'], ['--btn-text', 'rgb(0,0,255)'], ['--ctl-card-bg', 'rgb(255,255,255)'], ['--btn-border', 'rgb(255,0,0)'], ['--ov-chip', 'rgb(0,255,0)']]) r.setProperty(k, v); return 1; })()`);
    await sleep(300);
    ok(await p.ev(look) === look0, 'the control colours (any skin, solid or glass) leave the map\'s bar and cards as they are');
    await p.ev(`(() => { const r = document.documentElement.style; for (const k of ['--btn-bg', '--btn-text', '--ctl-card-bg', '--btn-border', '--ov-chip']) r.removeProperty(k); return 1; })()`);
    // ── only LIST is remembered (per narrow / wide); the transient cards always start folded ──
    await p.size(1440, 900, false); await shutAll(p);
    await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(200); await p.ev(`${BTN('map')}.click(); 1`); await sleep(300);
    const wide = await p.ev(`localStorage.getItem('vibesdr.mapCards.wide')`);
    ok(wide === '["list"]', `the wide window remembers LIST only (${wide})`);
    await p.size(390, 844, true);
    ok(!JSON.parse(await p.ev(VISIBLE_CARDS)).includes('list'), 'a phone-width window has its own memory (LIST shut there)');
    await p.size(1440, 900, false);
    ok(JSON.parse(await p.ev(VISIBLE_CARDS)).includes('list'), '…and back on the wide window, LIST is open again');
    // ── LIST stays put through the data ticks: the same node, the same scroll, no pop-out again ──
    await shutTransients(p);
    await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'); c.__mark = 'kept'; window.__anim = 0; c.addEventListener('animationstart', () => { window.__anim++; }); const cb = c.querySelector('.cb'); cb.scrollTop = 0; const rows = c.querySelector('.rows'); rows.scrollTop = 30; return 1; })()`);
    const st0 = await p.ev(`${V}.querySelector('.trkCard2[data-card="list"] .rows').scrollTop`);
    const lh = `(() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'); return Math.round(c.getBoundingClientRect().height) + ':' + c.querySelectorAll('.trkRow').length; })()`;
    const hs = [];
    for (let i = 0; i < 4; i++) { hs.push(await p.ev(lh)); await sleep(2500); }   // ★ the datalink report every second, aircraft.json every 3 s: well over three ticks
    ok(new Set(hs.map((x) => x.split(':')[0])).size === 1, `LIST keeps one height through the ticks (height:rows ${hs.join(' ')})`);
    ok(await p.ev(`/auto|scroll/.test(getComputedStyle(${V}.querySelector('.trkCard2[data-card="list"] .rows')).overflowY)`), 'only its rows scroll');
    ok(await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect(); const a = ${V}.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution'); return !a || c.bottom <= a.getBoundingClientRect().top + 1 || c.right <= a.getBoundingClientRect().left; })()`), 'its bottom is above the attribution line');
    ok(await p.ev(`${V}.querySelector('.trkCard2[data-card="list"]').__mark === 'kept' && window.__anim === 0`), `LIST through ten seconds of data: the same node, no pop-out again (${await p.ev('window.__anim')} animations)`);
    ok(await p.ev(`${V}.querySelector('.trkCard2[data-card="list"] .rows').scrollTop`) === st0, `…and the same scroll (${st0})`);
    // ── AUTO on, LIST open: the fit keeps every fitted aircraft clear of the LIST (Stuart: "the map should shift over") ──
    await p.ev(`(() => { const b = ${V}.querySelector('.trkCard2[data-card="map"] .fit'); if (!b.classList.contains('on')) b.click(); return 1; })()`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="map"] .fitOnce').click(); 1`);
    await sleep(3500);   // ★ the glide (2.2 s) and a tick
    const clear = JSON.parse(await p.ev(`JSON.stringify((() => { const v = window.__vibeTrkView; const { pts, map } = v.fitPointsForTest(); if (!map || !pts.length) return { n: 0, under: 0 };
      const l = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect(), h = ${V}.querySelector('.trkMapHost').getBoundingClientRect();
      let under = 0; for (const [lat, lon] of pts) { const q = map.latLngToContainerPoint([lat, lon]); const x = h.left + q.x, y = h.top + q.y; if (x > l.left && x < l.right && y > l.top && y < l.bottom) under++; }
      return { n: pts.length, under }; })())`));
    ok(clear.n > 0 && clear.under === 0, `AUTO on with LIST open: none of the ${clear.n} fitted points is under the LIST (${clear.under} under)`);
    // ── on a small phone: LAYERS opens OVER the list, with a shadow; folding it back leaves the list as it was ──
    await p.size(375, 550, true); await sleep(600);
    await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(400);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="list"]').__mark = 'phone'; 1`);
    await p.ev(`${BTN('layers')}.click(); 1`); await sleep(400);
    const over = JSON.parse(await p.ev(`JSON.stringify((() => { const l = ${V}.querySelector('.trkCard2[data-card="list"]'), y = ${V}.querySelector('.trkCard2[data-card="layers"]');
      const z = (el) => Number(getComputedStyle(el.parentElement).zIndex) || 0;
      return { list: !l.hidden && !!l.getClientRects().length, layers: !y.hidden, zl: z(l), zy: z(y), shadow: getComputedStyle(y).boxShadow }; })())`));
    ok(over.list && over.layers && over.zy > over.zl && /rgba?\(/.test(over.shadow), `375×550: LAYERS over the open LIST (z ${over.zy} > ${over.zl}) with a shadow`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="layers"] > .ch').click(); 1`); await sleep(400);
    ok(await p.ev(`${V}.querySelector('.trkCard2[data-card="list"]').__mark === 'phone' && !${V}.querySelector('.trkCard2[data-card="list"]').hidden`), '…folding LAYERS back leaves the same LIST open');
    // ── SERVERS over a full-height LIST: topmost where it is, with a shadow ──
    await p.size(1440, 900, false); await sleep(600);
    await p.ev(`${BTN('servers')}.click(); 1`); await sleep(500);
    const top = await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="servers"]'); const b = c.getBoundingClientRect(); const at = document.elementFromPoint(b.left + b.width / 2, b.top + b.height / 2); return !!at && c.contains(at) && getComputedStyle(c).boxShadow !== 'none'; })()`);
    ok(top, 'with LIST open, SERVERS is the topmost thing where it stands, with a shadow');
    // ★ its size and place are its own — the same with LIST open and shut; its "Back to…" row is the thing under the pointer
    const srv = async () => JSON.parse(await p.ev(`JSON.stringify((() => { const c = ${V}.querySelector('.trkCard2[data-card="servers"]'); const b = c.getBoundingClientRect(); const r = c.querySelector('[data-srv="back"]').getBoundingClientRect(); const at = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2); return { h: Math.round(b.height), t: Math.round(b.top), l: Math.round(b.left), row: !!at && !!at.closest('[data-srv="back"]') }; })())`));
    const sOpen = await srv();
    await p.ev(`${BTN('list')}.click(); 1`); await sleep(400);
    await p.ev(`(() => { const b = ${BTN('servers')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(400);
    const sShut = await srv();
    ok(Math.abs(sOpen.h - sShut.h) <= 2 && Math.abs(sOpen.t - sShut.t) <= 2 && Math.abs(sOpen.l - sShut.l) <= 2, `SERVERS the same size and place with LIST open (${JSON.stringify(sOpen)}) and shut (${JSON.stringify(sShut)})`);
    ok(sOpen.row, '…and with LIST open, its "Back to…" row is what a press there hits');
    await p.ev(`${BTN('list')}.click(); 1`); await sleep(300);
    await p.ev(`(() => { const b = ${BTN('servers')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(400);
    // ★ the LIST stops below the left chips: no overlap with any of them
    const clash = await p.ev(`(() => { const l = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect(); return [...${V}.querySelectorAll('.mChipsL .mDock:not([hidden]) .hCard')].some(c => { const b = c.getBoundingClientRect(); return b.right > l.left && b.left < l.right && b.bottom > l.top && b.top < l.bottom; }); })()`);
    ok(!clash, 'the LIST never overlaps the left chips');
    // ── a tap on the map folds the transient card back; LIST stays ──
    await p.ev(`(() => { const h = ${V}.querySelector('.trkMapHost'); const b = h.getBoundingClientRect(); const t = document.elementFromPoint(b.left + b.width * 0.7, b.top + b.height * 0.5); t.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); return 1; })()`); await sleep(300);
    const vis2 = JSON.parse(await p.ev(VISIBLE_CARDS));
    ok(!vis2.includes('servers') && vis2.includes('list'), `a tap on the map folds SERVERS back, LIST stays (${vis2.join(',')})`);
    // ── idle: a transient card folds back by itself (9 s untouched) ──
    await p.ev(`${BTN('map')}.click(); 1`); await sleep(10500);
    ok(!JSON.parse(await p.ev(VISIBLE_CARDS)).includes('map'), 'MAP CONTROLS folds back by itself after 9 s untouched');
    // ── one side or flanking, by the map's own height (Stuart's phones) ──
    for (const [ww, hh, want] of [[375, 667, 'R'], [375, 550, 'R'], [430, 932, 'L'], [440, 760, 'L'], [932, 430, 'R'], [956, 400, 'R']]) {
      await p.size(ww, hh, true); await sleep(700);
      const side = await p.ev(`(() => { const b = ${V}.querySelector('.mDock[data-dock="map"] .hCard').getBoundingClientRect(); return b.left <= 1 ? 'L' : 'R'; })()`);
      ok(side === want, `${ww}×${hh}: MAP CONTROLS ${want === 'L' ? 'down the left with the others' : 'top-right (flanking)'} — map ${await p.ev(`${V}.querySelector('.trkMapHost').clientHeight`)} px tall`);
    }
    await p.size(1440, 900, false);
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
    // ── the VIEWER from the door's landing: no TUNE, no rate ──
    await p.send('Page.navigate', { url: `http://127.0.0.1:${P}/` });
    ok(await p.until(`!!document.getElementById('splashAirSea') && !document.getElementById('splashAirSea').hidden`, 20000), 'the landing offers the AIR & SEA MAP');
    await p.ev(`document.getElementById('splashAirSea').click(); 1`);
    ok(await p.until(`!!${V}`, 10000), 'the viewer opens');
    await sleep(3000);
    for (const [w, h, m] of SIZES) { await p.size(w, h, m); await sleep(800); await sizeChecks(p, 'VIEWER', w, h, { radio: false, want: ['servers', 'layers', 'map', 'list'], not: ['tune'] }); }
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); feed.kill(); }
}
// ═════ B. one radio, no feed: ONE source → no SOURCES ═════
{
  const dir = mkdir('one');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map cards single', locator: 'IO92nh', adminPass: 'map-cards-admin',
    radios: [{ serial: 'mc-one', label: 'Only radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' }));
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  kids.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] }));
  ok(await up(`http://127.0.0.1:${port}/vibeserver.json`), 'the single radio is up');
  const p = await browser(dir);
  try {
    ok(await radioDatalink(p, `http://127.0.0.1:${port}/`), 'the DATALINK map is up');
    ok(await p.until(`/msg\\/min/.test(${V}?.querySelector('.trkTop .rate')?.textContent || '')`, 30000), 'the datalink report came');
    await sleep(4000);   // ★ aircraft.json polled (every 3 s): long enough for a second source to show if there were one
    await p.size(1440, 900, false);
    ok(await p.ev(`${V}.querySelector('.srcSect').hidden`), `one source: no SOURCES section (${await p.ev(`${V}.querySelectorAll('.srcRow').length`)} rows)`);
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); }
}
clearTimeout(watchdog);
console.log(`test-web-map-cards: ${pass} passed, ${fail} failed`);
for (const k of kids) { try { k.kill('SIGTERM'); } catch {} }
await sleep(500);
process.exit(fail ? 1 : 0);
