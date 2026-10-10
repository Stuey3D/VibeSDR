#!/usr/bin/env node
// test-pocket-e2e.mjs — the pocket box's FIRST RUN, in a real (headless, MUTED) browser, against the
// real vibeserver binary (2026-10-10).
//
// ★★★ WHAT IT PROVES. A brand-new pocket box (no password, no radio, no Wi-Fi) is set up entirely
//     from a phone-sized browser: the page offers "Choose an admin password", the claim signs the
//     page in, the Wi-Fi card lists the scanned networks, three networks + the box's own hotspot
//     are saved IN ORDER, the box refuses "Save and start" until the hotspot exists, and "Leave the
//     hotspot now" shows the addresses first and then asks the box to switch.
// ★★ WHAT IT CANNOT. There is no NetworkManager here: the root service (vibeserver-pocket) is played
//    by this script writing its state file. The radio side of that is the fake-NM test
//    (vibeserver/linux/pocket/test_vibeserver_pocket.py); the real radio is the Pi 3 A+ checklist.
// ★ Local only: a spare port, a temp config, every pocket path under a temp dir (VIBESERVER_POCKET_DIR).
//   ★★★ --mute-audio: the page plays nothing, but the rule is the rule (feedback_tests_must_be_silent).
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-pocket-e2e.mjs
//        (EDGE=/path/to/chromium-like-browser to override the browser)
import { spawn, execSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver"); process.exit(2); }
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
const PORT = 48991, CDP = 9339;
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}`); if (!c) fails++; };

// The LAN address: the claim is only allowed from a PRIVATE peer, never loopback (= the tunnel).
const lan = Object.values(os.networkInterfaces()).flat()
  .find(i => i && i.family === "IPv4" && !i.internal && /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(i.address));
if (!lan) { console.error("no private LAN address to test from"); process.exit(2); }
const BASE = `http://${lan.address}:${PORT}`;

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-pocket-e2e-"));
const PD = path.join(T, "pocket");
fs.mkdirSync(PD);
fs.writeFileSync(path.join(PD, "enabled"), "1\n");
const state = (o) => fs.writeFileSync(path.join(PD, "state.json"), JSON.stringify(Object.assign({
  v: 1, mode: "setup-ap", setupSsid: "VibeServer", country: "GB", ap: { set: false, ssid: "" }, saved: [],
  scan: [{ ssid: "Home", signal: 72, secure: true }, { ssid: "Neighbour", signal: 40, secure: true }],
  scanAt: Math.floor(Date.now() / 1000), apAddress: "10.42.0.1", lastError: "" }, o)) + "\n");
state({});
// ★ The image's first-boot config (image/pocket/stage-pocket/00-pocket/files/config.json), with this test's port.
const seed = JSON.parse(fs.readFileSync(new URL("../image/pocket/stage-pocket/00-pocket/files/config.json", import.meta.url)));
seed.port = PORT;
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify(seed));

const srv = spawn(BIN, ["--config", "/dev/null"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_POCKET_DIR: PD, HOME: T },
  stdio: ["ignore", fs.openSync(path.join(T, "server.log"), "w"), fs.openSync(path.join(T, "server.err"), "w")] });
const prof = path.join(T, "browser");
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--no-first-run", "--disable-extensions",
  `--remote-debugging-port=${CDP}`, `--user-data-dir=${prof}`, "--window-size=390,844", "about:blank"],
  { stdio: "ignore" });

async function cleanup() {
  try { br.kill(); } catch (e) {}
  try { srv.kill(); } catch (e) {}
  // ★ The server restarts ITSELF (execv) on a Mac with no service manager — same pid, so kill() covers it.
  await sleep(500);
  fs.rmSync(T, { recursive: true, force: true });
}

try {
  for (let i = 0; i < 40; i++) { try { await fetch(`${BASE}/vibeserver/pocket/hello`); break; } catch (e) { await sleep(250); } }
  let target;
  for (let i = 0; i < 40 && !target; i++) {
    try { target = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find(t => t.type === "page"); }
    catch (e) { await sleep(250); }
  }
  if (!target) throw new Error("browser did not start");
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  };
  const cdp = (method, params = {}) => new Promise(r => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => {
    const r = await cdp("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || "eval failed");
    return r.result?.result?.value;
  };
  const until = async (expr, ms = 8000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
  const visible = id => `(() => { const e = document.getElementById(${JSON.stringify(id)}); return !!e && e.offsetParent !== null; })()`;
  await cdp("Runtime.enable");
  await cdp("Page.enable");
  await cdp("Page.navigate", { url: `${BASE}/` });

  console.log("pocket first run — a real browser, the real binary");
  ok(await until(visible("claimCard")), "a new box offers 'Choose an admin password' (no TUI)");
  ok(!(await js(`document.getElementById("signinCard").offsetParent !== null`)), "…instead of a sign-in that names the terminal");
  await js(`document.getElementById("claimPass").value = "pocket123"; document.getElementById("claimPass2").value = "pocket12X";
            document.getElementById("claimBtn").click(); 1`);
  ok(await until(`document.getElementById("claimErr").textContent.includes("did not match")`), "mismatched passwords are caught in the page");
  await js(`document.getElementById("claimPass2").value = "pocket123"; document.getElementById("claimBtn").click(); 1`);
  ok(await until(visible("setup"), 10000), "claiming the password signs the page straight in");
  ok(await until(visible("wifiCard")), "the Wi-Fi card is drawn (pocket box), first on the Server tab");
  ok(await until(`[...document.querySelectorAll('#wifiNets select option')].some(o => o.value === "Home")`),
     "network 1 can be picked from the box's scan list");

  // Fill: 1 = Home (picked from the scan), 2 = the iPhone hotspot (typed, not visible), hotspot.
  await js(`(() => {
    const sel = document.querySelector('#wifiNets select'); sel.value = "Home"; sel.dispatchEvent(new Event("change"));
    return 1; })()`);
  await js(`(() => { const p = document.querySelector('#wifiNets .wifiRow [data-f="psk"]'); p.value = "homepass1"; p.dispatchEvent(new Event("input")); return 1; })()`);
  await js(`document.getElementById("wifiAdd").click(); 1`);
  await js(`(() => {
    const row = document.querySelectorAll('#wifiNets .wifiRow')[1];
    const sel = row.querySelector('select'); sel.value = ""; sel.selectedIndex = sel.options.length - 1; sel.dispatchEvent(new Event("change"));
    return 1; })()`);
  await js(`(() => {
    const row = document.querySelectorAll('#wifiNets .wifiRow')[1];
    const s = row.querySelector('[data-f="ssid"]'); s.value = "Stuart's iPhone"; s.dispatchEvent(new Event("input"));
    const p = row.querySelector('[data-f="psk"]'); p.value = "phonepass"; p.dispatchEvent(new Event("input"));
    const h = row.querySelector('[data-f="hidden"]'); h.checked = true; h.dispatchEvent(new Event("change"));
    return 1; })()`);
  // Hotspot password too short first.
  await js(`document.getElementById("apSsid").value = "Pocket-SDR"; document.getElementById("apPsk").value = "short";
            document.getElementById("wifiSave").click(); 1`);
  ok(await until(`document.getElementById("wifiErr").textContent.includes("8 to 63")`), "a 5-character hotspot password is refused in the page");
  // Order: move the iPhone up, then back down — the arrows must work.
  await js(`document.querySelectorAll('#wifiNets .wifiRow')[1].querySelector('[data-mv="-1"]').click(); 1`);
  ok(await js(`document.querySelectorAll('#wifiNets .wifiRow')[0].querySelector('[data-f="ssid"]').value === "Stuart's iPhone"`),
     "the ↑ arrow reorders (the iPhone is now 1)");
  await js(`document.querySelectorAll('#wifiNets .wifiRow')[1].querySelector('[data-mv="-1"]').click(); 1`);
  await js(`document.getElementById("apPsk").value = "pocketpass"; document.getElementById("wifiSave").click(); 1`);
  ok(await until(`document.getElementById("wifiMsg").textContent.includes("Saved")`), "Save Wi-Fi is accepted");
  const inbox = fs.readFileSync(path.join(PD, "pocket-wifi.request"), "utf8");
  const req = JSON.parse(inbox);
  ok(req.networks.map(n => n.ssid).join("|") === "Home|Stuart's iPhone", "the box received the networks IN ORDER: Home, then the iPhone");
  ok(req.networks[1].hidden === true && req.networks[1].psk === "phonepass", "the iPhone is marked not-visible, password intact");
  ok(req.ap.ssid === "Pocket-SDR" && req.ap.psk === "pocketpass", "the fallback hotspot and its password reached the box");
  ok((fs.statSync(path.join(PD, "pocket-wifi.request")).mode & 0o777) === 0o600, "…in a 0600 file");
  const log = fs.readFileSync(path.join(T, "server.log"), "utf8") + fs.readFileSync(path.join(T, "server.err"), "utf8");
  ok(!/homepass1|phonepass|pocketpass|pocket123/.test(log), "no password (Wi-Fi or admin) appears in the server's log");

  // The root service applies it (played here) — the page reads back saved rows without passwords.
  fs.unlinkSync(path.join(PD, "pocket-wifi.request"));
  state({ ap: { set: true, ssid: "Pocket-SDR" }, saved: [{ rank: 1, ssid: "Home", hidden: false }, { rank: 2, ssid: "Stuart's iPhone", hidden: true }] });
  await js(`wifiLoad().then(() => 1)`);
  ok(await js(`document.querySelectorAll('#wifiNets .wifiRow [data-f="psk"]')[0].placeholder.includes("saved")`),
     "saved networks come back as 'saved — leave blank to keep it' (passwords never leave the box)");
  ok(await until(visible("wifiFinish")), "on the setup hotspot, the Finish step is shown");
  ok(await js(`document.getElementById("wifiGo").disabled && document.getElementById("wifiFinishWhy").textContent.includes("Save and start")`),
     "…but it waits for 'Save and start' first");

  // Finish setup: the server restarts itself (no service manager on a Mac) and comes back configured.
  await js(`document.getElementById("saveBtn").click(); 1`);
  const back = await until(`document.getElementById("barMsg").textContent.includes("back up")`, 60000);
  ok(back, "Save and start: the box restarts and comes back configured");
  if (!back) console.log("     bar: " + await js(`document.getElementById("barMsg").textContent`)
                         + " | err: " + await js(`document.getElementById("saveErr").textContent`)
                         + "\n     config: " + (fs.readFileSync(path.join(T, "config.json"), "utf8").match(/"(port|configured)": [a-z0-9]+/g) || []).join(", ")
                         + "\n     server: " + (fs.readFileSync(path.join(T, "server.log"), "utf8")
                                               + fs.readFileSync(path.join(T, "server.err"), "utf8")).split("\n").slice(-12).join("\n     "));
  ok(await until(`!document.getElementById("wifiGo").disabled`), "the Finish step is now available");
  await js(`document.getElementById("wifiGo").click(); 1`);
  const done = await js(`document.getElementById("wifiDone").innerText`);
  ok(/Screenshot this/.test(done) && /Home/.test(done) && /Stuart's iPhone/.test(done), "the next steps name network 1, then 2 — BEFORE anything switches");
  ok(/\.local:48991\//.test(done) && /10\.42\.0\.1:48991/.test(done), "…and every address to reach the box afterwards (.local and the hotspot IP)");
  ok(!fs.existsSync(path.join(PD, "pocket-kick.request")), "nothing has been switched yet");

  // ── The tunnel: Enable + Resume, while the box is still its own hotspot ──
  ok(await until(visible("shareCard")), "the 'Share with the world' card is drawn on a pocket box");
  ok(await js(`!document.getElementById("tunnelOn").checked && document.getElementById("tunnelResumeRow").offsetParent === null`),
     "fresh box: the tunnel is OFF and 'Resume after a restart' is hidden");
  await js(`cfg.name = "Pocket SDR"; document.getElementById("tunnelOn").click(); 1`);
  ok(await until(`document.getElementById("shareMsg").textContent.includes("Tunnel waiting for an internet connection")`),
     "enabled on the hotspot: 'Tunnel waiting for an internet connection' — not an error");
  ok(/"dirList": true/.test(fs.readFileSync(path.join(T, "config.json"), "utf8")), "…and the switch is saved in the server config");
  ok(await js(`document.getElementById("tunnelResumeRow").offsetParent !== null && !document.getElementById("tunnelResume").checked`),
     "'Resume the tunnel after a restart' appears, OFF by default");
  await js(`document.getElementById("tunnelResume").click(); 1`);
  let resumed = false;
  for (let i = 0; i < 30 && !resumed; i++) {
    resumed = /"dirResume": true/.test(fs.readFileSync(path.join(T, "config.json"), "utf8"));
    if (!resumed) await sleep(200);
  }
  ok(resumed, "Resume is persisted in the server config");
  const slog = fs.readFileSync(path.join(T, "server.log"), "utf8") + fs.readFileSync(path.join(T, "server.err"), "utf8");
  ok(!/startTunnel|trycloudflare|NO TUNNEL/.test(slog), "no tunnel was attempted while there is no internet (no spawn, no retry spam)");
  await js(`document.getElementById("tunnelOn").click(); 1`);
  ok(await until(`document.getElementById("shareMsg").textContent.startsWith("Personal")`), "switched off: Personal again");
  await js(`document.getElementById("wifiGoNow").click(); 1`);
  ok(await until(`document.getElementById("wifiGoNow").textContent.includes("Switching")`), "Leave the hotspot now → the box is asked to switch");
  ok(fs.existsSync(path.join(PD, "pocket-kick.request")) && fs.readFileSync(path.join(PD, "pocket-kick.request"), "utf8") === "switch\n",
     "…by the one-word kick the root service reads");

  ok(errors.length === 0, "no JavaScript errors on the page" + (errors.length ? ": " + errors.slice(0, 3).join(" | ") : ""));
  ws.close();
} catch (e) {
  console.error(e); fails++;
}
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
