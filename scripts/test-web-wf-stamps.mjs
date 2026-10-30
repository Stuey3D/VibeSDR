// test-web-wf-stamps.mjs — THE WEB WATERFALL'S SCALE STAMPS (src/utils/wfStamps.ts, web/client/src/waterfall.ts _drawStamps)
// on its REAL drawing path (WebGL, then the 2D fallback), in headless Edge (--mute-audio: silent), driven by the same page
// as test-web-wf-fan.mjs (scripts/web-wf-fan-driver.ts — a synthetic band on a virtual clock).
// Stuart (2026-10-30, #57): "zoom in and a stamp of the previous frequency scale as it was is added onto the old waterfall
// position and it gently drops off the bottom of the screen, same with tuning too if needed".
//   • a zoom and a tune each place ONE stamp of the old scale, drawn at its rows (not above them), scrolling with them;
//   • a rapid zoom run places one, never crowded (the minimum gap, the cap of 3); a resize keeps them at their rows;
//   • they drop off the bottom. Pictures to WFSTAMP_PNG=<dir>.
//   node scripts/test-web-wf-stamps.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const PNG = process.env.WFSTAMP_PNG || '';
if (PNG) fs.mkdirSync(PNG, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wfstamp-'));
await build({ entryPoints: [path.join(root, 'scripts/web-wf-fan-driver.ts')], bundle: true, format: 'iife', target: ['chrome110'],
              outfile: path.join(dir, 'drv.js'), logLevel: 'error' });
fs.writeFileSync(path.join(dir, 'index.html'), '<!doctype html><html><body style="margin:0;background:#000"><canvas id="wf" style="display:block"></canvas><script src="drv.js"></script></body></html>');

const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
// ★ WFFAN_GPU=1: the Mac's own GPU (for the CPU figures — SwiftShader draws WebGL on the CPU and swamps them)
const GL_FLAGS = process.env.WFFAN_GPU ? ['--use-angle=metal', '--enable-gpu'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
const edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', ...GL_FLAGS,
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check', '--allow-file-access-from-files',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (600 s)'); try { edge.kill(); } catch {} process.exit(1); }, 600000);

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
  await send('Page.enable');

  for (const MODE of (process.env.WFSTAMP_MODES || 'webgl,2d').split(',')) {
    console.log(`   ── ${MODE} renderer ──`);
    await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 1000, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
    for (let i = 0; i < 50 && !(await ev('typeof __wfDrv !== "undefined"')); i++) await sleep(100);
    ok(await ev(`__wfDrv.setup(${JSON.stringify(MODE)}, 1000, 600, 'default', true)`), `drawing with the ${MODE} renderer, stamps on`);
    const D = (x) => ev(`__wfDrv.${x}`);
    const shot = async (name) => { if (!PNG) return; const u = await ev('__wfDrv.png()'); fs.writeFileSync(path.join(PNG, `${MODE}-${name}.png`), Buffer.from(u.split(',')[1], 'base64')); };
    // the stamp is drawn in the VFO colour (white when that clashes with the palette — an orange VFO over gqrx's orange
    // carriers): a magenta one here, which the gqrx palette never draws, then count magenta pixels on rows y0..y1
    await ev(`__wfDrv.wf.vfoColor = '#ff40ff', true`);
    const amberAt = async (y0, y1) => ev(`(() => { const c = document.getElementById('wf'); const d = c.getContext('2d').getImageData(0, ${y0}, c.width, ${y1 - y0}).data;
      let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 150 && d[i + 2] > 150 && d[i + 1] < 120) n++; return n; })()`);
    await D('run(4000)');
    let s = await D('stampInfo()');
    ok(s.stamps.length === 0, 'steady: no stamps');
    // ZOOM IN ×2 about the dial
    await D('ask(7150000, 48000)'); await D('run(2500)');
    s = await D('stampInfo()');
    ok(s.stamps.length === 1 && Math.abs(s.stamps[0].span - 96000) < 1 && Math.abs(s.stamps[0].lo - 7102000) < 1,
       `a zoom in places ONE stamp of the old scale (${s.stamps.map(x => `${x.lo}+${x.span}`).join(', ')})`);
    let y = s.top + (s.rowSeq - s.stamps[0].frame);
    ok(await amberAt(y, y + 15) > 40, `its labels are drawn at its rows (y ${y})`);
    const inBand = await amberAt(y, y + 15), above = await amberAt(Math.max(s.top, y - 17), y - 2);
    ok(inBand > 4 * Math.max(1, above), `and not above them (${inBand} magenta px in the band, ${above} in the rows above)`);
    await shot('1-zoom-in');
    // TUNE by a fifth of the view
    await D('ask(7159600, 48000)'); await D('run(2500)');
    s = await D('stampInfo()');
    ok(s.stamps.length === 2 && Math.abs(s.stamps[0].lo - 7126000) < 1, `a tune of 1/5 of the view places a second (${s.stamps.map(x => x.lo).join(', ')})`);
    y = s.top + (s.rowSeq - s.stamps[1].frame);
    const y0 = y;
    await shot('2-tune');
    // it scrolls WITH the history
    await D('run(1000)');
    s = await D('stampInfo()');
    const y1 = s.top + (s.rowSeq - s.stamps[1].frame);
    ok(y1 > y0 && await amberAt(y1, y1 + 15) > 40, `the older stamp moved down with its rows (${y0} → ${y1})`);
    // A RAPID ZOOM RUN: 10 steps 100 ms apart → ONE more stamp, at the pre-run scale
    const before = s.stamps.length;
    await D('sweep(10, 0, 0.9, 100)'); await D('run(2500)');
    s = await D('stampInfo()');
    ok(s.stamps.length <= 3 && Math.abs(s.stamps[0].span - 48000) < 1, `a rapid zoom run: one stamp, the pre-run scale (${s.stamps.length} total, newest span ${s.stamps[0]?.span})`);
    const ys = s.stamps.map(x => s.rowSeq - x.frame);
    ok(ys.every((v, i) => i === 0 || v - ys[i - 1] >= s.minGap), `none closer than the minimum gap (${ys.join(', ')}; gap ${s.minGap})`);
    await shot('3-zoom-run');
    void before;
    // A BAND CHANGE (7 → 14 MHz, a jump): the history is KEPT as drawn and stamped; a tap on an old row tunes by ITS scale
    await D('ask(14150000, 48000)'); await D('run(2500)');
    s = await D('stampInfo()');
    ok(Math.abs(s.stamps[0].lo + s.stamps[0].span / 2 - 7159600) < 2000, `a band change: stamped with the 7 MHz scale the old rows were drawn at (${s.stamps[0].lo}+${s.stamps[0].span})`);
    { const yy = s.rowSeq - s.stamps[0].frame + 4;   // css px (dpr 1) under the stamp's top, on the old rows
      const tap = await ev(`__wfDrv.wf.xyToHz(250, ${s.top + yy}, 1000, 600)`);
      const want = s.stamps[0].lo + 0.25 * s.stamps[0].span;
      ok(Math.abs(tap - want) < 50, `a tap on an old 7 MHz row tunes to ITS frequency (${tap.toFixed(0)}, want ${want.toFixed(0)})`);
      const live = await ev(`__wfDrv.wf.xyToHz(250, ${s.top + 2}, 1000, 600)`), liveX = await ev(`__wfDrv.wf.xToHz(250, 1000)`);
      ok(Math.abs(live - liveX) < 1, `a tap on the newest row: the live scale, as before (${live.toFixed(0)})`); }
    await shot('4-band-change');
    // RESIZE (a rotation-like change): the stamp stays at its rows
    await D('resize(800, 500)'); await D('run(300)');
    s = await D('stampInfo()');
    if (s.stamps.length) { const yy = s.top + (s.rowSeq - s.stamps[0].frame);
      ok(yy < s.top + s.H ? await amberAt(yy, yy + 15) > 20 : true, `after a resize the newest stamp is still drawn at its rows (y ${yy})`); }
    await shot('5-resized');
    // they drop off the bottom
    await D('run(40000)');
    s = await D('stampInfo()');
    ok(s.stamps.length === 0, 'they drop off the bottom and are pruned');
  }
} catch (e) { console.log('   FAIL', e?.message || e); fail++; }
clearTimeout(deadline);
try { edge.kill(); } catch {}
fs.rmSync(dir, { recursive: true, force: true });
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
