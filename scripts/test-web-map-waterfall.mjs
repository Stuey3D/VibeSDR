// test-web-map-waterfall.mjs — ★★★ V12 THE MAP DRAWER'S LIVE WATERFALL (Stuart 11-27; web/client/src/mapWf.ts) in the real
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

// ═════ one radio in the DATALINK mode (a fake RTL replaying a synthetic VDL2 capture) ═════
{
  const dir = mkdir('wf');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map waterfall', locator: 'IO92nh',
    radios: [{ serial: 'mw-one', label: 'Only radio', enabled: true, configured: true, users: 2, mode: 'single', askBeforeTuning: true }] }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', CLIP, '--hf-centre', '136850000'], { stdio: 'ignore' }));
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  kids.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2'], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] }));
  ok(await up(`http://127.0.0.1:${port}/vibeserver.json`), 'the radio is up');
  const p = await browser(dir);
  try {
    // every message the page sends, for "a tap never tunes"
    await p.send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => { const s = WebSocket.prototype.send; window.__sent = []; WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return s.call(this, d); }; })();` });
    ok(await radioDatalink(p, `http://127.0.0.1:${port}/`), 'the DATALINK map is up');
    ok(await p.until(`/msg\\/min/.test(${V}?.querySelector('.trkTop .rate')?.textContent || '')`, 30000), 'the datalink report came');
    await p.size(1440, 900, false); await sleep(1500);
    const WF = `(window.__vibeMapWf ? window.__vibeMapWf() : null)`;
    ok(await p.ev(`!!${V}.querySelector('.lSwapC') && !${V}.querySelector('.lSwapC').hidden`), 'a live map over a radio offers the waterfall (the ≋ tab)');
    // ★★★ beta97 the SERVER publishes the capability per radio in vibeserver.json (the app reads it there)
    { const vj = await p.ev(`fetch('/vibeserver.json').then((r) => r.json()).then((j) => j.specPause)`); ok(vj === 1, `vibeserver.json carries "specPause":1 (${vj})`); }
    // ★★★ beta97 (Stuart): only a server that says it can pause the spectrum (hwinfo specPause) gets the swap — an older one none
    ok(await p.ev(`window.__vibeSpecPauseSet && true`) && await p.ev(`(() => { window.__vibeSpecPauseSet(false); return 1; })()`) && await p.until(`${V}.querySelector('.lSwapC').hidden && ${V}.querySelector('.lSwap').hidden`, 3000),
       'a server without specPause (pre-beta97): no ≋ on the chip, no swap tab on the drawer');
    await p.ev(`window.__vibeSpecPauseSet(true); 1`);
    ok(await p.until(`!${V}.querySelector('.lSwapC').hidden`, 3000), '…with it: the swap is back');
    // closed: no waterfall, no spectrum
    await p.ev(`(() => { const c = ${V}.querySelector('.trkCard2[data-card="list"]'); if (c && !c.hidden) c.querySelector(':scope > .ch').click(); return 1; })()`); await sleep(1200);
    ok(await p.ev(`${WF} === null && window.__vibeSpecUnderMap().off === true`), `closed: no waterfall and the spectrum is off at the server (${await p.ev(`JSON.stringify(window.__vibeSpecUnderMap())`)})`);
    const fps = async (ms) => { const a = Number(await p.ev(`window.__vibeSpecFrames()`)); await sleep(ms); return (Number(await p.ev(`window.__vibeSpecFrames()`)) - a) / (ms / 1000); };
    { const f = await fps(4000); ok(f === 0, `a map mode, nobody's waterfall open: no spectrum frames reach this listener (${f.toFixed(1)}/s)`); }
    // the collapsed chip's ≋ tab opens the drawer on the waterfall
    await p.ev(`${V}.querySelector('.lSwapC').click(); 1`);
    ok(await p.until(`!!${WF} && ${WF}.rows > 5`, 10000), `the ≋ tab opens it and rows arrive (${await p.ev(`JSON.stringify(${WF} && { rows: ${WF}.rows, spec: ${WF}.spec })`)})`);
    ok(await p.ev(`window.__sent.some(m => /"type":"spec","on":1/.test(m))`), 'the spectrum turned on as it showed ({"type":"spec","on":1})');
    { await sleep(2500); const f = await fps(5000); ok(f > 0.5 && f <= 10.5, `…LIGHT frames while it shows: ≤ 10 a second (${f.toFixed(1)}/s)`); }
    ok(await p.ev(`${V}.querySelector('.trkList').dataset.pane === 'wf' && !${V}.querySelector('.trkList .rows').getClientRects().length`), 'the drawer shows the waterfall, not the list');
    // one labelled marker per VDL2 channel, from the server's table
    const st = JSON.parse(await p.ev(`JSON.stringify(${WF})`));
    const hw = await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).json().catch(() => null);
    const band = (hw?.avdataBands || []).find((b) => b.id === 'vdl2');
    const want = band?.chans?.length || 0;
    ok(want > 1 && st.marks === want && st.labels.length >= 2 && st.labels.length === st.marks && st.labels.every((l) => /^(Ch\d+ 136\.\d{3}|\d+)$/.test(l)) && st.inside, `one marker per VDL2 channel (${st.marks} of ${want}), labelled: ${st.labels.slice(0, 3).join(' · ')}…`);
    ok(band && st.lo < Math.min(...band.chans) && st.hi > Math.max(...band.chans), `the span covers every channel (${st.lo}–${st.hi})`);
    ok(/VDL2 · \d+ channels/.test(await p.ev(`${V}.querySelector('.trkList .wfInfo').textContent`)), `the bar says it in words: "${await p.ev(`${V}.querySelector('.trkList .wfInfo').textContent`)}"`);
    { // ★ Stuart 16:00 ("the waterfall history is making the window grow by itself"): 30 s of rows, the drawer unchanged
      const g = `JSON.stringify([${V}.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]').getBoundingClientRect(), document.documentElement.scrollHeight])`;
      const [a0, h0] = JSON.parse(await p.ev(g)); await sleep(30000); const [a1, h1] = JSON.parse(await p.ev(g));
      ok(['top', 'left', 'width', 'height'].every((k) => Math.abs(a0[k] - a1[k]) <= 1) && h0 === h1, `30 s of rows: the bottom drawer did not grow (${Math.round(a0.height)} → ${Math.round(a1.height)} px)`);
    }
    { // ★★ Stuart 16:17 / 16:19 ("very dark"): the waterfall area is lit — the floor visible, the bursts brighter — and it moves,
      //    in the small drawer and maximised
      const LUM = `(() => { const c = ${V}.querySelector('.mapWf canvas'); const sh = Math.min(90, Math.round(c.height * 0.28)); const y0 = sh + 2, n = Math.min(30, c.height - y0 - 1); const d = c.getContext('2d').getImageData(0, y0, c.width, n).data; let sum = 0; const px = []; for (let i = 0; i < d.length; i += 4) { const l = (d[i] + d[i + 1] + d[i + 2]) / 3; sum += l; px.push(l); } px.sort((a, b) => a - b); return JSON.stringify({ mean: sum / (d.length / 4), med: px[Math.floor(px.length / 2)], top: px[Math.floor(px.length * 0.995)], sig: (() => { let h = 0; for (let i = 0; i < d.length; i += 4) h = (h * 31 + d[i] + 7 * d[i + 1] + 13 * d[i + 2]) % 1000000007; return h; })() }); })()`;
      for (const max of [false, true]) {
        if (max) { await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(1500); }
        await sleep(3000);
        const a = JSON.parse(await p.ev(LUM)); await sleep(2500); const b = JSON.parse(await p.ev(LUM));
        ok(a.mean > 18 && a.mean < 200, `${max ? 'maximised' : 'the drawer'}: the waterfall is lit, not near-black (mean ${a.mean.toFixed(0)})`);
        ok(a.top > a.med + 25, `${max ? 'maximised' : 'the drawer'}: the bursts stand out from the floor (the brightest 0.5 % ${a.top.toFixed(0)} vs median ${a.med.toFixed(0)})`);
        ok(a.sig !== b.sig, `${max ? 'maximised' : 'the drawer'}: …and it moves`);
        if (max) { // ★ Stuart 16:25: beside a desktop's maximised drawer the ? and the scale stay (shifted, never hidden)
          const vis = await p.ev(`JSON.stringify(['.mHelp', '.mChipsScale .hCard'].map((q) => { const e = ${V}.querySelector(q); if (!e) return null; const b = e.getBoundingClientRect(); const L = ${V}.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]').getBoundingClientRect(); return { vis: getComputedStyle(e).visibility !== 'hidden' && !${V}.classList.contains('stackAway') && b.width > 0, clear: b.left >= L.right - 1 || b.bottom <= L.top + 1 }; }))`);
          ok(JSON.parse(vis).every((x) => x && x.vis && x.clear), `maximised (1440): the ? and the scale stay visible, clear of the drawer (${vis})`);
        }
        if (max) { await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(1500); }
      }
    }
    { // ★ a miniature of the page's waterfall: the page's palette, changed on the page → the next rows use the new LUT
      const lut0 = await p.ev(`${WF}.lut0`);
      const other = await p.ev(`(() => { const s = document.getElementById('palette'); const o = [...s.options].map(x => x.value).find(v => v !== s.value && !/custom/i.test(v)); s.value = o; s.dispatchEvent(new Event('change')); return o; })()`);
      ok(await p.until(`${WF}.lut0 !== ${JSON.stringify(lut0)}`, 3000), `the page's palette changed to ${other}: the mini waterfall's newest rows use its LUT (${lut0} → ${await p.ev(`${WF}.lut0`)})`);
    }
    // passive: a tap and a drag on it send nothing that tunes
    await p.ev(`window.__sent.length = 0; 1`);
    const r = JSON.parse(await p.ev(`JSON.stringify(${V}.querySelector('.mapWf').getBoundingClientRect())`));
    const x = r.left + r.width / 2, y = r.top + r.height / 2;
    for (const [type, dx] of [['mousePressed', 0], ['mouseMoved', 40], ['mouseReleased', 40], ['mousePressed', 0], ['mouseReleased', 0]]) await p.send('Input.dispatchMouseEvent', { type, x: x + dx, y, button: 'left', clickCount: 1 });
    await sleep(1500);
    const sent = await p.ev(`JSON.stringify(window.__sent.filter(m => !/"type":"(ping|fftRate|spec)"/.test(m)))`);
    ok(sent === '[]', `a tap and a drag on the waterfall send nothing (${sent})`);
    ok(await p.ev(`!!${WF}`), '…and it is still showing (the tap did not close or swap it)');
    // swap to the list in place: the spectrum stops
    await p.ev(`${V}.querySelector('.lSwap').click(); 1`);
    ok(await p.until(`${WF} === null && window.__vibeSpecUnderMap().off === true`, 4000), 'the top-left tab swaps to the list; the spectrum goes off');
    ok(await p.ev(`${V}.querySelector('.trkList').dataset.pane === 'list' && !${V}.querySelector('.trkCard2[data-card="list"]').hidden`), '…in place: the drawer stayed open, now the list');
    // back: the history kept (Stuart 11-27), a gap line for the time hidden, no rows taken meanwhile
    const rowsAt = Number(await p.ev(`window.__vibeMapWfRowsLast || 0`));
    await p.ev(`${V}.querySelector('.lSwap').click(); 1`);
    ok(await p.until(`!!${WF}`, 4000), 'and back to the waterfall');
    ok(await p.ev(`${WF}.gaps === 1`), `…its history kept, the gap marked (${await p.ev(`JSON.stringify({ rows: ${WF}.rows, gaps: ${WF}.gaps })`)})`);
    void rowsAt;
    { // ★★ Stuart 11-27: the history is kept for the TALLEST the drawer can be — rows filled in the SMALL drawer beyond what
      //    it can show appear when it is maximised, and restoring keeps them
      const lit = (a) => (a || []).filter((v) => v > 24).length;
      const smallH = Number(await p.ev(`(() => { const c = ${V}.querySelector('.mapWf canvas'); return c.height - Math.min(90, Math.round(c.height * 0.28)); })()`));
      ok(await p.until(`(${WF}?.histRows || 0) > ${smallH + 40}`, 60000), `rows beyond the small drawer's ${smallH} kept (${await p.ev(`${WF}?.histRows`)} of ${await p.ev(`${WF}?.histCap`)})`);
      await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(1500);
      const deep = JSON.parse(await p.ev(`JSON.stringify(window.__vibeMapWfStrip(${smallH + 20}))`));
      ok(lit(deep) > 32, `maximised: the older rows (row ${smallH + 20}, beyond the small view) appear (lit ${lit(deep)} of 64)`);
      await p.ev(`${V}.querySelector('.lMax').click(); 1`); await sleep(1500);
      const near = JSON.parse(await p.ev(`JSON.stringify(window.__vibeMapWfStrip(10))`));
      ok(lit(near) > 32, `restored: the history is still drawn (lit ${lit(near)} of 64)`);
    }
    // tabs: not on each other, the maximise tab, the ? / scale stack or another card
    for (const [w, h, m] of [[390, 844, true], [375, 667, true], [1440, 900, false]]) {
      await p.size(w, h, m); await sleep(1500);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(900);
      const box = await p.ev(`JSON.stringify((() => { const r = (s) => { const e = ${V}.querySelector(s); if (!e || !e.getClientRects().length) return null; const b = e.getBoundingClientRect(); return { l: b.left, r: b.right, t: b.top, b: b.bottom }; }; return { sw: r('.lSwap'), mx: r('.lMax'), help: r('.mHelp'), scale: r('.mChipsScale .hCard') }; })())`);
      const B = JSON.parse(box), hit = (a, b) => a && b && a.l < b.r - 1 && a.r > b.l + 1 && a.t < b.b - 1 && a.b > b.t + 1;
      ok(B.sw && B.mx && !hit(B.sw, B.mx) && !hit(B.sw, B.help) && !hit(B.sw, B.scale) && !hit(B.mx, B.help) && !hit(B.mx, B.scale) && B.sw.l >= 0 && B.mx.r <= w, `${w}×${h}: the swap tab and ⌃ clear of each other and the ? / scale stack (${box})`);
    }
    { /* ★★★ beta97 hotfix (Stuart 11-28): NO spectrum trace in the mini view — the waterfall only, its history from the box's
       *  top, whatever the main view's SHOW / HIDE */
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); if (${V}.querySelector('.trkList').dataset.pane !== 'wf') ${V}.querySelector('.lSwap').click(); return 1; })()`);
      await p.until(`!!${WF} && ${WF}.rows > 3`, 8000);
      const t = await p.ev(`${WF}?.trace`);
      const top = JSON.parse(await p.ev(`JSON.stringify(window.__vibeMapWfStrip(1))`)) || [];
      ok(t === false && top.filter((v) => v > 24).length > 16, `no trace in the mini view: the waterfall from its top (trace ${t}, ${top.filter((v) => v > 24).length}/64 drawn)`);
    }
    { // rotation keeps it: a phone turned on its side (the side panel)
      await p.size(390, 844, true); await sleep(1500);
      await p.ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); if (${V}.querySelector('.trkList').dataset.pane !== 'wf') ${V}.querySelector('.lSwap').click(); return 1; })()`); await sleep(1500);
      const h0 = Number(await p.ev(`${WF}?.histRows || 0`));
      await p.size(844, 390, true); await sleep(2000);
      const h1 = Number(await p.ev(`${WF}?.histRows || 0`));
      const deep = JSON.parse(await p.ev(`JSON.stringify(window.__vibeMapWfStrip(40))`));
      ok(h0 > 50 && h1 >= h0 && (deep || []).filter((v) => v > 24).length > 32, `rotated (390×844 → 844×390): the history kept (${h0} → ${h1} rows, drawn)`);
    }
    await p.size(1440, 900, false); await sleep(800);
    // closing the drawer stops it
    await p.ev(`${V}.querySelector('[data-cbtn="list"]').click(); 1`);
    ok(await p.until(`${WF} === null && window.__vibeSpecUnderMap().off === true`, 4000), 'the drawer shut: the waterfall and the spectrum stop');
    { await sleep(1500); const f = await fps(3000); ok(f === 0, `…and no frames reach it (${f.toFixed(1)}/s)`); }
    { // leave the map (the radio page again): normal frames come back
      await p.ev(`${V}.querySelector('.trkTop .exit').click(); 1`); await sleep(1500);
      await p.ev(`(() => { const b = [...document.querySelectorAll('button')].find((x) => /^(EXIT|LEAVE)/i.test(x.textContent.trim()) && x.getClientRects().length); b && b.click(); return 1; })()`);
      await sleep(4000);
      const f = await fps(4000);
      ok(f > 1.5, `leaving the map: the radio page's spectrum flows again (${f.toFixed(1)}/s; map ${await p.ev(`!!${V}`)})`);
    }
    ok(!p.errs.length, `no page errors (${p.errs.slice(0, 2).join(' | ')})`);
  } catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
  finally { p.close(); }
}
clearTimeout(watchdog);
console.log(`test-web-map-waterfall: ${pass} passed, ${fail} failed`);
for (const k of kids) { try { k.kill('SIGTERM'); } catch {} }
await sleep(500);
process.exit(fail ? 1 : 0);
