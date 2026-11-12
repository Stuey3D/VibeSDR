#!/usr/bin/env node
// test-web-health-tuck.mjs — V12 (2026-11-12), two things on the served web page, in a real (headless, MUTED) browser
// against the real vibeserver and a fake radio:
//
//  1. THE OWNER'S MESSAGE WITH **bold** AND *italic* (src/utils/landingMarkup.ts): the landing screen draws <b>/<i> for
//     the markers, and nothing else — a <script>, an <img onerror> inside bold, and a typed </b> arrive as TEXT, no
//     element is created and nothing runs; an unclosed marker is shown as typed; the line breaks stay.
//  2. THE SERVER HEALTH PILL TUCKS INTO THE EDGE (healthTuck.ts, the app's EdgeChip): the › tucks it to a tab in its
//     health colour, the tab brings it back; a mouse drag of > 50 px tucks it, a short one springs back; it tucks
//     itself while the decoder box is open (not while minimised); a CRITICAL report brings it out over the box and
//     puts it back on auto; at phone width (390 px) the tab and the pill stay on screen and nothing scrolls sideways;
//     with transparency off and with the silver / black fonts the pill and tab still draw. No exception on the page.
//
// ★ The page under test is the one JUST BUILT (web/dist) — served through CDP interception in place of the binary's own,
//   as test-web-bookmark-tooltip.mjs does — so no vibeserver rebuild is needed for a web change.
// ★ SILENT: --mute-audio and --disable-audio-output.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-health-tuck.mjs   (exit 3 = not run)
//        VIBE_SHOT_DIR=dir saves the screenshots there (default: a temp dir, printed).
import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const BIN = process.env.VIBESERVER_BIN;
if (!BIN || !fs.existsSync(BIN)) { console.error("set VIBESERVER_BIN to a built vibeserver — not run"); process.exit(3); }
const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
if (!fs.existsSync(BROWSER)) { console.error("no Edge — not run"); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) fails++; };

const MSG = "Welcome! **Please retune** before you leave.\n"
  + "*The waterfall slows at night* and ***that is normal***.\n"
  + "<script>window.__pwned=1</script> **<img src=x onerror=\"window.__pwned=2\">** x</b><i>\n"
  + "Unclosed **marker and 5 * 3 = 15";

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-htuck-"));
const SHOTS = process.env.VIBE_SHOT_DIR || fs.mkdtempSync(path.join(os.tmpdir(), "vs-htuck-shots-"));
fs.mkdirSync(SHOTS, { recursive: true });
for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Health tuck test", locator: "IO92nh",
  adminPass: "ht-admin-pass", landingMessage: MSG, landingLinkUrl: "https://example.com/", landingLinkLabel: "Support this server",
  radios: [{ serial: "ht", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(400);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"), VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T },
  stdio: process.env.DEBUG ? ["ignore", "inherit", "inherit"] : "ignore" });
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output",
  "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,BlockInsecurePrivateNetworkRequests",
  "--no-first-run", "--disable-extensions", "--autoplay-policy=no-user-gesture-required",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1280,800", "about:blank"], { stdio: "ignore" });

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
  const shot = async (name) => { const r = await call("Page.captureScreenshot", { format: "png" }); const p = path.join(SHOTS, name); fs.writeFileSync(p, Buffer.from(r.result.data, "base64")); return p; };
  await call("Runtime.enable"); await call("Page.enable");
  // ★ Keep every WebSocket the page opens, so the test can hand the spectrum socket a CRITICAL health report.
  await call("Page.addScriptToEvaluateOnNewDocument", { source: `(() => { const W = window.WebSocket; window.__wsAll = [];
    window.WebSocket = function (...a) { const s = new W(...a); window.__wsAll.push(s); return s; };
    window.WebSocket.prototype = W.prototype; Object.assign(window.WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 }); })();` });
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
  return { ws, call, js, until, mouse, tap, shot, errors };
}

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
  return p.until(`parseFloat(document.getElementById("freqInput").value) > 0`);
}

const TUCKED = `document.getElementById("srvHealth")?.classList.contains("tucked")`;
const STATE = `JSON.stringify((() => { const d = document.getElementById("srvHealth"), c = d.querySelector(".hCard").getBoundingClientRect(),
  t = d.querySelector(".hTab").getBoundingClientRect();
  return { tucked: d.classList.contains("tucked"), pref: d.dataset.pref, crit: d.classList.contains("crit"), vw: innerWidth,
           card: { l: Math.round(c.left), r: Math.round(c.right), t: Math.round(c.top), w: Math.round(c.width) },
           tab: { l: Math.round(t.left), r: Math.round(t.right), t: Math.round(t.top), h: Math.round(t.height) },
           hc: getComputedStyle(d).getPropertyValue("--hc").trim(), tabBorder: getComputedStyle(d.querySelector(".hTab")).borderTopColor,
           scrollW: document.documentElement.scrollWidth }; })())`;
const st = async (p) => JSON.parse(await p.js(STATE));
const settle = () => sleep(450);   // the slide is 260 ms
const injectHealth = (p, cpu) => p.js(`(() => { const s = (window.__wsAll || []).filter((w) => w.readyState === 1 && typeof w.onmessage === "function");
  for (const w of s) w.onmessage(new MessageEvent("message", { data: JSON.stringify({ type: "health", cpu: ${cpu}, ram: 0, cpuPos: ${cpu}.2,
    temp: { kind: "none", level: 0 }, bat: { present: false } }) }));
  return s.length; })()`);
const centreOf = async (p, sel) => p.js(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) }; })()`);
async function drag(p, from, dx, steps = 8) {
  await p.mouse("mouseMoved", from.x, from.y);
  await p.mouse("mousePressed", from.x, from.y, { button: "left", buttons: 1, clickCount: 1 });
  for (let i = 1; i <= steps; i++) { await p.mouse("mouseMoved", Math.round(from.x + dx * i / steps), from.y, { button: "left", buttons: 1 }); await sleep(16); }
  await p.mouse("mouseReleased", Math.round(from.x + dx), from.y, { button: "left", buttons: 0, clickCount: 1 });
}

try {
  if (!fs.existsSync(path.join(ROOT, "web/dist/vibesdr.html"))) { console.error("run node scripts/build-web.mjs first — not run"); await cleanup(); process.exit(3); }
  for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  const H = `http://127.0.0.1:${port}/`;
  const p = await open(H);

  // ── 1. The owner's message ─────────────────────────────────────────────────────────────────────
  console.log("the owner's message: **bold** and *italic*, nothing else");
  ok(await p.until(`(document.getElementById("splashOwnerMsg")?.textContent || "").includes("Welcome")`, 15000), "the landing screen shows the message");
  const m = JSON.parse(await p.js(`JSON.stringify((() => { const h = document.getElementById("splashOwnerMsg"), d = h.querySelector("div");
    return { html: d.innerHTML, text: d.textContent, tags: [...d.querySelectorAll("*")].map((e) => e.tagName.toLowerCase()),
             b: [...d.querySelectorAll("b")].map((e) => e.textContent), i: [...d.querySelectorAll("i")].map((e) => e.textContent),
             ws: getComputedStyle(d).whiteSpace, pwned: window.__pwned ?? null,
             link: h.querySelector("a")?.getAttribute("href") }; })())`));
  ok(m.b.includes("Please retune"), "**Please retune** is bold");
  ok(m.i.includes("The waterfall slows at night"), "*The waterfall slows at night* is italic");
  ok(m.b.includes("that is normal") && m.i.includes("that is normal"), "***that is normal*** is both");
  ok(m.tags.every((t) => t === "b" || t === "i"), `the only elements are <b> and <i> (${[...new Set(m.tags)].join(", ")})`);
  ok(m.text.includes("<script>window.__pwned=1</script>"), "a <script> is shown as text");
  ok(m.b.includes('<img src=x onerror="window.__pwned=2">'), "**<img onerror>** is bold TEXT, not an image");
  ok(m.text.includes("x</b><i>"), "a typed </b><i> is text and closes nothing");
  ok(m.pwned === null, "nothing ran");
  ok(m.text.includes("Unclosed **marker and 5 * 3 = 15"), "an unclosed marker and a lone * are shown as typed");
  ok(m.ws === "pre-wrap", "drawn pre-wrap, so a line break the server sends is kept");
  ok(m.text.includes("before you leave.\n"), "the line break reaches the page (the server sends it as \\n, not a space)");
  // ★ FINDING (2026-11-12): the server's /vibeserver.json drops control characters from the message (the shim's
  //   jsonEscape keeps only >= 0x20), so a typed line break never reaches a client today. The tokenizer's line rules are
  //   covered by scripts/test_landing_markup.ts; this only records what arrives.
  console.log(`   (line breaks arriving from the server: ${m.text.split("\n").length - 1})`);
  ok(m.link === "https://example.com/", "the link is still its own box");
  await sleep(300);
  console.log(`   shot ${await p.shot("Web-description-bold-italic.png")}`);

  // ── 2. The health pill ─────────────────────────────────────────────────────────────────────────
  console.log("the server health pill tucks into the edge");
  ok(await start(p), "the receiver is running");
  ok(await p.until(`!!document.querySelector("#srvHealth .hCard .hCap")`, 15000), "the health pill is drawn (the server's first report)");
  await settle();
  let s = await st(p);
  ok(!s.tucked && s.pref === "auto", `out on arrival (${s.pref})`);
  ok(s.card.r === s.vw, `joined to the screen edge (card right ${s.card.r} = ${s.vw})`);
  ok(s.tab.l >= s.vw, "its tab waits past the edge");
  console.log(`   shot ${await p.shot("health-out-desktop.png")}`);

  const arrow = await centreOf(p, "#srvHealth .hTuck");
  await p.tap(arrow.x, arrow.y); await settle();
  s = await st(p);
  ok(s.tucked && s.pref === "tucked", "the › tucks it");
  ok(s.card.l >= s.vw, `the card is off the screen (left ${s.card.l})`);
  ok(s.tab.r === s.vw && s.tab.l < s.vw && s.tab.h === 52, `the tab is on the edge (${s.tab.l}–${s.tab.r}, ${s.tab.h} px tall)`);
  ok(s.hc && s.tabBorder && s.tabBorder !== "rgba(0, 0, 0, 0)", `the tab wears the health colour (${s.hc} / ${s.tabBorder})`);
  console.log(`   shot ${await p.shot("Web-health-pill-tucked.png")}`);

  const tab = await centreOf(p, "#srvHealth .hTab");
  await p.tap(tab.x, tab.y); await settle();
  s = await st(p);
  ok(!s.tucked && s.pref === "shown", "clicking the tab brings it back");

  // A short drag springs back; a long one tucks.
  const body = await centreOf(p, "#srvHealth .hBody");
  await drag(p, body, 24); await settle();
  ok(!(await p.js(TUCKED)), "a 24 px drag springs back");
  ok(await p.js(`document.querySelector("#srvHealth .hCard").style.transform === ""`), "…and leaves no transform behind");
  await drag(p, body, 90); await settle();
  s = await st(p);
  ok(s.tucked && s.pref === "tucked", "a 90 px drag to the right tucks it");
  await p.tap(tab.x, tab.y); await settle();
  ok(!(await p.js(TUCKED)), "and the tab brings it back again");

  // The decoder box (the class the page's own showDecBox / hideDecBox / minimise set).
  await p.js(`document.getElementById("decBox").classList.add("open"); 1`); await settle();
  s = await st(p);
  ok(s.tucked && s.pref === "auto", `a decoder box opening tucks it, and a 'shown' pill goes back on auto (${s.pref})`);
  await p.js(`document.getElementById("decBox").classList.add("min"); 1`); await settle();
  ok(!(await p.js(TUCKED)), "minimise the box: it comes back out");
  await p.js(`document.getElementById("decBox").classList.remove("min"); 1`); await settle();
  ok(await p.js(TUCKED), "restore the box: it tucks again");

  // CRITICAL brings it out over the box, and even from the listener's own 'tucked'.
  const n = await injectHealth(p, 3); await settle();
  s = await st(p);
  ok(n > 0, `(handed the page a CRITICAL report on ${n} socket(s))`);
  ok(!s.tucked && s.crit && s.pref === "auto", "critical: it comes out over the open box");
  console.log(`   shot ${await p.shot("health-critical-over-box.png")}`);
  await injectHealth(p, 0); await settle();
  ok(await p.js(TUCKED), "back to OK with the box still open: it tucks again (auto)");
  await p.js(`document.getElementById("decBox").classList.remove("open"); 1`); await settle();
  ok(!(await p.js(TUCKED)), "close the box: it is out");
  const arrow2 = await centreOf(p, "#srvHealth .hTuck");
  await p.tap(arrow2.x, arrow2.y); await settle();
  ok(await p.js(TUCKED), "the listener tucks it with no box open");
  await injectHealth(p, 3); await settle();
  s = await st(p);
  ok(!s.tucked && s.pref === "auto", "critical brings a tucked pill out and puts it back on auto");
  await injectHealth(p, 0); await settle();
  ok(!(await p.js(TUCKED)), "…and it stays out when the server recovers (no box open)");

  // ── Phone width, and the looks ─────────────────────────────────────────────────────────────────
  const looks = [
    ["default", {}], ["silver", { ctlFont: "#c8c8c8" }], ["black", { ctlFont: "#111111", ctlBg: "#eef3ff" }], ["solid", { ctlSolid: 100 }],
  ];
  for (const [w, h, tag] of [[390, 844, "phone"], [1280, 800, "desktop"]]) {
    for (const [name, look] of looks) {
      await p.call("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: tag === "phone" ? 3 : 1, mobile: tag === "phone" });
      await p.js(`localStorage.setItem("vibesdr_web_prefs_v1", JSON.stringify({ ...JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}"),
        ctlFont: "", ctlBg: "", ctlSolid: 0, ...${JSON.stringify(look)} })); location.reload(); 1`);
      await sleep(1200);
      await start(p);
      await p.until(`!!document.querySelector("#srvHealth .hCard .hCap")`, 15000); await settle();
      s = await st(p);
      const outOk = !s.tucked && s.card.r === s.vw && s.card.l >= 0 && s.scrollW <= s.vw;
      await p.shot(`health-${tag}-${name}-out.png`);
      const a = await centreOf(p, "#srvHealth .hTuck"); await p.tap(a.x, a.y); await settle();
      const s2 = await st(p);
      await p.shot(`health-${tag}-${name}-tucked.png`);
      const tuckOk = s2.tucked && s2.tab.r === s2.vw && s2.tab.l > s2.vw - 40 && s2.card.l >= s2.vw && s2.scrollW <= s2.vw;
      ok(outOk && tuckOk, `${tag} ${w} px, ${name}: out on the edge, tucks to an on-screen tab, no sideways scroll`
        + (outOk && tuckOk ? "" : ` ${JSON.stringify({ s, s2 })}`));
    }
  }
  await p.call("Emulation.clearDeviceMetricsOverride");
  ok(p.errors.length === 0, `no exception on the page${p.errors.length ? `: ${p.errors.slice(0, 3).join(" | ")}` : ""}`);
  console.log(`   screenshots in ${SHOTS}`);
} catch (e) {
  console.log("  FAIL threw:", e?.stack || e); fails++;
} finally {
  await cleanup();
}
console.log(fails ? `${fails} FAILED` : "all passed");
process.exit(fails ? 1 : 0);
