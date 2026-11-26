// ★★ V12 beta96 THE STANDALONE AIR & SEA MAP (web/client/src/main.ts openAirSeaViewer) in headless Edge (--mute-audio).
// A FULL server whose only radio is never started (every radio busy / none — the case the viewer is for), with one
// server-level aircraft feed: a synthetic dumphfdl connecting IN with BaseStation lines (HFDL). Then:
//   · the landing page shows AIR & SEA MAP (there is something to show) and opens the map with NO radio: no WebSocket at
//     all, the server's aircraft in the AIRCRAFT list, the HFDL source named;
//   · it is LIVE: a new aircraft the feed sends appears without a reload;
//   · the feed going away shows the banner (AIRCRAFT FEED … lost);
//   · CLOSE MAP goes back to the landing;
//   · a server with nothing to show: no button (AGENTS.md — no dead controls).
// Exit 3 = not run (no VIBESERVER_BIN or no Edge).
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const kids = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } });

async function server(tag, feeds) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-assa-${tag}-`));
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  process.on('exit', () => { try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  let P = 0;
  for (let i = 0; i < 20 && !P; i++) { const p = await freePort(); if (await new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(p + 1, '127.0.0.1', () => s.close(() => r(true))); })) P = p; }
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Air and sea viewer test', locator: 'IO92nh', adminPass: 'assa-admin', port: P, fullMode: true,
    aircraftFeeds: feeds ? JSON.stringify(feeds) : '',
    radios: [{ serial: 'assa-rtl', driver: 'rtlsdr', label: 'RTL (not running)', enabled: true, configured: true, users: 1 }] }));
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const door = spawn(BIN, [], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'door.log'), 'w'), 'ignore'] });
  kids.push(door);
  for (let i = 0; i < 80; i++) { try { const r = await fetch(`http://127.0.0.1:${P}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  return { P, dir, door };
}
async function browser(dir) {
  const dbg = await freePort();
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  kids.push(edge);
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  const wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  // ★ count every WebSocket the page opens: the viewer must open none
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__ws=0;const W=window.WebSocket;window.WebSocket=function(u,p){window.__ws++;window.__wsUrl=String(u);return new W(u,p);};window.WebSocket.prototype=W.prototype;Object.assign(window.WebSocket,W);` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  return { ev, until, send, close: () => { try { wsC.close(); } catch {} edge.kill(); } };
}

console.log('── the standalone AIR & SEA map (beta96) — no radio taken ──');
{
  const sbsPort = await freePort();
  const S = await server('feed', [{ fmt: 'sbs', dir: 'listen', port: sbsPort, label: 'hfdl', name: 'UberSDR HFDL' }]);
  let c = null;
  for (let i = 0; i < 40 && !c; i++) { c = await new Promise((r) => { const s = net.connect(sbsPort, '127.0.0.1'); s.on('connect', () => r(s)); s.on('error', () => r(null)); }); if (!c) await sleep(250); }
  ok(!!c, 'the synthetic dumphfdl connects to the server\'s HFDL feed');
  const line = (hex, cs, lat, lon) => { const t = new Date(); const d = t.toISOString().slice(0, 10).replace(/-/g, '/'), hm = t.toISOString().slice(11, 19) + '.000';
    return `MSG,3,1,1,${hex},1,${d},${hm},${d},${hm},${cs},,,,${lat.toFixed(6)},${lon.toFixed(6)},,0,,,,0\n`; };
  c.write(line('AB12CD', 'BAW123', 51.5, -20.1) + line('AB12CE', 'DLH400', 48.0, -30.0));
  await sleep(4500);   // ★ the shared table is flushed every 2 s
  const B = await browser(S.dir);
  try {
    await B.send('Page.navigate', { url: `http://127.0.0.1:${S.P}/` });
    ok(await B.until(`!!document.getElementById('splashAirSea') && !document.getElementById('splashAirSea').hidden`, 15000), 'the landing page shows AIR & SEA MAP (the server has aircraft)');
    await B.ev(`document.getElementById('splashAirSea').click()`);
    const V = `document.querySelector('.trkView:not([hidden])')`;
    ok(await B.until(`!!${V}`, 6000), 'it opens the AIR & SEA map');
    ok(await B.ev(`document.getElementById('splash').classList.contains('hidden')`) === true, '…over the landing (no radio started)');
    ok(await B.until(`/BAW123/.test(${V}?.textContent || '') && /DLH400/.test(${V}?.textContent || '')`, 10000), 'the server\'s aircraft are listed (BAW123, DLH400 from the HFDL feed)');
    ok(await B.ev(`window.__ws`) === 0, `no WebSocket at all — nothing tuned, no spectrum (${await B.ev('window.__wsUrl || "none"')})`);
    // LIVE: a new aircraft without a reload
    c.write(line('AB12CF', 'EZY77', 50.0, -25.0));
    ok(await B.until(`/EZY77/.test(${V}?.textContent || '')`, 12000), 'LIVE: a new aircraft from the feed appears without a reload');
    // the feed goes away → the banner (the stall time is 90 s for a listen source with nobody connected: too long here,
    // so the banner is checked on the server's own word in test-server-aircraft-feeds; here: the map keeps working)
    ok(await B.ev(`${V}.querySelector('.exit')?.textContent`) === 'CLOSE MAP', 'its exit key is CLOSE MAP (it controls nothing)');
    await B.ev(`${V}.querySelector('.exit').click()`);
    ok(await B.until(`!document.getElementById('splash').classList.contains('hidden') && !${V}`, 4000), 'CLOSE MAP goes back to the landing');
    ok(await B.ev(`window.__ws`) === 0, 'still no WebSocket');
  } finally { B.close(); try { c.destroy(); } catch {} S.door.kill('SIGTERM'); }
}
{
  const S = await server('empty', null);
  const B = await browser(S.dir);
  try {
    await B.send('Page.navigate', { url: `http://127.0.0.1:${S.P}/` });
    await B.until(`!!document.getElementById('splashRadios')?.children.length`, 10000);
    await sleep(2000);
    ok(await B.ev(`!!document.getElementById('splashAirSea') && document.getElementById('splashAirSea').hidden`) === true, 'a server with nothing to show: no AIR & SEA MAP button');
  } finally { B.close(); S.door.kill('SIGTERM'); }
}
console.log(`\ntest-web-airsea-standalone: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
