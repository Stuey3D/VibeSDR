// test-web-map-landscape.mjs — ★★★ V12 SHORT LANDSCAPE + SAFE AREAS (Stuart 11-27, iPhone landscape): 932×370, 932×430, 852×340, 667×320 —
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

// ═════ one radio in the DATALINK mode (or the live preview: PREVIEW_URL=http://127.0.0.1:47610/r/<id>/) ═════
{
  let url = process.env.PREVIEW_URL || '';
  const dir = mkdir('ls');
  if (!url) {
    fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map landscape', locator: 'IO92nh',
      radios: [{ serial: 'ml-one', label: 'Only radio', enabled: true, configured: true, users: 2, mode: 'single', askBeforeTuning: true, sessionLimitMin: 30, sessionLimitSoft: true }] }));
    const rtlPort = await freePort(), port = await freePort();
    kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' }));
    await sleep(400);
    const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
    kids.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2'], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] }));
    ok(await up(`http://127.0.0.1:${port}/vibeserver.json`), 'the radio is up');
    url = `http://127.0.0.1:${port}/`;
  }
  const p = await browser(dir);
  const SHOT = process.env.SHOT_DIR || '';
  const shot = async (name) => { if (!SHOT) return; const r = await p.send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOT, name), Buffer.from(r.result.data, 'base64')); };
  // every control's rect and whether it is the element on top at its centre
  const RECTS = `JSON.stringify((() => { const V = ${V}; const out = {}; const r = (e) => { if (!e || !e.getClientRects().length || e.closest('[hidden]') || getComputedStyle(e).visibility === 'hidden') return null; const b = e.getBoundingClientRect(); if (!b.width || !b.height) return null; const cx = Math.min(innerWidth - 1, Math.max(0, b.left + b.width / 2)), cy = Math.min(innerHeight - 1, Math.max(0, b.top + b.height / 2)); const at = document.elementFromPoint(cx, cy); return { l: Math.round(b.left), t: Math.round(b.top), r: Math.round(b.right), b: Math.round(b.bottom), top: !!at && (e === at || e.contains(at) || at.contains(e)) }; };
    out.x = r(V.querySelector('.trkTop .exit')); out.pill = r(document.getElementById('mapShLine'));
    for (const d of V.querySelectorAll('.mChipsL .mDock')) out['L:' + d.dataset.dock] = r(d.querySelector('.hCard'));
    for (const d of V.querySelectorAll('.mChipsR > .mDock, .mChipsR > .edgeDock')) out['R:' + (d.dataset.dock || d.id)] = r(d.classList.contains('tucked') ? d.querySelector('.hTab') : d.querySelector('.hCard'));
    const hs = document.getElementById('srvHealth'); if (hs) out['R:health'] = r(hs.classList.contains('tucked') ? hs.querySelector('.hTab') : hs.querySelector('.hCard'));
    out.nTime = V.querySelectorAll('[data-dock="time"]').length; out.nHealth = document.querySelectorAll('#srvHealth').length;
    out.nHTabs = [...document.querySelectorAll('.hTab')].filter((e) => e.getClientRects().length && !e.closest('[hidden]')).length;
    out.zoom = r(V.querySelector('.maplibregl-ctrl-top-left .maplibregl-ctrl-group, .leaflet-top.leaflet-left .leaflet-bar'));
    out.scale = r(V.querySelector('.mChipsScale .hCard')); out.help = r(V.querySelector('.mHelp'));
    out.attrib = r(V.querySelector('.maplibregl-ctrl-attrib-button, .leaflet-control-attribution'));
    out.listChip = r(V.querySelector('.mDock[data-dock="list"] .hCard'));
    out.panel = r(V.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]'));
    out.hd = r(V.querySelector('.trkCard2[data-card="list"] .trkList .hd'));
    out.lMax = r(V.querySelector('.trkCard2[data-card="list"] .lMax')); out.lSwap = r(V.querySelector('.trkCard2[data-card="list"] .lSwap'));
    const rows = V.querySelector('.trkCard2[data-card="list"] .rows'), row = rows && rows.querySelector('.trkRow');
    out.cap = rows && rows.getClientRects().length ? rows.clientHeight / ((row && row.offsetHeight) || 44) : 0;
    out.short = V.hasAttribute('data-short'); out.vw = innerWidth; out.vh = innerHeight;
    const vv = window.visualViewport; out.vv = vv ? { t: vv.offsetTop, h: vv.height } : null;
    const cv = V.querySelector('.trkMapHost canvas, .trkMapHost .leaflet-container'); out.canvas = cv ? (() => { const b = cv.getBoundingClientRect(); return { l: b.left, r: b.right }; })() : null;
    return out; })())`;
  const hit = (a, b) => a && b && a.l < b.r - 1 && a.r > b.l + 1 && a.t < b.b - 1 && a.b > b.t + 1;
  const onScreen = (a, vw, vh) => a && a.l >= 0 && a.t >= 0 && a.r <= vw && a.b <= vh;
  const WF = `(window.__vibeMapWf ? window.__vibeMapWf() : null)`;
  try {
    ok(await radioDatalink(p, url), 'the DATALINK map is up');
    ok(await p.until(`/msg\\/min/.test(${V}?.querySelector('.trkTop .rate')?.textContent || '')`, 30000), 'the datalink report came');
    for (const [w, h] of (process.env.LS_SIZES ? process.env.LS_SIZES.split(',').map((x) => x.split('x').map(Number)) : [[932, 370], [932, 430], [852, 340], [667, 320]])) {
      const tag = `${w}×${h}`;
      await p.size(w, h, true); await sleep(1800);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(1200);
      await p.ev(`(() => { if (${V}.querySelector('.trkList').dataset.pane === 'wf') ${V}.querySelector('.lSwap')?.click(); return 1; })()`); await sleep(800);
      const R = JSON.parse(await p.ev(RECTS));
      await shot(`landscape-${w}x${h}.png`);
      ok(R.short, `${tag}: the short-landscape layout is on`);
      ok(R.x && R.x.top && R.pill && R.pill.top, `${tag}: the ✕ and the pill are visible, on top (${JSON.stringify({ x: R.x, pill: R.pill })})`);
      ok(R.panel && R.panel.l <= 1 && R.panel.t >= R.x.b - 1 && R.panel.b >= h - 2 && R.panel.b <= h, `${tag}: the drawer is a left side panel, full height under the ✕ (${JSON.stringify(R.panel)})`);
      ok(R.panel && R.panel.r - R.panel.l <= 362 && R.panel.r - R.panel.l >= 298, `${tag}: …300–360 px wide (${R.panel ? R.panel.r - R.panel.l : '-'})`);
      ok(!hit(R.panel, R.pill), `${tag}: the panel leaves the pill clear`);
      ok(R.cap >= 4, `${tag}: room for ≥ 4 rows (${R.cap.toFixed(1)})`);
      ok(R.hd && R.hd.b - R.hd.t <= 46, `${tag}: the panel's bar is one line (${R.hd ? R.hd.b - R.hd.t : '-'} px)`);
      const fam = Object.keys(R).filter((k) => k.startsWith('L:') && R[k]);
      ok(fam.length >= 3 && fam.every((k) => onScreen(R[k], w, h) && R[k].b >= h - 2 && R[k].l >= R.panel.r - 1), `${tag}: the family along the bottom edge right of the panel, all on screen (${fam.map((k) => `${k.slice(2)} ${R[k].l}-${R[k].r}/${R[k].t}-${R[k].b}`).join(', ')})`);
      ok(fam.length && fam.every((k) => R[k].top), `${tag}: …every family chip on top at its centre (${fam.filter((k) => !R[k].top).join(',') || 'all'})`);
      ok(R.nTime <= 1 && R.nHealth <= 1, `${tag}: one copy each of the time and health chips (${R.nTime} / ${R.nHealth})`);
      const right = ['R:rate', 'R:chat', 'R:time', 'R:health', 'R:map', 'zoom', 'scale', 'help', 'attrib'].filter((k) => R[k]);
      ok(right.every((k) => onScreen(R[k], w, h)), `${tag}: the right column on screen (${right.filter((k) => !onScreen(R[k], w, h)).map((k) => `${k} ${JSON.stringify(R[k])}`).join(' ') || right.join(',')})`);
      const all = ['x', 'pill', ...fam, ...right, 'lMax', 'lSwap', 'listChip'].filter((k) => R[k]);
      const clash = [];
      for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) if (hit(R[all[i]], R[all[j]])) clash.push(`${all[i]}×${all[j]}`);
      for (const k of [...fam, ...right, 'pill']) if (hit(R[k], R.panel)) clash.push(`${k}×panel`);
      ok(!clash.length, `${tag}: nothing overlaps (${clash.join(' ') || 'clear'})`);
      ok(right.every((k) => R[k].top), `${tag}: every right-column control on top at its centre (${right.filter((k) => !R[k].top).join(',') || 'all'})`);
      // the waterfall view in the same panel: signal within 3 s, the same frame
      await p.ev(`${V}.querySelector('.lSwap').click(); 1`);
      ok(await p.until(`(() => { const s = ${WF}; if (!s || s.rows < 3) return false; const c = ${V}.querySelector('.mapWf canvas'); const x = c.getContext('2d').getImageData(0, Math.round(c.height * 0.3) + 3, c.width, 1).data; let lit = 0; for (let i = 0; i < x.length; i += 4) if (x[i] + x[i + 1] + x[i + 2] > 24) lit++; return lit > c.width * 0.5; })()`, 3000), `${tag}: the waterfall shows signal within 3 s (${await p.ev(`JSON.stringify(${WF} && (({ rows, spec, lo, hi, idxMin, idxMax, lut0, binsIn, cHz, bwHz, skipped }) => ({ rows, spec, lo, hi, idxMin, idxMax, lut0, binsIn, cHz, bwHz, skipped }))(${WF}))`)})`);
      if (w === 932 && h === 370) {   // ★ Stuart 16:00: the history must never grow the panel
        const g0 = await p.ev(`JSON.stringify([${V}.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]').getBoundingClientRect(), document.documentElement.scrollHeight, document.documentElement.scrollWidth])`);
        await sleep(30000);
        const g1 = await p.ev(`JSON.stringify([${V}.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]').getBoundingClientRect(), document.documentElement.scrollHeight, document.documentElement.scrollWidth])`);
        if (!/^\[/.test(g1 || "")) console.log("   g1:", g1, await p.ev(`JSON.stringify({ open: ${V}?.querySelector(".trkCard2[data-card=\"list\"]")?.parentElement?.className, hidden: ${V}?.querySelector(".trkCard2[data-card=\"list\"]")?.hidden, view: !!${V} })`));
        const [a0, h0, w0] = JSON.parse(g0), [a1, h1, w1] = JSON.parse(g1);
        ok(['top', 'left', 'width', 'height'].every((k) => Math.abs(a0[k] - a1[k]) <= 1) && h0 === h1 && w0 === w1, `${tag}: 30 s of rows: the side panel did not grow (${Math.round(a0.height)} → ${Math.round(a1.height)} px; page ${h0} → ${h1})`);
      }
      const RW = JSON.parse(await p.ev(RECTS));
      await shot(`landscape-${w}x${h}-wf.png`);
      ok(RW.panel && R.panel && RW.panel.t === R.panel.t && RW.panel.r === R.panel.r, `${tag}: the waterfall view keeps the panel's frame`);
      ok(RW.x && RW.x.top && RW.pill && RW.pill.top, `${tag}: …the ✕ and the pill still visible`);
      await p.ev(`${V}.querySelector('.lSwap').click(); 1`); await sleep(600);
      // each family card opens upward and stays on screen
      for (const k of fam) {
        const id = k.slice(2);
        // ★ a locked listener's TUNE is dimmed and opens nothing (the ask rule) — not this test's business
        if (await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="${id}"]'); return !!b.dataset.dial && Number(getComputedStyle(b.closest('.mDock')).opacity) * Number(getComputedStyle(b).opacity) < 0.7 || !!b.closest('.dialLocked, .locked, .off'); })()`)) { console.log(`   (${tag}: ${id} is locked for this listener — skipped)`); continue; }
        await p.ev(`${V}.querySelector('[data-cbtn="${id}"]').click(); 1`); await sleep(700);
        const c = JSON.parse(await p.ev(`JSON.stringify((() => { const e = ${V}.querySelector('.trkCards .trkCard2[data-card="${id}"]'); if (!e || e.hidden) return null; const b = e.getBoundingClientRect(); return { l: Math.round(b.left), t: Math.round(b.top), r: Math.round(b.right), b: Math.round(b.bottom) }; })())`));
        ok(c && onScreen(c, w, h) && c.b >= h - 2, `${tag}: ${id}'s card opens upward from the bottom, on screen (${JSON.stringify(c)})`);
        if (id === fam[0].slice(2)) await shot(`landscape-${w}x${h}-${id}.png`);
        await p.ev(`(() => { const e = ${V}.querySelector('.trkCards .trkCard2[data-card="${id}"]'); if (e && !e.hidden) e.querySelector(':scope > .ch').click(); return 1; })()`); await sleep(500);
      }
      // collapsed: the ☰ chip at the left edge, the family right of it
      await p.ev(`${V}.querySelector('[data-cbtn="list"]').click(); 1`); await sleep(900);
      const C = JSON.parse(await p.ev(RECTS));
      ok(!C.panel && C.listChip && C.listChip.l <= 1 && fam.every((k) => C[k] && C[k].l >= C.listChip.r - 1), `${tag}: collapsed, the ☰ chip at the left edge and the family right of it`);
      await p.ev(`${V}.querySelector('[data-cbtn="list"]').click(); 1`); await sleep(700);
    }
    // ── safe areas: a notch on the left (59 px), the corner on the right (20), the home bar (21) ──
    await p.size(932, 430, true); await sleep(1200);
    await p.ev(`(() => { const v = ${V}; v.style.setProperty('--sai-l', '59px'); v.style.setProperty('--sai-r', '20px'); v.style.setProperty('--sai-b', '21px'); window.dispatchEvent(new Event('resize')); return 1; })()`); await sleep(1500);
    const S = JSON.parse(await p.ev(RECTS));
    await shot('landscape-932x430-insets.png');
    const ctl = ['x', 'listChip', 'lMax', 'lSwap', 'zoom', 'scale', 'help', 'attrib', ...Object.keys(S).filter((k) => (k.startsWith('L:') || k.startsWith('R:')) && S[k])].filter((k) => S[k]);
    const inInset = ctl.filter((k) => S[k].l < 59 - 1 || S[k].r > 932 - 20 + 1 || S[k].b > 430 - 21 + 1);
    { // ★ and anything else pressable in the map's chrome (a tucked tab, a lent chip's tab) — not only the named ones
      const stray = JSON.parse(await p.ev(`JSON.stringify([...document.querySelectorAll('.trkView:not([hidden]) button, #srvHealth button')].filter((e) => e.getClientRects().length && !e.closest('[hidden]') && getComputedStyle(e).visibility !== 'hidden' && !e.closest('.trkCards, .trkPark, .maplibregl-canvas-container, .maplibregl-marker, .maplibregl-popup')).map((e) => { const b = e.getBoundingClientRect(); return { c: (e.className || e.tagName) + '', l: Math.round(b.left), r: Math.round(b.right), b: Math.round(b.bottom) }; }).filter((b) => b.r - b.l > 2 && (b.l < 58 || b.r > 913 || b.b > 410)))`));
      ok(!stray.length, `insets: no other control in an inset area (${JSON.stringify(stray).slice(0, 400)})`);
    }
    ok(!inInset.length, `insets: no control in an inset area (${inInset.map((k) => `${k} ${JSON.stringify(S[k])}`).join(' ') || 'clear'})`);
    ok(S.canvas && S.canvas.l <= 0 && S.canvas.r >= 932, `insets: the map canvas runs edge to edge (${JSON.stringify(S.canvas)})`);
    ok(await p.ev(`/viewport-fit=cover/.test(document.querySelector('meta[name=viewport]').content)`), 'a map up: the viewport is viewport-fit=cover');
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); }
}
clearTimeout(watchdog);
console.log(`test-web-map-landscape: ${pass} passed, ${fail} failed`);
for (const k of kids) { try { k.kill('SIGTERM'); } catch {} }
await sleep(500);
process.exit(fail ? 1 : 0);
