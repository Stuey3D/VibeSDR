#!/usr/bin/env node
// test-web-spots-nav.mjs — ★★★ beta92 THE WEB PAGE'S SPOTS LIST BY PAD / KEYBOARD (Stuart, 2026-11-23: "make the spots
// navigatable by controller but make it so that you dont have to scroll a list of hundreds to get to the header buttons
// or to the control deck"). The app's half is scripts/test_spots_nav.ts.
//
// The REAL receiver page's markup and CSS (web/client/index.html, its scripts left out) with the REAL gamepad.ts and
// padList.ts bundled in, in headless MUTED Edge — no server: the rows are built with padListRow / padListRebuild exactly
// as main.ts renderSpotsRows builds them, 500 of them. Keys are real (CDP Input), the pad is simulated (as
// test-web-pad-keyboard.mjs). Checked:
//   • ENTER on the decoder box lands on the TOP row; ↓ walks the rows; the highlighted row is scrolled into view;
//   • a burst of 25 new spots (rows PREPENDED, the list rebuilt) leaves the highlight on its spot;
//   • ENTER tunes the highlighted spot (the row's click);
//   • BACK from row 400 → the header's first key in ONE press; BACK again → the box lets go, the box highlighted;
//   • ↑ from row 0 → the header; ↓ from the header → the TOP row even with the list scrolled deep;
//   • the QRZ link is not a second stop; a PAD's ✕ / ○ do the same as Enter / Backspace.
//
// Usage: node scripts/test-web-spots-nav.mjs   (EDGE=… to name the browser; exit 3 = not run: no browser)
import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import * as esbuild from "esbuild";

const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BROWSER = process.env.EDGE || "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge";
if (!fs.existsSync(BROWSER)) { console.error("no Edge at " + BROWSER + " — not run"); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) fails++; };
console.log("test-web-spots-nav");

const T = fs.mkdtempSync(path.join(os.tmpdir(), "vs-spotsnav-"));
// ── The page: index.html's markup + CSS, its scripts out, the two real modules in ─────────────────────────────────
const entry = path.join(T, "entry.ts");
fs.writeFileSync(entry, `
  import { initGamepad } from ${JSON.stringify(path.join(ROOT, "web/client/src/gamepad.ts"))};
  import { padListRow, padListRebuild } from ${JSON.stringify(path.join(ROOT, "web/client/src/padList.ts"))};
  (window as any).__pad = { id: 'Test pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  (window as any).__padOn = () => { (navigator as any).getGamepads = () => [(window as any).__pad, null, null, null];
    window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: (window as any).__pad })); };
  (window as any).__btn = (i: number, on: boolean) => { (window as any).__pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
  (navigator as any).getGamepads = () => [null, null, null, null];
  initGamepad({ tune() {}, zoom() {}, back() { return false; }, guide() {} });
  (window as any).__tuned = null;
  /** The list, as main.ts renderSpotsRows draws it: newest first, a QRZ link in each, one pad stop per row. */
  (window as any).__draw = (spots: Array<{ t: number; call: string; f: number }>) => {
    const host = document.getElementById('spotList')!;
    padListRebuild(host, () => {
      host.innerHTML = '';
      for (const sp of spots) {
        const row = document.createElement('div');
        row.className = 'sres spot';
        row.dataset.ra = 'sp-' + sp.call + '|20m';
        row.innerHTML = '<span class="t">12:00</span><span class="mode">FT8</span><span class="band">20m</span>'
          + '<span class="snr pos">5</span><span class="call"><a class="qrz" href="https://www.qrz.com/db/' + sp.call + '">' + sp.call
          + '</a></span><span class="cty">ENG</span><span class="km">100km</span>';
        padListRow(row);
        row.onclick = () => { (window as any).__tuned = sp.f; };
        host.appendChild(row);
      }
    });
  };
`);
const built = await esbuild.build({ entryPoints: [entry], bundle: true, write: false, format: "iife", platform: "browser", target: "es2020", logLevel: "silent" })
  .catch((e) => { console.error("does not build:\n", e.message); process.exit(1); });
let html = fs.readFileSync(path.join(ROOT, "web/client/index.html"), "utf8").replace(/<script\b[\s\S]*?<\/script>/g, "");
html += `<script>${built.outputFiles[0].text.replace(/<\/script/g, "<\\/script")}</script>
<script>
  // Only the decoder box on the page (the rest of the receiver needs main.ts): it is then the one stop outside itself.
  document.body.appendChild(document.getElementById('decBox'));
  for (const el of document.body.children) if (el.id !== 'decBox' && el.tagName !== 'SCRIPT') el.style.display = 'none';
  document.getElementById('decBox').classList.add('open');
  document.getElementById('spotList').classList.add('on');
</script>`;   // ★ index.html has no </body> — the browser closes it
fs.writeFileSync(path.join(T, "page.html"), html);

const cdp = await freePort();
const br = spawn(BROWSER, ["--headless=new", "--mute-audio", "--disable-audio-output", "--no-first-run", "--disable-extensions",
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, "browser")}`, "--window-size=1200,900", "about:blank"], { stdio: "ignore" });
async function cleanup() { try { br.kill(); } catch {} await sleep(400); fs.rmSync(T, { recursive: true, force: true }); }

try {
  let target;
  for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === "page"); } catch { await sleep(250); } }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call("Runtime.enable"); await call("Page.enable");
  await call("Page.navigate", { url: "file://" + path.join(T, "page.html") });
  for (let i = 0; i < 40 && !(await js("typeof window.__draw === 'function'")); i++) await sleep(150);
  const VK = { ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Backspace: 8 };
  const key = async (k, wait = 0) => {
    const text = k === "Enter" ? "\r" : undefined;
    await call("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key: k, windowsVirtualKeyCode: VK[k], text, unmodifiedText: text });
    await call("Input.dispatchKeyEvent", { type: "keyUp", key: k, windowsVirtualKeyCode: VK[k] });
    if (wait) await sleep(wait);
  };
  const lit = () => js(`(() => { const e = document.querySelector('.gpFocus'); if (!e) return null;
    const r = e.closest('#spotList > .spot');
    return { id: e.id, ra: r ? r.dataset.ra : null, idx: r ? [...r.parentElement.children].indexOf(r) : -1,
             head: !!e.closest('#decBoxHead'), firstHead: e === [...document.querySelectorAll('#decBoxHead button')].find((b) => b.offsetParent),
             grabbed: document.getElementById('decBox').classList.contains('gpGrab'), qrz: !!e.closest('a.qrz') }; })()`);

  // 500 spots, newest first.
  const spots = Array.from({ length: 500 }, (_, i) => ({ t: 500 - i, call: `G${i}ABC`, f: 14074000 + i }));
  await js(`window.__draw(${JSON.stringify(spots)})`);
  ok((await js("document.querySelectorAll('#spotList > .spot').length")) === 500, "500 rows drawn");

  await key("ArrowDown", 60);
  let h = await lit();
  ok(h?.id === "decBox", "the decoder box is ONE stop (highlighted)");
  await key("Enter", 60);
  h = await lit();
  ok(h?.idx === 0 && h.grabbed, `ENTER on the box → the TOP row (${JSON.stringify(h)})`);
  for (let i = 0; i < 400; i++) await key("ArrowDown");
  await sleep(80);
  h = await lit();
  ok(h?.idx === 400 && h.ra === "sp-G400ABC|20m" && !h.qrz, `↓ ×400 → row 400, one stop per row (the QRZ link is not one) (${h?.idx})`);
  ok(await js(`(() => { const r = document.querySelector('.gpFocus').getBoundingClientRect(), b = document.getElementById('decBody').getBoundingClientRect();
    return r.top >= b.top - 1 && r.bottom <= b.bottom + 1; })()`), "the highlighted row is scrolled into view");

  // New spots: a burst of 25, prepended (newest first), the list rebuilt.
  const burst = Array.from({ length: 25 }, (_, i) => ({ t: 600 - i, call: `M${i}NEW`, f: 7074000 + i }));
  await js(`window.__draw(${JSON.stringify([...burst, ...spots])})`);
  h = await lit();
  ok(h?.ra === "sp-G400ABC|20m" && h.idx === 425, `a 25-spot burst: the highlight stays on its spot (now row ${h?.idx})`);
  await key("Enter", 60);
  ok((await js("window.__tuned")) === 14074400, "ENTER tunes the highlighted spot (G400ABC, 14.074400 MHz)");
  await key("ArrowDown", 30);
  h = await lit();
  ok(h?.ra === "sp-G401ABC|20m", "↓ after the burst carries on from the same spot");

  await key("Backspace", 60);
  h = await lit();
  ok(h?.head && h.firstHead && h.grabbed, `BACK from deep in the list → the header's first key in ONE press (${JSON.stringify(h)})`);
  await key("ArrowDown", 60);
  h = await lit();
  ok(h?.idx === 0, `↓ from the header → the TOP row, not where the list was scrolled (${h?.idx})`);
  await key("ArrowUp", 60);
  h = await lit();
  ok(h?.head && h.firstHead, "↑ from the first row → the header");
  await key("ArrowUp", 60);
  h = await lit();
  ok(h?.head || h?.id === "decBox" || h?.idx === -1, "↑ in the header never wraps to the bottom row");
  await key("Backspace", 60);
  h = await lit();
  ok(h?.id === "decBox" && !h.grabbed, "BACK from the header → the box lets go, the box highlighted (the page and the deck next)");

  // A PAD: ✕ (bottom) = enter, ○ (right) = back, the D-pad moves.
  await js("window.__padOn()");
  const btn = async (i) => { await js(`window.__btn(${i}, true)`); await sleep(70); await js(`window.__btn(${i}, false)`); await sleep(70); };
  await btn(0);
  h = await lit();
  ok(h?.idx === 0 && h.grabbed, "pad ✕ on the box → the top row");
  for (let i = 0; i < 30; i++) await btn(13);
  h = await lit();
  ok(h?.idx === 30, `pad D-pad ↓ ×30 → row 30 (${h?.idx})`);
  await btn(1);
  h = await lit();
  ok(h?.head && h.grabbed, "pad ○ from row 30 → the header");
  await btn(1);
  h = await lit();
  ok(h?.id === "decBox" && !h.grabbed, "pad ○ again → the box lets go");
  ok(errors.length === 0, `no exception on the page${errors.length ? ": " + errors.join(" | ") : ""}`);
} finally {
  await cleanup();
}
console.log(fails ? `✗ test-web-spots-nav: ${fails} failed` : "✓ test-web-spots-nav");
process.exit(fails ? 1 : 0);
