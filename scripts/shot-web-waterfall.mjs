// shot-web-waterfall.mjs — PICTURES of the web waterfall through a tune and a zoom on a REAL band, to compare by eye with the
// reference recording of the first native build (~/Desktop/ScreenRecording_06-10-2026 00-02-18_1.mov: whole rows bend on a
// tune, trails are smooth continuous curves, rows blend softly, the whole history fans on a zoom with no black).
// Not a pass/fail test — scripts/test-web-waterfall-history.mjs is the gate. This makes the pictures for a person to judge.
//
// A real 40 m capture (~/VibeSDR-test-audio/iq/v4l_40m_7150000_2400k.cu8, 2.4 MS/s) replayed by the fake rtl_tcp into the
// REAL server, drawn by headless Edge (--mute-audio; tests are silent):
//   1-slow-sweep   the dial swept 60 kHz down in 500 Hz steps (a drum spin)
//   2-fast-tune    3 kHz every 60 ms (a fast spin — the "chunky broken diagonal lines")
//   3-zoom-in      240 → 40 kHz in wheel-sized steps
//   4-zoom-out     40 → 240 kHz in wheel-sized steps
//   5-compare      (WFSHOT_COMPARE=1) a SECOND listener that never zooms, shot at the same moment as 4-zoom-out and set
//                  beside it: the detailed full view vs the view whose zoomed-in history is filled by the full-capture
//                  layer — they must read as the same waterfall, the filled one only less sharp
//   VIBESERVER_BIN=…/vibeserver node --max-old-space-size=512 scripts/shot-web-waterfall.mjs <out-dir> [webgl|2d]
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const CLIP = path.join(os.homedir(), 'VibeSDR-test-audio/iq/v4l_40m_7150000_2400k.cu8');
const OUT = process.argv[2];
const MODE = process.argv[3] || 'webgl';
if (!OUT || !BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE) || !fs.existsSync(CLIP)) { console.log('   not run — out dir, VIBESERVER_BIN, Edge and the 40 m clip needed'); process.exit(3); }
fs.mkdirSync(OUT, { recursive: true });
const CENTRE = 7150000, SPAN = 240000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wfshot-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Waterfall pictures', locator: 'IO92nh', adminPass: 'wfshot-web-admin',
  radios: [{ serial: 'wfshot-web', label: 'Test radio', enabled: true, configured: true, users: 2 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000',
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(600);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(CENTRE), '--mode', 'lsb', '--users', '2'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=600,1000', 'about:blank'], { stdio: 'ignore' });
let wsC, code = 0;
const deadline = setTimeout(() => { console.log('   deadline'); try { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {} process.exit(1); }, 240000);
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  // ★ One driver per page — the second (WFSHOT_COMPARE) is a listener that never zooms.
  const drive = async (wsUrl) => {
    const c = new WebSocket(wsUrl);
    let id = 0; const pend = new Map();
    c.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    await new Promise((r) => { c.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); c.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Emulation.setDeviceMetricsOverride', { width: 600, height: 1000, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button')?.getBoundingClientRect() || null)`) || 'null');
    if (r) { const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 }); }
    await until(`!!window.__vibeWf()`, 15000);
    if (MODE === '2d') await ev(`(() => { const w = window.__vibeWf().wf; w.gl = null; w.glCanvas = null; w.wf.width = 0; w.resize(); return true; })()`);
    const shot = async (name) => {
      const q = JSON.parse(await ev(`JSON.stringify((() => { const c = document.getElementById('wf').getBoundingClientRect(); return { x: c.x, y: c.y, w: c.width, h: c.height }; })())`));
      const sh = await send('Page.captureScreenshot', { format: 'png', clip: { x: q.x, y: q.y, width: q.w, height: Math.min(q.h, 900), scale: 1 } });
      const file = path.join(OUT, `${name}-${MODE}.png`);
      fs.writeFileSync(file, Buffer.from(sh.result.data, 'base64'));
      console.log(`   wrote ${file}`);
      return file;
    };
    return { c, send, ev, until, shot };
  };
  const A = await drive(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  wsC = A.c;
  const { ev, shot } = A;
  let B = null;
  if (process.env.WFSHOT_COMPARE) {
    // ★ A separate browser CONTEXT: the receiver serves one listener per browser (it would say "already listening").
    const ver = await (await fetch(`http://127.0.0.1:${dbg}/json/version`)).json();
    const bw = new WebSocket(ver.webSocketDebuggerUrl);
    await new Promise((r) => { bw.onopen = r; });
    let bid = 0; const bp = new Map();
    bw.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && bp.has(m.id)) { bp.get(m.id)(m); bp.delete(m.id); } };
    const bsend = (method, params = {}) => new Promise((res) => { const i = ++bid; bp.set(i, res); bw.send(JSON.stringify({ id: i, method, params })); });
    const ctx = (await bsend('Target.createBrowserContext')).result.browserContextId;
    const tid = (await bsend('Target.createTarget', { url: 'about:blank', browserContextId: ctx })).result.targetId;
    B = await drive(`ws://127.0.0.1:${dbg}/devtools/page/${tid}`);
  }
  await ev(`(() => { const { spec } = window.__vibeWf(); spec.followVfo = true; spec.tune(${CENTRE}); spec.zoom(${CENTRE}, ${SPAN} / (spec.cfg.binCount || 1024)); return true; })()`);
  const tune = (f) => ev(`window.__vibeWf().spec.tune(${f})`);
  const zoom = (f, span) => ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${f}, ${span} / (s.cfg.binCount || 1024)); return true; })()`);
  await sleep(9000);
  let f = CENTRE;
  for (let i = 0; i < 120; i++) { f -= 500; await tune(f); await sleep(50); }
  await sleep(2500); await shot('1-slow-sweep');
  for (let i = 0; i < 16; i++) { f += 3000; await tune(f); await sleep(60); }
  await sleep(2500); await shot('2-fast-tune');
  await sleep(4000);
  for (let s = SPAN; s > 40000; s = Math.round(s * 0.85)) { await zoom(f, s); await sleep(80); }
  await zoom(f, 40000);
  await sleep(4000); await shot('3-zoom-in');
  if (process.env.WFSHOT_DEBUG) console.log('   layer', await ev(`JSON.stringify({ check: window.__vibeWf().wf.layerFloorCheck(), off: window.__vibeWf().wf.wideOff })`));
  for (let s = 40000; s < SPAN; s = Math.round(s / 0.85)) { await zoom(f, s); await sleep(80); }
  await zoom(f, SPAN);
  await sleep(2500);
  if (B) {
    const [fa, fb] = await Promise.all([shot('4-zoom-out'), B.shot('5-reference-never-zoomed')]);
    const out = path.join(OUT, `5-compare-${MODE}.png`);
    const { execFileSync } = await import('node:child_process');
    try { execFileSync('ffmpeg', ['-loglevel', 'error', '-y', '-i', fb, '-i', fa, '-filter_complex', 'hstack', out]); console.log(`   wrote ${out} (left: never zoomed · right: zoomed in and out, filled)`); }
    catch (e) { console.log('   (ffmpeg missing — the two pictures are beside each other in the folder)'); }
  } else await shot('4-zoom-out');
} catch (e) { console.log(`   driver: ${e && e.stack || e}`); code = 1; }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  fs.rmSync(dir, { recursive: true, force: true });
}
process.exit(code);
