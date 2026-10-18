#!/usr/bin/env node
// test-web-keyboard.mjs — THE KEYBOARD'S ONE SCHEME on the served receiver page (Stuart, 2026-10-18; memory:
// gamepad_mapping; src/utils/keyActions.ts), in a real (headless, MUTED) browser against the real vibeserver and a fake
// radio. Keys are REAL key events (CDP Input.dispatchKeyEvent), so they arrive exactly as a keyboard's would.
//
//  Arrows and W A S D move the highlight (and reach the frequency box and the mode); Enter presses; Backspace and
//  Escape close; < > , . tune; - + = zoom. Typing in the frequency field is never hijacked. The old shortcuts (arrows
//  tuning and zooming, M mute, [ ] step, T, mode letters) do nothing.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-keyboard.mjs   (exit 3 = not run: no binary)
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

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-keyboard-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Key test", locator: "IO92nh",
  adminPass: "key-admin-pass", radios: [{ serial: "kb", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: "ignore" });
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });

// ★ What the page asks of the server: the last view (binBandwidth) it sent — a zoom is a narrower one.
const SPY = `
  window.__bb = [];
  const s0 = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) {
    if (typeof d === 'string') { const m = /"binBandwidth":\\s*([0-9.eE+-]+)/.exec(d); if (m) window.__bb.push(+m[1]); }
    return s0.call(this, d);
  };
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
  await call("Page.addScriptToEvaluateOnNewDocument", { source: SPY });
  await call("Page.navigate", { url });
  // A real key: rawKeyDown for named keys, keyDown with text for characters (so a focused field types it).
  const key = async (k, mods = 0) => {
    // ★ Real virtual-key codes: '.'.charCodeAt(0) is 46, which is VK_DELETE — the browser would treat it as Delete.
    const named = { ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27, Backspace: 8,
                    ".": 190, ">": 190, ",": 188, "<": 188, "-": 189, "_": 189, "=": 187, "+": 187, "[": 219, "]": 221 };
    const vk = named[k] ?? k.toUpperCase().charCodeAt(0);
    const text = k === 'Enter' ? '\r' : (k.length === 1 ? k : undefined);
    await call("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key: k, windowsVirtualKeyCode: vk, text, unmodifiedText: text, modifiers: mods });
    await call("Input.dispatchKeyEvent", { type: "keyUp", key: k, windowsVirtualKeyCode: vk, modifiers: mods });
    await sleep(90);
  };
  return { ws, js, until, tap, key, errors };
}

const freq = (p) => p.js(`parseFloat(document.getElementById("freqInput").value)`);
const hl = (p) => p.js(`(() => { const h = document.querySelector(".gpFocus"); return h ? (h.id || (h.textContent || "").trim().slice(0, 20)) : ""; })()`);
const lastBb = (p) => p.js(`window.__bb.length ? window.__bb[window.__bb.length - 1] : 0`);
const near = (a, b) => Math.abs(a - b) < 1e-6;
const MOVES = ["ArrowRight", "s", "ArrowLeft", "w", "d", "ArrowDown", "a", "ArrowUp"];
// Walk the highlight (arrows and WASD in turn, a few presses each) until `test` holds of the highlighted control.
async function walkTo(p, test, max = 140) {
  for (let i = 0; i < max; i++) {
    if (await p.js(`(() => { const h = document.querySelector(".gpFocus"); return !!h && (${test})(h); })()`)) return true;
    await p.key(MOVES[Math.floor(i / 3) % MOVES.length]);
  }
  return false;
}
const IS_MENU = `(h) => /^\\s*MENU\\s*$/i.test(h.textContent || "")`;

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("the keyboard on the served page — real key events, a real browser, the real binary");
  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await sleep(1500); await p.tap(596, 312);                           // START RADIO (a real tap: audio needs a gesture)
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running");
  await p.js(`document.activeElement && document.activeElement.blur && document.activeElement.blur(); 1`);
  await sleep(600);

  // ── The old shortcuts do nothing ──
  const f0 = await freq(p);
  const step0 = await p.js(`document.getElementById("mStep")?.textContent || ""`);
  const mode0 = await p.js(`document.getElementById("mMode")?.textContent || ""`);
  const bb0 = await lastBb(p);
  await p.js(`document.querySelector(".gpFocus")?.classList.remove("gpFocus"); 1`);
  for (const k of ["m", "t", "[", "]", "u", "l", "n", "c", "f", "e", "h", "k"]) await p.key(k);
  await p.key("ArrowRight", 8);                                        // Shift+→ was ×10 tuning
  await sleep(400);
  ok(near(await freq(p), f0), `the arrows no longer tune, Shift+→ no longer runs ×10 (${await freq(p)} MHz)`);
  ok((await p.js(`document.getElementById("mStep")?.textContent || ""`)) === step0, "[ and ] no longer cycle the step");
  ok((await p.js(`document.getElementById("mMode")?.textContent || ""`)) === mode0, "mode letters (u l n c) no longer change the mode");
  ok(await p.js(`!document.getElementById("mAudio")?.classList.contains("muted") && !(window.__vibeAudio?.debugState?.().muted)`), "M no longer mutes");
  ok(near(await lastBb(p), bb0), "up/down arrows no longer zoom");
  ok(!(await p.js(`!!document.querySelector(".open")`)), "no panel opened from a letter key");

  // ── Movement: arrows and WASD drive the highlight ──
  await p.js(`document.querySelector(".gpFocus")?.classList.remove("gpFocus"); 1`);
  await p.key("s");
  ok(await p.until(`!!document.querySelector(".gpFocus")`, 2000), "S: a highlight box appears on a control");
  const a0 = await hl(p);
  await p.key("d"); const a1 = await hl(p);
  await p.key("ArrowLeft"); const a2 = await hl(p);
  ok(a1 !== a0 || a2 !== a1, `D and ← move the highlight (${a0} → ${a1} → ${a2})`);
  const seen = new Set();
  for (let i = 0; i < 80 && seen.size < 2; i++) {
    const id = await p.js(`document.querySelector(".gpFocus")?.id || ""`);
    if (id === "mFreqBox" || id === "mMode") seen.add(id);
    await p.key(MOVES[Math.floor(i / 2) % MOVES.length]);
  }
  ok(seen.has("mFreqBox"), "arrows/WASD: the highlight reaches the frequency box");
  ok(seen.has("mMode"), "arrows/WASD: the highlight reaches the mode (demodulator) button");

  // ── Enter opens; Backspace and Escape close ──
  ok(await walkTo(p, IS_MENU), "arrows/WASD reach the MENU key");
  await p.key("Enter");
  ok(await p.until(`document.getElementById("menu").classList.contains("open")`, 3000), "Enter presses the highlighted control (the MENU opens)");
  await p.key("s");
  ok(await p.js(`document.getElementById("menu").contains(document.querySelector(".gpFocus"))`), "with the menu open, S moves the highlight INSIDE it");
  await p.key("Backspace");
  ok(await p.until(`!document.getElementById("menu").classList.contains("open")`, 3000), "Backspace closes the menu");
  ok(await walkTo(p, IS_MENU), "back on the MENU key");
  await p.key("Enter");
  ok(await p.until(`document.getElementById("menu").classList.contains("open")`, 3000), "Enter opens it again");
  await p.key("Escape");
  ok(await p.until(`!document.getElementById("menu").classList.contains("open")`, 3000), "Escape closes it too");

  // ── Tune: < > and , . ──
  const t0 = await freq(p);
  await p.key(">"); await sleep(300); const t1 = await freq(p);
  ok(t1 > t0, `> tunes up one step (${t0} → ${t1})`);
  await p.key("<"); await sleep(300); const t2 = await freq(p);
  ok(near(t2, t0), `< tunes back down (${t2})`);
  await p.key("."); await sleep(300); const t3 = await freq(p);
  ok(near(t3, t1), `. (unshifted >) tunes up (${t3})`);
  await p.key(","); await sleep(300);
  ok(near(await freq(p), t0), `, (unshifted <) tunes down`);

  // ── Zoom: - + = ──
  await sleep(700);
  const z0 = await lastBb(p);
  await p.key("+"); await sleep(700); const z1 = await lastBb(p);
  ok(z1 > 0 && (z0 === 0 || z1 < z0), `+ zooms in (bin ${z0} → ${z1} Hz)`);
  await p.key("-"); await sleep(700); const z2 = await lastBb(p);
  ok(z2 > z1, `- zooms out (${z1} → ${z2} Hz)`);
  await p.key("="); await sleep(700); const z3 = await lastBb(p);
  ok(z3 < z2, `= (unshifted +) zooms in (${z2} → ${z3} Hz)`);
  await p.key("-"); await sleep(400);

  // ── Typing in the frequency field is never hijacked ──
  ok(await walkTo(p, `(h) => h.id === "mFreqBox"`), "back on the frequency box");
  await p.key("Enter");
  ok(await p.until(`document.activeElement?.id === "freqInput"`, 3000), "Enter on the frequency box: the field has the cursor");
  const before = await freq(p);
  await p.js(`document.getElementById("freqInput").value = ""; 1`);
  const hlBefore = await hl(p);
  for (const k of ["9", "7", ".", "5", "w", "a", "s", "d", ",", "-", "+", "<", ">"]) await p.key(k);
  await p.key("ArrowLeft"); await p.key("ArrowRight");
  const typed = await p.js(`document.getElementById("freqInput").value`);
  ok(typed.startsWith("97.5"), `the field took the digits and the full stop (${JSON.stringify(typed)})`);
  ok(await p.js(`document.activeElement?.id === "freqInput"`), "W A S D , - + < > and the arrows stayed in the field (it kept the cursor)");
  ok((await hl(p)) === hlBefore, "…and the highlight did not move while typing");
  await p.key("Backspace");
  const afterBs = await p.js(`document.getElementById("freqInput").value`);
  ok(afterBs.length === typed.length - 1 && await p.js(`document.activeElement?.id === "freqInput"`), "Backspace deletes a character in the field (it is not back there)");
  await p.js(`document.getElementById("freqInput").value = "${before}"; 1`);
  await p.key("Escape");
  ok(await p.until(`document.activeElement?.id !== "freqInput" && !document.querySelector(".open")`, 3000), "Escape leaves the field and closes its panel");
  ok(near(await freq(p), before), "nothing tuned by the typing");

  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
