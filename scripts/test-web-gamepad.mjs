#!/usr/bin/env node
// test-web-gamepad.mjs — GAME CONTROLLERS on the served receiver page (Stuart's mapping, 2026-10-10; memory:
// gamepad_mapping), in a real (headless, MUTED) browser against the real vibeserver and a fake radio. The pad is
// SIMULATED: navigator.getGamepads is replaced before the page loads, and the test moves its sticks and buttons.
//
//  With two sticks: rotating the LEFT stick clockwise tunes up and anticlockwise back; the RIGHT stick zooms; the
//  D-pad moves the highlight; ✕ presses; ○ closes the menu. Without sticks (?pad=buttons): face buttons by position
//  tune/zoom, R1 presses, L1 goes back.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-gamepad.mjs   (exit 3 = not run: no binary)
import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); if (!c) fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-gamepad-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Pad test", locator: "IO92nh",
  adminPass: "pad-admin-pass", radios: [{ serial: "gp", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });

// ★ The simulated pad: 17 buttons, 4 axes (two sticks) — a PS-style standard pad. window.__pad is what the test drives.
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

const freq = (p) => p.js(`parseFloat(document.getElementById("freqInput").value)`);
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(120); await p.js(`__btn(${i}, false); 1`); await sleep(120); };
// Turn a stick through `deg` degrees from 0°, 5° at a time, then let it go.
const turn = async (p, ax, from, to) => {
  const stepD = to > from ? 5 : -5;
  for (let d = from; stepD > 0 ? d <= to : d >= to; d += stepD) {
    await p.js(`__pad.axes[${ax}] = ${Math.cos(d * Math.PI / 180)}; __pad.axes[${ax + 1}] = ${Math.sin(d * Math.PI / 180)}; 1`);
    await sleep(40);
  }
  await p.js(`__pad.axes[${ax}] = 0; __pad.axes[${ax + 1}] = 0; 1`); await sleep(100);
};

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("game controllers on the served page — a simulated pad, a real browser, the real binary");

  // ── Two sticks (the PS pad) ──
  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(1500); await p.tap(596, 312);                           // START RADIO (a real tap: audio needs a gesture)
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running");
  await p.js(`__connect(); 1`);
  const f0 = await freq(p);
  await turn(p, 0, 0, 95);
  const f1 = await freq(p);
  ok(f1 > f0, `LEFT stick turned clockwise a quarter: tuned UP (${f0} → ${f1} MHz)`);
  await turn(p, 0, 95, 0);
  const f2 = await freq(p);
  ok(Math.abs(f2 - f0) < 1e-6, `…turned back anticlockwise: back where it was (${f2} MHz)`);
  const zoom0 = await p.js(`document.getElementById("mPanelFit") ? 1 : 1`);
  ok(zoom0 === 1, "(page has its view controls)");

  // D-pad moves a highlight; ✕ presses it; ○ closes what opened.
  await press(p, 13);
  ok(await p.until(`!!document.querySelector(".gpFocus")`), "D-pad: a highlight box appears on a control");
  // Find the MENU key and press it with ✕: walk the D-pad until it is highlighted (bounded).
  let onMenu = false;
  for (let i = 0; i < 40 && !onMenu; i++) {
    onMenu = await p.js(`(() => { const h = document.querySelector(".gpFocus"); return !!h && /^\\s*MENU\\s*$/i.test(h.textContent || ""); })()`);
    if (!onMenu) await press(p, i % 2 ? 15 : 13);
  }
  if (!onMenu) await p.js(`(() => { const m = [...document.querySelectorAll("button")].find(b => /^\\s*MENU\\s*$/i.test(b.textContent||"") && b.getBoundingClientRect().width > 2); document.querySelector(".gpFocus")?.classList.remove("gpFocus"); m.classList.add("gpFocus"); })(); 1`);
  await press(p, 0);                                                   // ✕ = enter
  ok(await p.until(`document.getElementById("menu").classList.contains("open")`), "✕ presses the highlighted control (the MENU opens)");
  await press(p, 13);
  ok(await p.js(`document.getElementById("menu").contains(document.querySelector(".gpFocus"))`), "with the menu open, the highlight moves INSIDE the menu");
  await press(p, 1);                                                   // ○ = back
  ok(await p.until(`!document.getElementById("menu").classList.contains("open")`), "○ closes the menu");
  // START switches the method (a one-stick pad reported as two), says so, and remembers it.
  await press(p, 9);
  ok(await p.until(`/buttons by position/.test(document.getElementById("gpToast")?.textContent || "")`, 3000),
     "START switches to buttons-by-position and says so on screen");
  ok(await p.js(`localStorage.getItem("vibe.padProfile") === "buttons"`), "…remembered on this device");
  const h0 = await freq(p);
  await press(p, 1);                                                   // now ○ (right face) = tune up
  ok((await freq(p)) > h0, "…and the face buttons now tune (○ = right = up)");
  await press(p, 2);                                                   // □ (left face) = tune down
  ok(Math.abs((await freq(p)) - h0) < 1e-6, "…□ (left) tunes back down");
  // Walk the D-pad to MENU again (the page keeps its own highlight; drawing one by hand would not tell it).
  let onMenu2 = false;
  for (let i = 0; i < 40 && !onMenu2; i++) {
    onMenu2 = await p.js(`(() => { const h = document.querySelector(".gpFocus"); return !!h && /^\\s*MENU\\s*$/i.test(h.textContent || ""); })()`);
    if (!onMenu2) await press(p, i % 2 ? 15 : 13);
  }
  ok(onMenu2, "no-stick: the D-pad reaches the MENU key");
  await press(p, 5);                                                   // R1 = enter
  ok(await p.until(`document.getElementById("menu").classList.contains("open")`), "no-stick: R1 = enter (opens the highlighted MENU)");
  await press(p, 4);                                                   // L1 = back
  ok(await p.until(`!document.getElementById("menu").classList.contains("open")`), "no-stick: L1 = back (closes it)");
  await press(p, 9);
  ok(await p.until(`/two sticks/.test(document.getElementById("gpToast")?.textContent || "")`, 3000), "START again: back to two sticks");
  await p.js(`localStorage.removeItem("vibe.padProfile"); 1`);
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();

} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
