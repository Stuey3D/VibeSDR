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
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 1800 s'); process.exit(1); }, 1_800_000);   // ★ the suite grew with the map work (widths, rows, stacks, pulses)
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
    /* ★★★ NO TOP BAR (Stuart 12:50–13:06): the X a corner piece top-left, the pill top-centre, RATE top-right, CHAT under
     *  it, the zoom under them — no header taking the map's height, nothing overlapping */
    const L = JSON.parse(await p.ev(`JSON.stringify((() => { const r = (e) => { if (!e || !e.getClientRects().length || e.closest('[hidden]')) return null; const b = e.getBoundingClientRect(); return { l: b.left, t: b.top, r: b.right, b: b.bottom }; };
      const m = ${V}.querySelector('.trkMapHost').getBoundingClientRect();
      return { map: { t: m.top, h: m.height }, vh: innerHeight, x: r(${V}.querySelector('.trkTop .exit')), pill: r(document.getElementById('mapShLine')), rate: r(${V}.querySelector('.mDock[data-dock="rate"] .hCard')),
        chat: r(${V}.querySelector('.mDock[data-dock="chat"] .hCard')), zoom: r(${V}.querySelector('.maplibregl-ctrl-top-left, .leaflet-top.leaflet-left')), servers: r(${V}.querySelector('.mDock[data-dock="servers"] .hCard')),
        rateTxt: ${V}.querySelector('.mDock[data-dock="rate"] .ru')?.textContent || '', rateName: ${V}.querySelector('.mDock[data-dock="rate"] .rn')?.textContent || '', rateStatic: ${V}.querySelector('.mDock[data-dock="rate"]').classList.contains('static') }; })())`));
    ok(L.map.t <= 1 && Math.abs(L.map.h - L.vh) <= 1, `${tag} ${w}: no header — the map runs the full height (${L.map.t}, ${L.map.h} of ${L.vh})`);
    ok(L.x && L.x.l <= 1 && L.x.t <= 1 && L.x.r - L.x.l >= 44, `${tag} ${w}: the X is the top-left corner piece, ≥ 44 px (${JSON.stringify(L.x)})`);
    ok(L.servers && L.x && L.servers.t >= L.x.b + 8, `${tag} ${w}: SERVERS under the X with a gap`);
    ok(L.rate && L.rate.t <= 1 && L.rate.r >= w - 1 && /\/min$/.test(L.rateTxt) && (narrow || /VDL2/.test(L.rateName)), `${tag} ${w}: RATE the top-right corner piece — THIS radio's mode first (${L.rateName}${L.rateTxt})`);
    ok(L.chat && L.rate && L.chat.t >= L.rate.b - 1, `${tag} ${w}: CHAT under RATE`);
    const hit = (a, b) => a && b && a.l < b.r - 1 && a.r > b.l + 1 && a.t < b.b - 1 && a.b > b.t + 1;
    ok(!hit(L.pill, L.x) && !hit(L.pill, L.rate) && !hit(L.pill, L.chat) && !hit(L.zoom, L.rate) && !hit(L.zoom, L.chat) && !hit(L.zoom, L.pill),
       `${tag} ${w}: the pill, the X, RATE, CHAT and the zoom never overlap`);
    // ★ Stuart 14:42: the bar wears the control deck's material — a skin put on #mcard reaches it; one line, never wrapped
    { const bg0 = await p.ev(`getComputedStyle(document.getElementById('mapShLine')).backgroundColor`);
      await p.ev(`(() => { const m = document.getElementById('mcard'); m.style.setProperty('--ctl-card-bg', 'rgb(186, 190, 196)'); m.style.setProperty('--bar-border', 'rgb(120, 124, 130)'); window.__vibeMapDeckTest = 1; return 1; })()`);
      await p.until(`getComputedStyle(document.getElementById('mapShLine')).backgroundColor === 'rgb(186, 190, 196)'`, 8000);
      const sk = JSON.parse(await p.ev(`JSON.stringify((() => { const e = document.getElementById('mapShLine'), cs = getComputedStyle(e); return { bg: cs.backgroundColor, rim: cs.borderLeftColor, top: Math.round(e.getBoundingClientRect().top), lines: Math.round(e.getBoundingClientRect().height / parseFloat(cs.lineHeight || '14')) }; })())`));
      ok(sk.bg === 'rgb(186, 190, 196)' && sk.rim === 'rgb(120, 124, 130)', `${tag} ${w}: a silver deck look reaches the shared-tuner bar (${sk.bg}, rim ${sk.rim}; was ${bg0})`);
      ok(sk.top <= 1, `${tag} ${w}: the bar is attached to the top edge (${sk.top})`);
      await p.ev(`(() => { const m = document.getElementById('mcard'); m.style.removeProperty('--ctl-card-bg'); m.style.removeProperty('--bar-border'); return 1; })()`); }
    // CHAT: its chip opens the chat card (the canned replies); the pill opens it too
    await p.ev(`${V}.querySelector('[data-cbtn="chat"]').click(); 1`); await sleep(500);
    ok(await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="chat"]'); return !!c && !c.hidden && !!c.querySelector('#chatPhrases') && !!c.querySelector('#chatLog'); })()`), `${tag} ${w}: CHAT opens its card — the log and the canned replies`);
    await shutTransients(p);
    ok(await p.ev(`!!document.getElementById('chatPanel')?.contains(document.getElementById('chatPhrases'))`), `${tag} ${w}: …folded, the chat's nodes are home again`);
    // ★ Stuart 14:59: the locked ✕'s nudge speaks in the bar's own width — "leaving", never under the ✕ or RATE
    { await p.ev(`window.__vibeMapNudge('Ask in the chat before leaving'); 1`); await sleep(400);
      const nz = JSON.parse(await p.ev(`JSON.stringify((() => { const r = (e) => { const b = e?.getBoundingClientRect(); return b && b.width ? b : null; }; const bar = r(document.getElementById('mapShLine')), x = r(${V}.querySelector('.trkTop .exit')), rt = r(${V}.querySelector('.mDock[data-dock="rate"]:not([hidden]) .hCard'));
        const hit = (a, c) => a && c && a.left < c.right - 1 && a.right > c.left + 1 && a.top < c.bottom - 1 && a.bottom > c.top + 1; const e = document.getElementById('mapShLine');
        const rg = document.createRange(); rg.selectNodeContents(e); const tb = rg.getBoundingClientRect(), cs = getComputedStyle(e), inner = e.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
        // ★ the words' own box against the line's content box (scrollWidth over-reports this padded, letter-spaced pill by ~24 px)
        return { text: e.textContent, clear: !hit(bar, x) && !hit(bar, rt), oneLine: tb.width <= inner + 1 && tb.height < 30 }; })())`));
      ok(/^ASK IN CHAT( BEFORE LEAVING)?$/.test(nz.text) && nz.clear && nz.oneLine, `${tag} ${w}: the locked ✕'s nudge fits the bar ("${nz.text}", clear of the ✕ and RATE)`);
      await sleep(2800); }
    // ★ a press on a locked control points at where to ask: the bar and CHAT pulse for ~2.5 s, then nothing runs (Stuart 14:46)
    await p.ev(`window.__vibeMapAskPulse(); 1`); await sleep(200);
    const pz = JSON.parse(await p.ev(`JSON.stringify({ bar: !!document.querySelector('#mapShLine.askPulse'), chat: !!document.querySelector('.trkView:not([hidden]) .mDock[data-dock="chat"] .hCard.askPulse') || !document.querySelector('.trkView:not([hidden]) .mDock[data-dock="chat"]:not([hidden])'), run: document.getAnimations().filter(a => a.playState === 'running').length })`));
    ok(pz.bar && pz.chat && pz.run > 0, `${tag} ${w}: a locked press pulses the shared-tuner bar and CHAT (${JSON.stringify(pz)})`);
    await sleep(4000);
    ok(await p.ev(`document.getAnimations().filter(a => a.playState === 'running' && /^trkAsk/.test(a.animationName || '')).length`) === 0, `${tag} ${w}: …and 4 s later the pulse has stopped (no trkAsk animation running)`);
    ok(/^Shared VFO(: .+| · \w+ ·) \d+\/\d+$/.test(await p.ev(`document.getElementById('mapShLine').textContent`)), `${tag} ${w}: the line reads "Shared VFO: … n/max" (${await p.ev(`document.getElementById('mapShLine').textContent`)})`);
  } else {
    ok(!(await p.ev(`${V}?.querySelector('.trkTop .rate')?.textContent || ''`)), `${tag} ${w}: the viewer shows no live rate (nothing live)`);
    ok(await p.ev(`(() => { const d = ${V}.querySelector('.mDock[data-dock="rate"]'); return d.hidden || /\/s$|\/min$/.test(d.querySelector('.ru').textContent); })()`), `${tag} ${w}: RATE shows a rate or nothing — never a count`);
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
  const sideTabs = Object.entries(geo).filter(([id]) => !['rate', 'list', 'scale'].includes(id)).map(([, g]) => g);
  ok(sideTabs.every((g) => g.w <= 48), `${tag} ${w}: every edge chip is a slim side tab at rest (${Object.entries(geo).filter(([id]) => !['rate', 'list', 'scale'].includes(id)).map(([id, g]) => id + ' ' + g.w).join(', ')} px) — RATE and LIST are corner pieces`);
  const lay = JSON.parse(await p.ev(`JSON.stringify((() => { const es = [...${V}.querySelectorAll('.mDock[data-dock="layers"] .le')]; if (!es.length) return { n: 0 }; const a = es[0].getBoundingClientRect(), b = es[es.length - 1].getBoundingClientRect(); return { n: es.length, col: es.length < 2 || b.top > a.top + 4, glyph: !!es[0].querySelector('svg') }; })())`));
  ok(lay.n >= 1 && lay.glyph && lay.col, `${tag} ${w}: the LAYERS tab stacks a glyph over a count per family (${lay.n})`);
  // ── no card: the map's share of the window ──
  await shutAll(p);
  const host = JSON.parse(await p.ev(`JSON.stringify(${V}.querySelector('.trkMapHost').getBoundingClientRect())`));
  const share = (host.width * host.height) / (w * h);
  ok(share >= 0.75, `${tag} ${w}: with no card open the map is ${(share * 100).toFixed(0)} % of the window (≥ 75 %)`);
  // ── each chip: ONE press opens its card out of it; ONE press on its heading folds it back ──
  const rateStatic = await p.ev(`!!${V}.querySelector('.mDock[data-dock="rate"].static')`);
  if (shown.includes('rate') && rateStatic) ok(await p.ev(`${BTN('rate')}.getAttribute('aria-disabled') === 'true' && !${BTN('rate')}.hasAttribute('aria-expanded')`), `${tag} ${w}: one source — RATE is a reading, not a button`);
  for (const id of [...shown.filter((x) => x !== 'scale' && x !== 'chat' && !(x === 'rate' && rateStatic)), 'help']) {
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
    if (id === 'tune' && o.radio) {   // ★ Stuart 15:01: ONE gain row — the radio's own AGC name, or the slider; never both
      const st = () => p.ev(`JSON.stringify((() => { const r = ${V}.querySelector('.trkCard2[data-card="tune"] .mapGainHost .gRow'); if (!r) return null; const a = r.querySelector('.gAgc');
        return { agc: a ? a.textContent : null, on: !!a && a.classList.contains('on'), slider: !!r.querySelector('.gSl'), val: r.querySelector('.gVal')?.textContent || '', rows: r.closest('.mapGainHost').querySelectorAll('.gRow').length, more: !!r.querySelector('.gMoreB') }; })())`);
      const g0 = JSON.parse(await st());
      ok(g0 && /AGC/i.test(g0.agc || '') && g0.rows === 1 && /dB/.test(g0.val) && g0.slider === !g0.on, `${tag} ${w}: TUNE's gain is one row — the radio's own AGC name (${g0 && g0.agc}), ${g0 && (g0.on ? 'lit, the gain it chose' : 'the slider')} (${g0 && g0.val})`);
      await p.ev(`${V}.querySelector('.mapGainHost .gAgc').click(); 1`); await sleep(500);
      const g1 = JSON.parse(await st());
      ok(g1 && g1.on !== g0.on && g1.slider === !g1.on && g1.rows === 1, `${tag} ${w}: its AGC button swaps the row (${JSON.stringify(g1)})`);
      await p.ev(`${V}.querySelector('.mapGainHost .gAgc').click(); 1`); await sleep(500);
      const hasExtra = await p.ev(`!!${V}.querySelector('.mapGainHost .gMore') && [...${V}.querySelector('.mapGainHost .gMore').children].some(c => !c.hidden)`);
      ok(g1.more === hasExtra && hasExtra, `${tag} ${w}: an RTL has more controls (tuner bandwidth, PPM, bias-T …) behind ⌄ (${hasExtra})`);
      if (g1.more) { await p.ev(`${V}.querySelector('.mapGainHost .gMoreB').click(); 1`); await sleep(400);
        ok(await p.ev(`!${V}.querySelector('.mapGainHost .gMore').hidden && !!${V}.querySelector('.mapGainHost .gMore #tunerBw, .mapGainHost .gMore #ppm')`), `${tag} ${w}: ⌄ opens them`);
        await p.ev(`${V}.querySelector('.mapGainHost .gMoreB').click(); 1`); await sleep(300); }
    }
    if (id === 'servers') ok(await p.ev(`!!${V}.querySelector('.srvBody [data-srv="back"]')`), `${tag} ${w}: SERVERS has the way back (${await p.ev(`${V}.querySelector('.srvBody [data-srv="back"]')?.textContent`)})`);
    if (id === 'layers') ok(await p.ev(`${V}.querySelectorAll('.trkCard2[data-card="layers"] .tgl [data-tgl]').length`) >= 2, `${tag} ${w}: LAYERS lists the modes, a toggle row each`);
    if (id === 'layers') {   // ★ Stuart 15:24: no red on anything that is not an error — a live layer wears the LIVE tag
      const red = await p.ev(`[...${V}.querySelectorAll('.trkCard2[data-card="layers"] [data-tgl]')].filter(b => { const isRed = (c) => { const m = (c || '').match(/\\d+/g) || []; const [r, g, bl] = m.map(Number); return r > 200 && g < 120 && bl < 120; }; const cs = getComputedStyle(b), bf = getComputedStyle(b, '::before');
        return isRed(cs.borderBottomColor) && cs.borderBottomWidth !== '0px' || /#?ff[0-9a-f]{2}[0-9a-f]{2}|rgb\\(2[0-5]\\d, \\d{1,2}, /.test(cs.boxShadow) && /inset/.test(cs.boxShadow) || (bf.content !== 'none' && isRed(bf.backgroundColor)); }).map(b => b.dataset.tgl).join(',')`);
      ok(!red, `${tag} ${w}: LAYERS rows carry no red mark${red ? ' — ' + red : ''}`);
      const lv = JSON.parse(await p.ev(`JSON.stringify((() => { const rows = [...${V}.querySelectorAll('.trkCard2[data-card="layers"] [data-tgl]')]; const live = rows.filter(b => b.classList.contains('live')), mixed = live.length > 0 && live.length < rows.length;
        const any = rows.some(b => b.querySelector('.lv'));
        const okMine = rows.every(b => !b.querySelector('.lv:not(.o)') || b.classList.contains('live')) && (!any || live.every(b => !!b.querySelector('.lv:not(.o)')));
        const okOther = rows.every(b => !b.querySelector('.lv.o') || !b.classList.contains('live'));
        const small = [...document.querySelectorAll('.trkView:not([hidden]) .trkCard2[data-card="layers"] .lv')].every(e => e.getBoundingClientRect().width <= 48 && e.getBoundingClientRect().height <= 18);
        return { mixed, ok: okMine && okOther && small, tagged: rows.filter(b => b.querySelector('.lv')).map(b => b.dataset.tgl + (b.querySelector('.lv.o') ? '(other)' : '')) }; })())`));
      ok(lv.ok, `${tag} ${w}: LIVE filled only on this radio's layers, outlined only on another source's, all small (${lv.tagged.join(',') || 'none needed'})`);
    }
    if (id === 'layers' || id === 'map') {
      // ★ Stuart 12:00: the ON / OFF tags in one column — every row's count column (just before its tag, a fixed width) ends at one edge
      const edges = JSON.parse(await p.ev(`JSON.stringify([...${V}.querySelectorAll('.trkCard2[data-card="${id}"] [data-tgl] .n, .trkCard2[data-card="${id}"] [data-src] .n')].filter(n => n.getClientRects().length).map(n => Math.round(n.getBoundingClientRect().right)))`));
      if (edges.length > 1) ok(Math.max(...edges) - Math.min(...edges) <= 1, `${tag} ${w}: ${id.toUpperCase()}: the ON / OFF tags line up (${[...new Set(edges)].join(',')})`);
    }
    if (id === 'help') ok(await p.ev(`${V}.querySelectorAll('.trkCard2[data-card="help"] .hRowH').length`) === 8, `${tag} ${w}: HELP has a line per chip (SERVERS, TUNE, LAYERS, MAP, LIST, STATS, RATE, CHAT)`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="${id}"] > .ch').click(); 1`); await sleep(300);
    ok(!JSON.parse(await p.ev(VISIBLE_CARDS)).includes(id) && await p.ev(`${BTN(id)}.getAttribute('aria-expanded') === 'false'`), `${tag} ${w}: one press on its heading folds ${id.toUpperCase()} back to its tab`);
  }
  // ── the cards never cover the whole map; no sideways scroll ──
  await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(200); await p.ev(`${BTN('layers')}.click(); 1`); await sleep(400);
  const cov = await p.ev(`(() => { const xs = new Set(); for (const x of ${V}.querySelectorAll('.trkCards .trkCard2${narrow ? ':not([data-card="list"])' : ''}')) { const b = x.getBoundingClientRect(); for (let i = Math.max(0, Math.floor(b.left)); i < Math.min(${w}, b.right); i++) xs.add(i); } return xs.size; })()`);
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
    ok(await p.until(`!!${V}?.querySelector('.lSwapC') && !${V}.querySelector('.lSwapC').hidden`, 5000), 'a live map over a radio: the ≋ waterfall tab is offered');
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
    // ── the map's credits: shown as the map opens, folded to their (i) by 6 s, the (i) opens them again ──
    await p.ev(`(() => { window.__vibeAsReopen = 1; return 1; })()`);
    await p.ev(`document.querySelector('.trkView:not([hidden]) .trkTop .exit') && 1`);
    { const A = `${V}?.querySelector('.maplibregl-ctrl-attrib')`;
      if (await p.ev(`!!${A}`)) {
        // ★ Stuart 14:52: SERVER HEALTH is the radio page's own chip, lent to the map — it must look the same in both places
        const HS = `JSON.stringify((() => { const h = document.getElementById('srvHealth'); if (!h) return null; const c = h.querySelector('.hCard') || h; const cs = getComputedStyle(c); const r = c.getBoundingClientRect();
          return { inMap: !!h.closest('.trkView'), tucked: h.classList.contains('tucked'), w: Math.round(r.width), h: Math.round(r.height), bg: cs.backgroundColor, border: cs.borderTopColor + ' ' + cs.borderTopWidth, radius: cs.borderRadius, font: cs.fontFamily + ' ' + cs.fontSize, pad: cs.padding }; })())`;
        const hMap = JSON.parse(await p.ev(HS));
        // ★ Stuart 15:13: its tab brings the card out, its › puts it back — both ways, with real clicks at their centres
        if (hMap) {
          const clickAt = async (sel) => { const r = JSON.parse(await p.ev(`JSON.stringify((() => { const e = document.querySelector('${'$'}{sel}'); if (!e) return null; const q = e.getBoundingClientRect(); return [q.left + q.width / 2, q.top + q.height / 2]; })())`)); if (r) await p.ev(`document.elementFromPoint(${'$'}{r[0]}, ${'$'}{r[1]})?.click(); 1`); };
          await clickAt('.trkView:not([hidden]) #srvHealth .hTab'); await sleep(500);
          const outNow = await p.ev(`!document.getElementById('srvHealth').classList.contains('tucked')`);
          await clickAt('.trkView:not([hidden]) #srvHealth .hTuck'); await sleep(500);
          const backNow = await p.ev(`document.getElementById('srvHealth').classList.contains('tucked')`);
          ok(outNow && backNow, `SERVER HEALTH on the map: its tab brings the card out (${outNow}), its › tucks it again (${backNow})`);
        }
        await p.ev(`${V}.querySelector('.trkTop .exit').click(); 1`); await sleep(1500);
        const hPage = JSON.parse(await p.ev(HS));
        if (hMap && hPage) { const same = ['bg', 'border', 'radius', 'font', 'pad'].every((k) => hMap[k] === hPage[k]);
          ok(hMap.inMap && hMap.tucked && !hPage.inMap && same, `SERVER HEALTH: lent to the map as its tab (tucked: the map is centre stage), home on the page after, the same chip's look (${same ? 'identical' : JSON.stringify({ hMap, hPage })})`); }
        await p.ev(`document.getElementById('mMode')?.click()`); await sleep(500);
        await p.ev(`document.getElementById('mModeAirSea')?.click(); 1`);
        ok(await p.until(`!!${A} && ${A}.classList.contains('maplibregl-compact-show')`, 8000), 'the map credits are shown as the map opens');
        await sleep(6000);
        ok(await p.ev(`!${A}.classList.contains('maplibregl-compact-show')`), '…folded to their (i) by 6 s');
        await p.ev(`${A}.querySelector('.maplibregl-ctrl-attrib-button').click(); 1`); await sleep(300);
        ok(await p.ev(`${A}.classList.contains('maplibregl-compact-show')`), '…and the (i) opens them again');
      } else console.log('   (the GPU map is not in this browser — the credits check needs it)');
    }
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
    ok(await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect(); const a = ${V}.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution'); return !a || c.bottom <= a.getBoundingClientRect().top + 1 || c.right <= a.getBoundingClientRect().left || getComputedStyle(a).visibility === 'hidden'; })()`), 'its bottom is above the attribution line, or the (i) waits hidden under it');
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
    // ── the AUTO-zoom state on the MAP chip: on · off after a pan · paused while a card is open ──
    await shutTransients(p);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="map"] .fitOnce').click(); 1`); await sleep(2500);
    const Z = `${V}.querySelector('.mDock[data-dock="map"]').dataset.zoom`;
    ok(await p.until(`${Z} === 'on'`, 4000) && await p.ev(`!!${V}.querySelector('.mDock[data-dock="map"] .zst svg')`), `AUTO on: the MAP chip says so (${await p.ev(Z)})`);
    { const c = JSON.parse(await p.ev(`JSON.stringify((() => { const b = ${V}.querySelector('.trkMapHost').getBoundingClientRect(); return { x: b.left + b.width * 0.65, y: b.top + b.height * 0.45 }; })())`));
      await p.send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: c.x, y: c.y, deltaX: 0, deltaY: -240 }); await sleep(1500); }
    ok(await p.until(`${Z} === 'off'`, 4000) && await p.ev(`!!${V}.querySelector('.mDock[data-dock="map"] .zst .zNo')`), `a zoom by hand: AUTO off, the red no-entry sign (${await p.ev(Z)})`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="map"] .fitOnce').click(); 1`); await sleep(1500);
    await p.ev(`${BTN('layers')}.click(); 1`); await sleep(500);
    ok(await p.until(`${Z} === 'paused'`, 3000), `LAYERS open: AUTO paused (${await p.ev(Z)})`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="layers"] > .ch').click(); 1`); await sleep(500);
    ok(await p.until(`${Z} === 'on'`, 3000), `LAYERS folded: AUTO on again (${await p.ev(Z)})`);
    // ── an ITEM's card (an aircraft from the LIST) pauses AUTO too: the map holds still under it; folding it resumes ──
    await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(400);
    await p.ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]')?.click(); 1`); await sleep(800);
    const row = await p.ev(`(() => { const r = [...${V}.querySelectorAll('.trkCard2[data-card="list"] .trkRow')].find(x => !x.classList.contains('nopos')); if (!r) return ''; r.click(); return r.dataset.key || 'row'; })()`);
    if (row) {
      await sleep(2500);   // ★ the fly to it
      const cam = `(() => { const m = window.__vibeTrkView.fitPointsForTest().map; const c = m.getCenter(); return [c.lat.toFixed(4), c.lng.toFixed(4), m.getZoom().toFixed(2)].join(','); })()`;
      const c0 = await p.ev(cam); await sleep(7000); const c1 = await p.ev(cam);
      ok(await p.ev(`${Z} === 'paused'`) && c0 === c1, `an aircraft's card open: AUTO paused, the map still through the ticks (${c0} → ${c1}, ${await p.ev(Z)})`);
      await p.ev(`${V}.querySelector('.trkMapHost').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); 1`); await sleep(1200);
      ok(await p.until(`${Z} === 'on'`, 4000), `its card folded: AUTO on again (${await p.ev(Z)})`);
    } else console.log('   (no placed aircraft in the LIST to open — the item-card pause not checked)');
    // ── the SCALE chip: right edge, clear of the LIST, live with the zoom ──
    const sc = JSON.parse(await p.ev(`JSON.stringify((() => { const d = ${V}.querySelector('.mScale:not([hidden]) .hCard'); if (!d) return null; const b = d.getBoundingClientRect(); const l = ${V}.querySelector('.trkCard2[data-card="list"]'); const lb = l && !l.hidden ? l.getBoundingClientRect() : null;
      return { right: Math.round(b.right), w: innerWidth, clash: !!lb && b.left < lb.right && b.right > lb.left && b.top < lb.bottom && b.bottom > lb.top, text: d.textContent.trim() }; })())`));
    ok(!!sc && sc.right >= sc.w - 1 && !sc.clash, `the SCALE chip on the right edge, clear of the LIST (${sc && sc.text})`);
    await p.ev(`(document.querySelector('.trkView:not([hidden]) .maplibregl-ctrl-zoom-in, .trkView:not([hidden]) .leaflet-control-zoom-in') || {}).click?.(); 1`); await sleep(1500);
    const sc2 = await p.ev(`${V}.querySelector('.mScale .hCard').textContent.trim()`);
    ok(!!sc && sc2 !== sc.text, `…its reading follows the zoom (${sc && sc.text} → ${sc2})`);
    // ── 750×950: a card that would cover the LIST sends it aside (measured); the cards are opaque; it comes back as it was ──
    await p.size(750, 950, false); await sleep(700); await shutTransients(p);
    await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(500);
    await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); r.scrollTop = 40; window.__lst = r.scrollTop; return 1; })()`);
    await p.ev(`${BTN('layers')}.click(); 1`); await sleep(600);
    const aside = JSON.parse(await p.ev(`JSON.stringify((() => { const l = ${V}.querySelector('.trkCard2[data-card="list"]'), y = ${V}.querySelector('.trkCard2[data-card="layers"]');
      const bg = getComputedStyle(y).backgroundColor; const a = /rgba/.test(bg) ? Number(bg.split(',')[3]) : 1;
      const lr = l.getBoundingClientRect(), yr = y.getBoundingClientRect(), lh = l.offsetHeight, lt = l.parentElement.getBoundingClientRect().top + l.offsetTop;
      const over = yr.left < lr.right - 1 && yr.right > lr.left + 1 && yr.top < lt + lh - 1 && yr.bottom > lt + 1;
      return { over, aside: l.classList.contains('stepAside'), vis: getComputedStyle(l).visibility, alpha: a, chipDim: ${V}.querySelector('.mDock[data-dock="list"]').classList.contains('aside') }; })())`));
    ok(aside.over ? aside.aside && aside.vis === 'hidden' && aside.chipDim : !aside.aside && aside.vis === 'visible', `750×950: the LIST steps aside exactly when LAYERS would cover it (${JSON.stringify(aside)})`);
    ok(aside.alpha === 1, `the cards are opaque (alpha ${aside.alpha})`);
    await p.ev(`${V}.querySelector('.trkCard2[data-card="layers"] > .ch').click(); 1`); await sleep(500);
    ok(await p.ev(`(() => { const l = ${V}.querySelector('.trkCard2[data-card="list"]'); return !l.classList.contains('stepAside') && l.querySelector('.rows').scrollTop === window.__lst; })()`), '…LAYERS folded: the LIST back, the same scroll');
    await p.size(1440, 900, false); await sleep(700);
    await p.ev(`${BTN('layers')}.click(); 1`); await sleep(600);
    ok(await p.ev(`!${V}.querySelector('.trkCard2[data-card="list"]').classList.contains('stepAside')`), '1440×900: no overlap — the LIST stays');
    await shutTransients(p);
    // ── the LIST drawer flush to the map's left and bottom (Stuart 12:34: "no gap on mac but there is on iphone") ──
    for (const [ww, hh, mob] of [[1440, 900, false], [390, 844, true]]) {
      await p.size(ww, hh, mob); await sleep(700); await shutTransients(p);
      await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(600);
      const fl = JSON.parse(await p.ev(`JSON.stringify((() => { const h = ${V}.querySelector('.trkMapHost').getBoundingClientRect(), c = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect(); return { l: Math.round(c.left - h.left), b: Math.round(h.bottom - c.bottom), lb: parseFloat(${V}.style.getPropertyValue('--trk-list-bottom')) || 0 }; })())`));
      ok(fl.l === 0 && Math.abs(fl.b - fl.lb) <= 1, `${ww}×${hh}: the LIST drawer flush — left ${fl.l} px, bottom ${fl.b} px (the attribution's line ${fl.lb})`);
    }
    // ── whole rows follow the viewport (Stuart 12:38 / 12:40): 3 at 375×667, 3–4 at 390×844, ≥ 8 at 1440×900, never a part row ──
    const rowsAt = async (ww, hh, mob) => { await p.size(ww, hh, mob); await sleep(800); await shutTransients(p);
      await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(700);
      return JSON.parse(await p.ev(`JSON.stringify((() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); const h = r.querySelector('.trkRow')?.offsetHeight || 1; return { n: r.clientHeight / h, total: r.querySelectorAll('.trkRow').length }; })())`)); };
    const r1 = await rowsAt(375, 667, true), r2 = await rowsAt(390, 844, true), r3 = await rowsAt(1440, 900, false);
    ok(r1.n >= 2.95 && r1.n <= 4.05 && Math.abs(r1.n - Math.round(r1.n)) < 0.05, `375×667: 3–4 whole rows (${r1.n.toFixed(2)})`);
    ok(Math.abs(r2.n - 4) < 0.05, `390×844: 4 whole rows (${r2.n.toFixed(2)})`);
    ok(r3.n >= 3.95 && r3.n <= 7.05 && Math.abs(r3.n - Math.round(r3.n)) < 0.05, `1440×900: a compact 4–7 whole rows (${r3.n.toFixed(2)})`);
    { const r4 = await rowsAt(2380, 1540, false);
      ok(r4.n <= 7.05 && Math.abs(r4.n - Math.round(r4.n)) < 0.05, `2380×1540: normal ≤ 7 whole rows (${r4.n.toFixed(2)})`);
      await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(700);
      const mx = await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); return r.clientHeight / (r.querySelector('.trkRow')?.offsetHeight || 1); })()`);
      ok(mx >= 2 * r4.n - 0.05, `…maximised shows at least twice the rows (${mx.toFixed(1)} vs ${r4.n.toFixed(1)})`);
      await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(600); }
    // ── 390×844, LIST open: the ? and the scale lifted clear of the drawer, the (i) hidden; LIST shut, all back down ──
    { await rowsAt(390, 844, true); await sleep(400);
      const q = JSON.parse(await p.ev(`JSON.stringify((() => { const L = ${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect();
        const ov = (e) => { if (!e) return false; const b = e.getBoundingClientRect(); return b.width > 0 && b.left < L.right && b.right > L.left && b.top < L.bottom && b.bottom > L.top; };
        const at = ${V}.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution');
        return { help: ov(${V}.querySelector('.mHelp')), scale: ov(${V}.querySelector('.mChipsScale')), attribHidden: !at || getComputedStyle(at).visibility === 'hidden' || !ov(at) }; })())`));
      ok(!q.help && !q.scale && q.attribHidden, `390×844 LIST open: the ? and the scale clear of the drawer, the (i) out of sight (${JSON.stringify(q)})`);
      await p.ev(`${BTN('list')}.click(); 1`); await sleep(600);
      const d = JSON.parse(await p.ev(`JSON.stringify({ help: ${V}.querySelector('.mHelp').style.transform, scale: (${V}.querySelector('.mChipsScale')?.style.transform || ''), vis: getComputedStyle(${V}.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution') || document.body).visibility })`));
      ok(!d.help && !d.scale && d.vis !== 'hidden', `…LIST shut: both back down, the (i) back (${JSON.stringify(d)})`);
      await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(400); }
    // ── the LIST drawer: no title row; its chip in the corner is the toggle (arrow down open, up shut); no row under it ──
    { await rowsAt(390, 844, true);
      const t = JSON.parse(await p.ev(`JSON.stringify((() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'), d = ${V}.querySelector('.mDock[data-dock="list"]');
        const rows = c.querySelector('.rows').getBoundingClientRect(), chip = d.querySelector('.hCard').getBoundingClientRect();
        const cr = c.getBoundingClientRect(), x = chip.left + 8, y = chip.bottom - 8, hit = document.elementFromPoint(x, y);
        return { title: getComputedStyle(c.querySelector('.ch')).display, chipVis: getComputedStyle(d).visibility, arrow: getComputedStyle(d.querySelector('.arw')).transform,
          rowsToEdge: Math.abs(rows.bottom - (cr.bottom - parseFloat(getComputedStyle(c).paddingBottom))) <= 1, over: chip.left <= cr.left + 1 && chip.bottom >= cr.bottom - 1 && chip.top < rows.bottom,
          onTop: !!hit && !!hit.closest('.mDock[data-dock="list"]'), expanded: d.querySelector('[data-cbtn]').getAttribute('aria-expanded') }; })())`));
      const bar = JSON.parse(await p.ev(`JSON.stringify((() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'), hd = c.querySelector('.trkList .hd').getBoundingClientRect(), rows = c.querySelector('.rows').getBoundingClientRect(), chip = ${V}.querySelector('.mDock[data-dock="list"] .hCard').getBoundingClientRect();
        const cr = c.getBoundingClientRect(), srt = c.querySelector('.srt').getBoundingClientRect(), tabs = c.querySelector('.tabWrap').getBoundingClientRect();
        const under = [...c.querySelectorAll('.rows .trkRow')].filter(r => { const b = r.getBoundingClientRect(); const bot = Math.min(b.bottom, rows.bottom); return b.height && bot > chip.top + 1 && b.top < chip.bottom - 1 && b.left < chip.right - 1; }).length;
        return { rowsTop: Math.round(rows.top - cr.top), barBelow: hd.top >= rows.bottom - 1, chipInBar: chip.top >= hd.top - 2 && chip.bottom <= hd.bottom + 2, sortInBar: srt.width === 0 || (srt.top >= hd.top - 1 && srt.bottom <= hd.bottom + 1), tabsRight: tabs.width === 0 || tabs.left >= chip.right - 1, under }; })())`));
      ok(t.title === 'none' && t.chipVis === 'visible' && t.arrow !== 'none' && t.expanded === 'true' && bar.rowsTop <= 2 && bar.barBelow && bar.chipInBar && bar.sortInBar && bar.tabsRight && bar.under === 0,
         `the rows from the drawer's top; ONE bar at its foot — the chip, the tabs, the sort; nothing over a row (${JSON.stringify(bar)})`);
      await p.ev(`${BTN('list')}.click(); 1`); await sleep(600);
      const u = JSON.parse(await p.ev(`JSON.stringify({ shut: ${V}.querySelector('.trkCard2[data-card="list"]').hidden || !${V}.querySelector('.trkCard2[data-card="list"]').offsetHeight, arrow: getComputedStyle(${V}.querySelector('.mDock[data-dock="list"] .arw')).transform })`));
      ok(u.shut && u.arrow === 'none', `…its chip folds it; the arrow points up (${JSON.stringify(u)})`);
      await p.ev(`${BTN('list')}.click(); 1`); await sleep(400); }
    // ── the right-bottom stack — (i), ?, the scale — never overlaps, LIST open or shut, phone and desktop (Stuart 12:44) ──
    for (const [ww, hh, mob, listOn, mx] of [[375, 667, true, true, false], [390, 844, true, true, false], [430, 932, true, true, false], [390, 844, true, true, true], [375, 667, true, true, true],
                                             [390, 844, true, false, false], [1440, 900, false, true, false], [1440, 900, false, false, false]]) {
      await p.size(ww, hh, mob); await sleep(700); await shutTransients(p);
      await p.ev(`(() => { const b = ${BTN('list')}; if ((b.getAttribute('aria-expanded') === 'true') !== ${listOn}) b.click(); return 1; })()`); await sleep(700);
      await p.ev(`(() => { const t = ${V}.querySelector('.lMax'); if (t && (t.getAttribute('aria-pressed') === 'true') !== ${mx}) t.click(); return 1; })()`); await sleep(700);
      const hits = await p.ev(`(() => { const vis = (e) => e && getComputedStyle(e).visibility !== 'hidden' && e.getBoundingClientRect().width > 0;
        const els = [['?', ${V}.querySelector('.mHelp')], ['scale', ${V}.querySelector('.mChipsScale')], ['(i)', ${V}.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution')], ['max', ${V}.querySelector('.lMax')]].filter(([, e]) => vis(e) && e.offsetParent !== null);
        const out = []; for (let i = 0; i < els.length; i++) for (let j = i + 1; j < els.length; j++) { const a = els[i][1].getBoundingClientRect(), b = els[j][1].getBoundingClientRect();
          const g = els[i][0] === 'max' || els[j][0] === 'max' ? 8 : 0;   // ★ 8 px between tap targets
          if (a.left < b.right + g - 1 && a.right > b.left - g + 1 && a.top < b.bottom + g - 1 && a.bottom > b.top - g + 1) out.push(els[i][0] + '×' + els[j][0]); } return out.join(','); })()`);
      ok(!hits, `${ww}×${hh} LIST ${listOn ? (mx ? 'maximised' : 'open') : 'shut'}: the ?, the scale, the (i) and the maximise tab apart${hits ? ' — ' + hits : ''}`);
      await p.ev(`(() => { const t = ${V}.querySelector('.lMax'); if (t && t.getAttribute('aria-pressed') === 'true') t.click(); return 1; })()`); await sleep(500);
    }
    // ── the tab row (Stuart 12:47): no "no pos" pill; it scrolls, a fade + arrow only at an edge with more beyond ──
    { await rowsAt(390, 844, true);
      const st = async () => JSON.parse(await p.ev(`JSON.stringify((() => { const w = ${V}.querySelector('.trkList .hd .tabWrap'), t = w.querySelector('.tabs');
        const vis = (q) => getComputedStyle(w.querySelector(q)).display !== 'none';
        return { pill: !!${V}.querySelector('.npChip') || /no pos/.test(${V}.querySelector('.trkList .hd').textContent), over: t.scrollWidth > t.clientWidth + 1, aL: vis('.tArr.l'), aR: vis('.tArr.r'),
          fade: getComputedStyle(t).webkitMaskImage || getComputedStyle(t).maskImage }; })())`));
      const s0 = await st();
      ok(!s0.pill, 'no "+N no pos" pill in the LIST header');
      if (s0.over) {
        ok(s0.aR && !s0.aL && /gradient/.test(s0.fade), `390×844 tabs overflow: the right fade + arrow, none on the left (${JSON.stringify(s0)})`);
        await p.ev(`(() => { const t = ${V}.querySelector('.trkList .hd .tabs'); t.style.scrollBehavior = 'auto'; t.scrollLeft = t.scrollWidth; return 1; })()`); await sleep(300);
        const s1 = await st();
        ok(s1.aL && !s1.aR, `…scrolled to the end: the left fade + arrow, none on the right (${JSON.stringify(s1)})`);
        await p.ev(`(() => { const t = ${V}.querySelector('.trkList .hd .tabs'); t.scrollLeft = 0; t.style.scrollBehavior = ''; return 1; })()`);
      } else ok(!s0.aL && !s0.aR, 'the tabs fit: no arrows');
    }
    // ── the maximise tab (Stuart 12:46): on the drawer's top edge near its right corner; it fills the map under the chips; AUTO waits; again restores ──
    { await rowsAt(390, 844, true);
      const g = async () => JSON.parse(await p.ev(`JSON.stringify((() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'), t = c.querySelector('.lMax'), m = ${V}.querySelector('.trkMapHost').getBoundingClientRect();
        const cb = c.getBoundingClientRect(), tb = t.getBoundingClientRect(), cap = parseFloat(${V}.style.getPropertyValue('--trk-cb-top')) || 0, lb = parseFloat(${V}.style.getPropertyValue('--trk-list-bottom')) || 0;
        return { tabOn: tb.width > 0 && Math.abs(tb.bottom - cb.top) <= 1 && tb.right > cb.right - 80 && tb.right <= cb.right, h: Math.round(cb.height), want: Math.round(m.height - cap - lb), pressed: t.getAttribute('aria-pressed'), zoom: ${V}.querySelector('.mDock[data-dock="map"]').dataset.zoom || '' }; })())`));
      const g0 = await g();
      ok(g0.tabOn && g0.pressed === 'false', `the maximise tab sits on the drawer's top edge near its right corner (${JSON.stringify(g0)})`);
      await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(700);
      const g1 = await g();
      ok(Math.abs(g1.h - g1.want) <= 2 && g1.pressed === 'true', `…pressed: the LIST fills the map under the chips (${g1.h} of ${g1.want} px)`);
      ok(g1.zoom !== 'on', `…AUTO waits while it is maximised (${g1.zoom})`);
      await p.ev(`document.activeElement?.blur?.(); ${V}.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); 1`); await sleep(700);
      const g2 = await g();
      ok(Math.abs(g2.h - g0.h) <= 1 && g2.pressed === 'false', `…Escape restores it first, the LIST still open (${g2.h} px)`);
    }
    // ── the drawer's WIDTH from the viewport (Stuart 13:00 / 13:03): a phone's full width; wide, 30 % (420–640 px) ──
    for (const [ww, hh, mob, want] of [[390, 844, true, 'full'], [1440, 900, false, 420], [2560, 1440, false, 560]]) {
      await p.size(ww, hh, mob); await sleep(800); await shutTransients(p);
      await p.ev(`(() => { const b = ${BTN('list')}; if (b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(700);
      const w = await p.ev(`Math.round(${V}.querySelector('.trkCard2[data-card="list"]').getBoundingClientRect().width)`);
      ok(want === 'full' ? w === ww : w >= want, `${ww}×${hh}: the LIST drawer ${w} px wide (${want === 'full' ? 'the full width' : '≥ ' + want})`);
    }
    // ── every row keeps its age at phone widths (Stuart 13:08: "the time received is missing") ──
    for (const [ww, hh] of [[375, 667], [390, 844]]) {
      await rowsAt(ww, hh, true);
      const bad = await p.ev(`(() => { const rows = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); const R = rows.getBoundingClientRect(); const out = [];
        for (const r of [...rows.querySelectorAll('.trkRow')].slice(0, 12)) { const a = r.querySelector('.ag'); const b = a?.getBoundingClientRect();
          if (!a || !b.width || !a.textContent.trim() || b.right > R.right + 1 || a.scrollWidth > a.clientWidth + 1) out.push(r.dataset.key); } return out.join(','); })()`);
      ok(!bad, `${ww}×${hh}: every row shows its age, whole${bad ? ' — not: ' + bad : ''}`);
    }
    // ── the chip covers a row only when the list scrolls (Stuart 13:13) ──
    { const r0 = await rowsAt(390, 844, true);
      const geo = async () => JSON.parse(await p.ev(`JSON.stringify((() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'), rows = c.querySelector('.rows'), chip = ${V}.querySelector('.mDock[data-dock="list"] .hCard').getBoundingClientRect();
        const R = rows.getBoundingClientRect(); const under = [...rows.querySelectorAll('.trkRow')].filter(r => { const b = r.getBoundingClientRect(); const bot = Math.min(b.bottom, R.bottom); return b.height && bot > chip.top + 1 && b.top < chip.bottom - 1 && b.left < chip.right - 1; }).length;
        return { scroll: rows.scrollHeight > rows.clientHeight + 1, under, h: Math.round(c.getBoundingClientRect().height), spacer: getComputedStyle(rows, '::after').height }; })())`));
      const g1 = await geo();
      ok(g1.scroll && g1.under === 0, `AIRCRAFT (scrolls): no row under the chip (${JSON.stringify(g1)})`);
      const ships = await p.ev(`(() => { const t = ${V}.querySelector('.trkList .hd .tab[data-tab="ships"]'); if (!t) return false; t.click(); return true; })()`);
      if (ships) { await sleep(900);
        const g2 = await geo();
        ok(!g2.scroll && g2.under === 0 && g2.h <= g1.h, `SHIPS (a few rows): the drawer fits them, no taller than normal (${JSON.stringify(g2)})`);
        await p.ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]')?.click(); 1`); await sleep(600); }
      void r0;
    }
    // ── the last thing in the list always scrolls clear of the chip, on every tab (Stuart 13:15) ──
    { await rowsAt(390, 844, true);
      const tabs = JSON.parse(await p.ev(`JSON.stringify([...${V}.querySelectorAll('.trkList .hd .tab')].map(t => t.dataset.tab))`));
      for (const tb of tabs.length ? tabs : ['']) {
        if (tb) { await p.ev(`${V}.querySelector('.trkList .hd .tab[data-tab="${tb}"]').click(); 1`); await sleep(800); }
        const g = JSON.parse(await p.ev(`JSON.stringify((() => { const rows = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); rows.scrollTop = rows.scrollHeight; const last = rows.lastElementChild;
          const chip = ${V}.querySelector('.mDock[data-dock="list"] .hCard').getBoundingClientRect(); return { last: last ? Math.round(last.getBoundingClientRect().bottom) : 0, chipTop: Math.round(chip.top) }; })())`));
        await sleep(200);
        ok(!g.last || g.last <= g.chipTop + 1, `${tb || 'the list'}: scrolled to the end, the last item ends above the bar and its chip (${g.last} ≤ ${g.chipTop})`);
        await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); r.scrollTop = 0; return 1; })()`); await sleep(300);
      }
      if (tabs.includes('aircraft')) { await p.ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]').click(); 1`); await sleep(600); }
    }
    // ── scrolled, the order holds (Stuart 13:15): rows update in place, new ones wait behind "‖ +N" ──
    { await rowsAt(390, 844, true);
      await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); r.scrollTop = 3 * (r.querySelector('.trkRow')?.offsetHeight || 45); r.dispatchEvent(new Event('scroll')); return 1; })()`); await sleep(300);
      const keys0 = await p.ev(`JSON.stringify([...${V}.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow')].map(r => r.dataset.key))`);
      // two new, nearer aircraft: the next draw's rows with two extra at the top. ★ All in ONE evaluation — the live list is
      //   redrawn every second, and a real update landing between the steps replaced the injected rows (a flaky test, 11-27)
      const one = JSON.parse(await p.ev(`JSON.stringify((() => { const v = window.__vibeTrkView; const html = v.rowsPending || v.lastRows; const extra = ['__t1', '__t2'].map(k => '<div class="trkRow kvRow" data-key="' + k + '"><span class="id"><b><span class="nmT">' + k + '</span></b><small></small></span><span class="rg"></span><span class="ag">now</span></div>').join('');
        v.lastRows = ''; v['frozenApply'](extra + html); v.rowsPending = extra + html;
        const keys1 = JSON.stringify([...v.root.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow')].map(r => r.dataset.key));
        const b = v.root.querySelector('.trkList .hd .lPause'); const pz = { shown: !b.hidden && b.getBoundingClientRect().width > 0, text: b.textContent.trim() };
        b.click();
        const after = { hid: b.hidden, n: v.root.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow[data-key^="__t"]').length };
        return { keys1, pz, after }; })())`));
      const keys1 = one.keys1, pz = one.pz;
      ok(keys0 === keys1 && pz.shown && /\+2$/.test(pz.text), `scrolled: the rows hold their order, "‖ +2" shows (${pz.text})`);
      const after = one.after;
      ok(after.hid && after.n === 2, `…pressed: the new rows are in, the key gone (${JSON.stringify(after)})`);
      await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); r.scrollTop = 200; r.dispatchEvent(new Event('scroll')); return 1; })()`); await sleep(300);
      const fz = await p.ev(`!${V}.querySelector('.trkList .hd .lPause').hidden`);
      await p.ev(`(() => { const r = ${V}.querySelector('.trkCard2[data-card="list"] .rows'); r.scrollTop = 0; r.dispatchEvent(new Event('scroll')); return 1; })()`); await sleep(300);
      ok(fz && await p.ev(`${V}.querySelector('.trkList .hd .lPause').hidden`), '…scrolled down it pauses again; back at the top it carries on by itself');
    }
    // ── the FL | distance pill beside the tags; no bearing in the rows (Stuart 13:10) ──
    for (const [ww, hh, mob] of [[375, 667, true], [390, 844, true], [1440, 900, false]]) {
      await rowsAt(ww, hh, mob);
      const q = JSON.parse(await p.ev(`JSON.stringify((() => { const rows = [...${V}.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow')].slice(0, 10);
        const pill = rows.filter(r => { const k = r.querySelector('.id b .kv'); if (!k) return false; const b = k.getBoundingClientRect(), B = r.querySelector('.id b').getBoundingClientRect(); return b.width > 0 && b.right <= B.right + 1; }).length;
        return { n: rows.length, pill, brg: rows.filter(r => r.querySelector('.brg')).length }; })())`));
      ok(q.n > 0 && q.pill === q.n && q.brg === 0, `${ww}×${hh}: every row wears its FL | distance pill whole, no bearing (${JSON.stringify(q)})`);
    }
    // ── HELP grows out of its ?: its bottom-right corner on the button ──
    for (const [ww, hh, mob] of [[750, 950, false], [390, 844, true], [1440, 900, false]]) {
      await p.size(ww, hh, mob); await sleep(700); await shutTransients(p);
      await p.ev(`${BTN('help')}.click(); 1`); await sleep(700);
      const d = await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="help"]').getBoundingClientRect(), q = ${V}.querySelector('.mHelp').getBoundingClientRect();
        const dx = Math.max(q.left - c.right, 0, c.right - q.right), dy = Math.max(q.top - c.bottom, 0, c.bottom - q.bottom); return Math.round(Math.hypot(dx, dy)); })()`);
      ok(d <= 4, `${ww}×${hh}: HELP's corner sits on its ? (${d} px)`);
      await shutTransients(p);
    }
    await p.size(1440, 900, false); await sleep(500);
    // ── Stuart 14:58: on the phones the left family stays on the left when it fits; nothing on either edge overlaps ──
    for (const [ww, hh] of [[430, 932], [393, 852], [375, 667]]) {
      await p.size(ww, hh, true); await sleep(900); await shutTransients(p);
      const L = JSON.parse(await p.ev(`JSON.stringify((() => { const m = ${V}.querySelector('.trkMapHost').getBoundingClientRect();
        const fam = ['servers', 'tune', 'layers', 'stats', 'map'].map(id => ${V}.querySelector('.mDock[data-dock="' + id + '"]:not([hidden])')).filter(Boolean);
        const left = fam.every(d => d.getBoundingClientRect().left <= m.left + 1);
        const all = [...${V}.querySelectorAll('.mDock:not([hidden]) .hCard, .trkTop .exit, #mapShLine, .maplibregl-ctrl-top-left, .mHelp, .mChipsScale, #srvHealth .hCard')].filter(e => e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden').map(e => { const r = e.getBoundingClientRect(); return { n: (e.closest('[data-dock]')?.dataset.dock || e.id || e.className).toString().slice(0, 20), l: r.left, t: r.top, r: r.right, b: r.bottom }; });
        const hits = []; for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) { const a = all[i], c = all[j]; if (a.l < c.r - 1 && a.r > c.l + 1 && a.t < c.b - 1 && a.b > c.t + 1 && !(a.l >= c.l && a.r <= c.r && a.t >= c.t && a.b <= c.b) && !(c.l >= a.l && c.r <= a.r && c.t >= a.t && c.b <= a.b)) hits.push(a.n + '×' + c.n); }
        return { mapH: Math.round(m.height), left, hits }; })())`));
      ok(L.mapH < 600 || L.left, `${ww}×${hh}: the left family on the left (map ${L.mapH} px)`);
      ok(!L.hits.length, `${ww}×${hh}: no chip overlaps another${L.hits.length ? ' — ' + L.hits.join(', ') : ''}`);
      // ★ Stuart 15:13: every chip is the TOP thing where it is drawn — a map marker never takes its tap
      const buried = await p.ev(`(() => { const out = []; for (const e of ${V}.querySelectorAll('.mDock:not([hidden]) .hCard, .mDock:not([hidden]) .hTab, .edgeDock .hTab, .trkTop .exit, .mHelp')) { if (!e.getClientRects().length || getComputedStyle(e).visibility === 'hidden') continue; const q = e.getBoundingClientRect(); if (q.width < 4 || q.right <= 0 || q.left >= innerWidth) continue; const h = document.elementFromPoint(q.left + q.width / 2, q.top + q.height / 2); if (h && !e.contains(h) && !h.contains(e) && !h.closest('.edgeDock, .mDock, .trkTop, .mHelp')) out.push((e.closest('[data-dock]')?.dataset.dock || e.className) + ' under ' + (h.className || h.tagName)); } return out.join(', '); })()`);
      ok(!buried, `${ww}×${hh}: every chip is on top where it is drawn${buried ? ' — ' + buried : ''}`);
    }
    // ── one side or flanking, by the map's own height (Stuart's phones) ──
    // ★ the agreed rule (mapCards oneSide): flank only when the left stack does NOT fit the map's height — at 375×667 it fits
    //   (the ✕, five chips and the LIST); 932×430 and 956×400 are SHORT landscape (the family is a bottom row, never the left edge)
    for (const [ww, hh, want] of [[375, 667, 'L'], [375, 550, 'R'], [430, 932, 'L'], [440, 760, 'L'], [932, 430, 'R'], [956, 400, 'R']]) {
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
    // ★★★ V12 the HISTORY viewer has no radio under it: no list ↔ waterfall tab, on the drawer or the collapsed chip (Stuart 11-27)
    ok(await p.ev(`(() => { const v = ${V}; const t = [...v.querySelectorAll('.lSwap, .lSwapC')]; return t.length > 0 && t.every((e) => e.hidden || !e.getClientRects().length); })()`), 'the viewer: no list ↔ waterfall tab');
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); feed.kill(); }
}
// ═════ B. one radio, no feed: ONE source → no SOURCES ═════
{
  const dir = mkdir('one');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map cards single', locator: 'IO92nh', adminPass: 'map-cards-admin', gainLock: true,
    radios: [{ serial: 'mc-one', label: 'Only radio', enabled: true, configured: true, users: 1, gainLock: true, gainLimits: '130M-140M:300' }] }));   // ★ the owner's gain lock: the 🔒 flow below
  // ★ the lock BITES only on a band with a ceiling (hwinfo gainLocked = cap here && lock) — the DATALINK band gets one
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
    // ★★ Stuart 15:01: the owner's gain lock in the TUNE card — 🔒 read-only; a wrong password refused; the right one → 🔓
    { await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="tune"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(800);
      const lk = await p.ev(`(() => { const r = ${V}.querySelector('.mapGainHost .gRow'); return r ? JSON.stringify({ lock: !!r.querySelector('button.gLock'), slider: !!r.querySelector('.gSl') }) : 'none'; })()`);
      ok(/"lock":true/.test(lk || '') && /"slider":false/.test(lk || ''), `gain locked by the owner: the row is read-only with its padlock (${lk})`);
      if (/"lock":true/.test(lk || '')) {
        await p.ev(`${V}.querySelector('.mapGainHost button.gLock').click(); 1`); await sleep(300);
        await p.ev(`(() => { const i = ${V}.querySelector('.mapGainHost .gPwIn'); i.value = 'not-the-password'; ${V}.querySelector('.mapGainHost .gPwGo').click(); return 1; })()`); await sleep(2500);
        const wrong = await p.ev(`(() => { const m = ${V}.querySelector('.mapGainHost .gPwMsg'); return (m ? m.textContent : '') + '|' + !!${V}.querySelector('.mapGainHost button.gLock'); })()`);
        ok(/Not accepted\|true/.test(wrong || ''), `…a wrong password is refused, still locked (${wrong})`);
        await sleep(1500);
        await p.ev(`(() => { const i = ${V}.querySelector('.mapGainHost .gPwIn'); i.value = 'map-cards-admin'; ${V}.querySelector('.mapGainHost .gPwGo').click(); return 1; })()`);
        ok(await p.until(`!!${V}.querySelector('.mapGainHost .gLock.open') || !${V}.querySelector('.mapGainHost button.gLock')`, 8000), '…the right one unlocks it (🔓)');
        await p.ev(`(() => { const a = ${V}.querySelector('.mapGainHost .gAgc'); if (a && a.classList.contains('on')) a.click(); return 1; })()`); await sleep(800);
        ok(await p.ev(`!!${V}.querySelector('.mapGainHost .gSl')`), '…and the gain is the slider again');
      } }
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); }
}
clearTimeout(watchdog);
console.log(`test-web-map-cards: ${pass} passed, ${fail} failed`);
for (const k of kids) { try { k.kill('SIGTERM'); } catch {} }
await sleep(500);
process.exit(fail ? 1 : 0);
