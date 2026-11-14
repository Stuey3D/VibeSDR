// test-ui.mjs — the flasher's window, driven in headless Edge with the Go side mocked: which JOB each flow sends.
// ★★★ Why: on the Update screen the backup choices were still visible ([hidden] lost to `.big { display:block }`) and
//     pressing one sent a MAKE job — Stuart's first Update test rebuilt his card from scratch (2026-11-14).
//   node tools/r36s-flasher/frontend/test-ui.mjs        (silent; no Wails, no card)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const PAGE = new URL('./dist/index.html', import.meta.url).pathname;
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.log('   not run — no Edge at ' + BROWSER); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'flasher-ui-'));
const cdpPort = await freePort();
const br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--no-first-run', '--disable-extensions', `--remote-debugging-port=${cdpPort}`,
  `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=800,900', 'about:blank'], { stdio: 'ignore' });

// The Go side, mocked: one VibeSDR card; Start records what it was asked.
const MOCK = `
  window.__starts = [];
  const card = { id: 'disk4', name: 'Built In SDXC Reader', size: 15.9e9, device: '/dev/disk4', looks: 'VibeSDR card', bus: 'SD', large: false };
  window.go = { main: { App: {
    Rescan: async () => ({ flasher: '0.2.1-localtest', localTest: true, imageOk: true, version: '12.0.0-beta77+handheld73', imageErr: '' }),
    Disks: async () => [card],
    DefaultBackupDir: async () => '/Users/x/Documents/VibeSDR Backups/test',
    ChooseFolder: async (d) => d,
    Start: async (id, kind, folder, large) => { window.__starts.push(kind); },
    RevealBackup: async () => {},
  } } };
  window.runtime = { EventsOn: () => {} };`;

let id = 0; const waiters = new Map(); let ws;
const send = (method, params = {}) => new Promise((res) => { const i = ++id; waiters.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
try {
  let target;
  for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdpPort}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && waiters.has(d.id)) { waiters.get(d.id)(d); waiters.delete(d.id); } };
  await send('Page.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: MOCK });

  const run = async (flow, press) => {
    await send('Page.navigate', { url: 'file://' + PAGE });
    await sleep(700);
    await ev(`document.getElementById('${flow === 'update' ? 'bUpdate' : 'bMake'}').click()`);
    await sleep(2300);                                                   // the card list polls every 2 s
    await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
    await ev(`document.getElementById('toBackup').click()`); await sleep(200);
    const visible = await ev(`[document.getElementById('kWhole'), document.getElementById('kBoot')].map((b) => b.offsetParent !== null)`);
    for (const k of press) await ev(`document.getElementById('${k}').click()`);
    await ev(`document.getElementById('start').click()`); await sleep(100);
    const text = await ev(`document.getElementById('confirmText').textContent + ' | ' + document.getElementById('cYes').textContent`);
    await ev(`document.getElementById('cYes').click()`); await sleep(200);
    return { visible, text, starts: await ev('window.__starts') };
  };

  let r = await run('update', ['kWhole', 'kBoot']);
  ok(r.visible && !r.visible[0] && !r.visible[1], `★ UPDATE: the backup choices are not on screen (visible: ${JSON.stringify(r.visible)})`);
  ok(JSON.stringify(r.starts) === '["update"]', `★★ UPDATE sends an update job even if the hidden choices are pressed (sent ${JSON.stringify(r.starts)})`);
  ok(/update my card/.test(r.text) && !/already a VibeSDR card/.test(r.text), 'UPDATE confirm: "update my card", no Make warning');

  r = await run('make', []);
  ok(r.visible && r.visible[0] && r.visible[1], 'MAKE: both backup choices are on screen');
  ok(JSON.stringify(r.starts) === '["whole"]', `MAKE sends "whole" by default (sent ${JSON.stringify(r.starts)})`);
  ok(/already a VibeSDR card/.test(r.text) && /whole-card backup/.test(r.text), '★ MAKE on a VibeSDR card warns, and says the whole-card backup keeps the recordings');

  r = await run('make', ['kBoot']);
  ok(JSON.stringify(r.starts) === '["boot"]', `MAKE with Boot partition only sends "boot" (sent ${JSON.stringify(r.starts)})`);
  ok(/does NOT keep them/.test(r.text) && /Update a VibeSDR card/.test(r.text), '★ MAKE + boot backup on a VibeSDR card: says the recordings are NOT kept, points at Update');
} finally {
  try { ws?.close(); } catch {}
  br.kill(); await sleep(300);
  fs.rmSync(T, { recursive: true, force: true });
}
console.log(fail ? `   ${fail} FAILED, ${pass} passed` : `   ${pass} passed, 0 failed`);
process.exit(fail ? 1 : 0);
