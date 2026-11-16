// test-web-tuned-label.mjs — THE TUNED STATION'S LABEL AT THE VFO on the web waterfall (web/client/src/waterfall.ts
// _drawTuned, src/utils/tunedStation.ts) on its REAL drawing path, in headless Edge (--mute-audio: silent), driven by
// scripts/web-tuned-label-driver.ts. Stuart (2026-11-15): "When the VTS disappears can we have a label on the VFO with the
// current station name".
//   • at a phone (390 px, dpr 3 → 2), a tablet (768) and a desktop (1400), the spectrum on and off, with the needle in
//     the middle and at both edges: the label is drawn, wholly on screen, CENTRED on the needle (Stuart, 2026-11-16:
//     "centre the frequency and bookmark reading on the VFO") or shifted inward at an edge, in ONE box with LISTEN's
//     readout under it;
//   • no label = nothing drawn. Pictures to TUNEDLABEL_PNG=<dir>.
//   node scripts/test-web-tuned-label.mjs
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
const PNG = process.env.TUNEDLABEL_PNG || '';
if (PNG) fs.mkdirSync(PNG, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-tunedlabel-'));
await build({ entryPoints: [path.join(root, 'scripts/web-tuned-label-driver.ts')], bundle: true, format: 'iife', target: ['chrome110'],
              outfile: path.join(dir, 'drv.js'), logLevel: 'error' });
fs.writeFileSync(path.join(dir, 'index.html'), '<!doctype html><html><body style="margin:0;background:#000"><canvas id="wf" style="display:block"></canvas><script src="drv.js"></script></body></html>');

const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
const edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader',
  '--enable-unsafe-swiftshader', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  '--allow-file-access-from-files', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (180 s)'); try { edge.kill(); } catch {} process.exit(1); }, 180000);

const overlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
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

  const SIZES = [{ name: 'phone', w: 390, h: 700, dpr: 3 }, { name: 'tablet', w: 768, h: 900, dpr: 2 }, { name: 'desktop', w: 1400, h: 900, dpr: 1 }];
  const NAME = 'China Radio Int', LABEL = '[EiBi] ' + NAME;
  for (const sz of SIZES) for (const spec of [0.3, 0]) {
    await send('Emulation.setDeviceMetricsOverride', { width: sz.w, height: sz.h, deviceScaleFactor: sz.dpr, mobile: sz.w < 800 });
    await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
    for (let i = 0; i < 50 && !(await ev('typeof __tl !== "undefined"')); i++) await sleep(100);
    const palette = spec ? 'default' : 'gqrx';
    await ev(`__tl.setup(${sz.w}, ${sz.h}, ${JSON.stringify(palette)}, ${spec}, '#ff8c1a')`);
    const tag = `${sz.name} ${sz.w}px, spectrum ${spec ? 'on' : 'off'}`;
    for (const [where, hz] of [['middle', 9_580_000], ['right edge', 9_647_500], ['left edge', 9_552_500]]) {
      const d = await ev(`__tl.draw(${hz}, ${JSON.stringify(NAME)})`);
      const t = d.tuned;
      ok(!!t && t.text === LABEL, `${tag}, needle at the ${where}: drawn`);
      if (!t) continue;
      ok(t.x >= 0 && t.x + t.w <= d.W && t.y >= 0 && t.y + t.h <= d.H, `   …wholly on screen (x ${t.x.toFixed(0)}–${(t.x + t.w).toFixed(0)} of ${d.W})`);
      const mid = t.x + t.w / 2;
      ok(where === 'middle' ? Math.abs(mid - d.nX) < 1 : t.x <= d.nX + 1 && d.nX <= t.x + t.w + 1,
         where === 'middle' ? `   …centred on the needle (box middle ${mid.toFixed(1)}, needle ${d.nX.toFixed(1)})`
                            : `   …shifted inward at the ${where}, still across the needle (${t.x.toFixed(0)}–${(t.x + t.w).toFixed(0)}, needle ${d.nX.toFixed(0)})`);
      ok(d.box?.lines?.length === 2 && d.box.lines[1] === d.readout, `   …one box: the name over LISTEN's readout ("${d.box?.lines?.join(' / ')}")`);
      if (PNG && where !== 'left edge') {
        const u = await ev('__tl.png()');
        fs.writeFileSync(path.join(PNG, `${sz.name}-${spec ? 'spectrum' : 'nospectrum'}-${where.replace(' ', '-')}.png`), Buffer.from(u.split(',')[1], 'base64'));
      }
    }
    const none = await ev(`__tl.draw(9580000, '')`);
    ok(none.tuned === null, `${tag}: no label, nothing drawn`);
    // ★ a supplement, never a repeat: the spectrum's own label for the bookmark is drawn → no "[User] Flex FM" under it
    const dup = await ev(`__tl.draw(9580000, 'Flex FM', 'user', true)`);
    ok(dup.tuned?.text === 'Flex FM' && dup.highlight >= 0, `${tag}: the bookmark's own spectrum label MOVES into the VFO box — "Flex FM", no "[User]" (highlight ${dup.highlight})`);
    ok(dup.placed.filter((l) => l === 'Flex FM').length === 1 && dup.box?.sigIdx === dup.highlight, `${tag}: …drawn once, the box standing for it (a tap still finds it)`);
    ok(dup.box && Math.abs(dup.box.lx + dup.box.w / 2 - dup.nX) < 1, `${tag}: …centred on the needle`);
    if (PNG && sz.name === 'phone' && spec) { const u = await ev('__tl.png()'); fs.writeFileSync(path.join(PNG, 'phone-spectrum-no-duplicate.png'), Buffer.from(u.split(',')[1], 'base64')); }
    const solo = await ev(`__tl.draw(9580000, 'Flex FM', 'user', false)`);
    ok(solo.tuned?.text === '[User] Flex FM' && solo.highlight === -1, `${tag}: signal labels off — the bookmark gets the label, nothing highlighted`);
  }
  wsC.close();
} catch (e) { fail++; console.log('   FAIL ' + (e?.message || e)); }
clearTimeout(deadline);
try { edge.kill(); } catch {}
await sleep(500);
try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* Edge may still be closing its profile */ }
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
