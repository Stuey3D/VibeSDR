#!/usr/bin/env node
// test-web-pointer-frequency.mjs — FREQUENCY AT THE POINTER on the served receiver page (Kiko, 2026-10-16: "show the
// frequency on the mouse cursor on desktop like in OpenWebRX"), in a real (headless, MUTED) browser against the real
// vibeserver and a fake radio.
//
//  A MOUSE over the waterfall (and the spectrum above it) shows a label with the frequency under it; that frequency is
//  exactly what a click at the same spot tunes to; the label goes on leave and while a button is down; a TOUCH never
//  shows it; the menu's FREQUENCY AT POINTER switch turns it off (and is remembered); and on a touch-only device the
//  switch is not offered at all.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-pointer-frequency.mjs   (exit 3 = not run: no binary)
//        SHOT=/path/to.png also saves a screenshot of the label over the waterfall.
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

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-ptrfreq-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Pointer test", locator: "IO92nh",
  adminPass: "ptr-admin-pass", radios: [{ serial: "pf", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
// ★ --mute-audio: tests are SILENT (an agent once blasted test tones through Stuart's speakers).
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });

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
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(100); } return false; };
  const mouse = (type, x, y, extra = {}) => call("Input.dispatchMouseEvent", { type, x, y, button: "none", ...extra });
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await mouse(type, x, y, { button: "left", clickCount: 1 }); };
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.navigate", { url });
  return { ws, call, js, until, mouse, tap, errors };
}

const SHOWN = `document.getElementById("ptrFreq").classList.contains("show")`;
// The label's frequency in Hz: main (MHz) + the fine 100/10 Hz digits when present. Same parts as the card readout.
const LABEL_HZ = `(() => { const e = document.getElementById("ptrFreq"); const m = e.querySelector(".m").textContent, f = e.querySelector(".f").textContent;
  return Math.round(parseFloat(m) * 1e6 + (f ? parseInt(f, 10) * 10 : 0)); })()`;
const CARD_HZ = `(() => { const m = document.getElementById("mFreq")?.textContent || "", f = document.getElementById("mFreqFine")?.textContent || "";
  return Math.round(parseFloat(m) * 1e6 + (f ? parseInt(f, 10) * 10 : 0)); })()`;
const CANVAS = `(() => { const r = document.getElementById("wf").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`;
const hitIsCanvas = (x, y) => `document.elementFromPoint(${x}, ${y})?.id === "wf"`;

async function start(p) {
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(1500);
  // START (a real tap: audio needs a gesture) — found by its label, wherever the splash put it.
  const at = await p.js(`(() => { const b = [...document.querySelectorAll("button")].find((x) => /^\\s*START( RADIO)?\\s*$/i.test(x.textContent || "") && x.getBoundingClientRect().width > 2);
    if (!b) return null; const r = b.getBoundingClientRect(); return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) }; })()`);
  if (at) await p.tap(at.x, at.y);
  // ★ The audio gate (#audioGate, "START RADIO") can arrive AFTER that first look — after a reload it often does — and
  //   then it covers the waterfall, so no mouse event reaches the canvas (the "switched back ON" flake, 2026-10-31). Wait
  //   until it is gone, pressing its button with a real tap whenever it is up.
  const GATE_UP = `(() => { const g = document.getElementById("audioGate"); return !!g && g.isConnected && g.style.display !== "none" && getComputedStyle(g).display !== "none"; })()`;
  for (let i = 0; i < 40; i++) {
    if (!(await p.js(GATE_UP))) { await sleep(250); if (!(await p.js(GATE_UP))) break; continue; }
    // "Press anywhere to start listening" — but NOT on its other buttons (View tutorial, the Transparency switch): find a
    //   spot on the gate whose top element is the gate itself or its START text, never a button.
    const g = await p.js(`(() => { const gate = document.getElementById("audioGate"); const r = gate.getBoundingClientRect();
      for (const fy of [0.08, 0.15, 0.25, 0.92, 0.5]) for (const fx of [0.1, 0.5, 0.9]) {
        const x = Math.round(r.left + r.width * fx), y = Math.round(r.top + r.height * fy), e = document.elementFromPoint(x, y);
        if (e && gate.contains(e) && !e.closest("button, a, input, label")) return { x, y };
      } return null; })()`);
    if (g) await p.tap(g.x, g.y);
    await sleep(300);
  }
  return p.until(`parseFloat(document.getElementById("freqInput").value) > 0 && ${CARD_HZ} > 0`);
}

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("frequency at the pointer on the served page — a real browser, the real binary");

  const p = await open(`http://127.0.0.1:${port}/`);
  ok(await start(p), "the receiver is running");
  await sleep(800);                                                    // a few frames, so the span is real
  const r = await p.js(CANVAS);
  // A spot over the WATERFALL (lower half) and one over the SPECTRUM (top of the same canvas), clear of overlays.
  const wx = Math.round(r.x + r.w * 0.3), wy = Math.round(r.y + r.h * 0.6);
  const sx = Math.round(r.x + r.w * 0.62), sy = Math.round(r.y + r.h * 0.12);
  ok(await p.js(hitIsCanvas(wx, wy)) && await p.js(hitIsCanvas(sx, sy)), "(the test points are on the waterfall canvas, not an overlay)");

  await p.mouse("mouseMoved", wx, Math.round(r.y - 30));            // the START tap left the mouse over the canvas
  ok(await p.until(`!${SHOWN}`, 2000), "no label while the mouse is off the waterfall");
  ok(await p.js(`!document.getElementById("rowPtrFreq").hidden`), "the menu offers FREQUENCY AT POINTER on a mouse device");
  ok(await p.js(`document.getElementById("ptrFreqTg").textContent.trim() === "ON"`), "…and it defaults ON");

  // ── Hover the waterfall ──
  await p.mouse("mouseMoved", wx - 20, wy); await p.mouse("mouseMoved", wx, wy);
  ok(await p.until(SHOWN, 2000), "mouse over the WATERFALL: the label appears");
  const lab = await p.js(`(() => { const e = document.getElementById("ptrFreq").getBoundingClientRect(); return { x: e.left, y: e.top, w: e.width, h: e.height }; })()`);
  ok(lab.x > wx && lab.y > wy, `…below-right of the pointer tip, not under it (label at ${Math.round(lab.x)},${Math.round(lab.y)}; pointer ${wx},${wy})`);
  const unit = await p.js(`document.querySelector("#ptrFreq .u").textContent`);
  ok(unit === "MHz", `…in the readout's unit (${unit})`);
  if (process.env.SHOT) {
    // Let the waterfall fill, point near the station, and draw a stand-in arrow (headless shots have no cursor).
    await sleep(8000);
    const cx = Math.round(r.x + r.w * 0.43), cy = Math.round(r.y + r.h * 0.42);
    await p.mouse("mouseMoved", cx, cy); await sleep(200);
    await p.js(`(() => { const d = document.createElement("div"); d.id = "__arrow"; d.style.cssText = "position:fixed;left:${cx}px;top:${cy}px;z-index:9999;pointer-events:none";
      d.innerHTML = '<svg width="14" height="21" viewBox="0 0 14 21"><path d="M1 1v16l4-4 3 7 3-1-3-7h5z" fill="#fff" stroke="#000" stroke-width="1.2"/></svg>';
      document.body.appendChild(d); })(); 1`);
    const shot = await p.call("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync(process.env.SHOT, Buffer.from(shot.result.data, "base64"));
    await p.js(`document.getElementById("__arrow").remove(); 1`);
    await p.mouse("mouseMoved", wx, wy); await sleep(150);
    console.log(`  (screenshot saved to ${process.env.SHOT})`);
  }
  const hzHover = await p.js(LABEL_HZ);
  const before = await p.js(CARD_HZ);
  ok(hzHover > 0 && Math.abs(hzHover - before) > 1000, `the label reads a frequency away from the dial (${hzHover} Hz; dial ${before} Hz)`);

  // ── The label says what the click does ──
  await p.mouse("mousePressed", wx, wy, { button: "left", clickCount: 1, buttons: 1 });
  await sleep(80);
  ok(!(await p.js(SHOWN)), "the label hides while the button is down (a drag pans)");
  await p.mouse("mouseReleased", wx, wy, { button: "left", clickCount: 1 });
  ok(await p.until(`${CARD_HZ} === ${hzHover}`, 4000), `a click at the same spot tunes to exactly the label's frequency (card ${await p.js(CARD_HZ)} Hz)`);
  await p.mouse("mouseMoved", wx + 1, wy);
  ok(await p.until(SHOWN, 2000), "…and it comes back on the next move");

  // ── The spectrum (top of the same canvas) ──
  await p.mouse("mouseMoved", sx, sy);
  ok(await p.until(`${SHOWN} && ${LABEL_HZ} > ${hzHover}`, 2000), "mouse over the SPECTRUM: the label follows, reading higher further right");

  // ── Right edge: it flips to the left of the pointer instead of clipping ──
  const ex = Math.round(r.x + r.w - 6), ey = Math.round(r.y + r.h * 0.55);
  if (await p.js(hitIsCanvas(ex, ey))) {
    await p.mouse("mouseMoved", ex, ey); await sleep(150);
    const e2 = await p.js(`(() => { const e = document.getElementById("ptrFreq").getBoundingClientRect(); return { l: e.left, r: e.right }; })()`);
    ok(e2.r <= r.x + r.w && e2.l < ex, `near the right edge the label flips left and stays on screen (right ${Math.round(e2.r)} ≤ ${Math.round(r.x + r.w)})`);
  }

  // ── Leave ──
  // ── SNAP WATERFALL TUNING TO STEP RATE (Stuart, 2026-10-31): the switch at the foot of the step menu ──
  //   ON (default) a click lands on the step grid; OFF it tunes to exactly where the click lands. The label agrees both ways.
  const stepHz = await p.js(`(() => { const t = document.getElementById("mStep").textContent.trim(); const n = parseFloat(t);
    return /MHz/.test(t) ? n * 1e6 : /kHz/.test(t) ? n * 1e3 : n; })()`);
  await p.js(`document.getElementById("mStep").click(); 1`);
  ok(await p.until(`!!document.getElementById("tapSnapSw")`, 2000), `the step menu has the SNAP switch at its foot (step ${stepHz} Hz)`);
  ok(await p.js(`document.getElementById("tapSnapSw").getAttribute("aria-checked") === "true"`), "…ON by default");
  const qx = wx + 37;
  // ★ Off the canvas and back on, then WAIT for the label (a single move after a reload does not always raise it).
  await p.mouse("mouseMoved", 2, 2); await p.mouse("mouseMoved", qx - 3, wy); await p.mouse("mouseMoved", qx, wy);
  await p.until(SHOWN, 2000);
  const snapHzOn = await p.js(LABEL_HZ);
  ok(stepHz > 1 && snapHzOn % stepHz === 0, `ON: the click target is on the step grid (${snapHzOn} Hz)`);
  await p.js(`document.getElementById("tapSnapSw").click(); 1`);
  ok(await p.js(`document.getElementById("tapSnapSw").getAttribute("aria-checked") === "false" && !!document.getElementById("stepMenu")`),
     "switched OFF — and the menu stays open (flicking it is not choosing a step)");
  ok(await p.js(`/exactly where/.test(document.getElementById("tapSnapHint").textContent)`), "…the line under it says what OFF does");
  await p.js(`document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true })); document.getElementById("stepMenu")?.remove(); 1`);
  await p.mouse("mouseMoved", 2, 2); await p.mouse("mouseMoved", qx + 1, wy); await p.mouse("mouseMoved", qx, wy);
  await p.until(SHOWN, 2000); await sleep(100);
  const freeHz = await p.js(LABEL_HZ);
  ok(freeHz > 0 && freeHz % stepHz !== 0, `OFF: the pointer reads the exact frequency, off the step grid (${freeHz} Hz; step ${stepHz} Hz)`);
  await p.mouse("mousePressed", qx, wy, { button: "left", clickCount: 1, buttons: 1 }); await sleep(80);
  await p.mouse("mouseReleased", qx, wy, { button: "left", clickCount: 1 });
  ok(await p.until(`${CARD_HZ} === ${freeHz}`, 4000), `OFF: a click tunes to exactly there (card ${await p.js(CARD_HZ)} Hz)`);
  ok(await p.js(`JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").tapSnap === false`), "…remembered in this browser's settings");
  await p.js(`document.getElementById("mStep").click(); 1`);
  await p.until(`!!document.getElementById("tapSnapSw")`, 2000);
  await p.js(`document.getElementById("tapSnapSw").click(); document.getElementById("stepMenu")?.remove(); 1`);
  ok(await p.js(`JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").tapSnap === true`), "switched back ON");
  await p.mouse("mouseMoved", wx, Math.round(r.y - 30));            // off the canvas, onto the scale above it
  ok(await p.until(`!${SHOWN}`, 2000), "the label hides when the mouse leaves the waterfall");

  // ── Touch never shows it ──
  await p.call("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
  const tf0 = await p.js(CARD_HZ);
  await p.call("Input.dispatchTouchEvent", { type: "touchStart", touchPoints: [{ x: wx + 40, y: wy }] });
  let touchShown = false;
  for (let i = 0; i < 6; i++) { await sleep(40); touchShown ||= await p.js(SHOWN); }
  await p.call("Input.dispatchTouchEvent", { type: "touchMove", touchPoints: [{ x: wx + 41, y: wy }] });
  await p.call("Input.dispatchTouchEvent", { type: "touchEnd", touchPoints: [] });
  for (let i = 0; i < 6; i++) { await sleep(40); touchShown ||= await p.js(SHOWN); }
  ok(!touchShown, "a TOUCH on the waterfall never shows the label");
  ok(await p.until(`${CARD_HZ} !== ${tf0}`, 3000), "(…and the touch did reach the waterfall: it tuned)");
  await p.call("Emulation.setTouchEmulationEnabled", { enabled: false });

  // ── The switch turns it off, and is remembered ──
  await p.js(`document.getElementById("ptrFreqTg").click(); 1`);
  ok(await p.js(`document.getElementById("ptrFreqTg").textContent.trim() === "OFF"`), "FREQUENCY AT POINTER switched OFF");
  await p.mouse("mouseMoved", wx, wy); await p.mouse("mouseMoved", wx + 3, wy);
  let offShown = false;
  for (let i = 0; i < 6; i++) { await sleep(40); offShown ||= await p.js(SHOWN); }
  ok(!offShown, "…and the mouse over the waterfall shows no label");
  ok(await p.js(`JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").ptrFreq === false`), "…remembered in this browser's settings");
  await p.mouse("mouseMoved", wx, Math.round(r.y - 30));
  await p.js(`location.reload(); 1`);
  ok(await start(p), "(reloaded)");
  await sleep(600);
  await p.mouse("mouseMoved", wx, wy); await p.mouse("mouseMoved", wx + 2, wy);
  let reShown = false;
  for (let i = 0; i < 6; i++) { await sleep(40); reShown ||= await p.js(SHOWN); }
  ok(!reShown && await p.js(`document.getElementById("ptrFreqTg").textContent.trim() === "OFF"`), "…still OFF after a reload");
  await p.js(`document.getElementById("ptrFreqTg").click(); 1`);
  await p.mouse("mouseMoved", wx + 5, wy);
  ok(await p.until(SHOWN, 2000), "switched back ON: the label returns");
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));

  // ── A touch-only device: the switch is not offered (it could do nothing there) ──
  //   Emulated LIVE on the running page, so this also proves the row follows a change (a mouse unplugged).
  ok(await p.js(`!document.getElementById("rowPtrFreq").hidden`), "(row shown while there is a mouse)");
  await p.call("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
  await p.call("Emulation.setEmitTouchEventsForMouse", { enabled: true, configuration: "mobile" });
  await sleep(300);
  const mq = await p.js(`matchMedia("(any-hover: hover) and (any-pointer: fine)").matches`);
  if (mq === false) ok(await p.until(`document.getElementById("rowPtrFreq").hidden`, 2000), "touch-only device: the FREQUENCY AT POINTER row is hidden");
  else console.log(`  (skipped: this browser cannot emulate any-hover/any-pointer: ${mq} ${await p.js(`[matchMedia("(any-hover: none)").matches, matchMedia("(hover: none)").matches, matchMedia("(any-pointer: coarse)").matches]`)})`);
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
