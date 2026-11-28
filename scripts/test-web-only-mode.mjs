#!/usr/bin/env node
// test-web-only-mode.mjs — V12 THE ONLY MODE on the REAL web page (headless MUTED Edge), any whole-radio mode (Stuart,
// 2026-11-18: "the same thing we did for DAB earlier where it is the only allowed mode, we need to do for all modes as the
// Almere server has an ADSB only radio and I've accidentally closed the ADSB a couple of times"). An ADS-B server (2.4 MS/s,
// fake rtl_tcp) whose owner has switched every other mode off: the page arrives straight in ADS-B without anyone pressing
// it, EXIT ADS-B is NOT there, the map's bar says "ADS-B only on this receiver", Escape (the keyboard's back) says why
// instead of leaving and sends no "ADS-B off", and the page is still in ADS-B after it. (The older-server inference is
// held by scripts/test-only-mode-clients.ts.) Silent: --mute-audio, and nothing to hear at 1090 MHz.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

const DEMODS = ['wfm', 'nfm', 'am', 'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'freedv', 'm17', 'dvsig'];
const BLOCKED = [...DEMODS, 'dab', 'ism', 'ndb', 'ais', 'avdata', 'sondes'].join(',');
const RATE = 2400000;
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-onlymode-'));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' }));
await sleep(400);
const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web only mode', locator: 'IO92nh', adminPass: 'web-onlymode-admin',
  radios: [{ serial: 'webonlymode', label: 'T', enabled: true, configured: true, users: 3, rate: RATE, blockedModes: BLOCKED }] }));
const port = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '96100000', '--mode', 'wfm', '--users', '3'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
  stdio: ['ignore', fs.openSync(path.join(T, 'server.log'), 'w'), fs.openSync(path.join(T, 'server.log'), 'a')] }));
for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }

const cdp = await freePort();
procs.push(spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions', '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' }));
let target;
for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map(); const errors = [];
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;
try {
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!!document.getElementById("btnConnect")`);
  await js(`document.getElementById("btnConnect").click(); 1`);
  await until(`!!document.getElementById("audioGate")`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await tap(at[0], at[1]);
    if (await until(`!document.getElementById("audioGate")`, 3000)) break;
  }
  // ★ beta96 ADS-B mode's map is the AIR & SEA map (a .trkView) in its ADS-B-live state
  const AS = '(() => { const v = [...document.querySelectorAll(".trkView")].find((x) => !x.hidden); return v || null; })()';
  ok(await until(`(() => { const v = ${AS}; return !!v && /ADS-B/.test(v.querySelector(".ttl")?.textContent || ""); })()`, 30000), '★ the page arrives IN ADS-B without anyone pressing ADS-B');
  ok(await until(`(() => { const v = ${AS}; const x = v && v.querySelector(".exit"); return !!x && getComputedStyle(x).display === "none"; })()`, 8000), '★ EXIT ADS-B is not there');
  ok(await until(`(() => { const v = ${AS}; const n = v && v.querySelector(".trkTop .onlyModeNote"); return !!n && !n.hidden && n.offsetParent !== null && n.textContent === "ADS-B only on this receiver"; })()`, 8000),
     '★ the bar says "ADS-B only on this receiver"');
  await js(`document.activeElement && document.activeElement.blur(); 1`);
  // ★ beta97 map cards: BACK shuts the map's top card first (the LIST on a wide window), THEN the way back — so up to
  //   three presses, and the map is never left
  let said = false;
  for (let i = 0; i < 3 && !said; i++) {
    for (const type of ['keyDown', 'keyUp']) await call('Input.dispatchKeyEvent', { type, key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
    said = await until(`/ADS-B only/.test(document.getElementById("vsPill")?.textContent || "")`, 1500);
  }
  ok(said, 'Escape (the way back, once the map\'s cards are shut) says why instead of leaving');
  await sleep(2500);
  const sent = (await js(`window.__sent || []`)) || [];
  ok(!sent.some((m) => /"type":"adsb"/.test(m) && /"on":0/.test(m)), `★ no "ADS-B off" was sent (${sent.filter((m) => /adsb/.test(m)).slice(0, 3).join(' ')})`);
  ok(!!(await js(`(() => { const v = ${AS}; return !!v && /ADS-B/.test(v.querySelector(".ttl")?.textContent || ""); })()`)), '…and the page is still in ADS-B');
  ok(!/only mode: a listener's/.test(fs.readFileSync(path.join(T, 'server.log'), 'utf8')), '…the server never had to refuse one');
  ok(errors.length === 0, `no page errors (${errors.slice(0, 2).join(' | ')})`);
} finally {
  if (fails) { try { console.log(fs.readFileSync(path.join(T, "server.log"), "utf8").split("\n").filter((l) => /ONLY|ADS-B|landing|session|refus|rate/i.test(l)).slice(-25).join('\n')); } catch {} }
  try { ws.close(); } catch {}
  for (const p of procs) try { p.kill('SIGKILL'); } catch {}
  await sleep(300);
  try { fs.rmSync(T, { recursive: true, force: true }); } catch {}
}
console.log(fails ? `FAIL  web only mode: ${fails} failed` : 'ok  web only mode');
process.exit(fails ? 1 : 0);
