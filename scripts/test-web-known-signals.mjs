// test-web-known-signals.mjs — KNOWN-SIGNAL LABELS on the REAL web waterfall (web/client/src/waterfall.ts _drawKnown,
// src/utils/knownSignals.ts), in headless Edge (--mute-audio: silent), driven by scripts/web-wf-fan-driver.ts (a
// synthetic band on a virtual clock) with carriers at the labelled frequencies.
// Stuart (2026-10-30): "those labels should sit just in the spectrum so as to not overcrowd the waterfall, only in the
// waterfall when spectrum is disabled".
//   • 4.17–4.71 MHz: DSC / NAVTEX, DWD's RTTY at 4583, Northwood's WEFAX at 4610 — labelled, in the spectrum, none in the
//     waterfall below it; LISTEN's box is never drawn over;
//   • 14.06–14.10 MHz: FT8 / FT4 / JS8 / WSPR with their spans;
//   • spectrum hidden: the labels at the top of the waterfall; toggle off: none at all.
//   • the FM band: a personal bookmark, a server-saved one and learnt RDS names (Stuart's follow-ups, 2026-10-30).
//   • TAPPED — the REAL BUILT PAGE (web/dist, `node scripts/build-web.mjs` first) against scripts/mock-vibeserver.mjs
//     (Stuart, 2026-10-30: "can they be tapped to tune and chose the decoder?"): a trusted click on DWD's label tunes
//     4582 kHz USB (what reached the "server", GET /debug/controls) and opens RTTY with 50 Bd / 450 Hz lit; a drag that
//     starts on a label pans the view and tunes nothing; a click beside the labels still tunes to the point.
// Pictures to KNOWN_PNG=<dir>.   node scripts/test-web-known-signals.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const PNG = process.env.KNOWN_PNG || '';
if (PNG) fs.mkdirSync(PNG, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-known-'));
// the driver, plus the labels module on the page's window
fs.writeFileSync(path.join(dir, 'entry.ts'), `import '${path.join(root, 'scripts/web-wf-fan-driver.ts')}';
import * as K from '${path.join(root, 'src/utils/knownSignals.ts')}';
(window as any).__known = K;`);
await build({ entryPoints: [path.join(dir, 'entry.ts')], bundle: true, format: 'iife', target: ['chrome110'],
              outfile: path.join(dir, 'drv.js'), logLevel: 'error' });
fs.writeFileSync(path.join(dir, 'index.html'), '<!doctype html><html><body style="margin:0;background:#000"><canvas id="wf" style="display:block"></canvas><script src="drv.js"></script></body></html>');

const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
const edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output',
  '--use-angle=swiftshader', '--enable-unsafe-swiftshader', `--remote-debugging-port=${dbg}`, '--no-first-run',
  '--no-default-browser-check', '--allow-file-access-from-files', `--user-data-dir=${path.join(dir, 'edge')}`,
  '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (300 s)'); try { edge.kill(); } catch {} process.exit(1); }, 300000);

try {
  await sleep(2500);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  const wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 600)); return r.result?.result?.value; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1200, height: 700, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
  for (let i = 0; i < 50 && !(await ev('typeof __wfDrv !== "undefined"')); i++) await sleep(100);
  ok(await ev(`__wfDrv.setup('webgl', 1200, 700, 'default', false)`), 'the real waterfall, WebGL');
  const shot = async (name) => { if (!PNG) return; const u = await ev('__wfDrv.png()'); fs.writeFileSync(path.join(PNG, `${name}.png`), Buffer.from(u.split(',')[1], 'base64')); };
  // lilac (#d9b8ff) pixels between rows y0..y1: r > 190, b > 230, g 160..205
  const lilac = (y0, y1) => ev(`(() => { const c = document.getElementById('wf'); const d = c.getContext('2d').getImageData(0, ${y0}, c.width, ${y1 - y0}).data;
    let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 190 && d[i + 2] > 230 && d[i + 1] > 160 && d[i + 1] < 205) n++; return n; })()`);
  const geo = () => ev(`(() => { const w = __wfDrv.wf; return { H: w.canvas.height, specH: w.canvas.height - w.wf.height, placed: w.knownPlaced.map(p => ({ label: p.sig.label, x: p.x, lx: p.lx, w: p.w, row: p.row })) }; })()`);
  // the needle back on (the driver hides it), the spectrum on, and the labels: the curated table + two EiBi rows
  await ev(`(() => { const w = __wfDrv.wf; delete w._drawVfo; w.setSpecRatio(0.4); w.showSpec = true;
    const eibi = __known.eibiKnownSignals([{ name: 'VFF Iqaluit Radio', frequency: 4177500, comment: '-TY', source: 'eibi' },
                                           { name: 'Shannon HFDL (ID7)', frequency: 2998000, comment: '-HF', source: 'eibi' }], 600);
    w.knownSignals = __known.knownSignalList(eibi); w.knownGen++; return true; })()`);

  // ── 1. 4.17–4.71 MHz ──
  await ev(`__wfDrv.setCarriers([4177500, 4207500, 4208000, 4208500, 4209000, 4209500, 4583000, 4610000, 4400000])`);
  await ev(`(__wfDrv.wf.vfoHz = 4400000, __wfDrv.wf.filterLow = 0, __wfDrv.wf.filterHigh = 2800, true)`);
  await ev(`__wfDrv.ask(4440000, 540000)`); await ev('__wfDrv.run(3000)');
  let g = await geo();
  const labels = g.placed.map(p => p.label);
  console.log('      placed:', labels.join(' | '));
  ok(labels.includes('RTTY · DWD') && labels.includes('WEFAX · Northwood'), 'DWD 4583 RTTY and Northwood 4610 WEFAX are labelled');
  ok(labels.some(l => l.startsWith('NAVTEX') || l.startsWith('DSC')), 'the 4.2 MHz maritime cluster gets its safety label');
  ok(!labels.includes('RTTY · Iqaluit'), 'an EiBi label 30 kHz from the safety cluster gives way to it (lower priority)');
  ok(g.placed.length <= 12 && g.placed.every(p => p.row < 2), `${g.placed.length} labels, on two rows at most`);
  const inSpec = await lilac(0, g.specH), inWf = await lilac(g.specH + 2, g.H);
  ok(inSpec > 200 && inWf === 0, `drawn in the spectrum only (${inSpec} lilac px there, ${inWf} in the waterfall)`);
  // LISTEN's box (row 0, right of the needle) is never under a label
  const nX = (4400000 - (4440000 - 270000)) / 540000 * 1200;
  ok(!g.placed.some(p => p.row === 0 && p.lx < nX + 6 + 90 && p.lx + p.w > nX), 'nothing drawn over LISTEN on its row');
  await shot('1-4.17-4.71MHz-spectrum');

  // ── 1a. EiBi on its own: VFF Iqaluit (-TY) ──
  await ev(`__wfDrv.ask(4185000, 60000)`); await ev('__wfDrv.run(2500)');
  g = await geo();
  ok(g.placed.some(p => p.label === 'RTTY · Iqaluit'), `an EiBi teletype station on air is labelled (${g.placed.map(p => p.label).join(', ')})`);
  await shot('1a-eibi-iqaluit-spectrum');

  // ── 1b. zoomed in on 4.5–4.7 MHz ──
  await ev(`__wfDrv.ask(4600000, 200000)`); await ev(`(__wfDrv.wf.vfoHz = 4608100, true)`); await ev('__wfDrv.run(2500)');
  g = await geo();
  ok(g.placed.map(p => p.label).join() === 'WEFAX · Northwood,RTTY · DWD' || (g.placed.length === 2), `4.5–4.7 MHz: ${g.placed.map(p => p.label).join(', ')}`);
  await shot('2-4.5-4.7MHz-spectrum');

  // ── 2. 20 m digital ──
  await ev(`__wfDrv.setCarriers([14074800, 14075600, 14076400, 14081200, 14079000, 14097100, 14070900])`);
  await ev(`(__wfDrv.wf.vfoHz = 14074000, __wfDrv.wf.filterLow = 0, __wfDrv.wf.filterHigh = 3000, true)`);
  await ev(`__wfDrv.ask(14082000, 40000)`); await ev('__wfDrv.run(3000)');
  g = await geo();
  const dl = g.placed.map(p => p.label);
  console.log('      placed:', dl.join(' | '));
  ok(['FT8', 'FT4', 'JS8', 'WSPR'].every(m => dl.includes(m)), 'FT8, FT4, JS8 and WSPR labelled on 20 m');
  await shot('3-20m-digital-spectrum');

  // ── 3. spectrum hidden: the top of the waterfall ──
  await ev(`(__wfDrv.wf.showSpec = false, true)`); await ev('__wfDrv.run(1500)');
  g = await geo();
  const top = await lilac(0, 160), below = await lilac(200, g.H);
  ok(g.specH === 0 && top > 100 && below === 0, `spectrum hidden: at the top of the waterfall (${top} px in the top rows, ${below} below)`);
  await shot('4-20m-digital-no-spectrum');

  // ── 3b. bookmarks: the FM band with learnt RDS names, a personal and a server-saved bookmark ──
  await ev(`(() => { const w = __wfDrv.wf; w.showSpec = true;
    const bms = __known.bookmarkSignals(
      [{ name: 'Radio 2 (mine)', frequency: 89100000, mode: 'wfm' }],
      [{ name: 'Club FM relay', frequency: 94000000, mode: 'wfm', source: 'server', manual: true },
       { name: 'BBC R2', frequency: 89100000, mode: 'wfm', source: 'server', manual: false, pi: 0xC202 },
       { name: 'Classic FM', frequency: 100900000, mode: 'wfm', source: 'server', manual: false, pi: 0xC203 },
       { name: 'Heart', frequency: 96400000, mode: 'wfm', source: 'server', manual: false, pi: 0xC3A1 },
       { name: 'BBC R4', frequency: 93500000, mode: 'wfm', source: 'server', manual: false, pi: 0xC204 },
       { name: 'PI4322 98.0MHz', frequency: 98000000, mode: 'wfm', source: 'server', manual: false, pi: 0x4322, nameSrc: 0 }], false);
    w.knownSignals = __known.knownSignalList([], bms); w.knownGen++; return true; })()`);
  await ev(`__wfDrv.setCarriers([89100000, 93500000, 94000000, 96400000, 98000000, 100900000])`);
  await ev(`(__wfDrv.wf.vfoHz = 100900000, __wfDrv.wf.filterLow = -100000, __wfDrv.wf.filterHigh = 100000, true)`);
  await ev(`__wfDrv.ask(95000000, 14000000)`); await ev('__wfDrv.run(3000)');
  g = await geo();
  const fl = g.placed.map(p => p.label);
  console.log('      placed:', fl.join(' | '));
  ok(['Radio 2 (mine)', 'Club FM relay', 'Classic FM', 'Heart', 'BBC R4'].every(l => fl.includes(l)),
     'FM: the personal and server-saved bookmarks and the learnt RDS names are labelled');
  ok(!fl.includes('BBC R2') && !fl.some(l => l.startsWith('PI4322')), 'the personal name wins its channel; a provisional PI name is not a label');
  await shot('5-fm-bookmarks-learnt-rds');

  // ── 4. off ──
  await ev(`(__wfDrv.wf.showSpec = true, __wfDrv.wf.knownSignals = null, __wfDrv.wf.knownGen++, true)`); await ev('__wfDrv.run(1000)');
  ok((await lilac(0, (await geo()).H)) === 0, 'toggle off: nothing drawn');
  // ── cost: a steady view lays out once ──
  await ev(`(() => { const w = __wfDrv.wf; w.knownSignals = __known.KNOWN_SIGNALS; w.knownGen++; return true; })()`);
  await ev('__wfDrv.run(200)');
  const k1 = await ev('__wfDrv.wf.knownKey'); await ev('__wfDrv.run(500)'); const k2 = await ev('__wfDrv.wf.knownKey');
  ok(k1 === k2, 'a steady view keeps its cached layout (no re-layout per frame)');

  // ── 5. TAPPED: the real page against the mock server ──
  if (!fs.existsSync(path.join(root, 'web/dist/vibesdr.html'))) { console.log('   (tap part not run — build the page first: node scripts/build-web.mjs)'); }
  else await tapPart(send, ev);
} catch (e) {
  console.log('   FAIL', e?.message || e); fail++;
} finally {
  clearTimeout(deadline);
  try { edge.kill(); } catch {}
  try { mock?.kill(); } catch {}
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
}

var mock = null;
async function tapPart(send, ev) {
  console.log('tapped (the built page, the mock server)');
  const port = await freePort();
  mock = spawn('nice', ['-n', '10', process.execPath, path.join(root, 'scripts/mock-vibeserver.mjs'), '--no-pin', '--port', String(port)], { stdio: 'ignore' });
  await sleep(800);
  const controls = async () => (await fetch(`http://127.0.0.1:${port}/debug/controls`)).json();
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 200) { if (await ev(x)) return true; await sleep(200); } return !!(await ev(x)); };
  const mouse = async (type, x, y) => send('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1 });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 860, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://localhost:${port}/index.html?join` });
  await sleep(4500);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  // ★ START RADIO needs a trusted click (the audio gate) — the waterfall plays out in step with the audio
  if (await until(`!!document.querySelector('#audioGate button')`, 8000)) {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    await mouse('mousePressed', r.x + r.width / 2, r.y + r.height / 2); await mouse('mouseReleased', r.x + r.width / 2, r.y + r.height / 2);
    await sleep(1500);
  }
  ok(await until(`!!(window.__vibeWf && window.__vibeWf())`, 15000), 'the page is up (its waterfall and spectrum client)');
  // the labels on, the spectrum shown, the dial on 4.6 MHz in AM (the tap must bring USB) and the view 4.5–4.7 MHz (as the controls do it)
  await ev(`(() => { const { wf, spec } = window.__vibeWf(); spec.tune(4600000, 'am'); spec.zoom(4600000, 200000 / (spec.cfg?.binCount || 4096)); return true; })()`);
  const placedQ = `(() => { const { wf } = window.__vibeWf(); return (wf.knownPlaced || []).map(p => p.sig.label).join('|'); })()`;
  ok(await until(`${placedQ}.includes('RTTY · DWD')`, 10000), `DWD's label is drawn (${await ev(placedQ)})`);
  // a label's box centre in CSS px on the page (the waterfall's own geometry — _drawKnown)
  const boxAt = (label) => ev(`(() => { const { wf } = window.__vibeWf(); const c = wf.canvas, r = c.getBoundingClientRect();
    const p = wf.knownPlaced.find(q => q.sig.label === ${JSON.stringify(label)}); if (!p) return null;
    const k = c.width / c.clientWidth, ky = c.height / c.clientHeight, dpr = c.width / c.clientWidth;
    const y = wf.markerLabelY(c.height, p.row) - 9 * dpr + 6.5 * dpr;
    return { x: r.left + (p.lx + p.w / 2) / k, y: r.top + y / ky }; })()`);
  const decOpen = `(() => { const t = document.getElementById('decTitle'); return document.getElementById('decBox').classList.contains('open') ? t.textContent : ''; })()`;
  const lit = (host, attr, v) => ev(`!!document.querySelector('#${host} [data-${attr}="${v}"]')?.classList.contains('on')`);

  // ── a DRAG that starts on a label pans and tunes nothing ──
  const before = await controls();
  const c0 = await ev(`window.__vibeWf().spec.viewCenterHz()`);
  let b = await boxAt('WEFAX · Northwood') || await boxAt('RTTY · DWD');
  await mouse('mousePressed', b.x, b.y);
  for (let i = 1; i <= 6; i++) { await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: b.x + i * 15, y: b.y, button: 'left', buttons: 1 }); await sleep(30); }
  await mouse('mouseReleased', b.x + 90, b.y);
  await sleep(700);
  const c1 = await ev(`window.__vibeWf().spec.viewCenterHz()`);
  const after = await controls();
  ok(Math.abs(c1 - c0) > 5000, `a drag starting on a label pans the view (centre ${(c0 / 1e3).toFixed(1)} → ${(c1 / 1e3).toFixed(1)} kHz)`);
  ok(JSON.stringify(after.tune) === JSON.stringify(before.tune) && !(await ev(decOpen)), 'and tunes nothing, opens nothing');
  // back to the view, labels re-laid out
  await ev(`(() => { const { spec } = window.__vibeWf(); spec.zoom(4600000, 200000 / (spec.cfg?.binCount || 4096)); return true; })()`);
  await until(`${placedQ}.includes('RTTY · DWD')`, 6000);
  await sleep(600);

  // ── a CLICK on DWD's label: 4582 USB, RTTY open at 50 Bd / 450 Hz ──
  b = await boxAt('RTTY · DWD');
  ok(!!b, 'DWD\'s label is on the page');
  await mouse('mousePressed', b.x, b.y); await mouse('mouseReleased', b.x, b.y);
  let tuned = null;
  for (let i = 0; i < 20; i++) { await sleep(200); tuned = (await controls()).tune; if (tuned?.frequency === 4582000) break; }
  ok(tuned?.frequency === 4582000, `tuned to 4582 kHz (the dial 1 kHz below DWD's 4583) — the server got ${tuned?.frequency}`);
  const modeNow = await ev(`window.__vibeWf().spec.mode`);
  ok(modeNow === 'usb' && tuned?.mode === 'usb', `in USB, in the same tune message, from AM (page ${modeNow}, server ${tuned?.mode})`);
  ok(await until(`${decOpen} === 'RTTY'`, 4000), `the RTTY decoder is open ("${await ev(decOpen)}")`);
  ok(await lit('rttyBaud', 'baud', '50') && await lit('rttyShift', 'shift', '450') && await lit('rttyPreset', 'preset', 'weather'),
     'at 50 Bd / 450 Hz shift — the WEATHER preset lit');
  ok(await ev(`document.getElementById('rttyInv').classList.contains('on')`), 'polarity reversed, as the WEATHER preset (DWD heard in USB)');

  // ── a click BESIDE the labels still tunes to the point ──
  // ★ high in the spectrum, well clear of the label rows and of the decoder box now open over the waterfall
  const pt = await ev(`(() => { const { wf } = window.__vibeWf(); const r = wf.canvas.getBoundingClientRect(); return { x: r.left + r.width * 0.3, y: r.top + r.height * 0.12 }; })()`);
  ok(await ev(`document.elementFromPoint(${pt.x}, ${pt.y})?.id === 'wf'`), 'the point beside the labels is the waterfall canvas itself');
  const want = await ev(`(() => { const { wf } = window.__vibeWf(); const r = wf.canvas.getBoundingClientRect(); return wf.xyToHz(r.width * 0.3, r.height * 0.12); })()`);
  await mouse('mousePressed', pt.x, pt.y); await mouse('mouseReleased', pt.x, pt.y);
  await sleep(700);
  const t2 = (await controls()).tune?.frequency;
  ok(t2 && t2 !== 4582000 && Math.abs(t2 - want) < 5000, `a click away from the labels tunes to the point (${t2} ≈ ${Math.round(want)})`);
}
