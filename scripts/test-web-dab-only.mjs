#!/usr/bin/env node
// test-web-dab-only.mjs — V12 DAB ONLY on the REAL web page (headless MUTED Edge): a whole-radio DAB server whose owner has
// switched every other mode off. The page arrives straight in DAB without anyone pressing DAB, EXIT DAB is NOT there (Stuart,
// 2026-11-17: "if DAB is the only allowed mode then the exit button goes"), the box says "DAB only on this receiver", and
// the mode menu offers nothing else. (The older-server inference is held by scripts/test-dab-only-clients.ts.) Silent.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = process.env.DAB_CLIP || process.env.VIBE_DAB_CLIP || path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh, or DAB_CLIP=) — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

const OTHERS = 'wfm,nfm,am,sam,sau,sal,ecssu,ecssl,amst,usb,lsb,cwu,cwl,drm,freedv,m17,dvsig,adsb,ism,ndb,ais,avdata,sondes';
const BLOCK = 194_064_000;   // 7D
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-dabonly-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(BLOCK)], { stdio: 'ignore' }));
await sleep(400);
const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web DAB only', locator: 'IO92nh', adminPass: 'web-dabonly-admin',
  radios: [{ serial: 'webdabonly', label: 'T', enabled: true, configured: true, users: 3, blockedModes: OTHERS, landingDabChannel: 11 }] }));
const port = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'], {
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
/** Each row of the page's station list: sid, marker text, highlighted. */
const ROWS = `[...document.querySelectorAll("#dabStations .dabSvc")].map(e => ({ sid: Number(e.dataset.sid), pl: e.querySelector(".pl")?.textContent || "", on: e.classList.contains("on") }))`;
try {
  await call('Runtime.enable'); await call('Page.enable');
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
  ok(await until(`document.getElementById("decTitle")?.textContent === "DAB"`, 30000), '★ the page arrives IN DAB without anyone pressing DAB');
  ok(await until(`document.querySelectorAll("#dabStations .dabSvc").length > 0`, 40000), '…the usual DAB view: the station list');
  ok(await until(`getComputedStyle(document.getElementById("dabExit")).display === "none"`, 8000), '★ EXIT DAB is not there');
  ok(await until(`(() => { const n = document.getElementById("dabOnlyNote"); return !!n && !n.hidden && n.offsetParent !== null && /DAB only on this receiver/.test(n.textContent); })()`, 8000),
     '★ the box says "DAB only on this receiver"');
  await js(`document.getElementById("mMode").click(); 1`); await sleep(800);
  const offered = await js(`[...document.querySelectorAll(".mModeOpt")].filter(b => b.offsetParent !== null).map(b => b.textContent)`) || [];
  ok(!offered.some((t) => /^(WFM|NFM|AM|USB|LSB|CW)/.test(t)), `the mode menu offers no other mode (${JSON.stringify(offered)})`);
  ok(errors.length === 0, `no page errors (${errors.slice(0, 2).join(' | ')})`);
} finally {
  if (fails) { try { console.log(fs.readFileSync(path.join(T, 'server.log'), 'utf8').split('\n').filter((l) => /DAB|dab|landing|session|refus/.test(l)).slice(-25).join('\n')); } catch {} }
  try { ws.close(); } catch {}
  for (const p of procs) try { p.kill('SIGKILL'); } catch {}
  await sleep(300);
  try { fs.rmSync(T, { recursive: true, force: true }); } catch {}
}
console.log(fails ? `FAIL  web DAB only: ${fails} failed` : 'ok  web DAB only');
process.exit(fails ? 1 : 0);
