// test-web-known-signals.mjs — KNOWN-SIGNAL LABELS on the REAL web waterfall (web/client/src/waterfall.ts _drawKnown,
// src/utils/knownSignals.ts), in headless Edge (--mute-audio: silent), driven by scripts/web-wf-fan-driver.ts (a
// synthetic band on a virtual clock) with carriers at the labelled frequencies.
// Stuart (2026-10-30): "those labels should sit just in the spectrum so as to not overcrowd the waterfall, only in the
// waterfall when spectrum is disabled".
//   • 4.17–4.71 MHz: DSC / NAVTEX, DWD's RTTY at 4583, Northwood's WEFAX at 4610 — labelled, in the spectrum, none in the
//     waterfall below it; LISTEN's box is never drawn over;
//   • 14.06–14.10 MHz: FT8 / FT4 / JS8 / WSPR with their spans;
//   • spectrum hidden: the labels at the top of the waterfall; toggle off: none at all.
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

  // ── 4. off ──
  await ev(`(__wfDrv.wf.showSpec = true, __wfDrv.wf.knownSignals = null, __wfDrv.wf.knownGen++, true)`); await ev('__wfDrv.run(1000)');
  ok((await lilac(0, (await geo()).H)) === 0, 'toggle off: nothing drawn');
  // ── cost: a steady view lays out once ──
  await ev(`(() => { const w = __wfDrv.wf; w.knownSignals = __known.KNOWN_SIGNALS; w.knownGen++; return true; })()`);
  await ev('__wfDrv.run(200)');
  const k1 = await ev('__wfDrv.wf.knownKey'); await ev('__wfDrv.run(500)'); const k2 = await ev('__wfDrv.wf.knownKey');
  ok(k1 === k2, 'a steady view keeps its cached layout (no re-layout per frame)');
} catch (e) {
  console.log('   FAIL', e?.message || e); fail++;
} finally {
  clearTimeout(deadline);
  try { edge.kill(); } catch {}
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
}
