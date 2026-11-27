// test-web-map-leaks.mjs — ★★ Stuart 16:53: no old mode panel (the ADS-B table, its chips, its credit…) inside the LIST drawer, in every AIR & SEA live mode the sim radio offers. VIBE_ADSB_CLIP= adds ADS-B.
// page from a real vibeserver, headless Edge with --mute-audio: offered on a live map over a radio; the spectrum runs ONLY
// while it shows; one labelled marker per VDL2 channel from the server's table; passive (a tap / drag sends nothing); the
// top-left tab swaps list ↔ waterfall in place; fresh each time; the tabs clear of ⌃ and the ? / scale stack.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/v.cu8 node scripts/test-web-map-waterfall.mjs     (exit 3 = not run)
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
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 300 s'); process.exit(1); }, 300_000);   // ★ the suite grew with the map work (widths, rows, stacks, pulses)
const SIZES = [[390, 844, true], [820, 1180, true], [1440, 900, false]];

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

// ═════ one radio (a fake RTL: the VDL2 clip on HF, synthetic ADS-B at 1090), every AIR & SEA live mode it offers ═════
{
  const ADSB = process.env.VIBE_ADSB_CLIP || '';
  const dir = mkdir('lk');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map leaks', locator: 'IO92nh',
    radios: [{ serial: 'lk-one', label: 'Only radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000', ...(ADSB && fs.existsSync(ADSB) ? ['--adsb', ADSB] : [])], { stdio: 'ignore' }));
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  kids.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] }));
  ok(await up(`http://127.0.0.1:${port}/vibeserver.json`), 'the radio is up');
  const p = await browser(dir);
  // what the LIST drawer may hold: the card system's own elements, nothing of an old panel
  const LEAKS = `JSON.stringify((() => { const c = ${V}?.querySelector('.trkCard2[data-card="list"]'); if (!c) return ['no list card'];
    const bad = [];
    for (const sel of ['#adsbList', '#adsbChips', '.adsbCredit', '.avCredit', '#aisList', '#ismList', '#sdList', '#ndbList', '#vmList', 'table'])
      for (const e of c.querySelectorAll(sel)) if (e.getClientRects().length) bad.push(sel);
    const OK_CB = ['facts', 'trkList', 'lSwap', 'lMax'], OK_TL = ['hd', 'safety', 'filt', 'rows', 'wfPane', 'trkSlot'];
    for (const k of c.querySelector(':scope > .cb').children) if (k.getClientRects().length && !OK_CB.some((x) => k.classList.contains(x))) bad.push('cb>' + (k.id || k.className));
    for (const k of c.querySelector('.trkList').children) if (k.getClientRects().length && !OK_TL.some((x) => k.classList.contains(x))) bad.push('list>' + (k.id || k.className));
    for (const s of c.querySelectorAll('.trkList > .trkSlot')) for (const k of s.children) if (k.getClientRects().length) bad.push('slot>' + (k.id || k.className));
    for (const k of c.querySelector('.rows').children) if (k.getClientRects().length && !/(^|\\s)(trkRow|trkEmpty|sec|trkMsg)(\\s|$)/.test(k.className)) bad.push('rows>' + (k.id || k.className));
    return bad; })())`;
  try {
    await p.send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await p.until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 20000);
    await p.ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await p.until(`!!document.querySelector('#audioGate button')`, 15000);
    await p.ev(`document.querySelector('#audioGate button')?.click(); 1`);
    await sleep(2500);
    await p.size(1440, 900, false);
    for (const m of ['adsb', 'ais', 'av', 'sd']) {
      if (await p.ev(`!!${V}`)) { await p.ev(`${V}.querySelector('.trkTop .exit').click(); 1`); await sleep(2500); }
      await p.ev(`document.getElementById('mMode')?.click(); 1`); await sleep(600);
      await p.ev(`localStorage.setItem('vibesdr.airSeaLast', '${m}'); document.getElementById('mModeAirSea')?.click(); 1`);
      if (!(await p.until(`!!${V}`, 12000))) { console.log(`   (${m}: no map — not offered here)`); continue; }
      await sleep(8000);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(1500);
      const mode = await p.ev(`${V}.querySelector('.trkTop .rate')?.textContent || ''`);
      const bad = JSON.parse(await p.ev(LEAKS));
      ok(!bad.length, `${m} (${mode}): the LIST drawer holds only the card system's own elements (${bad.join(', ') || 'clean'})`);
      if (m === 'adsb') ok(await p.ev(`!!${V}.querySelector('.trkCard2[data-card="help"] .hCredit')`), 'adsb: the aircraft data credit is in HELP');
    }
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); }
}
clearTimeout(watchdog);
console.log(`test-web-map-leaks: ${pass} passed, ${fail} failed`);
for (const k of kids) { try { k.kill('SIGTERM'); } catch {} }
await sleep(500);
process.exit(fail ? 1 : 0);
