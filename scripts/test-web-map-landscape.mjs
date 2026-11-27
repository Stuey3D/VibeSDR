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
    for (const d of V.querySelectorAll('.mChipsR > .mDock, .mChipsR > .edgeDock:not(#srvHealth)')) out['R:' + (d.dataset.dock || d.id)] = r(d.classList.contains('tucked') ? d.querySelector('.hTab') : d.querySelector('.hCard'));
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
      { const lim = await p.ev(`(() => { const t = document.getElementById('rxTimeLeft'); return !!t && !t.hidden && /\\d/.test(t.textContent || ''); })()`);
        ok(!!R['R:time'] === !!lim, `${tag}: the TIME tab shows exactly when a limit is in force for this listener (limit ${lim}, tab ${!!R['R:time']})`); }
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
      { const st = JSON.parse(await p.ev(`JSON.stringify(${WF})`)); ok(st && st.marks > 0 && st.labels.length === st.marks && st.inside, `${tag}: every channel keeps a label (${st && st.labels.length} of ${st && st.marks}: ${st && st.labels.join(' ')})`); }
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
    // ── ★★ a real iPhone 17 Pro Max on its side in Safari (Stuart 17:0x): 932×370, iOS reporting the inset on BOTH sides ──
    for (const island of ['left', 'right']) { await p.size(932, 370, true); await sleep(1200);
      await p.ev(`document.documentElement.dataset.island = '${island}'; 1`);
      await p.ev(`(() => { const v = ${V}; v.style.setProperty('--sai-l', '59px'); v.style.setProperty('--sai-r', '59px'); v.style.setProperty('--sai-b', '21px'); window.dispatchEvent(new Event('resize')); return 1; })()`); await sleep(1800);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); if (${V}.querySelector('.trkList').dataset.pane === 'wf') ${V}.querySelector('.lSwap').click(); return 1; })()`); await sleep(1200);
      const D = JSON.parse(await p.ev(RECTS));
      await shot(`landscape-932x370-device-island-${island}.png`);
      const vw = await p.ev(`Math.round(window.visualViewport ? visualViewport.width : innerWidth)`);
      // ★ Stuart 17:1x: the EDGE pieces sit on the physical edge, their content in by the inset (the app's EdgeChip)
      const edge = JSON.parse(await p.ev(`JSON.stringify([...document.querySelectorAll('.trkView:not([hidden]) .mChipsR > .edgeDock:not([hidden]), .trkView:not([hidden]) .mChipsR > #srvHealth')].map((d) => { const show = [...d.querySelectorAll(':scope > .hCard, :scope > .hTab')].find((e) => getComputedStyle(e).visibility !== 'hidden' && e.getBoundingClientRect().width > 0); if (!show) return null; const b = show.getBoundingClientRect(); const kids = [...show.querySelectorAll('*')].filter((k) => k.getClientRects().length && !k.children.length && getComputedStyle(k).visibility !== 'hidden'); const cr = Math.max(...kids.map((k) => k.getBoundingClientRect().right), 0); return { id: d.dataset.dock || d.id, r: Math.round(b.right), cr: Math.round(cr) }; }).filter(Boolean))`));
      ok(edge.length >= 2 && edge.every((e) => e.r >= vw - 1 && e.cr <= vw - 59 + 1), `device 932×370 (island ${island}): every right-edge chip on the physical edge, its content in by the inset (${JSON.stringify(edge)})`);
      const stray = JSON.parse(await p.ev(`JSON.stringify([...document.querySelectorAll('.trkView:not([hidden]) .mHelp, .trkView:not([hidden]) .mChipsScale .hCard, .trkView:not([hidden]) .maplibregl-ctrl-group, .trkView:not([hidden]) .maplibregl-ctrl-attrib-button')].filter((e) => e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden').map((e) => { const b = e.getBoundingClientRect(); return { c: String(e.className || e.tagName).slice(0, 30), l: Math.round(b.left), r: Math.round(b.right) }; }).filter((b) => b.r - b.l > 2 && (b.l < 58 || b.r > ${vw} - 58)))`));
      ok(!stray.length, `device 932×370 (island ${island}): no free-standing control (?, scale, zoom, credits) in an inset (${JSON.stringify(stray).slice(0, 400)})`);
      const tabs = JSON.parse(await p.ev(`JSON.stringify(['#srvHealth', '.trkView:not([hidden]) [data-dock="time"]'].map((q) => { const d = document.querySelector(q); if (!d || d.hidden || !d.getClientRects().length) return 0; return [...d.querySelectorAll(':scope > .hCard, :scope > .hTab')].filter((e) => getComputedStyle(e).visibility !== 'hidden' && e.getBoundingClientRect().width > 0).length; }))`));
      ok(tabs.every((n) => n <= 1), `device 932×370 (island ${island}): health and time each show once — card or tab, never both (${tabs.join(' / ')})`);
      { const bar = JSON.parse(await p.ev(`JSON.stringify((() => { const hd = ${V}.querySelector('.trkCard2[data-card="list"] .trkList .hd').getBoundingClientRect(), ch = ${V}.querySelector('.mDock[data-dock="list"] .hCard').getBoundingClientRect(), tb = ${V}.querySelector('.trkCard2[data-card="list"] .trkList .hd .tabWrap'); const t = tb && tb.getClientRects().length ? tb.getBoundingClientRect() : null; return { hdT: Math.round(hd.top), hdB: Math.round(hd.bottom), chT: Math.round(ch.top), chB: Math.round(ch.bottom), chR: Math.round(ch.right), tT: t ? Math.round(t.top) : null, tL: t ? Math.round(t.left) : null }; })())`));
        ok(bar.hdB - bar.hdT <= 46 && bar.chT >= bar.hdT - 2 && bar.chB <= bar.hdB + 2 && (bar.tT == null || (bar.tT >= bar.hdT - 1 && bar.tL >= bar.chR - 1)), `device 932×370 (home bar 21): the drawer's bar is ONE row — ☰ n ⌄ then the tabs, level (${JSON.stringify(bar)})`); }
      ok(!!D.scale, `device 932×370 (island ${island}): the scale strip is there (${JSON.stringify(D.scale)})`);
      ok(D.zoom && D.zoom.b >= 370 - 50 && D.zoom.r - D.zoom.l > D.zoom.b - D.zoom.t, `device 932×370 (island ${island}): the zoom is a horizontal pair on the bottom edge (${JSON.stringify(D.zoom)})`);
      const all = ['x', 'pill', 'zoom', 'scale', 'help', 'attrib', 'listChip', 'lMax', 'lSwap', ...Object.keys(D).filter((k) => (k.startsWith('L:') || k.startsWith('R:')) && D[k])].filter((k) => D[k]);
      const clash = []; for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) if (hit(D[all[i]], D[all[j]])) clash.push(`${all[i]}×${all[j]}`);
      for (const k of all) if (D[k] && D.panel && !['x', 'listChip', 'lMax', 'lSwap'].includes(k) && hit(D[k], D.panel)) clash.push(`${k}×panel`);
      ok(!clash.length, `device 932×370 (island ${island}): nothing overlaps (${clash.join(' ') || 'clear'})`);
      await p.ev(`(() => { const v = ${V}; for (const k of ['--sai-l', '--sai-r', '--sai-b']) v.style.removeProperty(k); return 1; })()`);
    }
    // ── ★ the no-regression check (Stuart: "portrait perfect"): 430×932 with the portrait insets — not the short layout ──
    { await p.size(430, 932, true); await sleep(1200);
      await p.ev(`(() => { const v = ${V}; v.style.setProperty('--sai-t', '59px'); v.style.setProperty('--sai-b', '34px'); v.style.setProperty('--sai-l', '0px'); v.style.setProperty('--sai-r', '0px'); window.dispatchEvent(new Event('resize')); return 1; })()`); await sleep(1500);
      const P = JSON.parse(await p.ev(RECTS));
      await shot('portrait-430x932-insets.png');
      ok(!P.short, 'portrait 430×932: not the short layout');
      { const c = JSON.parse(await p.ev(`JSON.stringify((() => { const d = ${V}.querySelector('.mDock[data-dock="list"]'); const h = d.querySelector('.hCard').getBoundingClientRect(); const kids = [...d.querySelectorAll('.mchip *')].filter((k) => k.getClientRects().length && !k.children.length); return { open: d.classList.contains('open'), frameB: Math.round(h.bottom), contentB: Math.round(Math.max(...kids.map((k) => k.getBoundingClientRect().bottom))) }; })())`));
        ok(c.open || (c.frameB >= 932 - 1 && c.contentB <= 932 - 34 + 1), `portrait 430×932: the collapsed LIST chip's frame on the bottom edge, its content above the 34 px inset (${JSON.stringify(c)})`); }
      const all = ['x', 'pill', 'zoom', 'scale', 'help', 'listChip', ...Object.keys(P).filter((k) => (k.startsWith('L:') || k.startsWith('R:')) && P[k])].filter((k) => P[k]);
      const clash = []; for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) if (hit(P[all[i]], P[all[j]])) clash.push(`${all[i]}×${all[j]}`);
      ok(!clash.length, `portrait 430×932: nothing overlaps (${clash.join(' ') || 'clear'})`);
      await p.ev(`(() => { const v = ${V}; for (const k of ['--sai-l', '--sai-r', '--sai-b', '--sai-t']) v.style.removeProperty(k); return 1; })()`);
    }
    // ── ★★ ANDROID (Stuart: "our servers get a lot of Androids using browsers"): Chrome's UA (no iOS island rule — env() as
    //    reported), a punch-hole inset at the top in portrait / the side in landscape, gesture navigation at the bottom ──
    await p.send('Emulation.setUserAgentOverride', { userAgent: 'Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36' });
    for (const [name, w, h, ins] of [['pixel7', 412, 915, { t: 32, b: 24, l: 0, r: 0 }], ['pixel7', 915, 412, { t: 0, b: 24, l: 32, r: 0 }],
                                     ['small', 360, 740, { t: 28, b: 20, l: 0, r: 0 }], ['small', 740, 360, { t: 0, b: 20, l: 28, r: 0 }],
                                     ['galaxyS', 384, 854, { t: 30, b: 24, l: 0, r: 0 }], ['galaxyS', 854, 384, { t: 0, b: 24, l: 0, r: 30 }]]) {
      const tag = `android ${name} ${w}×${h}`;
      await p.size(w, h, true); await sleep(1200);
      await p.ev(`(() => { const v = ${V}; v.style.setProperty('--sai-t', '${ins.t}px'); v.style.setProperty('--sai-b', '${ins.b}px'); v.style.setProperty('--sai-l', '${ins.l}px'); v.style.setProperty('--sai-r', '${ins.r}px'); window.dispatchEvent(new Event('resize')); return 1; })()`); await sleep(1500);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(1000);
      const A = JSON.parse(await p.ev(RECTS));
      await shot(`${name}-${w}x${h}.png`);
      ok(!(await p.ev(`document.documentElement.dataset.island || ''`)), `${tag}: no iOS island rule on Android`);
      const all = ['x', 'pill', 'zoom', 'scale', 'help', 'attrib', 'listChip', ...Object.keys(A).filter((k) => (k.startsWith('L:') || k.startsWith('R:')) && A[k])].filter((k) => A[k]);
      const clash = []; for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) if (hit(A[all[i]], A[all[j]])) clash.push(`${all[i]}×${all[j]}`);
      ok(!clash.length, `${tag}: nothing overlaps (${clash.join(' ') || 'clear'})`);
      ok(all.filter((k) => k !== 'pill').every((k) => onScreen(A[k], w, h)), `${tag}: every control on screen (${all.filter((k) => k !== 'pill' && !onScreen(A[k], w, h)).join(',') || 'all'})`);
      ok(!A.hd || A.hd.b - A.hd.t <= 46, `${tag}: the drawer's bar is one line (${A.hd ? A.hd.b - A.hd.t : '-'} px)`);
      if (w > h) ok(A.short && A.panel && A.panel.b >= h - 2, `${tag}: short landscape — the drawer a side panel`);
      await p.ev(`(() => { const v = ${V}; for (const k of ['--sai-l', '--sai-r', '--sai-b', '--sai-t']) v.style.removeProperty(k); return 1; })()`);
    }
    await p.send('Emulation.setUserAgentOverride', { userAgent: '' });
    // ── ★★ Stuart 17:1x: the TIME card — urgency brings it out, a › (a real touch) tucks it, it STAYS tucked while the clock runs,
    //    and only the next, more urgent stage brings it out again (the app's SDRScreen timeOut rule) ──
    { await p.size(932, 370, true); await sleep(800);
      const TD = `${V}.querySelector('[data-dock="time"]')`;
      const st = async () => JSON.parse(await p.ev(`JSON.stringify((() => { const d = ${TD}; return d ? { hidden: d.hidden, tucked: d.classList.contains('tucked'), num: d.querySelector('.tNum')?.textContent || '' } : null; })())`));
      await p.ev(`window.__vibeSessionLeft(100); 1`); await sleep(1500);
      const a = await st();
      ok(a && !a.hidden && !a.tucked, `TIME: under 2 minutes the card comes out (${JSON.stringify(a)})`);
      const r = JSON.parse(await p.ev(`JSON.stringify(${TD}.querySelector('.hTuck').getBoundingClientRect())`));
      const x = r.left + r.width / 2, y = r.top + r.height / 2;
      await p.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
      await p.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      await sleep(800);
      const b = await st();
      ok(b && b.tucked, `TIME: a tap on its › tucks it (${JSON.stringify(b)})`);
      await sleep(10000);
      const c = await st();
      ok(c && c.tucked && c.num !== b.num, `TIME: …and it stays tucked for 10 s while the countdown runs (${b.num} → ${c.num})`);
      await p.ev(`window.__vibeSessionLeft(400); 1`); await sleep(1500);
      const d0 = await st();
      ok(d0 && d0.tucked, 'TIME: a calmer stage leaves it tucked');
      await p.ev(`window.__vibeSessionLeft(100); 1`); await sleep(1500);
      const e = await st();
      ok(e && !e.tucked, `TIME: the next, more urgent stage brings it out again (${JSON.stringify(e)})`);
    }
    // ── safe areas: a notch on the left (59 px), the corner on the right (20), the home bar (21) ──
    await p.size(932, 430, true); await sleep(1200);
    await p.ev(`(() => { const v = ${V}; v.style.setProperty('--sai-l', '59px'); v.style.setProperty('--sai-r', '20px'); v.style.setProperty('--sai-b', '21px'); window.dispatchEvent(new Event('resize')); return 1; })()`); await sleep(1500);
    const S = JSON.parse(await p.ev(RECTS));
    await shot('landscape-932x430-insets.png');
    // ★ Stuart 17:1x: EDGE pieces (✕, chips, tabs, the drawer) sit on the physical edge with their content padded in — checked
    //   in the device case above; here the FREE-STANDING controls stand clear of the insets
    const ctl = ['zoom', 'scale', 'help', 'attrib', 'lMax', 'lSwap'].filter((k) => S[k]);
    const inInset = ctl.filter((k) => S[k].l < 59 - 1 || S[k].r > 932 - 20 + 1 || S[k].b > 430 - 21 + 1);
    ok(!inInset.length, `insets: no free-standing control in an inset area (${inInset.map((k) => `${k} ${JSON.stringify(S[k])}`).join(' ') || 'clear'})`);
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
