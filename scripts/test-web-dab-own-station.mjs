#!/usr/bin/env node
// ★★ V12 DAB OWN STATION ON THE WEB PAGE (docs/v12/DAB-OWN-STATION.md) — the binary's embedded page in headless Edge (muted)
// plus a second listener over a bare socket, on one shared dial with own station ON (benchmark N 5, limit 3):
//   1. the page's station list marks the playing station "▶ 2" (both listeners on it);
//   2. the other listener picks another service: the page's row for it reads "▶ 1", the page's own row "▶ 1", and the
//      page's highlighted station did NOT move (its report is its own);
//   3. the page picks a third service from its list: the pick is sent and the page's highlight moves to it, while the
//      other listener's report still names its own station.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-dab-own-station.mjs (exit 3 = not run)
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

const BLOCK = 194_064_000;   // 7D
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-dabown-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(BLOCK)], { stdio: 'ignore' }));
await sleep(400);
const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web DAB own', locator: 'IO92nh', adminPass: 'web-dabown-admin',
  radios: [{ serial: 'webdabown', label: 'T', enabled: true, configured: true, users: 3 }] }));
fs.writeFileSync(path.join(D, 'benchmark.json'), JSON.stringify({ v: 1, at: 1, rows: [], dabStations: { n: 5, perStationPct: 9.5, frontPct: 60, memKB: 900 } }));
const port = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
  stdio: 'ignore' }));
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
// the other listener: spectrum + audio sockets, one session, a ping every 8 s
const sess = 'wdo' + Math.random().toString(16).slice(2, 8);
const other = { msgs: [] };
other.ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sess}&bins=256&proto=1`);
other.au = new WebSocket(`ws://127.0.0.1:${port}/ws/audio?user_session_id=${sess}`);
other.ws.onmessage = (e) => { if (typeof e.data === 'string') { try { const m = JSON.parse(e.data); if (m.type === 'dab') other.msgs.push(m); } catch {} } };
const beat = setInterval(() => { try { other.ws.send('{"type":"ping"}'); } catch {} }, 8000);

try {
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!!document.getElementById("btnConnect")`);
  await js(`localStorage.setItem("vibesdr_web_prefs_v1", JSON.stringify({ dabChannel: "7D" })); 1`);
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
  await until(`parseFloat(document.getElementById("freqInput").value) > 0`);
  await js(`document.getElementById("mMode").click(); 1`);
  await until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`);
  await js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
  ok(await until(`document.getElementById("decTitle")?.textContent === "DAB"`, 20000), 'into DAB from the mode menu');
  ok(await until(`${ROWS}.some(r => r.on)`, 40000), 'the stations are listed and one is playing');
  // ── 1. both listeners on the radio's own station ──
  ok(await until(`${ROWS}.some(r => r.on && r.pl === "\\u25B6 2")`, 15000), `the playing row reads ▶ 2 (${JSON.stringify((await js(ROWS)).filter((r) => r.pl))})`);
  const rows0 = await js(ROWS);
  const mine = rows0.find((r) => r.on).sid;
  const theirs = rows0.find((r) => r.sid !== mine).sid;
  // ── 2. the other listener picks another service ──
  other.ws.send(JSON.stringify({ type: 'dab_service', sid: theirs }));
  ok(await until(`${ROWS}.some(r => r.sid === ${theirs} && r.pl === "\\u25B6 1") && ${ROWS}.some(r => r.sid === ${mine} && r.on && r.pl === "\\u25B6 1")`, 10000),
     `their station ▶ 1, the page's still highlighted and ▶ 1 (${JSON.stringify((await js(ROWS)).filter((r) => r.pl || r.on))})`);
  ok(other.msgs.at(-1)?.mine === theirs, 'the other listener hears its own pick');
  // ── 3. the page picks a third ──
  const third = rows0.find((r) => r.sid !== mine && r.sid !== theirs).sid;
  const sentBefore = await js(`window.__sent.length`);
  await js(`document.querySelector('#dabStations .dabSvc[data-sid="${third}"]').click(); 1`);
  ok(await until(`window.__sent.slice(${sentBefore}).some(s => s.includes('"dab_service"') && s.includes('${third}'))`, 3000), 'the page sent its pick');
  ok(await until(`${ROWS}.some(r => r.sid === ${third} && r.on)`, 10000), 'the page now highlights its own pick');
  await sleep(1500);
  ok(other.msgs.at(-1)?.sid === theirs, `the other listener's station did not move (${other.msgs.at(-1)?.sid?.toString(16)})`);
  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors.slice(0, 2).join(' | ') : ''));
} catch (e) { console.error(e); fails++; }
clearInterval(beat);
try { other.ws.close(); other.au.close(); } catch {}
try { ws.close(); } catch {}
for (const p of procs) { try { p.kill(); } catch {} }
await sleep(600);
fs.rmSync(T, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
