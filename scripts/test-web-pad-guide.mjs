#!/usr/bin/env node
// test-web-pad-guide.mjs — THE CONTROLLER GUIDE on the served receiver page (Stuart approved the prototype 2026-10-19;
// web/client/src/padGuide.ts, src/utils/padGuide.ts), in a real (headless, MUTED) browser against the real vibeserver
// and a fake radio. The pad is SIMULATED, as in test-web-gamepad.mjs.
//
//  SELECT opens the guide and SELECT closes it; back closes it (○ on two sticks, L on buttons); it opens on the layout
//  in use and follows a HOLD START switch while open; the D-pad left / right moves between the tabs; the menu's
//  CONTROLLER GUIDE key opens it; the layout follows the WINDOW (three columns with leader lines, then the pad over a
//  list with no lines). SHOTS=<dir> also saves screenshots at each window size, for a look.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-pad-guide.mjs   (exit 3 = not run: no binary)
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

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-padguide-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Guide test", locator: "IO92nh",
  adminPass: "pad-admin-pass", radios: [{ serial: "gp", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
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
  return { ws, js, until, tap, errors, call };
}

// START RADIO: wait for the screen, press its button (a real tap — audio needs a gesture), retry until it has gone.
// ★ Not a fixed wait and fixed coordinates: a tap that landed before the screen existed started nothing, now and then.
const startRadio = async (p) => {
  await p.until(`!!document.getElementById("audioGate")`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await p.js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null;
      const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await p.tap(at[0], at[1]);
    if (await p.until(`!document.getElementById("audioGate")`, 3000)) break;
  }
};
const freq = (p) => p.js(`parseFloat(document.getElementById("freqInput").value)`);
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(120); await p.js(`__btn(${i}, false); 1`); await sleep(120); };
// Hold a button (START switches the controls only when HELD — a tap must never do it).
const hold = async (p, i, ms) => { await p.js(`__btn(${i}, true); 1`); await sleep(ms); await p.js(`__btn(${i}, false); 1`); await sleep(120); };
// Turn a stick through `deg` degrees from 0°, 5° at a time, then let it go.
const turn = async (p, ax, from, to) => {
  const stepD = to > from ? 5 : -5;
  for (let d = from; stepD > 0 ? d <= to : d >= to; d += stepD) {
    await p.js(`__pad.axes[${ax}] = ${Math.cos(d * Math.PI / 180)}; __pad.axes[${ax + 1}] = ${Math.sin(d * Math.PI / 180)}; 1`);
    await sleep(40);
  }
  await p.js(`__pad.axes[${ax}] = 0; __pad.axes[${ax + 1}] = 0; 1`); await sleep(100);
};

const SHOTS = process.env.SHOTS;
const shot = async (p, name) => {
  if (!SHOTS) return;
  await sleep(350);
  const r = await p.call("Page.captureScreenshot", { format: "png" });
  if (r.result?.data) { fs.mkdirSync(SHOTS, { recursive: true }); fs.writeFileSync(path.join(SHOTS, name + ".png"), Buffer.from(r.result.data, "base64")); }
};
const size = (p, width, height) => p.call("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: false });
const isOpen = (p) => p.js(`document.getElementById("padGuide").classList.contains("open")`);
const tab = (p) => p.js(`document.querySelector("#pgTabs button.on")?.dataset.g || ""`);
const title = (p) => p.js(`document.getElementById("pgTitle").textContent`);
const lineCount = (p) => p.js(`((document.querySelector("#pgLines path")?.getAttribute("d") || "").match(/M/g) || []).length`);

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("the controller guide on the served page — a simulated pad, a real browser, the real binary");

  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`localStorage.removeItem("vibe.padProfile"); document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running");
  await p.js(`__connect(); 1`);
  await sleep(300);
  await size(p, 1200, 900);

  // ── SELECT opens it, on the layout in use (a two-stick pad) ──
  ok(!(await isOpen(p)), "the guide starts closed");
  await press(p, 8);
  ok(await p.until(`document.getElementById("padGuide").classList.contains("open")`, 3000), "SELECT opens the controller guide");
  ok((await tab(p)) === "sticks" && (await title(p)) === "TWO ANALOGUE STICKS", "…on the TWO STICKS tab (the pad in use has two sticks)");
  await sleep(400);
  ok(await p.js(`document.getElementById("pgGuide").classList.contains("pgCols3")`), "wide window: three columns");
  ok((await lineCount(p)) === 7, `…with a leader line to each of the 7 labels (${await lineCount(p)})`);
  ok(await p.js(`document.querySelectorAll("#pgPad svg circle").length > 10 && !!document.querySelector(".pgLab.back")`), "the drawing and the labels are there, BACK coloured as back");
  await shot(p, "1200x900-sticks");
  // A line ends beside its number: check the first one lands within a few px of the number's edge.
  ok(await p.js(`(() => { const host = document.getElementById("pgGuide").getBoundingClientRect();
      const d = document.querySelector("#pgLines path").getAttribute("d").trim().split(/(?=M)/)[0];
      const nums = d.match(/-?[0-9.]+/g).map(Number); const x1 = nums[4], y1 = nums[5];
      const n = document.querySelector(".pgLab[data-i='0'] .pgNum").getBoundingClientRect();
      return Math.abs(host.left + x1 - (n.right + 6)) < 2 && Math.abs(host.top + y1 - (n.top + n.height / 2)) < 2; })()`),
     "…and a line ends exactly beside its number (measured from live positions)");

  // ── The D-pad moves between the tabs ──
  await press(p, 15);
  ok((await tab(p)) === "buttons" && (await title(p)) === "BUTTONS BY POSITION", "D-pad right: the BUTTONS tab");
  await sleep(300);
  ok((await lineCount(p)) === 9, "…its 9 labels each get a line");
  await shot(p, "1200x900-buttons");
  await press(p, 15);
  ok((await tab(p)) === "buttons", "…right again: stays there (two tabs)");
  await press(p, 14);
  ok((await tab(p)) === "sticks", "D-pad left: back to TWO STICKS");
  const f0 = await freq(p);
  await press(p, 0);
  ok(await isOpen(p), "enter (✕) on the guide changes nothing — it stays open");

  // ── SELECT closes it; BACK closes it ──
  await press(p, 8);
  ok(await p.until(`!document.getElementById("padGuide").classList.contains("open")`, 3000), "SELECT again closes it");
  await press(p, 8);
  ok(await p.until(`document.getElementById("padGuide").classList.contains("open")`, 3000), "SELECT opens it again");
  await press(p, 1);
  ok(await p.until(`!document.getElementById("padGuide").classList.contains("open")`, 3000), "BACK (○) closes it");
  ok(Math.abs((await freq(p)) - f0) < 1e-6, "…and none of that tuned the radio");

  // ── HOLD START while it is open: the tab follows the switch ──
  await press(p, 8);
  await p.until(`document.getElementById("padGuide").classList.contains("open")`, 3000);
  await hold(p, 9, 1200);
  ok(await p.until(`document.querySelector("#pgTabs button.on")?.dataset.g === "buttons"`, 3000), "HOLD START with the guide open: it follows to BUTTONS");
  await press(p, 4);                                                   // now L1 = back
  ok(await p.until(`!document.getElementById("padGuide").classList.contains("open")`, 3000), "…and back is now L, which closes it");
  await press(p, 8);
  ok(await p.until(`document.getElementById("padGuide").classList.contains("open")`, 3000) && (await tab(p)) === "buttons",
     "SELECT now opens it on BUTTONS (the layout in use)");
  await press(p, 8);
  await p.until(`!document.getElementById("padGuide").classList.contains("open")`, 3000);

  // ── The menu entry ──
  await p.js(`document.getElementById("menu").classList.add("open"); 1`);
  await p.js(`document.getElementById("btnPadGuide").click(); 1`);
  ok(await isOpen(p), "the menu's CONTROLLER GUIDE key opens the guide");
  ok(!(await p.js(`document.getElementById("menu").classList.contains("open")`)), "…in place of the menu");
  ok((await tab(p)) === "buttons", "…on the layout in use");
  await p.js(`document.getElementById("pgClose").click(); 1`);
  ok(!(await isOpen(p)), "its CLOSE key closes it");
  await p.js(`document.getElementById("btnPadGuide").click(); 1`);
  await p.js(`window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" })); 1`);
  ok(!(await isOpen(p)), "Escape closes it, as every panel");

  // ── The layout follows the window ──
  await hold(p, 9, 1200);                                              // back to two sticks
  await press(p, 8);
  await p.until(`document.getElementById("padGuide").classList.contains("open")`, 3000);
  for (const [w, h, cls] of [[844, 390, "pgCols2"], [390, 844, "pgCols1"], [700, 900, "pgCols2"], [1440, 900, "pgCols3"]]) {
    await size(p, w, h);
    await sleep(400);
    const got = await p.js(`["pgCols1","pgCols2","pgCols3"].find(c => document.getElementById("pgGuide").classList.contains(c))`);
    const lines = await lineCount(p);
    ok(got === cls && (cls === "pgCols3" ? lines === 7 : lines === 0), `${w}×${h}: ${cls}${cls === "pgCols3" ? ", with lines" : ", no leader lines"} (${got}, ${lines} lines)`);
    ok(await p.js(`document.documentElement.scrollWidth <= innerWidth && document.getElementById("padGuide").scrollWidth <= document.getElementById("padGuide").clientWidth + 1`),
       `${w}×${h}: nothing runs off sideways`);
    await shot(p, `${w}x${h}-sticks`);
  }
  // Transparency SOLID (the control colours): the guide is opaque like every panel.
  // ★ Two input events: main.ts slider() applies BEFORE it saves, and applyCtlLook reads the saved pref — so one
  //   event applies the previous value.
  await p.js(`(() => { const s = document.getElementById("ctlSolid"); if (!s) return 0; s.value = "100"; s.dispatchEvent(new Event("input", { bubbles: true })); s.dispatchEvent(new Event("input", { bubbles: true })); return 1; })()`);
  await sleep(300);
  const bg = await p.js(`getComputedStyle(document.getElementById("padGuide")).backgroundColor`);
  const menuBg = await p.js(`getComputedStyle(document.getElementById("menu")).backgroundColor`);
  ok(/^rgb\(/.test(bg) && bg === menuBg, `transparency SOLID: the guide's glass goes opaque, as the menu's (${bg} / ${menuBg})`);
  await shot(p, "1440x900-solid");
  await press(p, 8);
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  await p.js(`localStorage.removeItem("vibe.padProfile"); 1`);
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
