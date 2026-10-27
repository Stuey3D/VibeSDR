#!/usr/bin/env node
// test-web-bookmark-tooltip.mjs — THE BOOKMARK UNDER THE POINTER (GitHub #60; Stuart: "a little bookmark on the mouse
// tooltip alongside the frequency like we added for Kiko but I do not want to crowd the waterfall with labels"), in a
// real (headless, MUTED) browser against the real vibeserver and a fake radio.
//
//  Hovering a bookmarked frequency adds the bookmark glyph and its name to the frequency-at-pointer label; off the
//  bookmark the label is the frequency alone; overlapping bookmarks say "+N more"; a click still just tunes; nothing is
//  drawn on the waterfall for a bookmark.
//
// ★ The page under test is the one JUST BUILT (web/dist, node scripts/build-web.mjs) — served to the browser through
//   CDP request interception in place of the page compiled into the binary, so no vibeserver rebuild is needed.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-bookmark-tooltip.mjs   (exit 3 = not run: no binary)
//        SHOT=/path/to.png also saves a screenshot of the label over the bookmark.
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

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-bmtip-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Bookmark tooltip test", locator: "IO92nh",
  adminPass: "bm-admin-pass", radios: [{ serial: "bt", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: process.env.DEBUG ? ["ignore", "inherit", "inherit"] : "ignore" });
// ★ --mute-audio: tests are SILENT (an agent once blasted test tones through Stuart's speakers).
// ★ A FULFILLED document has no IP address space of its own, so Local Network Access would block its sockets to 127.0.0.1.
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,BlockInsecurePrivateNetworkRequests", "--disable-audio-output", "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });

async function cleanup() { for (const p of [br, srv, rtl]) { try { p.kill(); } catch {} } await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }

async function open(url) {
  let target;
  for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === "page"); } catch { await sleep(250); } }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (process.env.DEBUG && (m.method === "Runtime.consoleAPICalled" || m.method === "Log.entryAdded")) console.log("  [page]", JSON.stringify(m.params.args?.map((a) => a.value ?? a.description) ?? m.params.entry?.text).slice(0, 300));
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(100); } return false; };
  const mouse = (type, x, y, extra = {}) => call("Input.dispatchMouseEvent", { type, x, y, button: "none", ...extra });
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await mouse(type, x, y, { button: "left", clickCount: 1 }); };
  await call("Runtime.enable"); await call("Page.enable"); if (process.env.DEBUG) await call("Log.enable");
  // ★ Serve the freshly built page (web/dist) instead of the one compiled into the binary.
  const DIST = path.join(ROOT, "web/dist");
  // ★ At the RESPONSE stage: the server's own headers (its cookies, its policy) are kept, only the body is swapped.
  ws.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.method !== "Fetch.requestPaused") return;
    const u = new URL(m.params.request.url);
    const file = u.pathname === "/" ? path.join(DIST, "vibesdr.html") : path.join(DIST, u.pathname);
    if (u.origin !== new URL(url).origin || !(u.pathname === "/" || u.pathname.startsWith("/vs/"))
        || !file.startsWith(DIST) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      call("Fetch.continueRequest", { requestId: m.params.requestId }); return;
    }
    // ★ The new build's scripts have new hashed names the binary does not know (its answer is a 404): always 200, and
    //   the type by extension, keeping the rest of what the server said (the page's cookies and its policy).
    const type = file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html; charset=utf-8";
    const headers = (m.params.responseHeaders || [])
      .filter((h) => !/^(content-encoding|content-length|content-type|etag|last-modified|cache-control)$/i.test(h.name))
      .concat([{ name: "Content-Type", value: type }, { name: "Cache-Control", value: "no-store" }]);
    call("Fetch.fulfillRequest", { requestId: m.params.requestId, responseCode: 200,
      responseHeaders: headers, body: fs.readFileSync(file).toString("base64") });
  });
  // ★ Documents and scripts only — a WebSocket handshake must never be paused.
  await call("Fetch.enable", { patterns: ["Document", "Script", "Stylesheet"].map((resourceType) => ({ urlPattern: "*", resourceType, requestStage: "Response" })) });
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
  // START RADIO (a real tap: audio needs a gesture). The page here is served through interception, which is slower than
  // the binary's own, so wait for the audio gate's button itself rather than a fixed delay.
  // The splash's START and then the audio gate's START RADIO, each tapped when it shows, until neither does.
  const BTN = `[...document.querySelectorAll("button")].find((x) => /^\\s*START( RADIO)?\\s*$/i.test(x.textContent || "") && x.getBoundingClientRect().width > 2)`;
  for (let i = 0; i < 6; i++) {
    if (!(await p.until(`!!${BTN}`, 4000))) break;
    const at = await p.js(`(() => { const b = ${BTN}; const r = b.getBoundingClientRect(); return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) }; })()`);
    if (!at) break;
    await p.tap(at.x, at.y);
    await sleep(800);
  }
  return p.until(`parseFloat(document.getElementById("freqInput").value) > 0 && ${CARD_HZ} > 0`);
}

try {
  if (!fs.existsSync(path.join(ROOT, "web/dist/vibesdr.html"))) { console.error("run node scripts/build-web.mjs first — not run"); await cleanup(); process.exit(3); }
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("the bookmark under the pointer on the served page — a real browser, the real binary, the page just built");

  const p = await open(`http://127.0.0.1:${port}/`);
  const started = await start(p);
  if (process.env.DEBUG) console.log(await p.js(`JSON.stringify({ app: [...document.scripts].map((x) => x.src).join(","), bm: !!document.querySelector("#ptrFreq .bm"), gate: !!document.getElementById("audioGate") && getComputedStyle(document.getElementById("audioGate")).display, btns: [...document.querySelectorAll("button")].filter((b) => b.getBoundingClientRect().width > 2).map((b) => b.textContent.trim()).slice(0, 12) })`), p.errors);
  ok(started, "the receiver is running");
  await sleep(800);
  ok(await p.js(`!!document.querySelector("#ptrFreq .bm")`), "(the page under test is the new build: the label has a bookmark slot)");
  const r = await p.js(CANVAS);
  const wx = Math.round(r.x + r.w * 0.3), wy = Math.round(r.y + r.h * 0.6);
  const fx = Math.round(r.x + r.w * 0.7);
  await p.mouse("mouseMoved", wx - 20, wy); await p.mouse("mouseMoved", wx, wy);
  ok(await p.until(SHOWN, 2000), "mouse over the waterfall: the label appears");
  if (process.env.DEBUG) console.log(await p.js(`JSON.stringify({ cls: document.getElementById("ptrFreq").className, card: ${CARD_HZ}, inp: document.getElementById("freqInput").value, tg: document.getElementById("ptrFreqTg")?.textContent, hit: document.elementFromPoint(${wx}, ${wy})?.id })`));
  const H = await p.js(LABEL_HZ);
  ok(await p.js(`!document.getElementById("ptrFreq").classList.contains("hasBm")`), "no bookmark there yet: the frequency alone");

  // ── Two of the listener's own bookmarks AT that frequency (an overlap), then reload ──
  await p.js(`localStorage.setItem("lsv_user_bookmarks", JSON.stringify([
    { name: "Hover Test Station", frequency: ${H}, mode: "wfm", scope: "" },
    { name: "Overlapping Neighbour", frequency: ${H + 60000}, mode: "wfm", scope: "" }])); 1`);
  await p.mouse("mouseMoved", wx, Math.round(r.y - 30));
  await p.js(`location.reload(); 1`);
  ok(await start(p), "(reloaded with the bookmarks)");
  await sleep(800);
  const tf0 = await p.js(CARD_HZ);
  await p.mouse("mouseMoved", wx - 20, wy); await p.mouse("mouseMoved", wx, wy);
  ok(await p.until(`${SHOWN} && document.getElementById("ptrFreq").classList.contains("hasBm")`, 3000), "hovering the bookmarked frequency: the bookmark shows in the label");
  const name = await p.js(`document.querySelector("#ptrFreq .bn").textContent`);
  ok(name === "Hover Test Station", `…named, the nearest one first (${JSON.stringify(name)})`);
  const more = await p.js(`document.querySelector("#ptrFreq .bx").textContent`);
  ok(more === "+1 more", `…and the overlapping one counted (${JSON.stringify(more)})`);
  ok(await p.js(`getComputedStyle(document.querySelector("#ptrFreq .bm")).display !== "none" && !!document.querySelector("#ptrFreq .bm svg")`), "…with the bookmark glyph");
  if (process.env.SHOT) {
    const shot = await p.call("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync(process.env.SHOT, Buffer.from(shot.result.data, "base64"));
    console.log(`  (screenshot saved to ${process.env.SHOT})`);
  }
  // ── Far from both: frequency alone again ──
  await p.mouse("mouseMoved", fx, wy);
  ok(await p.until(`${SHOWN} && !document.getElementById("ptrFreq").classList.contains("hasBm")`, 2000), "away from the bookmarks: the frequency alone again");
  // ── Nothing drawn on the scale: no bookmark label elements anywhere but the pointer label ──
  ok(await p.js(`![...document.querySelectorAll("body *")].some((e) => e.id !== "ptrFreq" && !e.closest("#ptrFreq") && !e.closest("#bookmarksPanel") && e.children.length === 0 && e.textContent === "Hover Test Station" && e.getBoundingClientRect().width > 0)`),
     "the bookmark name is drawn nowhere else on screen (no labels over the waterfall)");
  // ── A click still tunes ──
  await p.mouse("mouseMoved", wx, wy);
  await p.until(`document.getElementById("ptrFreq").classList.contains("hasBm")`, 2000);
  await p.mouse("mousePressed", wx, wy, { button: "left", clickCount: 1, buttons: 1 });
  await sleep(80);
  await p.mouse("mouseReleased", wx, wy, { button: "left", clickCount: 1 });
  ok(await p.until(`${CARD_HZ} === ${H} && ${CARD_HZ} !== ${tf0}`, 4000), `a click there tunes, exactly as before (card ${await p.js(CARD_HZ)} Hz)`);
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
