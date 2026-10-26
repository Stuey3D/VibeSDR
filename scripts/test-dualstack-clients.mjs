#!/usr/bin/env node
// ★★★ AN IPv4 CLIENT AND AN IPv6 CLIENT, ONE SERVER (2026-10-25). Nick (G4IRX), a real Orange Pi One:
//     "vibeserversetup.local resolves to a link local ipv6, then a public ipv6, but no ipv4 … 'site not found'" — the
//     server listened on IPv4 only. net::listen is dual-stack now; this runs the REAL binary and proves, end to end:
//       · the front door answers on 127.0.0.1 AND [::1] (and on this host's own non-loopback IPv6, if it has one);
//       · /vibeserver.json classifies each client: loopback → local + lan; this host's own ULA/global IPv6 → lan,
//         not local; the host's own LAN IPv4 → lan, not local (exactly as before);
//       · the admin connection log records the IPv4 client as "127.0.0.1" (never "::ffff:127.0.0.1") and the IPv6
//         client as "::1".
// Silent: no browser, no audio. Usage: VIBESERVER_BIN=<build>/vibeserver node scripts/test-dualstack-clients.mjs
import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BIN = process.env.VIBESERVER_BIN || path.join(ROOT, "vibeserver/build/vibeserver");
if (!fs.existsSync(BIN)) { console.log(`test-dualstack-clients: not run — no server binary at ${BIN}`); process.exit(3); }
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
const ok = (c, what, extra = "") => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}${c ? "" : "  — " + extra}`); if (!c) fails++; };

// ★ Short: the radio's hand-over socket path must fit in sun_path (104 bytes on macOS).
const T = fs.mkdtempSync(path.join(os.tmpdir(), "vds-"));
const ADMIN = "dualstack-admin-pass";
fs.writeFileSync(path.join(T, "config.json"), JSON.stringify({ configured: true, name: "Dual-stack test", locator: "IO92nh",
  adminPass: ADMIN, radios: [{ serial: "ds", label: "T", enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
await sleep(500);
const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"),
         VIBESERVER_RUNTIME_DIR: path.join(T, "run"), HOME: T }, stdio: ["ignore", "pipe", "pipe"] });
let log = ""; srv.stdout.on("data", (d) => { log += d; }); srv.stderr.on("data", (d) => { log += d; });
const done = (code) => { try { srv.kill("SIGTERM"); } catch {} try { rtl.kill("SIGTERM"); } catch {} fs.rmSync(T, { recursive: true, force: true }); process.exit(code); };
process.on("SIGINT", () => done(1));

const getJson = async (base, p) => { try { const r = await fetch(`${base}${p}`, { signal: AbortSignal.timeout(4000) }); return r.ok ? await r.json() : null; } catch { return null; } };
let up = false;
for (let i = 0; i < 60 && !up; i++) { await sleep(500); up = !!(await getJson(`http://127.0.0.1:${port}`, "/vibeserver.json")); }
ok(up, "the server answers on 127.0.0.1");
if (!up) { console.log(log.slice(-2000)); done(1); }

// ── which addresses this host has ──
const ifs = Object.values(os.networkInterfaces()).flat().filter(Boolean);
const own6 = ifs.find((a) => a.family === "IPv6" && !a.internal && !a.address.startsWith("fe80"))?.address || "";
const own4 = ifs.find((a) => a.family === "IPv4" && !a.internal)?.address || "";
const have6 = await new Promise((res) => { const s = net.createServer(); s.once("error", () => res(false)); s.listen(0, "::1", () => s.close(() => res(true))); });

const cases = [
  { base: `http://127.0.0.1:${port}`, want: { local: true, lan: true }, what: "IPv4 loopback" },
  ...(have6 ? [{ base: `http://[::1]:${port}`, want: { local: true, lan: true }, what: "★ IPv6 loopback [::1] (the listener was IPv4-only before)" }] : []),
  ...(own6 ? [{ base: `http://[${own6}]:${port}`, want: { local: false, lan: true }, what: `★ this host's own IPv6 ${own6} — the LAN, not the host's loopback` }] : []),
  ...(own4 ? [{ base: `http://${own4}:${port}`, want: { local: false, lan: true }, what: `this host's own IPv4 ${own4} — as before` }] : []),
];
if (!have6) console.log("  --   this host has no IPv6 loopback: the IPv6 half is skipped");
for (const c of cases) {
  const j = await getJson(c.base, "/vibeserver.json");
  ok(!!j, `${c.what}: the front door answers`);
  if (j) ok(j.local === c.want.local && j.lan === c.want.lan, `${c.what}: local=${c.want.local} lan=${c.want.lan}`, `got local=${j.local} lan=${j.lan}`);
}

// ── the connection log: a spectrum socket from each family ──
const openWs = (host) => new Promise((res) => {
  const sid = crypto.randomBytes(8).toString("hex");
  const ws = new WebSocket(`ws://${host}:${port}/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  const t = setTimeout(() => { try { ws.close(); } catch {} res(false); }, 5000);
  ws.onmessage = () => { clearTimeout(t); setTimeout(() => { ws.close(); res(true); }, 300); };
  ws.onerror = () => { clearTimeout(t); res(false); };
});
ok(await openWs("127.0.0.1"), "an IPv4 spectrum socket opens");
if (have6) ok(await openWs("[::1]"), "an IPv6 spectrum socket opens");
await sleep(1500);
const auth = await getJson(`http://127.0.0.1:${port}`, "/vibeserver/auth");
const nonce = auth?.nonce || "";
ok(!!nonce, "an admin challenge is offered");
const tok = crypto.createHmac("sha256", ADMIN).update(nonce).digest("hex");
const conns = await getJson(`http://127.0.0.1:${port}`, `/vibeserver/admin/connections?vs_admin_nonce=${nonce}&vs_admin_auth=${tok}`);
const rows = (conns && (conns.connections || conns.rows || conns)) || [];
const addrs = (Array.isArray(rows) ? rows : []).map((r) => r.ip || r.addr || r.address || "");
ok(Array.isArray(rows) && rows.length > 0, "the admin connection log is readable", JSON.stringify(conns).slice(0, 300));
ok(addrs.includes("127.0.0.1"), "★ the IPv4 client is logged as \"127.0.0.1\"", addrs.join(","));
ok(!addrs.some((a) => a.startsWith("::ffff:")), "★ no address is logged in its ::ffff: form", addrs.join(","));
if (have6) ok(addrs.includes("::1"), "the IPv6 client is logged as \"::1\"", addrs.join(","));

console.log(`test-dualstack-clients: ${fails ? fails + " FAILED" : "PASS"}`);
done(fails ? 1 : 0);
