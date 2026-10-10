#!/usr/bin/env node
// test-pocket-connection-web.mjs — PORTABLE WI-FI on the served receiver page (Stuart, 2026-10-10), in a real
// (headless, MUTED) browser, against the real vibeserver binary with a fake radio.
//
// ★ What it proves: on a pocket box reached on its own network, the menu draws Network | Own Wi-Fi with the link's
//   figures, the "Weak Wi-Fi on this VibeServer Portable" notice appears when the server calls the link weak, and
//   Switch (after the confirmation) writes the one-word kick the root service reads. Through loopback — the tunnel's
//   path — none of it is drawn and nothing is polled.
// ★ The root service is played by this script writing its state file (the fake-NM test covers the radio side).
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-pocket-connection-web.mjs
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

const lan = Object.values(os.networkInterfaces()).flat()
  .find((i) => i && i.family === "IPv4" && !i.internal && /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(i.address));
if (!lan) { console.error("no private LAN address to test from"); process.exit(2); }

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-pocketconn-"));
const PD = path.join(T, "pocket"); fs.mkdirSync(PD);
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(PD, "enabled"), "1\n");
const state = (o) => fs.writeFileSync(path.join(PD, "state.json"), JSON.stringify(Object.assign({
  v: 1, mode: "client", ssid: "Meow!", hold: false, ap: { set: true, ssid: "Pocket-SDR" },
  saved: [{ rank: 1, ssid: "Meow!", hidden: false }], link: { signal: -78, tx: 13.0, rx: 72.2, retry: 9 } }, o)) + "\n");
state({});
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Pocket test", locator: "IO92nh",
  adminPass: "pocket-admin-pass", radios: [{ serial: "pk", label: "T", enabled: true, configured: true, users: 3 }] }));

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"),
         VIBESERVER_RUNTIME_DIR: path.join(T, "run"), VIBESERVER_POCKET_DIR: PD, HOME: T },
  stdio: ["ignore", fs.openSync(path.join(T, "server.log"), "w"), fs.openSync(path.join(T, "server.err"), "w")] });
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });

async function cleanup() {
  for (const p of [br, srv, rtl]) { try { p.kill(); } catch {} }
  await sleep(500);
  fs.rmSync(T, { recursive: true, force: true });
}

async function page(url) {
  let target;
  for (let i = 0; i < 40 && !target; i++) {
    try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === "page"); }
    catch { await sleep(250); }
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  };
  const shot = async (name) => {
    if (!process.env.POCKET_SHOTS) return;
    const r = await call("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync(path.join(process.env.POCKET_SHOTS, name + ".png"), Buffer.from(r.result.data, "base64"));
  };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.navigate", { url });
  // ★ A TRUSTED press (the START RADIO gate wants a real gesture to start audio).
  const tap = async (x, y) => {
    for (const type of ["mousePressed", "mouseReleased"]) await call("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 });
  };
  return { ws, js, until, errors, shot, tap };
}

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://${lan.address}:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("Portable Wi-Fi on the served page — a real browser, the real binary");

  const p = await page(`http://${lan.address}:${port}/`);
  // ★ The page waits on its start screen until the listener presses CONNECT — the notice is for while listening.
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  ok(await p.until(`!document.getElementById("pocketConn").hidden`), "on the box's own network: the PORTABLE WI-FI row is drawn");
  ok(await p.js(`document.getElementById("pcLine").textContent`) === "On “Meow!” · −78 dBm · 13 Mbit/s · 9 % retries",
     "…with the link: network, signal, rate, retries");
  ok(await p.js(`document.getElementById("pcNet").classList.contains("on") && !document.getElementById("pcOwn").classList.contains("on")`),
     "…NETWORK lit");
  ok(await p.until(`!!document.getElementById("pocketWeak")`), "the server calls the link weak → the notice appears");
  ok(/Weak Wi-Fi on this VibeServer Portable/.test(await p.js(`document.getElementById("pocketWeak").textContent`)), "…in Stuart's words");
  await p.js(`document.getElementById("menu").classList.add("open"); document.getElementById("pocketConn").scrollIntoView(); 1`);
  await p.tap(596, 312); await sleep(2000);
  await p.js(`document.getElementById("menu").classList.add("open"); document.getElementById("pocketConn").scrollIntoView(); 1`);
  await sleep(400); await p.shot("pc-weak-and-menu");
  await p.js(`document.getElementById("pocketWeakLater").click(); 1`);
  ok(!(await p.js(`!!document.getElementById("pocketWeak")`)), "NOT NOW puts it away");
  await sleep(300); await p.shot("pc-menu");
  await p.js(`window.confirm = (m) => { window.__asked = m; return true; }; document.getElementById("pcOwn").click(); 1`);
  ok(await p.until(`!!window.__asked`), "OWN WI-FI asks first");
  ok(/leave this session, join “Pocket-SDR” on this device, and start again/.test(await p.js(`window.__asked`)),
     "…saying you will need to leave this session, join the box's hotspot and start again");
  let kicked = false;
  for (let i = 0; i < 30 && !kicked; i++) { kicked = fs.existsSync(path.join(PD, "pocket-kick.request")); if (!kicked) await sleep(200); }
  ok(kicked && fs.readFileSync(path.join(PD, "pocket-kick.request"), "utf8") === "own\n", "…then the box is asked to switch (kick: own)");
  ok(/Switching… join “Pocket-SDR”/.test(await p.js(`document.getElementById("pcNote").textContent`)), "…and the menu says what to do next");
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();

  // ★ Through loopback — how the tunnel reaches the box — nothing is drawn.
  const q = await page(`http://127.0.0.1:${port}/`);
  await q.until(`!!document.getElementById("btnConnect")`);
  await q.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(3000);
  ok(await q.js(`document.getElementById("pocketConn").hidden && !document.getElementById("pocketWeak")`),
     "through loopback (the tunnel's path): no PORTABLE WI-FI row and no notice");
  q.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
