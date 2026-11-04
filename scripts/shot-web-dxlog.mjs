#!/usr/bin/env node
// shot-web-dxlog.mjs — PICTURES of the web client's DX LOG (V12, 2026-11-04) at phone and tablet widths, on the default,
// silver and black control looks, with transparency on and off (AGENTS/memory: no visible polish gaps). A real vibeserver
// with a DX log seeded on disk (a DX station, an opening list, a local, a suspect, a DAB multiplex), headless Edge
// (--mute-audio: silent), the DECODERS panel's DX LOG key, the list and one entry.
// usage: VIBESERVER_BIN=… node scripts/shot-web-dxlog.mjs OUTDIR   (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const OUT = process.argv[2];
if (!OUT || !BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — OUTDIR, VIBESERVER_BIN and Edge needed'); process.exit(3); }
fs.mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dxshot-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DX log pictures', locator: 'IO92nh', adminPass: 'dxshot-admin',
  radios: [{ serial: 'dxshot', label: 'Test radio', enabled: true, configured: true, users: 16, rate: 2400000,
             dxScan: JSON.stringify({ on: true, bands: 'both', waitMin: 60 }) }] }));
const now = Math.floor(Date.now() / 1000);
const E = (o) => ({ openings: 0, live: false, presence: 0, peakSnrDb: 20, timesHeard: 1, lastSweep: 0, buckets: [], ...o });
fs.writeFileSync(path.join(dir, 'data', 'dxlog-dxshot.json'), JSON.stringify({
  v: 1, created: now - 3 * 86400, nextId: 10, sweepsFm: 40, sweepsDab: 12, alertSeq: 3, fmCursorKhz: 87500, dabCursor: 0, nextDab: false, bootDone: true,
  entries: [
    E({ id: 1, band: 'FM', key: 'FM:5201:98800', class: 'dx', khz: 98800, piN: 0x5201, ps: 'RAI R1', psVariants: ['RAI R1'], firstHeard: now - 7200, lastHeard: now - 60,
        timesHeard: 14, peakSnrDb: 31, peakLevelDb: -38, lastSweep: 40, openingList: [{ start: now - 1500, end: now - 60, detections: 9, peakSnrDb: 31 }, { start: now - 7200, end: now - 6000, detections: 5, peakSnrDb: 22 }] }),
    E({ id: 2, band: 'FM', key: 'FM:F201:104600', class: 'dx', khz: 104600, piN: 0xF201, ps: 'FR INTER', psVariants: ['FR INTER', 'FRANCE', 'INTER'], firstHeard: now - 90000,
        lastHeard: now - 80000, timesHeard: 3, peakSnrDb: 18, openingList: [{ start: now - 90000, end: now - 80000, detections: 3, peakSnrDb: 18 }] }),
    E({ id: 3, band: 'FM', key: 'FM:C201:89300', class: 'local', khz: 89300, piN: 0xC201, ps: 'BBC R4', firstHeard: now - 3 * 86400, lastHeard: now - 100, timesHeard: 400, peakSnrDb: 52 }),
    E({ id: 4, band: 'FM', key: 'FM:C201:91000', class: 'suspect', suspectWhy: 'image of 89.3 MHz (same PI C201, 32 dB weaker)', khz: 91000, piN: 0xC201, ps: 'BBC R4',
        firstHeard: now - 3 * 86400, lastHeard: now - 100, timesHeard: 300, peakSnrDb: 12 }),
    E({ id: 5, band: 'DAB', key: 'DAB:C18E:12D', class: 'dx', blk: 34, block: '12D', khz: 229072, eidN: 0xC18E, ecc: 225, ensemble: 'Coventry',
        services: [{ sid: 50001, label: 'Free Radio' }, { sid: 50002, label: 'Hits Radio' }, { sid: 50003, label: 'Greatest Hits' }],
        firstHeard: now - 3000, lastHeard: now - 3000, timesHeard: 1, peakSnrDb: 9.6, lastSweep: 12, openingList: [{ start: now - 3000, end: now - 3000, detections: 1, peakSnrDb: 9.6, services: ['Free Radio', 'Hits Radio'] }] }),
  ], alerts: [] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(500);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm', '--users', '16'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=900,1100', 'about:blank'], { stdio: 'ignore' });
let code = 0;
const deadline = setTimeout(() => { console.log('   deadline'); try { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {} process.exit(1); }, 300000);
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
  await send('Page.enable'); await send('Runtime.enable');
  const looks = [
    { name: 'default', prefs: {} },
    { name: 'silver-solid', prefs: { ctlBg: '#c8c8c8', ctlSolid: 100 } },
    { name: 'black-glass', prefs: { ctlBg: '#000000', ctlSolid: 0 } },
  ];
  const sizes = [{ name: 'phone', w: 390, h: 844, mobile: true }, { name: 'tablet', w: 900, h: 1100, mobile: false }];
  for (const look of looks) for (const sz of sizes) {
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
      if (await until(`!!document.querySelector('#audioGate button') || !document.getElementById('dxLogBtn').hidden`, 1500)) break;
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
    const shown = await until(`!document.getElementById('dxLogBtn').hidden`, 15000);
    if (!shown) {
      const sf = await send('Page.captureScreenshot', { format: 'png' });
      fs.writeFileSync(path.join(OUT, `dxlog-${look.name}-${sz.name}-FAILED.png`), Buffer.from(sf.result.data, 'base64'));
      console.log(`   FAIL ${look.name}/${sz.name}: the DX LOG key never appeared`); code = 1; continue;
    }
    await ev(`document.getElementById('dxLogBtn').click()`);
    await until(`document.querySelectorAll('#dxList .sres').length >= 2`, 8000);
    await sleep(600);
    const s1 = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(OUT, `dxlog-${look.name}-${sz.name}-list.png`), Buffer.from(s1.result.data, 'base64'));
    await ev(`(() => { document.getElementById('dxShowLocal').click(); document.getElementById('dxShowSuspect').click(); return true; })()`);
    await until(`document.querySelectorAll('#dxList .sres').length >= 5`, 8000);
    await sleep(400);
    const s2 = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(OUT, `dxlog-${look.name}-${sz.name}-all.png`), Buffer.from(s2.result.data, 'base64'));
    await ev(`document.querySelector('#dxList .sres').click()`);
    await until(`!document.getElementById('dxView').hidden && document.querySelectorAll('#dxViewBody tr').length >= 2`, 8000);
    await sleep(400);
    const s3 = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(OUT, `dxlog-${look.name}-${sz.name}-entry.png`), Buffer.from(s3.result.data, 'base64'));
    // ★ the overflow checks: nothing wider than the panel
    const over = await ev(`(() => { const p = document.getElementById('dxLogPanel').getBoundingClientRect(); return [...document.querySelectorAll('#dxLogPanel *')]
                     .filter(e => e.offsetParent && e.getBoundingClientRect().right > p.right + 1).map(e => e.id || e.className || e.tagName).slice(0, 5); })()`);
    console.log(`   ${look.name}/${sz.name}: list, all, entry written${over && over.length ? ` — OVERFLOW: ${JSON.stringify(over)}` : ''}`);
    if (over && over.length) code = 1;
    await ev(`(() => { document.getElementById('dxBack').click(); document.getElementById('dxShowLocal').click(); document.getElementById('dxShowSuspect').click(); document.getElementById('dxClose').click(); return true; })()`);
  }
  c.close();
} catch (e) { console.log(`   driver: ${e && e.stack || e}`); code = 1; }
finally {
  clearTimeout(deadline);
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  fs.rmSync(dir, { recursive: true, force: true });
}
process.exit(code);
