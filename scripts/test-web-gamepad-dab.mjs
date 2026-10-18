#!/usr/bin/env node
// test-web-gamepad-dab.mjs — a GAME CONTROLLER in DAB on the served receiver page, the decoder box working as it does in
// the app (Stuart, 2026-10-18: "move up to select it then it grabs the up down left and right with enter and backspace
// to select an item or to move the controls away from the decoder back to the main controls").
//
// Real server, real multiplex (the bench clip through fake-rtl-tcp --dab), a real headless MUTED browser, a simulated
// standard pad (navigator.getGamepads replaced before the page loads — see test-web-gamepad.mjs).
//   1. DAB takes the D-pad by itself on the first press: the highlight is on the first station;
//   2. DOWN walks every station in order, scrolling the list; it never leaves the box; LEFT/RIGHT reach the header keys;
//   3. ✕ picks a station; ○ gives the D-pad back with the box itself highlighted (the box stays open);
//   4. from the box, DOWN goes to the main controls and UP comes back to the BOX (one stop, not into the list);
//   5. ✕ on the box takes the D-pad again; ○ releases it; it does not take it by itself a second time.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-gamepad-dab.mjs   (exit 3 = not run)
import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const CLIP = path.join(ROOT, "vibeserver/bench-clip/dab-bench-clean.vbu8");
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error("no bench clip (vibeserver/bench-clip/make-clip.sh) — not run"); process.exit(3); }
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); if (!c) fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-gamepad-dab-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Pad DAB test", locator: "IO92nh",
  adminPass: "pad-dab-admin-pass", radios: [{ serial: "gpdab", label: "T", enabled: true, configured: true, users: 3 }] }));
const raw = path.join(T, "clip.u8");
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));   // VIBEBU81 header: magic + rate + centre + sid
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--dab", raw], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
// ★ A short window, so the station list overflows the box and has to scroll.
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
const UP = 12, DOWN = 13, CROSS = 0;
// Where the highlight is: "row:<n>" (a station), "head" (the box's header keys), "box" (the box itself), "inbox"
// (elsewhere inside it), "card" (the main controls), or "page:…".
const WHERE = `(() => {
  const h = document.querySelector(".gpFocus"); if (!h) return "none";
  const rows = [...document.querySelectorAll("#dabStations .dabSvc")];
  const i = rows.indexOf(h); if (i >= 0) return "row:" + i;
  if (h.id === "decBox") return "box";
  if (document.getElementById("decBoxHead")?.contains(h) || document.getElementById("decHeadWrap")?.contains(h)) return "head";
  if (document.getElementById("decBox")?.contains(h)) return "inbox";
  if (document.getElementById("mcard")?.contains(h)) return "card";
  return "page:" + (h.id || h.className || h.tagName);
})()`;
const grabbed = (p) => p.js(`document.getElementById("decBox").classList.contains("gpGrab")`);
const LEFT = 14, RIGHT = 15, CIRCLE = 1;

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("a game controller in DAB — the bench multiplex, a simulated pad, a real browser, the real binary");
  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(1500); await p.tap(596, 312);                           // START RADIO (a real tap: audio needs a gesture)
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running");
  // Into DAB the way a listener goes: the mode button, then DAB.
  await p.js(`document.getElementById("mMode").click(); 1`);
  ok(await p.until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`), "the mode picker offers DAB");
  await p.js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
  ok(await p.until(`document.querySelectorAll("#dabStations .dabSvc").length >= 5`, 40000), "DAB: the multiplex's stations are listed");
  await sleep(1500);                                                   // let the list settle (its skeleton only rebuilds on change)
  const n = await p.js(`document.querySelectorAll("#dabStations .dabSvc").length`);
  ok(await p.js(`(() => { const b = document.getElementById("decBody"); return b.scrollHeight > b.clientHeight + 4; })()`),
     `the list overflows its box (${n} stations), so walking it must scroll`);
  await p.js(`__connect(); 1`);

  // 1. DAB takes the D-pad by itself: the first press lands on the first station.
  await press(p, DOWN);
  ok((await p.js(WHERE)) === "row:0" && await grabbed(p), `DAB: the first press puts the highlight on the first station, the box has the D-pad (at: ${await p.js(WHERE)})`);

  // 2. DOWN walks every station in order, scrolling; at the bottom it stays in the box.
  const visited = [0];
  for (let i = 1; i < n; i++) { await press(p, DOWN); const w = await p.js(WHERE); visited.push(w.startsWith("row:") ? +w.slice(4) : w); }
  ok(visited.every((v, i) => v === i), `DOWN walks every station in order (${visited.join(" ")})`);
  ok(await p.js(`document.getElementById("decBody").scrollTop > 0`), "…and the list scrolled to keep the highlight in view");
  ok(await p.js(`(() => { const h = document.querySelector(".gpFocus").getBoundingClientRect(), b = document.getElementById("decBody").getBoundingClientRect();
                 return h.top >= b.top - 1 && h.bottom <= b.bottom + 1; })()`), "…the last station is in view");
  await press(p, DOWN); await press(p, DOWN);
  ok((await p.js(WHERE)) === `row:${n - 1}` && await grabbed(p), `DOWN at the bottom stays in the box (at: ${await p.js(WHERE)})`);
  for (let i = 0; i < 4; i++) await press(p, UP);
  await press(p, RIGHT);
  let w = await p.js(WHERE);
  if (w !== "head") { for (let i = 0; i < n && w !== "head"; i++) { await press(p, UP); w = await p.js(WHERE); } }
  ok(w === "head", `the header's keys are reachable inside the box (at: ${w})`);
  ok(await grabbed(p), "…still inside the box");
  for (let i = 0; i < 3; i++) await press(p, DOWN);

  // 3. ✕ picks a station; ○ gives the D-pad back, the box highlighted and still open.
  w = await p.js(WHERE);
  if (!w.startsWith("row:")) { await press(p, DOWN); w = await p.js(WHERE); }
  if (await p.js(`document.querySelector(".gpFocus").classList.contains("on")`)) { await press(p, DOWN); }
  const sid = await p.js(`+document.querySelector(".gpFocus").dataset.sid`);
  await press(p, CROSS);
  ok(await p.until(`document.querySelector('#dabStations .dabSvc.on')?.dataset.sid === "${sid}"`, 15000), `✕ picks the highlighted station (SId ${sid.toString(16).toUpperCase()})`);
  await press(p, CIRCLE);
  ok((await p.js(WHERE)) === "box" && !(await grabbed(p)), `○ gives the D-pad back: the box itself is highlighted (at: ${await p.js(WHERE)})`);
  ok(await p.js(`document.getElementById("decBox").classList.contains("open")`), "…and the DAB box stays open");

  // 4. The box is one stop: DOWN to the main controls, UP back to the box (not into its list).
  await press(p, DOWN);
  ok((await p.js(WHERE)) === "card", `from the box, DOWN goes to the main controls (at: ${await p.js(WHERE)})`);
  let back2 = await p.js(WHERE);
  for (let i = 0; i < 6 && back2 !== "box"; i++) { await press(p, UP); back2 = await p.js(WHERE); }
  ok(back2 === "box", `UP from the main controls comes back to the box, not into its list (at: ${back2})`);

  // 5. ✕ on the box takes the D-pad again; ○ releases; no second automatic take.
  await press(p, CROSS);
  ok((await p.js(WHERE)) === "row:0" && await grabbed(p), `✕ on the box hands it the D-pad again, first station highlighted (at: ${await p.js(WHERE)})`);
  await press(p, CIRCLE);
  await press(p, DOWN);
  ok((await p.js(WHERE)) === "card" && !(await grabbed(p)), `after ○, the box does not take the D-pad by itself again (at: ${await p.js(WHERE)})`);

  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
