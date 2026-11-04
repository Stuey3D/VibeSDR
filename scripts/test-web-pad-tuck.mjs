#!/usr/bin/env node
// test-web-pad-tuck.mjs — V12 THE TUNE AND ZOOM KEYS TUCK AWAY while a pad, keyboard or remote drives (Stuart,
// 2026-11-04: "if gamepad, keyboard, remote control detected the tune and zoom controls … disappear, if I revert back
// to touchscreen or trackpad/mouse then they … back"; "a simple fade in and out"; on the R36S "they dont show at all
// unless i plug a mouse into it"). index.html body.padTuck, main.ts padTuckSync, gamepad.ts driveInput. A real
// (headless, MUTED) Edge against the real vibeserver and a fake radio; the pad is SIMULATED, as in
// test-web-pad-keyboard.mjs.
//
//  Checked:
//   • a fresh page with a mouse: keys out; a pad merely CONNECTED (stick drift) changes nothing;
//   • a pad press → tucked (faded, then 0×0 and hidden), the bar re-flows, nothing clipped — at 640×480 too;
//   • the D-pad highlight never lands on a tucked key; the pad still tunes (left stick) and zooms (right stick);
//   • a mouse click → back out; a real mouse MOVE → back out; a wheel → back out;
//   • a keyboard key that drives (> tune) → tucked; a character typed into a box → not;
//   • the option OFF → never tucks; ON again → follows the input again;
//   • pad disconnect → back out (a device with a mouse);
//   • PAD-FIRST START: no touch and no fine pointer (matchMedia emulated) → tucked before any input; ?input=pad too;
//   • prefers-reduced-motion: no transition at all;
//   • the 640×480 status line takes the portrait arrangement (two lines).
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-pad-tuck.mjs   (exit 3 = not run: no binary)
//   WEB_DIST=web/dist   serve THIS tree's built page (node scripts/build-web.mjs) in front of an older binary
//   SHOTS=<dir>         save before/after pictures at 640×480, 480×640, 390×844 and 1280×800 in three control looks
import { spawn } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
const DIST = process.env.WEB_DIST ? path.resolve(ROOT, process.env.WEB_DIST) : null;
const SHOTS = process.env.SHOTS;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); if (!c) fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-padtuck-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Tuck test", locator: "IO92nh",
  adminPass: "pad-admin-pass", radios: [{ serial: "tk", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), srvPort = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(srvPort), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });

// ★ WEB_DIST: the page and its scripts from this tree's build, everything else (the API, the sockets) proxied to the
//   binary — so the page under test is this tree's even when the binary was built from an older one.
let port = srvPort, front = null;
if (DIST) {
  port = await freePort();
  const type = (f) => (f.endsWith(".js") ? "text/javascript" : f.endsWith(".css") ? "text/css" : "application/octet-stream");
  front = http.createServer((req, res) => {
    const u = (req.url || "/").split("?")[0];
    if (u === "/" || u === "/index.html") {
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" });
      res.end(fs.readFileSync(path.join(DIST, "vibesdr.html"))); return;
    }
    if (u.startsWith("/vs/") && fs.existsSync(path.join(DIST, u))) {
      res.writeHead(200, { "Content-Type": type(u), "Cache-Control": "no-store" }); res.end(fs.readFileSync(path.join(DIST, u))); return;
    }
    const up = http.request({ host: "127.0.0.1", port: srvPort, path: req.url, method: req.method, headers: { ...req.headers, host: `127.0.0.1:${srvPort}` } },
      (r) => { res.writeHead(r.statusCode || 502, r.headers); r.pipe(res); });
    up.on("error", () => { try { res.writeHead(502).end(); } catch {} });
    req.pipe(up);
  });
  front.on("upgrade", (req, sock, head) => {
    const up = net.connect(srvPort, "127.0.0.1", () => {
      up.write(`${req.method} ${req.url} HTTP/1.1\r\n` + Object.entries({ ...req.headers, host: `127.0.0.1:${srvPort}` }).map(([k, v]) => `${k}: ${v}`).join("\r\n") + "\r\n\r\n");
      if (head?.length) up.write(head);
      up.pipe(sock); sock.pipe(up);
    });
    up.on("error", () => sock.destroy()); sock.on("error", () => up.destroy());
  });
  await new Promise((r) => front.listen(port, "127.0.0.1", r));
}

const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1280,800", "about:blank"], { stdio: "ignore" });

// The simulated pad (17 buttons, 4 axes — two sticks), NOT connected until __connect(): navigator.getGamepads shows it only then.
const FAKE_PAD = `
  window.__pad = { id: 'Test pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  window.__padOn = /[?&]padAtLoad=1/.test(location.search);
  navigator.getGamepads = () => [window.__padOn ? window.__pad : null, null, null, null];
  window.__btn = (i, on) => { window.__pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
  window.__connect = () => { window.__padOn = true; window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: window.__pad })); };
  window.__disconnect = () => { window.__padOn = false; window.dispatchEvent(Object.assign(new Event('gamepaddisconnected'), { gamepad: window.__pad })); };
  // ★ A handheld: no touch, no mouse (CDP cannot emulate any-pointer, so the page's own question is answered here).
  if (sessionStorage.getItem('__noPointer') === '1') {
    const real = window.matchMedia.bind(window);
    window.matchMedia = (q) => /any-pointer:\\s*(fine|coarse)|any-hover:\\s*hover/.test(q)
      ? { matches: false, media: q, onchange: null, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}, dispatchEvent() { return false; } }
      : real(q);
    Object.defineProperty(navigator, 'maxTouchPoints', { get: () => 0 });
  }
`;

async function cleanup() { for (const p of [br, srv, rtl]) { try { p.kill(); } catch {} } try { front?.close(); } catch {} await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }

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
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(120); } return false; };
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await call("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 }); };
  const key = async (k, code, vk) => {
    await call("Input.dispatchKeyEvent", { type: "keyDown", key: k, text: k.length === 1 ? k : undefined, code, windowsVirtualKeyCode: vk });
    await call("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  };
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_PAD });
  await call("Page.navigate", { url });
  return { ws, js, until, tap, key, errors, call };
}

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
const connectAndRun = async (p) => {
  await p.until(`!!document.getElementById("btnConnect")`, 15000);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  return p.until(`parseFloat(document.getElementById("freqInput").value) > 0`, 15000);
};
const B = { S: 0, E: 1, W: 2, N: 3, L1: 4, R1: 5, SELECT: 8, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
const freq = (p) => p.js(`parseFloat(document.getElementById("freqInput").value)`);
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(90); await p.js(`__btn(${i}, false); 1`); await sleep(90); };
// Turn a stick (axes ax, ax+1) from `from`° to `to`°, then let it go.
const turn = async (p, ax, from, to) => {
  const st = to > from ? 5 : -5;
  for (let d = from; st > 0 ? d <= to : d >= to; d += st) {
    await p.js(`__pad.axes[${ax}] = ${Math.cos(d * Math.PI / 180)}; __pad.axes[${ax + 1}] = ${Math.sin(d * Math.PI / 180)}; 1`);
    await sleep(30);
  }
  await p.js(`__pad.axes[${ax}] = 0; __pad.axes[${ax + 1}] = 0; 1`); await sleep(100);
};
const tucked = (p) => p.js(`document.body.classList.contains("padTuck")`);
// The keys as drawn: both dials' boxes and look.
const dials = (p) => p.js(`["mVfoDial", "mZoomDial"].map((id) => { const e = document.getElementById(id), r = e.getBoundingClientRect(), c = getComputedStyle(e);
  return { w: Math.round(r.width), h: Math.round(r.height), op: +c.opacity, vis: c.visibility }; })`);
const shown = async (p) => (await dials(p)).every((d) => d.w > 20 && d.h > 20 && d.op > 0.99 && d.vis === "visible");
const gone = async (p) => (await dials(p)).every((d) => d.w === 0 && d.h === 0 && d.vis === "hidden");
// Nothing on the card is clipped or off screen: every control inside the viewport, no text wider than its box.
const fits = (p) => p.js(`(() => { const bad = []; const vw = innerWidth, vh = innerHeight;
  const c = document.getElementById("mcard").getBoundingClientRect();
  if (c.left < -1 || c.right > vw + 1 || c.top < -1 || c.bottom > vh + 1) bad.push("card off screen");
  for (const el of document.querySelectorAll("#mcard #mPill, #mcard .mBtn, #mcard #mFreqBox, #mcard #mMode, #mcard #mStatus")) {
    const r = el.getBoundingClientRect(); if (!r.width || getComputedStyle(el).visibility === "hidden") continue;
    if (el.closest("#mVfoDial, #mZoomDial") && document.body.classList.contains("padTuck")) continue;
    if (r.left < c.left - 1 || r.right > c.right + 1) bad.push((el.id || el.className) + " outside the card");
    if (el.scrollWidth > el.clientWidth + 2 && getComputedStyle(el).overflow !== "visible") bad.push((el.id || el.className) + " clipped");
  }
  const f = document.getElementById("mFreq"); if (f.scrollWidth > f.clientWidth + 2) bad.push("mFreq clipped");
  // pill, stacks and dials never overlap one another
  const parts = ["mPill", "mStackL", "mStackR", "mVfoDial", "mZoomDial"].map((id) => [id, document.getElementById(id).getBoundingClientRect()]).filter(([, r]) => r.width > 1 && r.height > 1);
  for (let i = 0; i < parts.length; i++) for (let j = i + 1; j < parts.length; j++) { const a = parts[i][1], b = parts[j][1];
    if (a.left < b.right - 1 && b.left < a.right - 1 && a.top < b.bottom - 1 && b.top < a.bottom - 1) bad.push(parts[i][0] + " overlaps " + parts[j][0]); }
  return bad; })()`);
const setSize = async (p, w, h) => { await p.call("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: false }); await sleep(450); };
const shot = async (p, name) => {
  if (!SHOTS) return;
  const r = await p.call("Page.captureScreenshot", { format: "png" });
  if (r.result?.data) { fs.mkdirSync(SHOTS, { recursive: true }); fs.writeFileSync(path.join(SHOTS, `${name}.png`), Buffer.from(r.result.data, "base64")); }
};
const span = (p) => p.js(`(() => { const s = window.__vibeWf?.()?.spec; return s ? (s.view?.binBandwidth || 0) * (s.cfg?.binCount || 0) : 0; })()`);

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("tune and zoom keys tuck away while a pad / keyboard / remote drives — a simulated pad, a real browser, the real binary");
  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`, 15000);
  await p.js(`localStorage.removeItem("vibe.padProfile"); 1`);
  ok(await connectAndRun(p), "the receiver is running");
  await setSize(p, 640, 480);

  // ── A hand, then a pad merely connected ──
  ok(!(await tucked(p)) && await shown(p), "a mouse-and-screen device: the tune and zoom keys are out");
  const before = await p.js(`(() => { const c = document.getElementById("mcard").getBoundingClientRect(), f = document.getElementById("mPill").getBoundingClientRect();
    return { cardH: Math.round(c.height), pillW: Math.round(f.width) }; })()`);
  await shot(p, "640x480-keys-out");
  await p.js(`__connect(); __pad.axes[0] = 0.2; __pad.axes[1] = -0.1; __pad.axes[2] = 0.3; 1`);
  await sleep(500);
  ok(!(await tucked(p)) && await shown(p), "a pad connected, sticks drifting under the dead zone: nothing changes");
  await p.js(`__pad.axes[0] = 0; __pad.axes[1] = 0; __pad.axes[2] = 0; 1`);

  // ── A pad press: fade, then the room ──
  await p.js(`__btn(${B.DOWN}, true); 1`); await sleep(70);
  ok(await tucked(p), "a pad press: tucked");
  const mid = await dials(p);
  ok(mid.every((d) => d.w > 20 && d.op < 1 && d.op > 0), `…fading first, still in place (opacity ${mid.map((d) => d.op.toFixed(2)).join(", ")})`);
  await p.js(`__btn(${B.DOWN}, false); 1`);
  await sleep(350);
  ok(await gone(p), "…then gone: 0×0 and hidden");
  const after = await p.js(`(() => { const c = document.getElementById("mcard").getBoundingClientRect(), f = document.getElementById("mPill").getBoundingClientRect();
    return { cardH: Math.round(c.height), pillW: Math.round(f.width) }; })()`);
  ok(after.cardH < before.cardH - 20, `640×480: the card is a row shorter (${before.cardH} → ${after.cardH} px)`);
  const bad640 = await fits(p);
  ok(bad640.length === 0, "640×480 tucked: nothing clipped, nothing overlapping" + (bad640.length ? ": " + bad640.join("; ") : ""));
  ok(await p.js(`getComputedStyle(document.getElementById("mStatus")).flexDirection === "column"`),
     "640×480: the status line takes the portrait arrangement (clock line over link line)");
  await shot(p, "640x480-keys-tucked");

  // ── Focus never lands on a tucked key ──
  const visited = new Set();
  for (let i = 0; i < 48; i++) {
    await press(p, [B.RIGHT, B.DOWN, B.LEFT, B.UP][Math.floor(i / 3) % 4]);
    const h = await p.js(`(() => { const h = document.querySelector(".gpFocus"); return h ? (h.id || h.className) + (h.closest("#mVfoDial, #mZoomDial") ? "@dial" : "") : ""; })()`);
    if (h) visited.add(h);
    if (await p.js(`!!document.querySelector(".open")`)) await press(p, B.E);   // something opened: back out of it
  }
  ok(visited.size >= 3 && ![...visited].some((v) => v.endsWith("@dial")), `the D-pad highlight walks the bar and never lands on a tucked key (${[...visited].slice(0, 8).join(", ")})`);

  // ── The pad still tunes and zooms ──
  const f0 = await freq(p);
  await turn(p, 0, 0, 95);
  ok((await freq(p)) > f0, `left stick turned: tunes (${f0} → ${await freq(p)} MHz)`);
  await turn(p, 0, 95, 0);
  const s0 = await span(p);
  await turn(p, 2, 0, 140);
  await sleep(500);
  const s1 = await span(p);
  ok(s0 > 0 && s1 > 0 && s1 < s0, `right stick turned: zooms in (${Math.round(s0)} → ${Math.round(s1)} Hz)`);
  ok(await tucked(p), "…and the keys stay tucked while the pad drives");

  // ── A hand brings them back: click, real mouse move, wheel ──
  const wfAt = await p.js(`(() => { const r = document.getElementById("wf").getBoundingClientRect(); return [r.left + r.width * 0.5, r.top + 30]; })()`);
  await p.call("Input.dispatchMouseEvent", { type: "mousePressed", x: 5, y: 5, button: "left", clickCount: 1 });
  await p.call("Input.dispatchMouseEvent", { type: "mouseReleased", x: 5, y: 5, button: "left", clickCount: 1 });
  ok(!(await tucked(p)), "a mouse click: back out");
  ok(await p.until(`(() => { const e = document.getElementById("mVfoDial"); return +getComputedStyle(e).opacity > 0.99 && e.getBoundingClientRect().width > 20; })()`, 1500),
     "…in place at once, faded back in");
  await press(p, B.DOWN);
  ok(await tucked(p), "pad again: tucked");
  // A move of the cursor that does not travel (what a browser fires when the layout shifts under it) is not a hand.
  await p.call("Input.dispatchMouseEvent", { type: "mouseMoved", x: 5, y: 5 });
  await sleep(100);
  ok(await tucked(p), "a mouse event without travel (the layout moving under a parked cursor): still tucked");
  for (let i = 1; i <= 6; i++) { await p.call("Input.dispatchMouseEvent", { type: "mouseMoved", x: 5 + i * 8, y: 5 + i * 6 }); await sleep(20); }
  ok(!(await tucked(p)), "the mouse really moved: back out");
  await press(p, B.DOWN);
  await p.call("Input.dispatchMouseEvent", { type: "mouseWheel", x: wfAt[0], y: wfAt[1], deltaX: 0, deltaY: 40 });
  await sleep(150);
  ok(!(await tucked(p)), "a wheel / trackpad scroll: back out");

  // ── The keyboard (and a remote's D-pad, which arrives as keys) ──
  await p.js(`document.activeElement?.blur?.(); 1`);
  const k0 = await freq(p);
  await p.key(">", "Period", 190);
  await sleep(200);
  ok(await tucked(p), "a key that drives the receiver (> tune up): tucked");
  ok((await freq(p)) !== k0, "…and it tuned");
  await p.tap(5, 5);
  ok(!(await tucked(p)), "a click: back out");
  await p.key("ArrowDown", "ArrowDown", 40);
  await sleep(150);
  ok(await tucked(p), "an arrow key (a TV remote's D-pad): tucked");
  await p.tap(5, 5);
  // A character typed into a box is not driving: open the frequency card and type in it.
  await p.js(`document.getElementById("mFreqBox").click(); 1`);
  await sleep(300);
  await p.js(`document.getElementById("freqInput").focus(); 1`);
  await p.key("9", "Digit9", 57);
  await sleep(150);
  ok(!(await tucked(p)), "typing into the frequency box: not driving, keys stay out");
  await p.js(`document.getElementById("freqInput").blur(); 1`);
  await press(p, B.E); await sleep(200);                                 // ○ back: closes the card (and tucks)
  await p.tap(5, 5);

  // ── The option ──
  ok(await p.js(`document.getElementById("padTuckTg").textContent === "ON"`), "MENU → GAME CONTROLLER: TUCK AWAY TUNE AND ZOOM KEYS is ON by default");
  await p.js(`document.getElementById("padTuckTg").click(); 1`);
  await press(p, B.DOWN); await p.key(">", "Period", 190); await sleep(350);
  ok(!(await tucked(p)) && await shown(p), "OFF: a pad and a key never tuck them");
  ok(await p.js(`JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").padTuck === false`), "…remembered (savePref)");
  await p.js(`document.getElementById("padTuckTg").click(); 1`);
  ok(await tucked(p), "ON again: follows the input at once (the last was the keyboard → tucked)");

  // ── Pad unplugged on a device with a mouse ──
  await press(p, B.DOWN);
  await p.js(`__disconnect(); 1`);
  ok(await p.until(`!document.body.classList.contains("padTuck")`, 1500), "the pad unplugged: back out");

  // ── Reduced motion ──
  await p.call("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-motion", value: "reduce" }] });
  await p.js(`__connect(); 1`);
  await press(p, B.DOWN);
  ok(await gone(p), "prefers-reduced-motion: gone at once, no fade");
  ok(await p.js(`getComputedStyle(document.getElementById("mVfoDial")).transitionDuration.split(",").every((d) => parseFloat(d) === 0)`), "…no transition at all");
  await p.call("Emulation.setEmulatedMedia", { features: [] });

  // ── Screenshots: the sizes and looks, out and tucked ──
  if (SHOTS) {
    const looks = { default: {}, silver: { ctlFont: "#c8c8c8" }, black: { ctlBg: "#eef3ff", ctlFont: "#111111", ctlSolid: 100 } };
    for (const [look, prefs] of Object.entries(looks)) {
      await p.js(`(() => { const k = "vibesdr_web_prefs_v1", v = JSON.parse(localStorage.getItem(k) || "{}");
        for (const x of ["ctlBg", "ctlBtn", "ctlFont", "ctlSolid"]) delete v[x]; Object.assign(v, ${JSON.stringify(prefs)}); localStorage.setItem(k, JSON.stringify(v)); })(); 1`);
      await p.call("Page.reload"); await sleep(1500);
      await connectAndRun(p);
      await p.js(`__connect(); 1`);
      for (const [w, h] of [[640, 480], [480, 640], [390, 844], [1280, 800]]) {
        await setSize(p, w, h);
        await p.tap(5, 5); await sleep(300);
        const bo = await fits(p);
        ok(bo.length === 0, `${look} ${w}×${h} keys out: fits` + (bo.length ? ": " + bo.join("; ") : ""));
        await shot(p, `${look}-${w}x${h}-out`);
        await press(p, B.DOWN); await sleep(350);
        const bt = await fits(p);
        ok(bt.length === 0 && await gone(p), `${look} ${w}×${h} tucked: fits` + (bt.length ? ": " + bt.join("; ") : ""));
        await shot(p, `${look}-${w}x${h}-tucked`);
      }
    }
    await p.js(`(() => { const k = "vibesdr_web_prefs_v1", v = JSON.parse(localStorage.getItem(k) || "{}");
      for (const x of ["ctlBg", "ctlBtn", "ctlFont", "ctlSolid"]) delete v[x]; localStorage.setItem(k, JSON.stringify(v)); })(); 1`);
  }
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));

  // ── PAD-FIRST START: a handheld (no touch, no mouse) — tucked before any input ──
  await p.call("Emulation.clearDeviceMetricsOverride");
  await p.js(`sessionStorage.setItem("__noPointer", "1"); 1`);
  await p.call("Page.reload"); await sleep(1500);
  await setSize(p, 640, 480);
  await p.until(`!!document.getElementById("btnConnect")`, 15000);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await p.until(`!!document.getElementById("audioGate")`, 15000);
  await press(p, B.R1);   // (any way past the audio gate — a pad's enter is what a handheld has)
  await p.js(`document.querySelector("#audioGate button")?.click(); 1`);
  await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`, 15000);
  ok(await p.js(`document.body.classList.contains("padTuck")`), "no touch and no fine pointer: tucked from the start, before any input");
  await shot(p, "640x480-padfirst-start");
  await p.js(`__connect(); 1`);
  await p.js(`__disconnect(); 1`); await sleep(300);
  ok(await tucked(p), "…a pad-first device keeps them tucked when its pad is unplugged");
  await p.js(`sessionStorage.removeItem("__noPointer"); 1`);

  // ── ?input=pad (the kiosk's hint) ──
  await p.call("Page.navigate", { url: `http://127.0.0.1:${port}/?input=pad` }); await sleep(1500);
  await p.until(`!!document.getElementById("btnConnect")`, 15000);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await p.until(`!!document.getElementById("audioGate")`, 15000);
  await p.js(`document.querySelector("#audioGate button")?.click(); 1`);
  await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`, 15000);
  ok(await p.js(`document.body.classList.contains("padTuck")`), "?input=pad: tucked from the start, before any input");
  await p.tap(5, 5);
  ok(!(await tucked(p)), "…and a mouse plugged in and clicked brings them out");
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n${fails} FAILED` : "\nall passed");
process.exit(fails ? 1 : 0);
