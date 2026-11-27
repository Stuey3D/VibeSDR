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
async function shutAll(p) { await p.ev(`[...(${V}?.querySelectorAll('[data-cbtn][aria-expanded="true"]') || [])].forEach(b => b.click()); 1`); await sleep(300); }
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
async function sizeChecks(p, tag, w, h, o) {
  const narrow = w < 600;
  // ── always on ──
  ok(await p.ev(`!!${V}?.querySelector('.trkTop .exit')?.getClientRects().length`), `${tag} ${w}: the way out is on screen (${await p.ev(`${V}?.querySelector('.trkTop .exit')?.textContent`)})`);
  if (o.radio) {
    ok(await p.until(`!!document.getElementById('mapShLine')?.getClientRects().length`, 6000), `${tag} ${w}: the shared tuner line (listeners / ask) is on screen: ${await p.ev(`document.getElementById('mapShLine')?.textContent`)}`);
    ok(await p.ev(`!!document.getElementById('mapChat')?.getClientRects().length`), `${tag} ${w}: CHAT is on screen`);
    const r = await p.ev(`${V}?.querySelector('.trkTop .rate')?.textContent || ''`);
    ok(/^VDL2 [\d.,]+ msg\/min$/.test(r), `${tag} ${w}: top right is only the message rate: "${r}"`);
    // ★★★ tweak 2 (Stuart 11-27): the bar is EXACTLY EXIT | Shared VFO: … n/max | CHAT | the rate — in that order
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
  ok(await p.ev(`!${V}.querySelector('.trkTop [data-cbtn]') && ${V}.querySelectorAll('.trkMapHost .mDock.edgeDock [data-cbtn]').length >= 4`), `${tag} ${w}: the chips are edge docks on the map (the health pill's), not in the bar`);
  // ★ the sides: TUNE / LAYERS on the left edge, SERVERS / MAP top right, LIST bottom right
  const sides = await p.ev(`JSON.stringify(Object.fromEntries([...${V}.querySelectorAll('.mDock:not([hidden])')].map(d => { const b = d.getBoundingClientRect(); return [d.dataset.dock, b.left < ${w} / 2 ? 'L' : 'R']; })))`);
  ok(/"layers":"L"/.test(sides) && /"map":"R"/.test(sides) && /"list":"R"/.test(sides) && /"servers":"R"/.test(sides) && (!/"tune"/.test(sides) || /"tune":"L"/.test(sides)), `${tag} ${w}: chips on their edges ${sides}`);
  // ★ the LAYERS chip: a glyph and a count per family; a slim column on a phone-width window
  const lay = JSON.parse(await p.ev(`JSON.stringify((() => { const es = [...${V}.querySelectorAll('.mDock[data-dock="layers"] .le')]; if (es.length < 1) return { n: 0 }; const a = es[0].getBoundingClientRect(), b = es[es.length - 1].getBoundingClientRect(); return { n: es.length, col: es.length > 1 && b.top > a.top + 4 && Math.abs(b.left - a.left) < 4, glyph: !!es[0].querySelector('svg') }; })())`));
  ok(lay.n >= 1 && lay.glyph, `${tag} ${w}: the LAYERS chip shows a glyph and a count per family (${lay.n})`);
  if (lay.n > 1) ok(narrow ? lay.col : !lay.col, `${tag} ${w}: LAYERS ${narrow ? 'stacks as a slim column' : 'stays a row'} on this window`);
  // ★ tuck and back, as the health pill: › tucks it to a tab on its edge, the tab brings it back
  await p.ev(`${V}.querySelector('.mDock[data-dock="map"] .hTuck').click(); 1`); await sleep(400);
  const tucked = await p.ev(`(() => { const d = ${V}.querySelector('.mDock[data-dock="map"]'); const t = d.querySelector('.hTab').getBoundingClientRect(); return d.classList.contains('tucked') && t.right >= ${w} - 1 && t.width > 10; })()`);
  ok(tucked, `${tag} ${w}: MAP tucks into a tab on the right edge`);
  await p.ev(`${V}.querySelector('.mDock[data-dock="map"] .hTab').click(); 1`); await sleep(400);
  ok(await p.ev(`!${V}.querySelector('.mDock[data-dock="map"]').classList.contains('tucked')`), `${tag} ${w}: …and its tab brings it back`);
  ok(await p.ev(`!${V}.querySelector('.trkTop .ttl').getClientRects().length`), `${tag} ${w}: no title in the bar`);
  ok(await p.ev(`(() => { const s = ${V}.querySelector('.trkTop .st'); const r = s.getBoundingClientRect(); return r.width <= 1 && r.height <= 1; })()`), `${tag} ${w}: the status paragraph is not on screen`);
  // ── the buttons ──
  const shown = JSON.parse(await p.ev(SHOWN));
  for (const id of o.want) ok(shown.includes(id), `${tag} ${w}: the ${id.toUpperCase()} button is there`);
  for (const id of o.not) ok(!shown.includes(id), `${tag} ${w}: no ${id.toUpperCase()} chip (nothing to tune)`);
  // ── no card: the map's share of the window ──
  await shutAll(p);
  const host = JSON.parse(await p.ev(`JSON.stringify(${V}.querySelector('.trkMapHost').getBoundingClientRect())`));
  const share = (host.width * host.height) / (w * h);
  ok(share >= 0.75, `${tag} ${w}: with no card open the map is ${(share * 100).toFixed(0)} % of the window (≥ 75 %)`);
  // ── each card opens and shuts ──
  for (const id of shown) {
    await p.ev(`${BTN(id)}.click(); 1`); await sleep(350);
    const vis = JSON.parse(await p.ev(VISIBLE_CARDS));
    ok(vis.includes(id) && await p.ev(`${BTN(id)}.getAttribute('aria-expanded') === 'true'`), `${tag} ${w}: ${id.toUpperCase()} opens its card`);
    if (narrow) ok(vis.length === 1, `${tag} ${w}: a phone-width window shows ONE card (${vis.join(',')})`);
    if (id === 'map') {
      ok(await p.ev(`!${V}.querySelector('.trkCard2[data-card="map"] .fitOnce').disabled && !${V}.querySelector('.trkCard2[data-card="map"] .fit').disabled`), `${tag} ${w}: MAP: AUTO and FIT are never greyed`);
      ok(/far outliers/.test(await p.ev(`${V}.querySelector('.fitOnce').title`) || ''), `${tag} ${w}: MAP: FIT says it leaves far outliers out`);
    }
    if (id === 'map' && o.multi) ok(await p.ev(`!${V}.querySelector('.srcSect').hidden && ${V}.querySelectorAll('.srcRow[data-src]').length >= 2`), `${tag} ${w}: MAP CONTROLS lists each source as a toggle (${await p.ev(`[...${V}.querySelectorAll('.srcRow b')].map(b => b.textContent).join(' · ')`)})`);
    if (id === 'map') ok(await p.ev(`!!${V}.querySelector('.trkCard2[data-card="map"] .keySect summary')`), `${tag} ${w}: the KEY is a fold in MAP CONTROLS`);
    if (id === 'servers') ok(await p.ev(`!!${V}.querySelector('.srvBody [data-srv="back"]')`), `${tag} ${w}: SERVERS has the way back (${await p.ev(`${V}.querySelector('.srvBody [data-srv="back"]')?.textContent`)})`);
    if (id === 'tune') ok(await p.ev(`${V}.querySelectorAll('.tunePop [data-tune]').length`) >= 2, `${tag} ${w}: TUNE holds the TUNE rows`);
    await p.ev(`${BTN(id)}.click(); 1`); await sleep(300);
    ok(!JSON.parse(await p.ev(VISIBLE_CARDS)).includes(id) && await p.ev(`${BTN(id)}.getAttribute('aria-expanded') === 'false'`), `${tag} ${w}: ${id.toUpperCase()} shuts again`);
  }
  // ── several at once: the window decides ──
  for (const id of shown) await p.ev(`${BTN(id)}.click(); 1`);
  await sleep(500);
  const vis = JSON.parse(await p.ev(VISIBLE_CARDS));
  const lim = await p.ev(`${V}.querySelectorAll('[data-cbtn]').length && (${w} < 600 ? 1 : null)`);
  if (narrow) ok(vis.length === 1 && vis[0] === shown[shown.length - 1], `${tag} ${w}: opening another shut the first — one card (${vis.join(',')})`);
  else {
    ok(vis.length >= 2, `${tag} ${w}: a wide window holds several cards (${vis.join(',')})`);
    // ★ tweak 3: cards beside the chips on both edges — what they cover across the map's width (each x column once)
    const cov = await p.ev(`(() => { const xs = new Set(); for (const x of ${V}.querySelectorAll('.trkCards .trkCard2')) { const b = x.getBoundingClientRect(); for (let i = Math.max(0, Math.floor(b.left)); i < Math.min(${w}, b.right); i++) xs.add(i); } return xs.size; })()`);
    ok(cov <= w * 0.8, `${tag} ${w}: the cards never cover the map's width (${cov} of ${w} px)`);
  }
  void lim;
  const sw = await p.ev(`document.documentElement.scrollWidth`);
  ok(sw <= w, `${tag} ${w}: no sideways scroll (${sw})`);
  // ── Escape shuts the top card, one at a time ──
  const before = JSON.parse(await p.ev(VISIBLE_CARDS)).length;
  await p.ev(`${V}.querySelector('.trkCards .trkCard2 .x')?.focus(); 1`);
  await p.key('Escape'); await sleep(350);
  const after = JSON.parse(await p.ev(VISIBLE_CARDS)).length;
  ok(after === before - 1 && await p.ev(`!!${V}`), `${tag} ${w}: Escape shut the top card (${before} → ${after}), the map stays`);
  await shutAll(p);
}

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
    for (const [w, h, m] of SIZES) { await p.size(w, h, m); await sleep(800); await sizeChecks(p, 'DATALINK', w, h, { radio: true, multi: true, want: ['list', 'tune', 'layers', 'map', 'servers'], not: [] }); }
    // ── the listener's CONTROL COLOURS (the web's skins: background, button, font, transparency — controlColours.ts) never
    //    reach the map's own chrome: the bar and the cards keep their palette, solid or glass ──
    const look = `JSON.stringify([...${V}.querySelectorAll('.trkTop .cbtn, .trkTop .exit, .trkTop .rate')].slice(0, 4).map(e => { const c = getComputedStyle(e); return c.backgroundColor + '|' + c.color; }))`;
    const look0 = await p.ev(look);
    await p.ev(`(() => { const r = document.documentElement.style; for (const [k, v] of [['--btn-bg', 'rgb(250,250,250)'], ['--btn-text', 'rgb(0,0,255)'], ['--ctl-card-bg', 'rgb(255,255,255)'], ['--btn-border', 'rgb(255,0,0)'], ['--ov-chip', 'rgb(0,255,0)']]) r.setProperty(k, v); return 1; })()`);
    await sleep(300);
    ok(await p.ev(look) === look0, 'the control colours (any skin, solid or glass) leave the map\'s bar and cards as they are');
    await p.ev(`(() => { const r = document.documentElement.style; for (const k of ['--btn-bg', '--btn-text', '--ctl-card-bg', '--btn-border', '--ov-chip']) r.removeProperty(k); return 1; })()`);
    // ── remembered per viewer: wide and narrow separately ──
    await p.size(1440, 900, false);
    await p.ev(`${BTN('map')}.click(); 1`); await sleep(300);
    const wide = await p.ev(`localStorage.getItem('vibesdr.mapCards.wide')`);
    ok(/"map"/.test(wide || ''), `the wide window's cards are remembered (${wide})`);
    await p.size(390, 844, true);
    ok(JSON.parse(await p.ev(VISIBLE_CARDS)).length === 0, 'a phone-width window starts with nothing open (its own memory)');
    await p.size(1440, 900, false);
    ok(JSON.parse(await p.ev(VISIBLE_CARDS)).includes('map'), '…and back on the wide window, the MAP card is open again');
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
    // ── the VIEWER from the door's landing: no TUNE, no rate ──
    await p.send('Page.navigate', { url: `http://127.0.0.1:${P}/` });
    ok(await p.until(`!!document.getElementById('splashAirSea') && !document.getElementById('splashAirSea').hidden`, 20000), 'the landing offers the AIR & SEA MAP');
    await p.ev(`document.getElementById('splashAirSea').click(); 1`);
    ok(await p.until(`!!${V}`, 10000), 'the viewer opens');
    await sleep(3000);
    for (const [w, h, m] of SIZES) { await p.size(w, h, m); await sleep(800); await sizeChecks(p, 'VIEWER', w, h, { radio: false, want: ['list', 'layers', 'map', 'servers'], not: ['tune'] }); }
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
