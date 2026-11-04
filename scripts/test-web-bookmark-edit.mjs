#!/usr/bin/env node
// test-web-bookmark-edit.mjs — EDIT A BOOKMARK in the web client (V12, GitHub discussion #59), in a real (headless,
// MUTED) Edge against the real vibeserver and a fake radio.
//
//  1. A listener (not signed in, on a receiver with an admin password): EDIT on their OWN bookmarks, and NO EDIT / ✕ on
//     the receiver's — a write the server would refuse is never offered.
//  2. Their own bookmark: EDIT opens the form prefilled; "use what I'm listening to now" takes the dial; SAVE writes it
//     to this browser's list (name kept, frequency/mode/passband from the dial).
//  3. A rule broken in the form (low edge above high) is refused in words and nothing is saved.
//  4. Signed in as admin: EDIT appears on the receiver's rows; a LEARNED row's form says it becomes the owner's; a move
//     with a passband lands on the server (old gone, new manual); moving onto another bookmark shows the SERVER's words.
//  5. Keyboard: Enter on a focused EDIT opens the form, Enter in a field saves.
//
// ★ The page under test is the one JUST BUILT (web/dist, node scripts/build-web.mjs), served through CDP interception.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-bookmark-edit.mjs   (exit 3 = not run)
//        SHOT=/path/prefix also saves screenshots (…-own.png, …-server.png).
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

const ADMIN = "bm-edit-web-pass";
const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-bmeditw-"));
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Bookmark edit test", locator: "IO92nh",
  adminPass: ADMIN, radios: [{ serial: "bw", label: "T", enabled: true, configured: true, users: 3 }] }));
const now = Math.floor(Date.now() / 1000);
fs.writeFileSync(path.join(T, "data", "bookmarks-bw.json"), JSON.stringify([
  { frequency: 95_800_000, name: "OWNER SAVED", pi: -1, lastHeard: now, manual: true, mode: "wfm", source: "server" },
  { frequency: 97_700_000, name: "LEARNT FM", pi: 0xC777, lastHeard: now, manual: false, mode: "wfm", source: "server" },
]));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: process.env.DEBUG ? ["ignore", "inherit", "inherit"] : "ignore" });
// ★ --mute-audio: tests are SILENT.
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
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(100); } return false; };
  const mouse = (type, x, y, extra = {}) => call("Input.dispatchMouseEvent", { type, x, y, button: "none", ...extra });
  const tap = async (x, y) => { for (const type of ["mousePressed", "mouseReleased"]) await mouse(type, x, y, { button: "left", clickCount: 1 }); };
  const key = async (k, code, vk) => { for (const type of ["keyDown", "keyUp"]) await call("Input.dispatchKeyEvent", { type, key: k, code, windowsVirtualKeyCode: vk, ...(type === "keyDown" && k === "Enter" ? { text: "\r", unmodifiedText: "\r" } : {}) }); };
  await call("Runtime.enable"); await call("Page.enable");
  const DIST = path.join(ROOT, "web/dist");
  ws.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.method !== "Fetch.requestPaused") return;
    const u = new URL(m.params.request.url);
    const file = u.pathname === "/" ? path.join(DIST, "vibesdr.html") : path.join(DIST, u.pathname);
    if (u.origin !== new URL(url).origin || !(u.pathname === "/" || u.pathname.startsWith("/vs/"))
        || !file.startsWith(DIST) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      call("Fetch.continueRequest", { requestId: m.params.requestId }); return;
    }
    const type = file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html; charset=utf-8";
    const headers = (m.params.responseHeaders || [])
      .filter((h) => !/^(content-encoding|content-length|content-type|etag|last-modified|cache-control)$/i.test(h.name))
      .concat([{ name: "Content-Type", value: type }, { name: "Cache-Control", value: "no-store" }]);
    call("Fetch.fulfillRequest", { requestId: m.params.requestId, responseCode: 200, responseHeaders: headers, body: fs.readFileSync(file).toString("base64") });
  });
  await call("Fetch.enable", { patterns: ["Document", "Script", "Stylesheet"].map((resourceType) => ({ urlPattern: "*", resourceType, requestStage: "Response" })) });
  await call("Page.navigate", { url });
  return { ws, call, js, until, mouse, tap, key, errors };
}
const CARD_HZ = `(() => { const m = document.getElementById("mFreq")?.textContent || "", f = document.getElementById("mFreqFine")?.textContent || "";
  return Math.round(parseFloat(m) * 1e6 + (f ? parseInt(f, 10) * 10 : 0)); })()`;
async function start(p) {
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
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
/** The rows of the open bookmarks list: name, and which buttons it carries. */
const ROWS = `[...document.querySelectorAll("#bmList .sres")].filter((r) => r.querySelector(".n")).map((r) => ({
  name: r.querySelector(".n").textContent, edit: !!r.querySelector(".bmEditBtn"), del: [...r.querySelectorAll("button")].some((b) => b.textContent === "✕") }))`;
const clickEdit = (name) => `(() => { const r = [...document.querySelectorAll("#bmList .sres")].find((x) => x.querySelector(".n")?.textContent === ${JSON.stringify(name)});
  const b = r && r.querySelector(".bmEditBtn"); if (!b) return false; b.click(); return true; })()`;
const setField = (id, v) => `(() => { const e = document.getElementById(${JSON.stringify(id)}); e.value = ${JSON.stringify(v)}; e.dispatchEvent(new Event("input", { bubbles: true })); return true; })()`;
const shot = async (p, suffix) => {
  if (!process.env.SHOT) return;
  for (const [w, h] of [[1200, 900], [390, 844]]) {
    await p.call("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: w < 900 });
    await sleep(400);
    await p.js(`document.getElementById("bmEdit").scrollIntoView({ block: "start" }); 1`);
    const r = await p.call("Page.captureScreenshot", { format: "png" });
    const f = `${process.env.SHOT}-${suffix}-${w}.png`;
    fs.writeFileSync(f, Buffer.from(r.result.data, "base64"));
    console.log(`  (screenshot ${f})`);
  }
  await p.call("Emulation.clearDeviceMetricsOverride");
  await sleep(300);
};
const serverList = () => fetch(`http://127.0.0.1:${port}/bookmarks`, { cache: "no-store" }).then((r) => r.json());

try {
  if (!fs.existsSync(path.join(ROOT, "web/dist/vibesdr.html"))) { console.error("run node scripts/build-web.mjs first — not run"); await cleanup(); process.exit(3); }
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  console.log("editing bookmarks in the web client — a real browser, the real binary, the page just built");
  const p = await open(`http://127.0.0.1:${port}/`);
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`localStorage.setItem("lsv_user_bookmarks", JSON.stringify([{ name: "My Station", frequency: 99000000, mode: "wfm", scope: "", group: "Mine" }])); location.reload(); 1`);
  ok(await start(p), "the receiver is running");
  ok(await p.js(`!!document.getElementById("bmEdit")`), "(the page under test is the new build: the edit form exists)");
  const openList = async () => { await p.js(`document.getElementById("mBookmarks").click(); 1`); return p.until(`document.querySelectorAll("#bmList .sres .n").length >= 3`, 6000); };
  ok(await openList(), "the bookmarks panel lists yours and the receiver's");

  // ── 1. a listener ──
  let rows = await p.js(ROWS);
  const own = rows.find((r) => r.name === "My Station"), saved = rows.find((r) => r.name === "OWNER SAVED"), learnt = rows.find((r) => r.name === "LEARNT FM");
  ok(own?.edit && own?.del, "your own bookmark: EDIT and ✕");
  ok(saved && !saved.edit && !saved.del && learnt && !learnt.edit && !learnt.del, `the receiver's rows, not signed in: no EDIT, no ✕ (${JSON.stringify([saved, learnt])})`);

  // ── 2. edit your own, from the dial ──
  ok(await p.js(clickEdit("My Station")), "EDIT on your bookmark");
  ok(await p.until(`!document.getElementById("bmEdit").hidden`, 2000), "…opens the form");
  const pre = await p.js(`({ t: document.getElementById("bmEditTitle").textContent, n: document.getElementById("bmEditName").value, f: document.getElementById("bmEditFreq").value,
    m: document.getElementById("bmEditMode").value, d: document.getElementById("bmEditDefBw").checked, note: document.getElementById("bmEditNote").hidden })`);
  ok(pre.n === "My Station" && pre.f === "99" && pre.m === "wfm" && pre.d === true && pre.note === true && /YOUR/.test(pre.t), `prefilled: ${JSON.stringify(pre)}`);
  await p.js(`document.getElementById("bmEditDial").click(); 1`);
  const dialHz = await p.js(CARD_HZ);
  const fromDial = await p.js(`document.getElementById("bmEditFreq").value`);
  ok(Math.abs(parseFloat(fromDial) * 1e6 - dialHz) < 2, `"use what I'm listening to now" takes the dial (${fromDial} MHz, card ${dialHz} Hz), the name stays`);
  await p.js(setField("bmEditName", "My Station (fixed)"));
  await shot(p, "own");
  await p.js(`document.getElementById("bmEditSave").click(); 1`);
  ok(await p.until(`document.getElementById("bmEdit").hidden`, 3000), "SAVE closes the form");
  const ls = await p.js(`JSON.parse(localStorage.getItem("lsv_user_bookmarks"))`);
  ok(ls.length === 1 && ls[0].name === "My Station (fixed)" && Math.abs(ls[0].frequency - dialHz) < 2 && ls[0].group === "Mine",
     `saved in this browser, the rest of the bookmark kept: ${JSON.stringify(ls)}`);

  // ── 3. a broken rule ──
  await p.js(clickEdit("My Station (fixed)"));
  await p.js(`document.getElementById("bmEditDefBw").checked = false; document.getElementById("bmEditDefBw").dispatchEvent(new Event("change")); 1`);
  const lsBefore = await p.js(`localStorage.getItem("lsv_user_bookmarks")`);
  await p.js(setField("bmEditLo", "5000")); await p.js(setField("bmEditHi", "-5000"));
  await p.js(`document.getElementById("bmEditSave").click(); 1`);
  await sleep(300);
  const err = await p.js(`document.getElementById("bmEditMsg").textContent`);
  ok(/low edge/.test(err) && !(await p.js(`document.getElementById("bmEdit").hidden`)), `low above high is refused in words, the form stays: "${err}"`);
  ok((await p.js(`localStorage.getItem("lsv_user_bookmarks")`)) === lsBefore, "…and nothing was saved");
  await p.js(`document.getElementById("bmEditCancel").click(); 1`);

  // ── 4. signed in as admin ──
  await p.js(`(() => { const i = document.getElementById("adminPwInput"); i.value = ${JSON.stringify(ADMIN)}; document.getElementById("adminPwGo").click(); })()`);
  ok(await p.until(`(${ROWS}).find((r) => r.name === "LEARNT FM")?.edit === true`, 8000), "signed in: EDIT appears on the receiver's rows");
  ok(await p.js(clickEdit("LEARNT FM")), "EDIT on a LEARNED row");
  const note = await p.js(`document.getElementById("bmEditNote").hidden ? "" : document.getElementById("bmEditNote").textContent`);
  ok(/stops expiring/.test(note) && /will not rename/.test(note), `…which says it becomes yours: "${note}"`);
  await p.js(setField("bmEditFreq", "97.75"));
  await p.js(`document.getElementById("bmEditDefBw").checked = false; document.getElementById("bmEditDefBw").dispatchEvent(new Event("change")); 1`);
  await p.js(setField("bmEditLo", "-60000")); await p.js(setField("bmEditHi", "60000"));
  await shot(p, "server");
  await p.js(`document.getElementById("bmEditSave").click(); 1`);
  ok(await p.until(`document.getElementById("bmEdit").hidden`, 4000), "SAVE on the receiver closes the form");
  const sl = await serverList();
  const moved = sl.find((b) => b.frequency === 97_750_000);
  ok(!sl.some((b) => b.frequency === 97_700_000) && moved?.manual === true && moved?.bandwidth_low === -60000 && moved?.name === "LEARNT FM",
     `on the server: moved, the owner's now, with its passband: ${JSON.stringify(moved)}`);
  ok(await p.until(`(${ROWS}).some((r) => r.name === "LEARNT FM")`, 3000), "…and the list redraws it");
  // The server's own words for a move onto another bookmark.
  await p.js(clickEdit("LEARNT FM"));
  await p.js(setField("bmEditFreq", "95.8"));
  await p.js(`document.getElementById("bmEditSave").click(); 1`);
  ok(await p.until(`/already a bookmark/.test(document.getElementById("bmEditMsg").textContent)`, 4000),
     `moving onto another bookmark shows the server's words: "${await p.js(`document.getElementById("bmEditMsg").textContent`)}"`);
  await p.js(`document.getElementById("bmEditCancel").click(); 1`);

  // ── 5. keyboard ──
  await p.js(`(() => { const r = [...document.querySelectorAll("#bmList .sres")].find((x) => x.querySelector(".n")?.textContent === "OWNER SAVED"); r.querySelector(".bmEditBtn").focus(); })()`);
  await p.key("Enter", "Enter", 13);
  ok(await p.until(`!document.getElementById("bmEdit").hidden && document.activeElement?.id === "bmEditName"`, 2000), "Enter on a focused EDIT opens the form, focus in the name");
  await p.js(setField("bmEditName", "OWNER SAVED BY KEY"));
  ok(!(await p.js(`document.getElementById("bmEdit").hidden`)), "(the form is open before the Enter)");
  await p.key("Enter", "Enter", 13);
  ok(await p.until(`document.getElementById("bmEdit").hidden`, 4000), "Enter in the field saves");
  ok((await serverList()).some((b) => b.name === "OWNER SAVED BY KEY" && b.frequency === 95_800_000), "…on the receiver, in place");
  ok(p.errors.length === 0, "no JavaScript errors" + (p.errors.length ? ": " + p.errors.slice(0, 2).join(" | ") : ""));
  p.ws.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : "\n\x1b[32mall passed\x1b[0m");
process.exit(fails ? 1 : 0);
