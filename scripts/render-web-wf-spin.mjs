// render-web-wf-spin.mjs — PICTURES of the web waterfall through a run of tune steps and zoom steps (v12-wf-spin), on its
// REAL drawing path (web/client/src/waterfall.ts — WebGL or the 2D fallback) in headless Edge (--mute-audio: silent), on a
// virtual clock: an AM band (a carrier every 9 kHz, some missing), a 200 kHz view at 1052 kHz, a 10 fps feed.
//   WFSPIN_OUT=<dir> [WFSPIN_ROOT=<a tree to draw with — e.g. the code before the change>] [WFSPIN_TAG=after]
//   [WFSPIN_CASES=webgl:2:60,2d:1:60,webgl:1:120] node scripts/render-web-wf-spin.mjs
// Writes <out>/<tag>-<mode>-dpr<d>-<hz>hz-<scenario>.png. Nothing is asserted here (scripts/test_wf_spin.ts and
// scripts/test-web-wf-fan.mjs do that) — these are the evidence images.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const here = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = process.env.WFSPIN_ROOT || here;
const OUT = process.env.WFSPIN_OUT; if (!OUT) { console.log('WFSPIN_OUT=<dir> needed'); process.exit(2); }
fs.mkdirSync(OUT, { recursive: true });
const TAG = process.env.WFSPIN_TAG || 'after';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wfspin-'));
// the driver from THIS tree, the Waterfall from `root` (the driver imports ../web/client/src/waterfall)
fs.mkdirSync(path.join(root, 'scripts'), { recursive: true });
const drv = path.join(root, 'scripts', '.wfspin-driver.ts');
fs.copyFileSync(path.join(here, 'scripts/web-wf-fan-driver.ts'), drv);
await build({ entryPoints: [drv], bundle: true, format: 'iife', target: ['chrome110'], outfile: path.join(dir, 'drv.js'), logLevel: 'error' });
fs.rmSync(drv);
fs.writeFileSync(path.join(dir, 'index.html'), '<!doctype html><html><body style="margin:0;background:#000"><canvas id="wf" style="display:block"></canvas><script src="drv.js"></script></body></html>');

const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
const edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check', '--allow-file-access-from-files',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });

// the AM band: a carrier every 9 kHz from 531 kHz, ~60 % of channels on air
const carriers = []; { let s = 3; for (let f = 531000; f <= 1602000; f += 9000) { s = (s * 1103515245 + 12345) & 0x7fffffff; if (s / 0x7fffffff < 0.6) carriers.push(f); } }
const C = 1052000, SPAN = 200000;
const SCEN = {
  'a-500Hz-steps': (D, hz) => [...Array(8)].map((_, i) => [`ask(${C - (i + 1) * 500}, ${SPAN})`, `run(330, ${hz})`]).flat(),
  'b-9k-step': (D, hz) => [`ask(${C - 9000}, ${SPAN})`],
  'c-9k-fast-spin': (D, hz) => [...Array(15)].map((_, i) => [`ask(${C - (i + 1) * 9000}, ${SPAN})`, `run(125, ${hz})`]).flat(),
  'd-9k-slow-spin': (D, hz) => [...Array(6)].map((_, i) => [`ask(${C - (i + 1) * 9000}, ${SPAN})`, `run(500, ${hz})`]).flat(),
  'e-100k-step': (D, hz) => [`ask(${C - 100000}, ${SPAN})`],
  'f-zoom-sqrt2-key': (D, hz) => [`ask(${C}, ${SPAN / Math.SQRT2})`],
  'g-zoom-sqrt2-x4-quick': (D, hz) => [...Array(4)].map((_, i) => [`ask(${C}, ${SPAN / Math.pow(Math.SQRT2, i + 1)})`, `run(150, ${hz})`]).flat(),
  'h-zoom-x2-rungs': (D, hz) => [[SPAN / 2], [SPAN / 4], [SPAN / 2], [SPAN]].map(([s]) => [`ask(${C}, ${s})`, `run(700, ${hz})`]).flat(),
  'i-zoom-x4': (D, hz) => [`ask(${C}, ${SPAN / 4})`],
};
const only = process.env.WFSPIN_SCEN ? process.env.WFSPIN_SCEN.split(',') : Object.keys(SCEN);
const cases = (process.env.WFSPIN_CASES || 'webgl:2:60').split(',').map((c) => { const [m, d, h] = c.split(':'); return { mode: m, dpr: +d, hz: +h }; });
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
  for (const cs of cases) for (const name of only) {
    await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 1000, deviceScaleFactor: cs.dpr, mobile: false });
    await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
    await sleep(600);
    await ev(`__wfDrv.setup(${JSON.stringify(cs.mode)}, 1000, 600)`);
    await ev(`__wfDrv.setCarriers(${JSON.stringify(carriers)})`);
    await ev(`__wfDrv.feedMs = 100`);
    await ev(`__wfDrv.wf.applySettings({ manualRange: { minDb: -120, maxDb: -50 }, wfSharpness: 0 })`);
    await ev(`__wfDrv.ask(${C}, ${SPAN})`);
    await ev(`__wfDrv.run(4000, ${cs.hz})`);
    for (const step of SCEN[name](null, cs.hz)) await ev(`__wfDrv.${step}`);
    await ev(`__wfDrv.run(2500, ${cs.hz})`);
    const u = await ev('__wfDrv.png()');
    const f = path.join(OUT, `${TAG}-${cs.mode}-dpr${cs.dpr}-${cs.hz}hz-${name}.png`);
    fs.writeFileSync(f, Buffer.from(u.split(',')[1], 'base64'));
    console.log(`   wrote ${f}`);
  }
} catch (e) { console.log('   FAIL', e?.message || e); }
try { edge.kill(); } catch {}
fs.rmSync(dir, { recursive: true, force: true });
