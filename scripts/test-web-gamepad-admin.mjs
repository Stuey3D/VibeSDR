#!/usr/bin/env node
// test-web-gamepad-admin.mjs — a GAME CONTROLLER in the ADMIN panel of the served page (Stuart, 2026-10-18: "in the
// admin menu there is no way to navigate it with the controller"). The panel is shown by `hidden`, not .open, and sits
// above the splash — so the pad's highlight stayed on the page underneath and ○ did not close it.
//
// Real server (fake radio), a real headless MUTED browser, a simulated standard pad (see test-web-gamepad.mjs):
//   connect as admin, open SERVER ADMIN, then the D-pad walks INSIDE the panel (and scrolls it),
//   carries on from the same place after the panel redraws its tables, and ○ closes it.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-gamepad-admin.mjs   (exit 3 = not run)
import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); if (!c) fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-gamepad-admin-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Pad admin test", locator: "IO92nh",
  adminPass: "pad-admin-pass", radios: [{ serial: "gpadm", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
// ★ A short window, so the admin panel's body overflows and has to scroll.
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,700", "about:blank"], { stdio: "ignore" });
const FAKE_PAD = `
  window.__pad = { id: 'Test pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad, null, null, null];
  window.__btn = (i, on) => { window.__pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
  window.__connect = () => window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: window.__pad }));
`;
async function cleanup() { for (const p of [br, srv, rtl]) { try { p.kill(); } catch {} } await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }
async function open(url) {
  let target;
  for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === "page"); } catch { await sleep(250); } }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await call("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 }); };
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_PAD });
  await call("Page.navigate", { url });
  return { ws, js, until, tap, errors };
}
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(120); await p.js(`__btn(${i}, false); 1`); await sleep(120); };
const UP = 12, DOWN = 13, LEFT = 14, RIGHT = 15, CROSS = 0, CIRCLE = 1;
const focusId = (p) => p.js(`(() => { const h = document.querySelector(".gpFocus"); return h ? (h.id || h.textContent.trim().slice(0, 24)) : ""; })()`);
const inAdmin = (p) => p.js(`!!document.querySelector("#adminPanel .gpFocus")`);

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("a game controller in the admin panel — a simulated pad, a real browser, the real binary");
  const p = await open(`http://127.0.0.1:${port}/`);
  ok(await p.until(`!document.getElementById("btnAdmin").hidden`, 15000), "the splash offers ADMIN");
  // A single-radio server: ADMIN, the password, CONNECT (joins as admin), then SERVER ADMIN in the menu.
  await p.js(`document.getElementById("btnAdmin").click(); document.getElementById("gateAdminPw").value = "pad-admin-pass"; 1`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(1500); await p.tap(596, 312);                           // START RADIO (a real tap: audio needs a gesture)
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running, as admin");
  await p.js(`__connect(); 1`);
  await p.js(`document.getElementById("btnServerAdmin").click(); 1`);
  ok(await p.until(`!document.getElementById("adminPanel").hidden`), "SERVER ADMIN opens the admin panel");
  ok(await p.until(`document.getElementById("adminPanelBody").textContent.includes("CHECK FOR UPDATES")`, 10000), "…and it has loaded");

  // The highlight goes INTO the panel, and stays there however far it walks.
  await press(p, DOWN);
  ok(await inAdmin(p), `D-pad: the highlight is inside the admin panel (at: ${await focusId(p)})`);
  let stayed = true; const seen = new Set();
  for (let i = 0; i < 40; i++) { await press(p, DOWN); stayed &&= await inAdmin(p); seen.add(await focusId(p)); }
  ok(stayed, "DOWN ×40: the highlight never leaves the panel for the page underneath");
  // Each row's other controls are LEFT/RIGHT of the one DOWN lands on (down runs the right-hand column): zigzag.
  for (let i = 0; i < 40; i++) await press(p, UP);
  for (let i = 0; i < 12; i++) {
    await press(p, DOWN);
    for (const d of [LEFT, LEFT, LEFT, LEFT, RIGHT, RIGHT, RIGHT, RIGHT]) { await press(p, d); stayed &&= await inAdmin(p); seen.add(await focusId(p)); }
  }
  const total = await p.js(`[...document.querySelectorAll("#adminPanel button, #adminPanel input, #adminPanel select")].filter(e => e.getBoundingClientRect().width > 2).length`);
  ok(stayed, "…nor LEFT/RIGHT along each row");
  ok(seen.size >= Math.min(12, total - 2), `the D-pad reaches the panel's controls (${seen.size} of ${total})`);
  for (let i = 0; i < 40; i++) await press(p, DOWN);
  ok(await p.js(`document.getElementById("adminPanelBody").scrollTop > 0`), "…scrolling the panel to keep the highlight in view");

  // The panel redraws its tables every 2 s. Wait through a redraw: the next press carries on from where it was.
  for (let i = 0; i < 20; i++) await press(p, UP);
  const yBefore = await p.js(`document.querySelector(".gpFocus").getBoundingClientRect().top`);
  await sleep(4500);
  await press(p, DOWN);
  const yAfter = await p.js(`(document.querySelector("#adminPanel .gpFocus") || {getBoundingClientRect: () => ({top: -1e9})}).getBoundingClientRect().top`);
  ok(await inAdmin(p) && yAfter >= yBefore - 2, `after the panel redraws, DOWN carries on from the same place (y ${Math.round(yBefore)} → ${Math.round(yAfter)})`);

  // ○ closes it.
  await press(p, CIRCLE);
  ok(await p.until(`document.getElementById("adminPanel").hidden`), "○ closes the admin panel");

  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
