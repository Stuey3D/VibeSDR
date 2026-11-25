#!/usr/bin/env node
// shot-web-skyplot.mjs — ★★★ beta94 THE SKY PLOT'S TWO VIEWS on the web page, through a REAL vibeserver with a seeded gallery
// (vibeserver/tool-skyplot-fixtures --dirs: SYNTHETIC dots, the cells built by the server's own vibe_skyplot.h), headless
// Edge (--mute-audio: silent). The gallery's SKY PLOT row → the dome:
//   · plenty of data: AUTO = the HEATMAP (paths, no track dots; the line "heatmap · 30 days · … strongest east (az 70–110°)"),
//     TRACKS = dots, HEATMAP again;
//   · three passes only: AUTO = TRACKS.
// Pictures to OUTDIR (skyplot-heatmap-auto.png, skyplot-heatmap-tracks.png, skyplot-few-auto.png).
// usage: VIBESERVER_BIN=… SKYFIX_BIN=… node scripts/shot-web-skyplot.mjs OUTDIR   (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', FIX = process.env.SKYFIX_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const OUT = process.argv[2];
if (!OUT || !BIN || !fs.existsSync(BIN) || !FIX || !fs.existsSync(FIX) || !fs.existsSync(EDGE)) { console.log('   not run — OUTDIR, VIBESERVER_BIN, SKYFIX_BIN and Edge needed'); process.exit(3); }
fs.mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

async function run(tag, seedIdx, steps) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-skyshot-${tag}-`));
  for (const d of ['data', 'run', 'edge', 'data/gallery', 'other']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  const dirs = [path.join(dir, 'data/gallery'), path.join(dir, 'other')];
  if (seedIdx === 1) dirs.reverse();
  execFileSync(FIX, ['--dirs', dirs[0], dirs[1]], { stdio: 'ignore' });
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Sky plot pictures', locator: 'IO92nh', adminPass: 'skyshot-admin',
    radios: [{ serial: 'skyshot', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2400000 }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  await sleep(500);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '137500000', '--mode', 'nfm', '--users', '4'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=600,1000', 'about:blank'], { stdio: 'ignore' });
  try {
    await sleep(3000);
    const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
    const c = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
    let id = 0; const pend = new Map();
    c.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    await new Promise((r) => { c.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); c.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(OUT, name), Buffer.from(s.result.data, 'base64')); };
    await send('Page.enable'); await send('Runtime.enable');
    const look = { prefs: {} }, sz = { w: 600, h: 1000, mobile: false };
    await send('Emulation.setDeviceMetricsOverride', { width: sz.w, height: sz.h, deviceScaleFactor: 1, mobile: sz.mobile });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await until(`document.readyState === 'complete'`, 15000);
    await ev(`(() => { try { const p = JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}'); for (const k of ['ctlBg','ctlBtn','ctlFont','ctlSolid']) delete p[k];
                Object.assign(p, ${JSON.stringify(look.prefs)}); localStorage.setItem('vibesdr_web_prefs_v1', JSON.stringify(p)); } catch {} return true; })()`);
    await send('Page.reload');
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    // ★ START until the splash has gone (a press during its fade-in is ignored)
    for (let k = 0; k < 10; k++) {
      await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); b && b.click(); return !!b; })()`);
      if (await until(`!!document.querySelector('#audioGate button') || !!document.getElementById('galleryBtn')?.offsetParent`, 1500)) break;
    }
    // ★ the audio gate wants a real press (a synthetic click is not a user gesture) — pressed until it has gone
    for (let k = 0; k < 8; k++) {
      await until(`!!document.querySelector('#audioGate button')?.offsetParent`, 1500);
      const g = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button')?.offsetParent ? document.querySelector('#audioGate button').getBoundingClientRect() : null)`) || 'null');
      if (!g) { if (k > 1) break; continue; }
      const x = g.x + g.width / 2, y = g.y + g.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
      await sleep(700);
    }
    ok(await until(`!!document.getElementById('galleryBtn')?.offsetParent`, 15000), `${tag}: the page is up`);
    await ev(`document.getElementById('galleryBtn').click()`);
    const found = await until(`!!document.querySelector('.galSky')`, 15000);
    if (!found) { console.log('   (debug) ' + String(await ev(`fetch('/vibeserver/skyplot.json?kind=orbcomm').then(r => r.text()).then(t => t.slice(0, 200) + ' len ' + t.length)`))
                  + ' | ' + fs.readFileSync(path.join(dir, 'server.log'), 'utf8').split('\n').filter((l) => /gallery|skyplot/i.test(l)).slice(0, 4).join(' / ')); await shot(`skyplot-${tag}-FAILED.png`); }
    ok(found, `${tag}: the gallery's SKY PLOT row`);
    await ev(`document.querySelector('.galSky').click()`);
    ok(await until(`!!document.querySelector('#galSky svg')`, 10000), `${tag}: the dome drawn`);
    await sleep(500);
    await steps({ ev, until, shot });
    c.close();
  } catch (e) { console.log(`   driver: ${e && e.stack || e}`); fail++; }
  finally { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(1500); try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 }); } catch {} }
}
const state = `(() => { const s = document.querySelector('#galSky svg'); return JSON.stringify({ heat: s ? s.querySelectorAll('path[data-heat]').length : -1,
  dots: s ? s.querySelectorAll('circle').length : -1, on: document.querySelector('#galSky .sky-modes button.on')?.dataset.v || '',
  sum: document.querySelector('#galSky .galSkySum')?.textContent || '' }); })()`;
const deadline = setTimeout(() => { console.log('   deadline'); process.exit(1); }, 300000);
await run('heat', 0, async ({ ev, until, shot }) => {
  let s = JSON.parse(await ev(state));
  ok(s.on === 'auto' && s.heat >= 5 && s.dots <= 4 && /^heatmap · 30 days · [\d,]+ cells heard · strongest east \(az 70–110°\) · [\d,]+ dots/.test(s.sum), `AUTO: the heatmap (${s.heat} paths, ${s.dots} circles) — ${s.sum}`);
  await shot('skyplot-heatmap-auto.png');
  await ev(`document.querySelector('#galSky .sky-modes button[data-v="tracks"]').click()`);
  await until(`!!document.querySelector('#galSky .sky-modes button.on[data-v="tracks"]')`, 8000); await sleep(400);
  s = JSON.parse(await ev(state));
  ok(s.heat === 0 && s.dots > 1000, `TRACKS: dots, no cells (${s.dots} circles)`);
  await shot('skyplot-heatmap-tracks.png');
  await ev(`document.querySelector('#galSky .sky-modes button[data-v="heatmap"]').click()`);
  await until(`!!document.querySelector('#galSky .sky-modes button.on[data-v="heatmap"]')`, 8000);
  s = JSON.parse(await ev(state));
  ok(s.heat >= 5, 'HEATMAP again');
});
await run('few', 1, async ({ ev, shot }) => {
  const s = JSON.parse(await ev(state));
  ok(s.on === 'auto' && s.heat === 0 && s.dots > 300 && /^363 dots/.test(s.sum), `three passes: AUTO = tracks (${s.dots} circles) — ${s.sum}`);
  await shot('skyplot-few-auto.png');
});
clearTimeout(deadline);
console.log(`\nshot-web-skyplot: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
