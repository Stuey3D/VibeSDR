#!/usr/bin/env node
// test-web-handheld.mjs — the R36S handheld's PAGE (web/client/src/handheld.ts) against the real supervisor
// (`vibeserver --handheld`), fake radios and the fake hardware service, in a real headless MUTED Edge at 640×480 with a
// simulated pad. TESTS MUST BE SILENT: --mute-audio --disable-audio-output.
//
//  · no radio: "Please connect an SDR", no landing page, no start-audio screen
//  · a radio plugged in: the page reloads straight into the receiver; the status row says "Spectrum rate … fps" and
//    nothing about the link (no KB/s, ping, buffer, no link bars); no CHAT, a DEVICE key in its cell
//  · the menu: R36S HARDWARE / SDR HARDWARE / DISPLAY, one open at a time, ○ (back) collapses; the radio's own gain
//    row lives in SDR HARDWARE; brightness, SPECTRUM RATE, DRAWING RATE and SCREEN OFF in DISPLAY
//  · the drawing cap (30 fps) and the Performance overlay ("UI … fps", plain text)
//  · the hardware service's volume event → the overlay; battery_low → the banner; a brightness change reaches it
//  · per-radio memory: a pref set on radio A is gone on radio B and back on A
//  · a recording lands on the card and is listed with a player
//  · SPECTRUM RATE 5 / 10 / 20: the status row follows; the engine's CPU and the page's main-thread time at each
//    (indicative only on a Mac — the R36S figures come from the integration pass)
//  · SET FROM MAP (handheldMap.ts), with NO radio (from "Please connect an SDR") and from the menu: the in-page map over
//    the page, the offline map files only; the D-pad / stick pan, the shoulders / right stick zoom, ✕ stores the
//    crosshair's point (and its locator is the one shown), ○ leaves the location alone; the keyboard does the same;
//    nothing underneath tunes or moves while it is up
//  · and on an ORDINARY server: the landing page's START takes the D-pad (the fix is for every mode, TV remotes too)
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-handheld.mjs   (exit 3 = not run)
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { startFakeHwd } from "./fake-vibe-hwd.mjs";

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
if (!fs.existsSync(BROWSER)) { console.error("no Edge at " + BROWSER + " — not run"); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0, passes = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); c ? passes++ : fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-web-handheld-"));
const data = path.join(T, "data"), card = path.join(T, "card"), radios = path.join(T, "radios.txt");
fs.mkdirSync(data); fs.mkdirSync(card); fs.writeFileSync(radios, "");
const hwd = await startFakeHwd(path.join(T, "hwd.sock"));
const kids = [];
const fakeRtl = async () => { const p = await freePort();
  kids.push(spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(p), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" })); return p; };
const rtlA = await fakeRtl(), rtlB = await fakeRtl();
const port = await freePort(), cdp = await freePort();
const sup = spawn(BIN, ["--handheld", "--port", String(port)], {
  env: { ...process.env, VIBESERVER_DATA_DIR: data, VIBESERVER_CARD_DIR: card, VIBE_HWD_SOCK: path.join(T, "hwd.sock"),
         VIBE_HANDHELD_FAKE_RADIOS: radios, VIBE_HANDHELD_RESET_FLAG: path.join(T, "reset"), HOME: T,
         VIBESERVER_MAPDATA_DIR: path.join(ROOT, "assets/mapdata/v1") },   // ★ the basic map pack, as the image ships it
  stdio: ["ignore", fs.openSync(path.join(T, "handheld.log"), "w"), fs.openSync(path.join(T, "handheld.log"), "a")] });
kids.push(sup);
const plug = (serial, rtl) => fs.writeFileSync(radios, rtl ? `${serial} rtlsdr 127.0.0.1:${rtl}\n` : "");
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions",
  "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`,
  "--window-size=640,480", ...(process.getuid && process.getuid() === 0 ? ["--no-sandbox"] : []), "about:blank"], { stdio: "ignore" });   // ★ root in a Linux test container: Chromium needs --no-sandbox
kids.push(br);
const FAKE_PAD = `
  window.__pad = { id: 'R36S (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad, null, null, null];
  window.__btn = (i, on) => { window.__pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
  window.__connect = () => window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: window.__pad }));
`;
async function page(url) {
  let target;
  for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === "page"); } catch { await sleep(250); } }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { try { if (await js(expr)) return true; } catch { /* navigating */ } await sleep(150); } return false; };
  await call("Runtime.enable"); await call("Page.enable"); await call("Performance.enable");
  await call("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_PAD });
  await call("Page.navigate", { url });
  return { ws, js, until, call, errors };
}
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(120); await p.js(`__btn(${i}, false); 1`); await sleep(140); };
const UP = 12, DOWN = 13, LEFT = 14, RIGHT = 15, CROSS = 0, CIRCLE = 1, L1 = 4, R1 = 5;
/** A stick held for `ms`, then let go: axes 0/1 = left, 2/3 = right. */
const stick = async (p, axis, v, ms) => { await p.js(`__pad.axes[${axis}] = ${v}; 1`); await sleep(ms); await p.js(`__pad.axes[${axis}] = 0; 1`); await sleep(150); };
/** A real key, through the browser's input pipeline (not a synthetic event). */
const VK = { ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Enter: 13, Escape: 27, PageUp: 33, PageDown: 34, "+": 187, "-": 189 };
const key = async (p, k) => {
  const base = { key: k, code: k.length === 1 ? (k === "+" ? "Equal" : "Minus") : k, windowsVirtualKeyCode: VK[k], nativeVirtualKeyCode: VK[k] };
  await p.call("Input.dispatchKeyEvent", { type: "keyDown", ...base, ...(k === "Enter" ? { text: "\r" } : k.length === 1 ? { text: k } : {}) });
  await p.call("Input.dispatchKeyEvent", { type: "keyUp", ...base }); await sleep(120);
};
/** The 6-character locator — the test's own, so the page's is checked, not trusted. */
const locatorOf = (lat, lon) => { const A = "ABCDEFGHIJKLMNOPQRSTUVWX"; const la = lat + 90, lo = ((lon + 180) % 360 + 360) % 360;
  return A[Math.floor(lo / 20)] + A[Math.floor(la / 10)] + Math.floor((lo % 20) / 2) + Math.floor(la % 10)
       + A[Math.floor((lo % 2) * 12)].toLowerCase() + A[Math.floor((la % 1) * 24)].toLowerCase(); };
const pick = (p) => p.js(`(() => { const d = (document.getElementById("hhMapPick") || {}).dataset || {}; return { lat: +d.lat, lon: +d.lon, zoom: +d.zoom, locator: d.locator }; })()`);
const mapReady = (p) => p.until(`!!document.querySelector("#hhMapPick .leaflet-container") && !!window.VibeMap && document.getElementById("hhMapPickNote").textContent === ""`, 15000);
const hhLoc = async () => (await fetch(`http://127.0.0.1:${port}/handheld/location`)).json();
const shown = (sel) => `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; const r = e.getBoundingClientRect(); return r.width > 2 && r.height > 2 && getComputedStyle(e).display !== 'none'; })()`;
const running = () => `(() => { const f = document.getElementById("freqInput"); return !!f && parseFloat(f.value) > 0 && !document.getElementById("hhConnect"); })()`;
const childPid = () => { try { return execFileSync("pgrep", ["-P", String(sup.pid)]).toString().trim().split("\n")[0]; } catch { return ""; } };
/** The process's CPU time so far, in seconds ("[[dd-]hh:]mm:ss.cc" from ps). */
const cpuOf = (pid) => { try {
  // ★ Linux: /proc to the clock tick (ps rounds to whole seconds there, which reads 0 % over a 5 s window)
  if (fs.existsSync(`/proc/${pid}/stat`)) { const f = fs.readFileSync(`/proc/${pid}/stat`, "utf8").split(") ")[1].split(" "); return (Number(f[11]) + Number(f[12])) / 100; }
  const t = execFileSync("ps", ["-o", "time=", "-p", pid]).toString().trim().replace(/^\d+-/, "");
  return t.split(":").reduce((a, x) => a * 60 + parseFloat(x), 0); } catch { return NaN; } };

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/handheld/session`); break; } catch { await sleep(250); } }
  console.log("the handheld page — the real supervisor, fake radios, a fake hardware service, headless muted Edge at 640×480");
  const p = await page(`http://127.0.0.1:${port}/?handheld=1`);
  ok(await p.until(shown("#hhConnect"), 15000), "no radio: \"Please connect an SDR\" fills the screen");
  ok(await p.js(`document.getElementById("hhConnect").textContent.includes("Please connect an SDR")`), "…in those words");
  ok(await p.js(`getComputedStyle(document.getElementById("splash")).display === "none"`), "no landing page behind it");

  console.log("── SET FROM MAP with no radio ──");
  ok(await p.until(`${shown("#hhConnectMap")} && document.getElementById("hhConnectLoc").textContent === "Location: not set"`, 5000),
     `"Please connect an SDR" offers SET LOCATION FROM MAP (${await p.js(`(document.getElementById("hhConnectLoc") || {}).textContent`)})`);
  await p.js(`__connect(); 1`);
  await press(p, DOWN);
  ok(await p.js(`!!document.querySelector(".gpFocus") && document.querySelector(".gpFocus").id === "hhConnectMap"`), "the D-pad lands on it (and on nothing hidden behind the screen)");
  await press(p, CROSS);
  ok(await p.until(shown("#hhMapPick"), 5000), "✕ opens the map — in the page, over the whole screen");
  ok(await mapReady(p), "the basemap renderer draws it (Leaflet + VibeMap, from the engine)");
  const box = await p.js(`(() => { const r = document.getElementById("hhMapPick").getBoundingClientRect(); return r.left === 0 && r.top === 0 && r.width === innerWidth && r.height === innerHeight ? "full" : r.width + "x" + r.height + " of " + innerWidth + "x" + innerHeight; })()`);
  ok(box === "full", `the whole viewport (${await p.js(`innerWidth + "×" + innerHeight`)} in a 640×480 window)`);
  const m0 = await pick(p);
  ok(m0.zoom === 1 && m0.lat === 20 && m0.lon === 0, `no location yet: the whole world (zoom ${m0.zoom}, ${m0.lat}, ${m0.lon})`);
  ok(m0.locator === locatorOf(m0.lat, m0.lon), `the crosshair's locator in the corner (${m0.locator})`);
  for (let i = 0; i < 3; i++) await press(p, R1);
  await press(p, L1);
  const m1 = await pick(p);
  ok(m1.zoom === 3, `R zooms in ×3, L out ×1 → zoom ${m1.zoom}`);
  await stick(p, 3, -1, 250);
  const zr = (await pick(p)).zoom;
  ok(zr === 4, `the right stick pushed up zooms in (zoom ${zr})`);
  const m2a = await pick(p);
  await press(p, RIGHT); await press(p, RIGHT); await press(p, DOWN);
  const m2 = await pick(p);
  ok(m2.lon > m2a.lon + 1 && m2.lat < m2a.lat - 0.5, `the D-pad pans: right ×2, down ×1 (${m2a.lat}, ${m2a.lon} → ${m2.lat}, ${m2.lon})`);
  await stick(p, 0, 0.9, 600);
  const m3 = await pick(p);
  ok(m3.lon > m2.lon + 1 && Math.abs(m3.lat - m2.lat) < 0.5, `the left stick pans too (${m2.lon} → ${m3.lon})`);
  // ★ HH_SHOTS=<dir>: a picture of the map layer, for a person to look at
  if (process.env.HH_SHOTS) { await sleep(1500); fs.writeFileSync(path.join(process.env.HH_SHOTS, "hh-map-pick.png"), Buffer.from((await p.call("Page.captureScreenshot", { format: "png" })).result.data, "base64")); }
  ok(m3.locator === locatorOf(m3.lat, m3.lon) && await p.js(`document.getElementById("hhMapPickLoc").textContent.startsWith(${JSON.stringify(m3.locator)})`),
     `…the corner follows live (${m3.locator})`);
  const res = await p.js(`performance.getEntriesByType("resource").map(e => e.name)`);
  ok(res.some((u) => u.includes("/mapdata/v1/")), `drawn from the map files on the device (${res.filter((u) => u.includes("/mapdata/v1/")).length} requests to /mapdata/v1/)`);
  ok(res.every((u) => u.startsWith(`http://127.0.0.1:${port}/`) || u.startsWith("blob:") || u.startsWith("data:")), "…and nothing from anywhere else — no internet on the handheld");
  await press(p, CROSS);
  ok(await p.until(`!document.getElementById("hhMapPick")`, 3000), "✕ sets it and the map goes");
  let st0 = {};
  for (let i = 0; i < 30; i++) { st0 = await hhLoc(); if (st0.set) break; await sleep(150); }
  ok(st0.set && Math.abs(parseFloat(st0.lat) - m3.lat) < 1e-5 && Math.abs(parseFloat(st0.lon) - m3.lon) < 1e-5,
     `the stored location is the crosshair's point (${st0.lat}, ${st0.lon} vs ${m3.lat}, ${m3.lon})`);
  ok(await p.until(`document.getElementById("hhConnectLoc").textContent.includes(${JSON.stringify(st0.lat)})`, 3000), `…and the screen says so (${await p.js(`document.getElementById("hhConnectLoc").textContent`)})`);
  // ○ cancels: opens where it was set, moves, and leaves the stored location alone
  await press(p, CROSS);
  ok(await p.until(shown("#hhMapPick"), 5000) && await mapReady(p), "opened again");
  const m4 = await pick(p);
  ok(Math.abs(m4.lat - m3.lat) < 1e-4 && Math.abs(m4.lon - m3.lon) < 1e-4 && m4.zoom === 8, `…centred on the location set, zoomed in (zoom ${m4.zoom}, ${m4.lat}, ${m4.lon})`);
  await press(p, LEFT); await press(p, UP);
  ok((await pick(p)).lon < m4.lon, "moved");
  await press(p, CIRCLE);
  ok(await p.until(`!document.getElementById("hhMapPick")`, 3000), "○ closes it");
  const st1 = await hhLoc();
  ok(st1.lat === st0.lat && st1.lon === st0.lon, `…and the location is as it was (${st1.lat}, ${st1.lon})`);
  ok(await p.js(shown("#hhConnect")), "back on \"Please connect an SDR\"");
  // the keyboard: Enter opens it, arrows and + pan and zoom, Escape cancels, Enter sets
  await key(p, "Enter");
  ok(await p.until(shown("#hhMapPick"), 5000) && await mapReady(p), "the keyboard: Enter opens it");
  const k0 = await pick(p);
  await key(p, "ArrowRight"); await key(p, "ArrowUp"); await key(p, "+"); await key(p, "PageUp"); await key(p, "-");
  const k1 = await pick(p);
  ok(k1.lon > k0.lon && k1.lat > k0.lat && k1.zoom === k0.zoom + 1, `…arrows pan, + / Page Up / − zoom (zoom ${k0.zoom} → ${k1.zoom})`);
  await key(p, "Escape");
  ok(await p.until(`!document.getElementById("hhMapPick")`, 3000) && (await hhLoc()).lon === st0.lon, "…Escape cancels");
  await key(p, "Enter");
  ok(await p.until(shown("#hhMapPick"), 5000) && await mapReady(p), "…Enter again");
  await key(p, "ArrowLeft"); await key(p, "ArrowLeft"); await key(p, "ArrowDown");
  const k2 = await pick(p);
  await key(p, "Enter");
  let st2 = {};
  for (let i = 0; i < 30; i++) { st2 = await hhLoc(); if (st2.lon !== st0.lon) break; await sleep(150); }
  ok(!(await p.js(`!!document.getElementById("hhMapPick")`)) && Math.abs(parseFloat(st2.lat) - k2.lat) < 1e-5 && Math.abs(parseFloat(st2.lon) - k2.lon) < 1e-5,
     `…and Enter sets the crosshair's point (${st2.lat}, ${st2.lon})`);
  ok(p.errors.length === 0, `no page errors so far${p.errors.length ? ": " + p.errors.slice(0, 3).join(" | ") : ""}`);
  // ★ the rest of this test begins on a fresh card: the location is cleared again
  await fetch(`http://127.0.0.1:${port}/handheld/location`, { method: "POST", body: JSON.stringify({ clear: true }) });

  plug("RADIOA", rtlA);
  ok(await p.until(running(), 30000), "radio A plugged in: the page goes straight into the receiver");
  // ★★ THE START SCREEN STAYS (Stuart 2026-11-05) — and on a handheld with no touchscreen it must work from the pad.
  ok(await p.until(shown("#audioGate"), 10000), "the START RADIO screen is there, as on every receiver");
  await p.js(`__connect(); 1`);
  const gateFocus = () => p.js(`(() => { const h = document.querySelector("#audioGate .gpFocus"); return h ? (h.id || h.textContent.trim()) : ""; })()`);
  const seenGate = new Set();
  for (let i = 0; i < 8; i++) { await press(p, DOWN); seenGate.add(await gateFocus()); }
  for (let i = 0; i < 8; i++) { await press(p, UP); seenGate.add(await gateFocus()); }
  ok(seenGate.has("START RADIO") && seenGate.has("gateTutBtn"), `the D-pad moves on it: ${[...seenGate].filter(Boolean).join(" · ")}`);
  // the tour, from the pad
  for (let i = 0; i < 8 && (await gateFocus()) !== "gateTutBtn"; i++) await press(p, DOWN);
  await press(p, CROSS);
  ok(await p.until(shown("#tutBox"), 5000), "✕ on View tutorial opens the tour");
  await sleep(500);
  // ★ the tour draws its box a frame or two after the press — a few tries, each a real D-pad press
  let inTour = false;
  for (let i = 0; i < 5 && !inTour; i++) { await press(p, i % 2 ? UP : DOWN); inTour = await p.js(`!!document.querySelector("#tutBox .gpFocus")`); }
  ok(inTour, `the D-pad moves inside the tour (${await p.js(`(document.querySelector(".gpFocus") || {}).textContent || ""`)})`);
  await press(p, CIRCLE);
  ok(await p.until(`!${shown("#tutBox")}`, 5000), "○ ends the tour");
  // ★ The tour starts the radio itself (the gate goes when its tutorial key is pressed): START is pressed only if it is
  //   back. Either way it must end with the radio running and the screen gone — all from the pad.
  if (await p.js(shown("#audioGate"))) {
    for (let i = 0; i < 8 && (await gateFocus()) !== "START RADIO"; i++) await press(p, UP);
    await press(p, CROSS);
  }
  ok(await p.until(`!document.getElementById("audioGate")`, 5000), "the radio is running, the start screen gone — all from the pad");
  ok(await p.until(`document.getElementById("status").textContent.startsWith("Spectrum rate")`, 6000),
     `the status row: "${await p.js(`document.getElementById("status").textContent`)}"`);
  const st = await p.js(`document.getElementById("status").textContent`);
  ok(!/KB\/s|ping|buf/.test(st) && !(await p.js(shown("#linkBars"))), "no KB/s, ping, buffer or link bars");
  ok(!(await p.js(shown("#mChat"))) && await p.js(shown("#mDevice")), "no CHAT; DEVICE sits in its cell");

  console.log("── the menu ──");
  await p.js(`document.getElementById("mMenu").click(); 1`);
  ok(await p.until(`document.getElementById("menu").classList.contains("open")`), "the menu opens");
  const accTitles = await p.js(`[...document.querySelectorAll("#menu .hhAcc")].map(b => b.textContent)`);
  ok(JSON.stringify(accTitles) === JSON.stringify(["R36S HARDWARE", "SDR HARDWARE", "DISPLAY"]), `three accordions: ${accTitles}`);
  ok(await p.js(`[...document.querySelectorAll("#menu .hhAccBody")].every(b => b.hidden)`), "all collapsed at first — the menu stays short");
  ok(await p.js(`document.getElementById("rowGain").closest(".hhAccBody") === document.querySelectorAll("#menu .hhAccBody")[1]`), "the radio's GAIN row lives in SDR HARDWARE");
  ok(await p.js(`!!document.getElementById("hhBright") && document.getElementById("hhBright").closest(".hhAccBody") === document.querySelectorAll("#menu .hhAccBody")[2]`), "BRIGHTNESS is in DISPLAY");
  ok(await p.js(`document.querySelectorAll("#menu .hhAccBody")[2].textContent.includes("SPECTRUM RATE")`), "…with SPECTRUM RATE");
  await p.js(`document.querySelectorAll("#menu .hhAcc")[0].click(); 1`);
  ok(await p.js(`(() => { const b = document.querySelectorAll("#menu .hhAccBody"); return !b[0].hidden && b[1].hidden && b[2].hidden; })()`), "R36S HARDWARE expands in place");
  ok(await p.js(`document.getElementById("hhLocNow").textContent`) === "not set", "LOCATION: not set on a fresh card");
  await p.js(`(() => { const i = document.getElementById("hhLocIn"); i.value = "jo01ab"; document.getElementById("hhLocSet").click(); })()`);
  ok(await p.until(`document.getElementById("hhLocNow").textContent === "JO01ab"`, 5000), "a locator typed and SET");
  ok(await (async () => { for (let i = 0; i < 40; i++) { try { const l = await (await fetch(`http://127.0.0.1:${port}/location`)).json(); if (l.grid === "JO01ab") return true; } catch {} await sleep(150); } return false; })(),
     "…the receiver's /location follows, live");
  ok(await p.until(`(document.getElementById("rxLoc") || {}).textContent.includes("JO01")`, 5000), `…and the page's own receiver line (${await p.js(`(document.getElementById("rxLoc") || {}).textContent`)})`);
  // ★ SET FROM MAP from the menu, with the radio playing: starts on the typed locator, the pad pans it, ✕ applies it live
  ok(await p.js(`document.getElementById("hhLocMap").closest(".mrow") === document.getElementById("hhLocNow").closest(".mrow")`), "SET FROM MAP sits in the LOCATION row");
  const fq0 = await p.js(`document.getElementById("freqInput").value`);
  await p.js(`document.getElementById("hhLocMap").click(); 1`);
  ok(await p.until(shown("#hhMapPick"), 5000) && await mapReady(p), "SET FROM MAP opens the map over the receiver");
  const g0 = await pick(p);
  ok(Math.abs(g0.lat - 51.0625) < 0.01 && Math.abs(g0.lon - 0.0417) < 0.01 && g0.locator === "JO01ab", `…centred on the locator typed (${g0.lat}, ${g0.lon} = ${g0.locator})`);
  await p.js(`__connect(); 1`);
  for (let i = 0; i < 3; i++) await press(p, LEFT);
  await stick(p, 0, -0.9, 400);
  await press(p, R1);
  const r1 = await pick(p);
  ok(r1.lon < g0.lon - 0.2 && r1.zoom === 9, `the pad pans and zooms it (${r1.lat}, ${r1.lon}, zoom ${r1.zoom})`);
  ok(await p.js(`document.getElementById("freqInput").value`) === fq0, `…and the radio underneath is not tuned by the stick (${fq0})`);
  await press(p, CROSS);
  ok(await p.until(`!document.getElementById("hhMapPick") && document.getElementById("hhLocNow").textContent === ${JSON.stringify(r1.lat.toFixed(5) + ", " + r1.lon.toFixed(5))}`, 5000),
     `✕: the menu's LOCATION reads the crosshair's point (${await p.js(`document.getElementById("hhLocNow").textContent`)})`);
  ok(await (async () => { for (let i = 0; i < 40; i++) { try { const l = await (await fetch(`http://127.0.0.1:${port}/location`)).json();
       if (typeof l.lat === "number" ? Math.abs(l.lat - r1.lat) < 0.05 && Math.abs(l.lon - r1.lon) < 0.05 : (l.grid || "").slice(0, 4) === r1.locator.slice(0, 4)) return true; } catch {} await sleep(150); } return false; })(),
     `…the receiver's /location follows, live (${JSON.stringify(await (await fetch(`http://127.0.0.1:${port}/location`)).json()).slice(0, 120)})`);
  ok(await p.js(`!document.querySelectorAll("#menu .hhAccBody")[0].hidden && document.getElementById("menu").classList.contains("open")`), "…the menu still open on R36S HARDWARE");
  await p.js(`document.getElementById("hhLocMap").click(); 1`);
  ok(await p.until(shown("#hhMapPick"), 5000), "opened again");
  await press(p, RIGHT);
  await press(p, CIRCLE);
  ok(await p.until(`!document.getElementById("hhMapPick")`, 3000) && await p.js(`!document.querySelectorAll("#menu .hhAccBody")[0].hidden`),
     "○ closes only the map — R36S HARDWARE stays open under it");
  const rl = await hhLoc();
  ok(Math.abs(parseFloat(rl.lat) - r1.lat) < 1e-5 && Math.abs(parseFloat(rl.lon) - r1.lon) < 1e-5, "…the location unchanged");
  ok(await p.until(`document.getElementById("hhBattVal").textContent.includes("83")`, 5000), `the battery from the hardware service (${await p.js(`document.getElementById("hhBattVal").textContent`)})`);
  await p.js(`document.querySelectorAll("#menu .hhAcc")[2].click(); 1`);
  ok(await p.js(`(() => { const b = document.querySelectorAll("#menu .hhAccBody"); return b[0].hidden && b[1].hidden && !b[2].hidden; })()`), "opening DISPLAY collapses R36S HARDWARE");
  await p.js(`__connect(); 1`);
  await press(p, CIRCLE);
  ok(await p.js(`[...document.querySelectorAll("#menu .hhAccBody")].every(b => b.hidden)`) && await p.js(`document.getElementById("menu").classList.contains("open")`),
     "○ (back) collapses it — the menu stays open");
  // the D-pad walks the accordion buttons; ✕ expands
  await p.js(`document.querySelectorAll("#menu .hhAcc")[1].scrollIntoView(); 1`);
  let reached = false;
  for (let i = 0; i < 30 && !reached; i++) { await press(p, DOWN); reached = await p.js(`!!document.querySelector("#menu .hhAcc.gpFocus") && document.querySelector(".gpFocus").textContent === "SDR HARDWARE"`); }
  if (!reached) for (let i = 0; i < 30 && !reached; i++) { await press(p, UP); reached = await p.js(`!!document.querySelector("#menu .hhAcc.gpFocus") && document.querySelector(".gpFocus").textContent === "SDR HARDWARE"`); }
  ok(reached, "the D-pad reaches SDR HARDWARE");
  await press(p, CROSS);
  ok(await p.js(`!document.querySelectorAll("#menu .hhAccBody")[1].hidden`), "✕ expands it");
  await press(p, CIRCLE);
  // brightness → the hardware service
  await p.js(`(() => { const i = document.getElementById("hhBright"); i.value = "77"; i.dispatchEvent(new Event("input")); })()`);
  ok(await (async () => { for (let i = 0; i < 30; i++) { if (hwd.state.brightness === 77) return true; await sleep(100); } return false; })(), "a brightness change reaches the hardware service");
  await p.js(`document.getElementById("menu").classList.remove("open"); 1`);

  console.log("── overlays ──");
  hwd.emit({ event: "volume", volume: 42, muted: false });
  ok(await p.until(`${shown("#hhVol")} && document.getElementById("hhVol").textContent.includes("VOL 42")`, 4000), "the volume buttons → the volume overlay (VOL 42)");
  hwd.emit({ event: "battery_low", pct: 10 });
  ok(await p.until(`${shown("#hhBatt")} && document.getElementById("hhBatt").textContent.includes("10 %")`, 4000), "battery_low → the 10 % banner");
  hwd.emit({ event: "battery", pct: 40, charging: true });
  ok(await p.until(`!${shown("#hhBatt")}`, 4000), "charging → the banner goes");
  await p.js(`localStorage.setItem("vibesdr_handheld_v1", JSON.stringify({ ...JSON.parse(localStorage.getItem("vibesdr_handheld_v1") || "{}"), perf: true })); 1`);
  await p.js(`document.getElementById("mDevice").click(); 1`);
  await p.js(`document.getElementById("hhPerfBtn").textContent === "ON" || document.getElementById("hhPerfBtn").click(); 1`);
  await p.js(`document.getElementById("hhPerfBtn").textContent === "ON" || document.getElementById("hhPerfBtn").click(); 1`);
  ok(await p.until(`${shown("#hhPerf")} && /UI \\d+ fps/.test(document.getElementById("hhPerf").textContent)`, 5000), "the Performance overlay: plain text with \"UI … fps\"");
  ok(await p.js(`!document.querySelector("#hhPerf canvas")`), "…no canvas in it");
  await sleep(2500);
  const ui = await p.js(`parseFloat(/UI (\\d+) fps/.exec(document.getElementById("hhPerf").textContent)[1])`);
  ok(ui > 20 && ui <= 33, `drawing capped at 30 fps (UI ${ui} fps)`);
  // ★ the screen off (a short press of power): nothing drawn, audio on; back on: drawing again
  hwd.emit({ event: "screen", screen: "off" });
  await sleep(1500);
  ok(await p.until(`/UI 0 fps/.test(document.getElementById("hhPerf").textContent)`, 4000), "screen off → the page stops drawing (UI 0 fps)");
  ok(await p.js(`!!window.__vibeAudio && !window.__vibeAudio.suspended`), "…the audio plays on");
  hwd.emit({ event: "screen", screen: "on" });
  ok(await p.until(`/UI (2\\d|3\\d) fps/.test(document.getElementById("hhPerf").textContent)`, 5000), "screen on → drawing again");
  // ★ a pad press resumes the audio from the pad's own poll (WPE: a synthetic click does not unlock it)
  await p.js(`window.__resumes = 0; const a = window.__vibeAudio; const r = a.resume.bind(a); a.resume = () => { window.__resumes++; return r(); }; 1`);
  await press(p, 2);
  ok(await p.js(`window.__resumes > 0`), "a pad press calls the audio's resume()");
  await p.js(`document.getElementById("menu").classList.remove("open"); 1`);

  console.log("── per-radio memory ──");
  await p.js(`(() => { const k = "vibesdr_web_prefs_v1"; const o = JSON.parse(localStorage.getItem(k) || "{}"); o.hhTestKey = 4242; o.palette = "test-palette"; localStorage.setItem(k, JSON.stringify(o)); })()`);
  const memA = async () => (await (await fetch(`http://127.0.0.1:${port}/handheld/session`)).json()).memory;
  ok(await (async () => { for (let i = 0; i < 40; i++) { const m = await memA(); if (m && m.hhTestKey === 4242) return true; await sleep(150); } return false; })(), "a pref set on radio A reaches the engine within ~2 s");
  ok(!(((await memA()) || {}).palette), "…the look (palette) is NOT part of the radio's memory");

  console.log("── a recording to the card ──");
  await p.js(`document.getElementById("mPanelRec").click(); 1`);
  await sleep(2500);
  await p.js(`document.getElementById("mPanelRec").click(); 1`);
  const recs = async () => (await (await fetch(`http://127.0.0.1:${port}/handheld/recordings`)).json()).recordings || [];
  ok(await (async () => { for (let i = 0; i < 40; i++) { if ((await recs()).length) return true; await sleep(150); } return false; })(), "REC / STOP: the WAV is on the card");
  const r0 = (await recs())[0];
  ok(r0 && /^\d{4}-\d\d-\d\d \d{4} - \d+\.\d{3} kHz [A-Z0-9]+\.wav$/.test(r0.name) && r0.bytes > 44, `named for a person: "${r0?.name}" (${r0?.bytes} bytes)`);
  await p.js(`document.getElementById("mPanelRecs").click(); 1`);
  ok(await p.until(`!!document.querySelector("#recsList audio[src*='/handheld/recordings/']")`, 5000), "the recordings panel lists it with a player");
  await p.js(`document.getElementById("recordingsPanel").classList.remove("open"); 1`);

  console.log("── hot swap ──");
  plug("RADIOB", rtlB);
  ok(await p.until(running() + ` && JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").hhTestKey === undefined`, 45000), "radio B: the receiver again, radio A's pref gone");
  if (fails) console.log("     page:", await p.js(`JSON.stringify({ conn: !!document.getElementById("hhConnect"), f: (document.getElementById("freqInput") || {}).value, prefs: localStorage.getItem("vibesdr_web_prefs_v1"), msg: (document.getElementById("splashMsg") || {}).textContent, live: document.getElementById("app").classList.contains("live"), ss: JSON.stringify(sessionStorage), hh: document.documentElement.dataset.handheld })`));
  ok(await p.js(`JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").palette === "test-palette"`), "…the look kept");
  plug("", 0);
  ok(await p.until(shown("#hhConnect"), 15000), "unplugged: \"Please connect an SDR\"");
  plug("RADIOA", rtlA);
  ok(await p.until(running() + ` && JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}").hhTestKey === 4242`, 30000), "radio A back: its pref restored");

  console.log("── spectrum rate 5 / 10 / 20 (indicative: a Mac, not the R36S) ──");
  // ★ The hot swap reloaded the page, so START RADIO is up again — and drawing waits behind it: press it (from the pad).
  await p.js(`__connect(); 1`);
  for (let i = 0; i < 12 && await p.js(shown("#audioGate")); i++) {
    for (let k = 0; k < 6 && (await gateFocus()) !== "START RADIO"; k++) await press(p, UP);
    await press(p, CROSS); await sleep(400);
  }
  ok(!(await p.js(shown("#audioGate"))), "START RADIO pressed again after the swap (pad)");
  await p.js(`document.getElementById("mMenu").click(); document.querySelectorAll("#menu .hhAcc")[2].click(); 1`);
  const notes = [];
  for (const r of ["5", "10", "20"]) {
    await p.js(`[...document.querySelectorAll("#menu .hhAccBody")[2].querySelectorAll("button[data-v]")].find(b => b.dataset.v === "${r}" && b.closest(".mrow").textContent.includes("SPECTRUM RATE")).click(); 1`);
    await sleep(4000);
    const t0 = (await p.call("Performance.getMetrics")).result.metrics; const c0 = childPid(); const e0 = cpuOf(c0);
    await sleep(5000);
    const t1 = (await p.call("Performance.getMetrics")).result.metrics;
    const m = (arr, k) => (arr.find((x) => x.name === k) || {}).value || 0;
    const task = (m(t1, "TaskDuration") - m(t0, "TaskDuration")) / 5 * 100;
    const shownRate = await p.js(`parseFloat(document.getElementById("status").textContent.replace("Spectrum rate", ""))`);
    const eng = (cpuOf(c0) - e0) / 5 * 100;   // ★ the engine's (radio process's) whole CPU, % of one core
    const fr = await p.js(`parseFloat((/UI (\\d+) fps/.exec((document.getElementById("hhPerf") || {}).textContent || "") || [0, 0])[1])`);
    notes.push(`${r} fps: status ${shownRate} fps, page main thread ${task.toFixed(1)} % of a core (${(task * 10 / Math.max(1, fr)).toFixed(2)} ms a drawn frame at UI ${fr} fps), engine ${Number.isFinite(eng) ? eng.toFixed(1) + " % of a core" : "?"}`);
    ok(Math.abs(shownRate - Number(r)) <= Math.max(1.5, Number(r) * 0.25), `SPECTRUM RATE ${r}: the status row follows (${shownRate} fps)`);
  }
  for (const n of notes) console.log("     " + n);
  ok(p.errors.length === 0, `no page errors${p.errors.length ? ": " + p.errors.slice(0, 3).join(" | ") : ""}`);

  console.log("── an ORDINARY server's landing page takes the D-pad ──");
  const rtlC = await fakeRtl();
  await sleep(600);   // ★ the fake dongle must be listening before the server dials it
  const sport = await freePort();
  fs.mkdirSync(path.join(T, "srv")); fs.mkdirSync(path.join(T, "srvrun"));
  kids.push(spawn(BIN, ["--tcp", `127.0.0.1:${rtlC}`, "--port", String(sport), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--name", "Landing pad test"], {
    env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "srv", "config.json"), VIBESERVER_DATA_DIR: path.join(T, "srv"), VIBESERVER_RUNTIME_DIR: path.join(T, "srvrun"), HOME: T }, stdio: "ignore" }));
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${sport}/vibeserver.json`); break; } catch { await sleep(250); } }
  await p.call("Page.navigate", { url: `http://127.0.0.1:${sport}/` });
  const landed = await p.until(shown("#btnConnect"), 20000);
  ok(landed, "the landing page with START");
  if (!landed) console.log("     page:", await p.js(`location.href + " | " + document.body.innerText.slice(0, 300)`));
  ok(!(await p.js(`document.documentElement.dataset.handheld === "1"`)), "an ordinary server is never handheld");
  await p.js(`__connect(); 1`);
  let hit = false;
  for (let i = 0; i < 8 && !hit; i++) { await press(p, DOWN); hit = await p.js(`!!document.querySelector("#splash .gpFocus")`); }
  ok(hit, `the D-pad highlights a landing-page control (${await p.js(`(document.querySelector(".gpFocus") || {}).id || ""`)})`);
  let onStart = await p.js(`!!document.querySelector("#btnConnect.gpFocus")`);
  for (let i = 0; i < 8 && !onStart; i++) { await press(p, i < 4 ? DOWN : UP); onStart = await p.js(`!!document.querySelector("#btnConnect.gpFocus")`); }
  ok(onStart, "…START among them");
  await press(p, CROSS);
  ok(await p.until(`document.getElementById("splash").classList.contains("hidden")`, 15000), "✕ on START connects");
} catch (e) {
  fails++; console.log("  FAIL threw:", e && e.stack || e);
} finally {
  for (const k of kids) { try { k.kill(); } catch {} }
  await hwd.close();
  await sleep(500);
  if (!fails) fs.rmSync(T, { recursive: true, force: true }); else console.log(`  (left for a look: ${T})`);
}
console.log(`test-web-handheld: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
