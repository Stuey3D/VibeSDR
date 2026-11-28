// test-web-map-pad.mjs — ★★★ V12 CONTROLLERS ON THE MAPS (Stuart's fixed scheme, gamepad.ts): the keyboard drives the same
// highlight as a pad — it walks the map's chrome only (never the page under it, never a map item); ENTER on a chip opens its
// card with the highlight inside it, BACK shuts it onto its chip; the LIST is one stop (enter: rows, back: let go); + zooms
// the MAP; tune on a locked dial is the ask pulse; HELP shows the controller lines. Portrait, landscape, desktop.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/v.cu8 node scripts/test-web-map-pad.mjs     (exit 3 = not run)
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


// ═════ one radio (the VDL2 clip): the live datalink map, driven by the KEYBOARD (the pad's own highlight path) ═════
{
  const dir = mkdir('pad');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map pad', locator: 'IO92nh',
    radios: [{ serial: 'pad-one', label: 'Only radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' }));
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  kids.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], { env, stdio: ['ignore', 'ignore', 'ignore'] }));
  ok(await up(`http://127.0.0.1:${port}/vibeserver.json`), 'the radio is up');
  const p = await browser(dir);
  ok(await radioDatalink(p, `http://127.0.0.1:${port}/`), 'the live datalink map is up');
  await p.ev(`(() => { const t = document.querySelector('.trkView:not([hidden]) .trkCard2[data-card="list"]'); if (!t || t.hidden) document.querySelector('.trkView:not([hidden]) [data-cbtn="list"]')?.click(); return 1; })()`);
  await sleep(4000);   // a few datalink rows
  const HL = `JSON.stringify((() => { const e = document.querySelector('.gpFocus'); if (!e) return null; const v = ${V};
    return { inView: !!v && v.contains(e), cbtn: e.dataset.cbtn || '', card: e.closest('.trkCard2')?.dataset.card || '', row: e.classList.contains('trkRow'),
      box: e.hasAttribute('data-navbox'), lctl: e.closest('.trkCard2[data-card="list"]') && (e.matches('.lMax, .lSwap') || !!e.closest('.trkList .hd')) ? (e.matches('.lMax') ? 'max' : e.matches('.lSwap') ? 'swap' : 'bar:' + (e.className || e.tagName).slice(0, 20)) : '', mapItem: !!e.closest('.maplibregl-map, .leaflet-container') && !e.closest('[data-gpkeep]'), zoomKey: !!e.closest('[data-gpkeep]') }; })())`;
  const hl = async () => JSON.parse(await p.ev(HL));
  // ★ a seeded wander (a fixed cycle can orbit two stops for ever): every direction, in no set order
  let seed = 7; const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  const DIRS = ['ArrowDown', 'ArrowRight', 'ArrowUp', 'ArrowLeft'];
  const PAT = Array.from({ length: 600 }, () => DIRS[Math.floor(rnd() * 4)]);
  async function walkTo(pred, max = 400) {
    for (let i = 0; i < max; i++) { const h = await hl(); if (h && pred(h)) return h; await p.key(PAT[i % PAT.length]); await sleep(120); }
    return null;
  }
  for (const [w, h, mobile, tag] of [[390, 844, true, 'portrait'], [956, 376, true, 'landscape'], [1440, 900, false, 'desktop']].filter(([, , , t]) => !process.env.PAD_ONLY || process.env.PAD_ONLY.split(',').includes(t))) {
    await p.size(w, h, mobile); await sleep(1500);
    // the LIST drawer open (BACK with nothing else up shuts the top card — the LIST counts)
    await p.ev(`(() => { const b = ${V}?.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(500);
    // ① the walk stays on the map, never the page under it, never a map item; it reaches chips, the LIST and the zoom keys
    const seen = { chips: new Set(), list: false, zoom: false, out: 0, item: 0, row: 0, lctl: new Set() };
    for (let i = 0; i < 200; i++) {
      await p.key(PAT[i % PAT.length]); await sleep(110);
      const x = await hl(); if (!x) continue;
      if (!x.inView) seen.out++; if (x.mapItem) seen.item++;
      if (x.cbtn) seen.chips.add(x.cbtn); if (x.box && x.card === 'list') seen.list = true; if (x.zoomKey) seen.zoom = true; if (x.row) seen.row++; if (x.lctl) seen.lctl.add(x.lctl.replace(/^bar:.*/, 'bar'));
    }
    ok(!seen.out && !seen.item, `${tag}: the highlight stays on the map's chrome (outside ${seen.out}, map items ${seen.item})`);
    ok(seen.chips.size >= 3, `${tag}: it reaches the chips (${[...seen.chips].join(' ')})`);
    if (!seen.lctl.has('bar') && await walkTo((x) => /^bar/.test(x.lctl || ''))) seen.lctl.add('bar');
    if (!seen.lctl.has('max') && await walkTo((x) => x.lctl === 'max')) seen.lctl.add('max');
    ok(seen.lctl.has('max') && seen.lctl.has('bar'), `${tag}: the LIST's own controls are stops of their own — the maximise tab and the bar's keys, each ringed (${[...seen.lctl].join(' ')})`);
    { // ENTER on the maximise tab maximises; again restores
      const mx = await walkTo((x) => x.lctl === 'max');
      if (mx) { await p.key('Enter'); await sleep(600); const on = await p.ev(`${V}.classList.contains('lMax')`); await p.key('Enter'); await sleep(600); const off = await p.ev(`!${V}.classList.contains('lMax')`);
        ok(on && off, `${tag}: ENTER on the maximise tab maximises the LIST, and again restores it (${on} / ${off})`); }
      else ok(false, `${tag}: the maximise tab never reached`);
    }
    ok(!seen.row, `${tag}: the LIST is ONE stop — the walk never visits a row (${seen.row}), so what lies past it is one press away`);
    // ② ENTER on a chip opens its card and the highlight goes INSIDE it; BACK shuts it and puts the highlight on the chip
    const chip = await walkTo((x) => ['layers', 'map', 'stats', 'servers', 'help'].includes(x.cbtn));
    if (chip) {
      await p.key('Enter'); await sleep(700);
      const inCard = await hl();
      const open = await p.ev(`${BTN(chip.cbtn)}?.getAttribute('aria-expanded')`);
      const scope = await p.ev(`!!${V}.querySelector('.trkCard2[data-card="${chip.cbtn}"][data-navscope]')`);
      ok(open === 'true' && scope && (!inCard || inCard.card === chip.cbtn), `${tag}: enter on ${chip.cbtn.toUpperCase()} opens its card, the highlight inside it (${JSON.stringify(inCard)})`);
      for (let i = 0; i < 6; i++) { await p.key(PAT[i]); await sleep(100); }
      const still = await hl();
      ok(!still || still.card === chip.cbtn, `${tag}: …and the D-pad stays inside the open card (${JSON.stringify(still)})`);
      // ★ the page's rule: an item chosen earlier (a LIST row) goes first — back clears it, then shuts the card
      for (let i = 0; i < 3; i++) { await p.key('Escape'); await sleep(600); if ((await p.ev(`${BTN(chip.cbtn)}?.getAttribute('aria-expanded')`)) !== 'true') break; }
      const after = await hl();
      const shutIn = await p.ev(`${BTN(chip.cbtn)}?.getAttribute('aria-expanded')`);
      ok(shutIn !== 'true' && after?.cbtn === chip.cbtn, `${tag}: back shuts it, the highlight back on its chip (${shutIn} ${JSON.stringify(after)} ${await p.ev(`JSON.stringify({ sel: window.__vibeTrkView?.sel, pop: !!document.querySelector('.maplibregl-popup, .vsadsb-pop-dark') })`)})`);
    } else ok(false, `${tag}: no card chip reached`);
    // ③ ★★★ STUART'S LIST FLOW (the R36S's): one stop → ENTER into the rows → ENTER on a row opens ITS CARD with the
    //    highlight inside it → BACK closes the card, the highlight on the SAME row → DOWN row by row, always in view →
    //    BACK leaves the list, the highlight on the LIST stop
    const CARD = `[...document.querySelectorAll('.trkView:not([hidden]) .maplibregl-popup, .trkView:not([hidden]) .leaflet-popup, #ndbCard:not([hidden])')].filter((c) => c.getClientRects().length)`;
    const box = await walkTo((x) => x.box && x.card === 'list');
    if (box) {
      const pre = process.env.PAD_DIAG ? await p.ev(`JSON.stringify({ act: (document.activeElement?.className || document.activeElement?.tagName || '').slice(0, 50), hl: (document.querySelector('.gpFocus')?.className || '').slice(0, 50), scope: [...document.querySelectorAll('[data-navscope]')].map((e) => e.className.slice(0, 30)), grab: (document.querySelector('.gpGrab')?.className || '').slice(0, 30) })`) : '';
      await p.key('Enter'); await sleep(400);
      if (pre) console.log('   ROWS', await p.ev(`JSON.stringify([...${V}.querySelectorAll('.trkCard2[data-card="list"] .trkRow')].slice(0, 2).map((e) => { const cs = getComputedStyle(e), b = e.getBoundingClientRect(); return { pe: cs.pointerEvents, vis: cs.visibility, w: b.width, h: b.height, skip: !!e.closest('[data-gpskip]'), inert: !!e.closest('[inert]') }; }))`));
      if (pre) console.log('   PRE', pre, 'POST', await p.ev(`JSON.stringify({ act: (document.activeElement?.className || '').slice(0, 50), hl: (document.querySelector('.gpFocus')?.className || '').slice(0, 50), grab: (document.querySelector('.gpGrab')?.className || '').slice(0, 30), grabs: [...document.querySelectorAll('.gpGrab')].map((g) => g.dataset.card + ':' + Math.round(g.getBoundingClientRect().width) + ':' + g.querySelectorAll('.trkRow').length), open: [...document.querySelectorAll('[aria-expanded="true"]')].map((b) => b.dataset.cbtn) })`));
      const r = await hl();
      const rowKey = await p.ev(`document.querySelector('.gpFocus')?.dataset.key || ''`);
      ok(!!r && r.row, `${tag}: ① ENTER on the LIST puts the highlight on a row (${rowKey || await p.ev(`JSON.stringify({ hl: (document.querySelector('.gpFocus')?.className || '-').slice(0, 60), rows: ${V}.querySelectorAll('.trkRow').length, pos: ${V}.querySelectorAll('.trkRow:not(.nopos)').length, pane: ${V}.querySelector('.trkList')?.dataset.pane, tab: window.__vibeAirSea?.()?.tab })`)})`);
      if (r?.row) {
        await p.key('Enter'); await sleep(1300);
        const inCard = await p.ev(`(() => { const c = ${CARD}; const e = document.querySelector('.gpFocus'); return JSON.stringify({ cards: c.length, inCard: !!e && c.some((x) => x.contains(e)), sel: !!${V}.querySelector('.trkRow.sel') }); })()`);
        ok(/"cards":[1-9]/.test(inCard) && /"inCard":true/.test(inCard), `${tag}: ② ENTER on a row opens its card and the highlight moves INTO it (${inCard})`);
        await p.key('Escape'); await sleep(700);
        const back1 = await p.ev(`JSON.stringify({ cards: ${CARD}.length, key: document.querySelector('.gpFocus')?.dataset.key || '', row: !!document.querySelector('.gpFocus.trkRow') })`);
        ok(/"cards":0/.test(back1) && back1.includes(`"key":"${rowKey}"`), `${tag}: ③ BACK closes the card, the highlight back on the SAME row (${back1})`);
      }
      // ★ down through a long list: the highlighted row is always in view, with a margin
      const inView = [];
      for (let i = 0; i < 14; i++) { await p.key('ArrowDown'); await sleep(140);
        inView.push(await p.ev(`(() => { const e = document.querySelector('.gpFocus'); if (!e || !e.classList.contains('trkRow')) return 'x'; let s = e.parentElement; while (s && s !== document.body && !(s.scrollHeight > s.clientHeight + 1 && /auto|scroll/.test(getComputedStyle(s).overflowY))) s = s.parentElement; const a = e.getBoundingClientRect(), b = (s && s !== document.body ? s : document.documentElement).getBoundingClientRect(); return a.top >= b.top - 1 && a.bottom <= b.bottom + 1 ? 'in' : 'out ' + Math.round(a.top) + '–' + Math.round(a.bottom) + ' of ' + Math.round(b.top) + '–' + Math.round(b.bottom); })()`)); }
      ok(inView.every((x) => x === 'in' || x === 'x') && inView.includes('in'), `${tag}: ④ DOWN row by row keeps the highlighted row in view (${inView.join(' ')})`);
      await p.key('Escape'); await sleep(400);
      const b = await hl();
      ok(!!b && b.box && b.card === 'list', `${tag}: ⑤ BACK leaves the list in ONE press, the highlight on the LIST stop (${JSON.stringify(b)})`);
    } else ok(false, `${tag}: the LIST never reached`);
    ok(seen.zoom || !!(await walkTo((x) => x.zoomKey, 40)), `${tag}: the zoom keys are stops`);
    // ④ ZOOM = the MAP's zoom; TUNE on a locked dial = the ask pulse, never a tune
    const z0 = await p.ev(`(() => { const v = window.__vibeTrkView; const m = v?.vm || v?.lmap; return m ? m.getZoom() : null; })()`);
    await p.key('+'); await sleep(450);
    const z1 = await p.ev(`(() => { const v = window.__vibeTrkView; const m = v?.vm || v?.lmap; return m ? m.getZoom() : null; })()`);
    ok(z0 != null && z1 != null && z1 > z0, `${tag}: + zooms the map (${z0} → ${z1})`);
    await p.ev(`window.__vibeTrkView.setDialLock('In use by someone else — ask to tune', false); 1`);
    const f0 = await p.ev(`document.getElementById('freq')?.textContent || ''`);
    await p.key('.'); await sleep(400);
    const pulsed = await p.ev(`!!document.querySelector('.askPulse')`);
    const f1 = await p.ev(`document.getElementById('freq')?.textContent || ''`);
    ok(pulsed && f0 === f1, `${tag}: tune on a locked dial gives the ask pulse and never tunes (${f0} / ${f1})`);
    await p.ev(`window.__vibeTrkView.setDialLock('', false); 1`);
  }
  // ⑤ the HELP card says how, in the layout in use — glyphs by position, never letters for the face buttons
  await p.ev(`${BTN('help')}?.click(); 1`); await sleep(500);
  const help = await p.ev(`(() => { const e = ${V}.querySelector('.trkCard2[data-card="help"] .hPad'); return e ? { svg: e.querySelectorAll('svg').length, text: e.textContent } : null; })()`);
  ok(!!help && help.svg >= 2 && /With a controller/.test(help.text) && !/\b[ABXY]\b button/.test(help.text), `the HELP card's controller lines (diamond glyphs: ${help?.svg})`);
  ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  p.close();
}
clearTimeout(watchdog);
console.log(`test-web-map-pad: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
