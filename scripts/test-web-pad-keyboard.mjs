#!/usr/bin/env node
// test-web-pad-keyboard.mjs — THE CONTROLLER KEYBOARD on the served receiver page (web/client/src/padKeyboard.ts; the
// layouts and rules are src/utils/kbLayouts.ts + controllerKeyboard.ts + kbHandover.ts, shared with the app; decided by
// Stuart 2026-10-20), in a real (headless, MUTED) browser against the real vibeserver and a fake radio. The pad is
// SIMULATED, as in test-web-pad-guide.mjs.
//
//  Stuart's rules, each checked:
//   • a pad that is only CONNECTED (and a stick drifting) is not pad use — a box focused then is the browser's, no overlay;
//   • the keyboard shows only when the pad was the last input: enter on a box opens it, and the box is NEVER focused
//     while it shows (a phone would pop its own keyboard up);
//   • open by pad, type, submit (START tap and ↵); an out-of-range frequency is refused with the card's own message;
//   • search follows the typing live; the left stick moves the caret; □ is ⌫; back peels one layer, then cancels;
//   • TOUCH hand-over: a tap closes ours and the box takes the cursor with the same text and caret;
//   • PAD hand-over: a pad press in a box being typed in brings ours back with the same text and caret;
//   • PHYSICAL-KEY hand-over: a key closes ours and types in place — the character lands in the box;
//   • pad BACK lets go of a box that has the cursor (the audit's trap);
//   • PASSWORD mode: dots, a neutral ring, no press flash, the masking kept across the hand-over, nothing logged;
//   • HOLD START is ignored while it is open; the BUTTONS profile has R enter / L back and the tune buttons as caret.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-pad-keyboard.mjs   (exit 3 = not run: no binary)
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

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-padkb-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Keyboard test", locator: "IO92nh",
  adminPass: "pad-admin-pass", radios: [{ serial: "kb", label: "T", enabled: true, configured: true, users: 3 }] }));
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
  let id = 0; const pending = new Map(); const errors = []; const logs = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    if (m.method === "Runtime.consoleAPICalled") logs.push(m.params.args.map((a) => String(a.value ?? a.description ?? "")).join(" ")); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await call("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 }); };
  // A real key on a real keyboard (trusted, through the browser's input pipeline).
  const key = async (k, text) => {
    await call("Input.dispatchKeyEvent", { type: "keyDown", key: k, text, unmodifiedText: text, code: `Key${k.toUpperCase()}`, windowsVirtualKeyCode: k.toUpperCase().charCodeAt(0) });
    await call("Input.dispatchKeyEvent", { type: "keyUp", key: k, code: `Key${k.toUpperCase()}`, windowsVirtualKeyCode: k.toUpperCase().charCodeAt(0) });
  };
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_PAD });
  await call("Page.navigate", { url });
  return { ws, js, until, tap, key, errors, logs, call };
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
const B = { S: 0, E: 1, W: 2, N: 3, L1: 4, R1: 5, SELECT: 8, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
const freq = (p) => p.js(`parseFloat(document.getElementById("freqInput").value)`);
const press = async (p, i) => { await p.js(`__btn(${i}, true); 1`); await sleep(110); await p.js(`__btn(${i}, false); 1`); await sleep(110); };
const hold = async (p, i, ms) => { await p.js(`__btn(${i}, true); 1`); await sleep(ms); await p.js(`__btn(${i}, false); 1`); await sleep(120); };
// Turn the left stick from `from`° to `to`° (5° a step), then let it go.
const turn = async (p, from, to) => {
  const st = to > from ? 5 : -5;
  for (let d = from; st > 0 ? d <= to : d >= to; d += st) {
    await p.js(`__pad.axes[0] = ${Math.cos(d * Math.PI / 180)}; __pad.axes[1] = ${Math.sin(d * Math.PI / 180)}; 1`);
    await sleep(30);
  }
  await p.js(`__pad.axes[0] = 0; __pad.axes[1] = 0; 1`); await sleep(100);
};
const kbShown = (p) => p.js(`(() => { const k = document.getElementById("padKb"); return !!k && !k.hidden; })()`);
const kbValue = (p) => p.js(`(() => { const k = document.getElementById("padKb"); return k ? k.querySelector(".kbBefore").textContent + "|" + k.querySelector(".kbAfter").textContent : ""; })()`);
const onKey = (p) => p.js(`document.querySelector("#padKb .kbOn")?.dataset.id || ""`);
const active = (p) => p.js(`document.activeElement ? (document.activeElement.id || document.activeElement.tagName) : ""`);
const inputFocused = (p) => p.js(`!!document.activeElement && /INPUT|TEXTAREA/.test(document.activeElement.tagName)`);
// Walk the highlight to the key `id` with the D-pad (greedy, by where the keys are drawn: along the row until the
// target is above or below, then up or down).
async function goTo(p, id) {
  for (let i = 0; i < 24; i++) {
    const d = await p.js(`(() => { const on = document.querySelector("#padKb .kbOn"), t = document.querySelector('#padKb .kbKey[data-id="${id}"]');
      if (!on || !t) return null; if (on === t) return "here"; const a = on.getBoundingClientRect(), b = t.getBoundingClientRect();
      const dx = (b.left + b.width / 2) - (a.left + a.width / 2), dy = (b.top + b.height / 2) - (a.top + a.height / 2);
      if (b.left > a.right) return "right"; if (b.right < a.left) return "left"; return dy > 0 ? "down" : "up"; })()`);
    if (d === "here") return true;
    if (!d) return false;
    await press(p, { up: B.UP, down: B.DOWN, left: B.LEFT, right: B.RIGHT }[d]);
  }
  return false;
}
async function typeKeys(p, ids, enterBtn = B.S) { for (const id of ids) { await goTo(p, id); await press(p, enterBtn); } }
// SHOTS=<dir> saves a picture of each keyboard, wide and at phone size, for a look.
const SHOTS = process.env.SHOTS;
const shot = async (p, name) => {
  if (!SHOTS) return;
  for (const [w, h] of [[1200, 900], [390, 844], [844, 390]]) {
    await p.call("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 2, mobile: w < 900 });
    await sleep(350);
    const r = await p.call("Page.captureScreenshot", { format: "png" });
    if (r.result?.data) { fs.mkdirSync(SHOTS, { recursive: true }); fs.writeFileSync(path.join(SHOTS, `${name}-${w}x${h}.png`), Buffer.from(r.result.data, "base64")); }
  }
  await p.call("Emulation.clearDeviceMetricsOverride");
  await sleep(300);
};
// Put the pad's highlight on a box: focusing it while the pad drives makes the page let go and highlight it instead.
const highlightField = (p, id) => p.js(`document.getElementById("${id}").focus(); 1`);

try {
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("the controller keyboard on the served page — a simulated pad, a real browser, the real binary");

  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`localStorage.removeItem("vibe.padProfile"); document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  ok(await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`), "the receiver is running");

  // ── A pad that is only CONNECTED, with a drifting stick, is not pad use ──
  await p.js(`__connect(); __pad.axes[0] = 0.18; __pad.axes[1] = -0.12; __pad.axes[2] = 0.3; 1`);
  await sleep(400);
  await p.js(`document.getElementById("mFreqBox").click(); 1`);       // the card opens its box and focuses it
  await sleep(300);
  ok((await active(p)) === "freqInput" && !(await kbShown(p)), "connected pad + stick drift: the box takes the cursor as always, no on-screen keyboard");
  const fb = await p.js(`(() => { const r = document.getElementById("freqInput").getBoundingClientRect(); return [r.left + 10, r.top + r.height / 2]; })()`);
  await p.tap(fb[0], fb[1]);
  await sleep(200);
  ok((await active(p)) === "freqInput" && !(await kbShown(p)), "a mouse click on the box: the browser's own typing, never ours");
  await p.js(`__pad.axes[0] = 0; __pad.axes[1] = 0; __pad.axes[2] = 0; document.activeElement.blur(); 1`);
  await sleep(100);

  // ── Open by pad, type, submit with a START tap ──
  await press(p, B.DOWN);                                              // a real press: the pad is now the input
  await highlightField(p, "freqInput");
  ok(await p.until(`document.activeElement !== document.getElementById("freqInput") && document.getElementById("freqInput").classList.contains("gpFocus")`, 2000),
     "a box that takes the cursor by itself while the pad drives lets go and is highlighted instead");
  await press(p, B.S);
  ok(await kbShown(p), "✕ on the frequency box opens the controller keyboard");
  ok(await p.js(`document.getElementById("padKb").classList.contains("numeric") && !!document.querySelector('#padKb .kbKey[data-id="unit"]')`),
     "…the number pad, with the card's kHz/MHz key");
  ok(!(await inputFocused(p)), "…and NO input has the cursor while it shows (no phone keyboard underneath)");
  ok((await onKey(p)) === "5", "…the highlight starts on 5, never on ↵");
  ok(await p.js(`(() => { const d = document.querySelector("#padKb .kbDigit"), f = document.getElementById("mFreq");
      const a = getComputedStyle(d), b = getComputedStyle(f); return a.fontVariantNumeric === b.fontVariantNumeric && a.fontFamily === b.fontFamily && a.color === b.color; })()`),
     "the number pad's digits are the frequency readout's own style (font, figures, colour)");
  await typeKeys(p, ["clear", "9", "6", ".", "5"]);
  await shot(p, "numeric");
  ok((await kbValue(p)) === "96.5|", `typed 96.5 with the caret at the end (${await kbValue(p)})`);
  await press(p, B.START);                                             // a TAP
  ok(await p.until(`!document.getElementById("padKb") || document.getElementById("padKb").hidden`, 3000), "a START tap submits and closes it");
  ok(await p.until(`!document.getElementById("freqPanel").classList.contains("open")`, 3000), "…the card's own Enter ran (TUNE, the card closed)");
  ok(await p.until(`Math.abs(parseFloat(document.getElementById("freqInput").value) - 96.5) < 1e-6`, 4000), `…and the radio is on 96.5 (${await freq(p)})`);

  // ── Out of range: refused with the card's own message, the keyboard stays ──
  await p.js(`document.getElementById("mFreqBox").click(); 1`);
  await sleep(300);
  ok(!(await inputFocused(p)), "the card's own focus on opening is let go while the pad drives");
  await press(p, B.S);
  ok(await kbShown(p), "✕ opens the keyboard again");
  await typeKeys(p, ["clear", "enter"]);
  const msg = await p.js(`document.querySelector("#padKb .kbErr").textContent`);
  ok((await kbShown(p)) && /frequency/i.test(msg || ""), `nothing typed, ↵: the card refuses, the keyboard stays and shows its message ("${msg}")`);
  ok(await p.js(`document.getElementById("freqPanel").classList.contains("open")`), "…with the card still open under it");
  // Back: one layer — there is none on the number pad, so it cancels; the box keeps what it had.
  await press(p, B.E);
  ok(!(await kbShown(p)), "○ cancels");
  ok(await p.js(`document.getElementById("freqInput").classList.contains("gpFocus")`), "…and the highlight is back on the box");
  await p.js(`document.querySelectorAll(".open").forEach(e => e.classList.remove("open")); 1`);

  // ── HOLD START is ignored while open ──
  await p.js(`document.getElementById("mFreqBox").click(); 1`);
  await sleep(300);
  await press(p, B.S);
  await hold(p, B.START, 1200);
  ok((await kbShown(p)) && (await p.js(`localStorage.getItem("vibe.padProfile")`)) === null, "HOLD START while typing: no profile switch, still open");
  await press(p, B.E);
  await p.js(`document.querySelectorAll(".open").forEach(e => e.classList.remove("open")); 1`);

  // ── QWERTY: search, live; caret by the left stick; □ = ⌫; layers; back peels one ──
  await p.js(`document.getElementById("mFreqBox").click(); 1`);
  await sleep(300);
  await highlightField(p, "search");
  await sleep(100);
  await press(p, B.S);
  ok((await kbShown(p)) && !(await p.js(`document.getElementById("padKb").classList.contains("numeric")`)), "✕ on the search box: the QWERTY keyboard");
  await typeKeys(p, ["lr", "la", "ld", "li", "lo"]);
  await shot(p, "qwerty");
  ok((await kbValue(p)) === "radio|", `typed "radio" (${await kbValue(p)})`);
  ok((await p.js(`document.getElementById("search").value`)) === "radio", "search follows the typing live");
  await turn(p, 0, -100);                                               // anticlockwise = caret left
  const kv = await kbValue(p);
  ok(/^radi?o?\|/.test(kv) && kv !== "radio|", `the left stick turned moves the caret (${kv})`);
  const before = kv.split("|")[0];
  await press(p, B.W);
  ok((await kbValue(p)).split("|")[0] === before.slice(0, -1), "□ is ⌫ (deletes before the caret)");
  await press(p, B.R1);
  ok(await p.js(`!!document.querySelector('#padKb .kbKey[data-id="abc"]')`), "R1: the symbols layer");
  await press(p, B.E);
  ok((await kbShown(p)) && (await p.js(`!!document.querySelector('#padKb .kbKey[data-id="lq"]')`)), "○ peels the layer first — still open, letters back");
  const typed = await kbValue(p);

  // ── TOUCH hand-over: same text and caret in the real box, which takes the cursor ──
  await p.tap(20, 20);
  await sleep(250);
  const val = await p.js(`document.getElementById("search").value`);
  const car = await p.js(`document.getElementById("search").selectionStart`);
  ok(!(await kbShown(p)) && (await active(p)) === "search", "a tap closes ours and the search box takes the cursor (the system keyboard)");
  ok(val === typed.replace("|", "") && car === typed.indexOf("|"), `…with the same text and caret (${val} @${car}, was ${typed})`);
  ok(await p.js(`document.getElementById("freqPanel").classList.contains("open")`), "…and the tap did not close the card under it");

  // ── PAD hand-over: a press while typing in the box brings ours back, same text and caret ──
  await p.js(`(() => { const s = document.getElementById("search"); s.setSelectionRange(2, 2); })(); 1`);
  await press(p, B.DOWN);
  ok((await kbShown(p)) && !(await inputFocused(p)), "a pad press while the box has the cursor: ours comes up, the box lets go");
  ok((await kbValue(p)) === val.slice(0, 2) + "|" + val.slice(2), `…with the same text and caret (${await kbValue(p)})`);

  // ── PHYSICAL-KEY hand-over: a key closes ours and types in place ──
  await p.key("x", "x");
  await sleep(200);
  ok(!(await kbShown(p)) && (await active(p)) === "search", "a key on a real keyboard closes ours; the box has the cursor");
  ok((await p.js(`document.getElementById("search").value`)) === val.slice(0, 2) + "x" + val.slice(2),
     `…and the key typed in place, at the caret (${await p.js(`document.getElementById("search").value`)})`);
  ok(Math.abs((await freq(p)) - 96.5) < 1e-6, "…and the key was not a page shortcut (nothing retuned)");

  // ── Pad BACK lets go of a box that has the cursor (the audit's trap) ──
  await press(p, B.E);
  ok(!(await inputFocused(p)) && !(await kbShown(p)), "○ in a box with the cursor: the box lets go (no keyboard opens)");
  ok(await p.js(`document.getElementById("search").classList.contains("gpFocus")`), "…and the highlight stays on it");
  await p.js(`document.querySelectorAll(".open").forEach(e => e.classList.remove("open")); 1`);

  // ── PASSWORD mode ──
  await p.js(`document.getElementById("menu").classList.add("open"); document.getElementById("adminPwRow").hidden = false; 1`);
  await sleep(200);
  await highlightField(p, "adminPwInput");
  await press(p, B.S);
  ok((await kbShown(p)) && (await p.js(`document.getElementById("padKb").classList.contains("secure")`)), "✕ on the admin password: the keyboard in password mode");
  await typeKeys(p, ["lz", "lq", "d7"]);
  await shot(p, "password");
  ok((await kbValue(p)) === "•••|", `the value is drawn as dots (${await kbValue(p)})`);
  ok(!(await p.js(`document.getElementById("padKb").textContent.includes("zq7")`)), "…and the typed text appears nowhere on the page");
  ok((await p.js(`getComputedStyle(document.querySelector("#padKb .kbOn")).outlineColor`)) === "rgb(154, 154, 154)", "a neutral grey ring, not the green one");
  await goTo(p, "ll");
  await p.js(`__btn(0, true); 1`);
  let flashSeen = false;
  for (let i = 0; i < 6; i++) { flashSeen ||= await p.js(`!!document.querySelector("#padKb .kbPress")`); await sleep(15); }
  await p.js(`__btn(0, false); 1`); await sleep(120);
  ok(!flashSeen, "no press flash in password mode");
  await p.tap(20, 20);
  await sleep(250);
  ok((await active(p)) === "adminPwInput" && (await p.js(`document.getElementById("adminPwInput").type`)) === "password"
     && (await p.js(`document.getElementById("adminPwInput").value`)) === "zq7l", "touch hand-over: the real password box, still masked, same text");
  ok(!p.logs.some((l) => l.includes("zq7")), "nothing logged the password");
  await p.js(`document.getElementById("adminPwInput").value = ""; document.activeElement.blur(); document.getElementById("menu").classList.remove("open"); 1`);

  // ── The BUTTONS profile: R enters, L backs out, the tune buttons move the caret ──
  await hold(p, B.START, 1200);
  ok((await p.js(`localStorage.getItem("vibe.padProfile")`)) === "buttons", "HOLD START (keyboard closed) switches to BUTTONS");
  await p.js(`document.getElementById("bookmarksPanel").classList.add("open"); 1`);
  await sleep(300);
  await highlightField(p, "bmName");
  await press(p, B.R1);
  ok(await kbShown(p), "R on the bookmark name opens the keyboard");
  await typeKeys(p, ["lq", "lz"], B.R1);
  await press(p, B.W);                                                 // west = caret left on this profile
  ok((await kbValue(p)) === "q|z", `R presses keys; the left face button moves the caret (${await kbValue(p)})`);
  ok(await goTo(p, "enter"), "the D-pad reaches ↵");
  await press(p, B.R1);
  ok(!(await kbShown(p)) && (await p.until(`document.getElementById("bmList").textContent.includes("qz")`, 3000)),
     "↵ is the name box's own Enter: the bookmark \"qz\" is saved");
  await press(p, B.R1);
  await press(p, B.L1);
  ok(!(await kbShown(p)), "L closes it");
  await hold(p, B.START, 1200);

  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  await p.js(`localStorage.removeItem("vibe.padProfile"); 1`);
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
