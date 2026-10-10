#!/usr/bin/env node
// test-pocket-added-radio.mjs — Stuart's pocket box, 2026-10-10: an Airspy HF+ added from the setup page carried the
// RTL default rate (2.4 MS/s), its tab offered that first ("not offered by this radio"), and Save hung. A box with
// the HF+ added but never set up must offer the HF+'s own rate (912 kS/s) only, and Save must finish.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-pocket-added-radio.mjs   (exit 3 = not run)
import { spawn } from "node:child_process"; import fs from "node:fs"; import os from "node:os"; import path from "node:path";
const R = new URL("..", import.meta.url).pathname.replace(/\/$/, ""), BIN = process.env.VIBESERVER_BIN;
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN — not run"); process.exit(3); }
let fails = 0; const ok = (c, w) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${w}`); if (!c) fails++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PORT = 48993, CDP = 9343;
const lan = Object.values(os.networkInterfaces()).flat().find(i => i && i.family === "IPv4" && !i.internal && /^192\.168\./.test(i.address));
const BASE = `http://${lan.address}:${PORT}`;
const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-p2-")), PD = path.join(T, "pocket"); fs.mkdirSync(PD);
fs.writeFileSync(path.join(PD, "enabled"), "1\n");
fs.writeFileSync(path.join(PD, "state.json"), JSON.stringify({ v: 1, mode: "client", ssid: "Home", ap: { set: true, ssid: "Pocket-SDR" },
  saved: [{ rank: 1, ssid: "Home", hidden: false }], scan: [], scanAt: 0, apAddress: "10.42.0.1", lastError: "" }) + "\n");
const seed = JSON.parse(fs.readFileSync(R + "/image/pocket/stage-pocket/00-pocket/files/config.json")); seed.port = PORT; seed.radios = [{ serial: "DD52B980BE4946DA", driver: "airspyhf", label: "Airspy HF+", enabled: true, configured: false }];
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify(seed));
const srv = spawn(BIN, ["--config", "/dev/null"], { env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_POCKET_DIR: PD, HOME: T },
  stdio: ["ignore", fs.openSync(path.join(T, "server.log"), "w"), fs.openSync(path.join(T, "server.err"), "w")] });
const br = spawn("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge", ["--headless=new", "--mute-audio", "--no-first-run",
  `--remote-debugging-port=${CDP}`, `--user-data-dir=${path.join(T, "b")}`, "--window-size=390,844", "about:blank"], { stdio: "ignore" });
try {
  for (let i = 0; i < 40; i++) { try { await fetch(`${BASE}/vibeserver/pocket/hello`); break; } catch { await sleep(250); } }
  let t; for (let i = 0; i < 40 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find(x => x.type === "page"); } catch { await sleep(250); } }
  const ws = new WebSocket(t.webSocketDebuggerUrl); await new Promise(r => ws.onopen = r);
  let id = 0; const pend = new Map(); ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  const call = (method, params = {}) => new Promise(r => { const n = ++id; pend.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async e => (await call("Runtime.evaluate", { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call("Runtime.enable"); await call("Page.navigate", { url: BASE + "/" }); await sleep(3000);
  await js(`document.getElementById("claimPass").value="pocket123";document.getElementById("claimPass2").value="pocket123";document.getElementById("claimBtn").click();1`);
  await sleep(4000);
  await js(`(() => { const b = [...document.querySelectorAll("#radioTabs button")].find(x => /Airspy/.test(x.textContent)); b && b.click(); return 1; })()`); await sleep(2500);
  ok(await js(`[...document.getElementById("rate").options].map(o=>o.value).join(",")`) === "912000",
     "the added HF+'s tab offers ONLY its own rate (912 kS/s) — not the RTL default it was stored with");
  await js(`document.getElementById("saveBtn").click();1`);
  let back = false;
  for (let i = 0; i < 30 && !back; i++) { await sleep(1000); back = /back up/i.test(await js(`document.getElementById("barMsg").textContent||""`)); }
  ok(back, "Save and start finishes — the receiver comes back");
  const cfgAfter = JSON.parse(fs.readFileSync(path.join(T, "config.json"), "utf8"));
  ok(cfgAfter.configured === true && cfgAfter.radios[0].rate === 912000, "…and the HF+ is saved at 912 kS/s, configured");
} catch (e) { console.error(e); fails++; }
for (const p of [br, srv]) try { p.kill(); } catch {}
// ★ Every run leaves its server and browser folders behind otherwise — 9.7 GB of them filled the Mac (2026-10-10).
await sleep(500); fs.rmSync(T, { recursive: true, force: true });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
