#!/usr/bin/env node
// ★★★ ONE .local NAME ON A PORTABLE (Stuart, 2026-10-25: "realistically the only .local address open after setup should
// be the one the user sets"). On a Portable the server runs NO mDNS responder of its own (ours answers IPv4 only, and a
// second responder for the box's name is a conflict): it hands the name it would advertise to the root service, which
// makes it the hostname that avahi publishes. This runs the REAL binary with a pocket marker and proves:
//   · set up, named "Nick's SDR" → pocket-mdns says {"label":"nick-s-sdr","port":<its port>,"pin":false};
//   · the server holds NO UDP 5353 socket (no responder of its own on a Portable);
//   · not set up → {"label":""} (nothing to publish but the setup name, which the root service owns);
//   · and WITHOUT the marker (any other install) nothing is written and the server's own responder runs as before.
// Silent. Usage: VIBESERVER_BIN=<build>/vibeserver node scripts/test-pocket-mdns.mjs   (exit 3 = not run)
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const ROOT = path.resolve(new URL("..", import.meta.url).pathname);
const BIN = process.env.VIBESERVER_BIN || path.join(ROOT, "vibeserver/build/vibeserver");
if (!fs.existsSync(BIN)) { console.log(`test-pocket-mdns: not run — no server binary at ${BIN}`); process.exit(3); }
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
const ok = (c, what, extra = "") => { console.log(`  ${c ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${what}${c ? "" : "  — " + extra}`); if (!c) fails++; };

const udp5353 = (pid) => {
  try { return execFileSync("lsof", ["-nP", "-a", "-p", String(pid), "-iUDP:5353"], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim().length > 0; }
  catch { return false; }                     // lsof exits 1 when nothing matches
};

async function run(cfg, marker) {
  const T = fs.mkdtempSync(path.join(os.tmpdir(), "vpm-"));
  const PD = path.join(T, "p"); fs.mkdirSync(PD);
  for (const d of ["data", "run"]) fs.mkdirSync(path.join(T, d));
  if (marker) fs.writeFileSync(path.join(PD, "enabled"), "1\n");
  fs.writeFileSync(path.join(PD, "state.json"), JSON.stringify({ v: 1, mode: "wired", wired: true, hasWifi: false }) + "\n");
  // ★ cfg null = a NEW Portable: no settings at all — the server makes its own first-boot file (configured false).
  if (cfg) fs.writeFileSync(path.join(T, "config.json"), JSON.stringify(Object.assign({ locator: "IO92nh", adminPass: "pocket-mdns-admin",
    radios: [{ serial: "pm", label: "T", enabled: true, configured: true, users: 3 }] }, cfg)));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(ROOT, "vibeserver/fake-rtl-tcp.mjs"), "--port", String(rtlPort), "--rate", "2048000", "--wfm", "96.1"], { stdio: "ignore" });
  await sleep(400);
  const srv = spawn(BIN, ["--tcp", `127.0.0.1:${rtlPort}`, "--port", String(port), "--rate", "2048000", "--freq", "96100000", "--mode", "wfm", "--users", "3"], {
    env: { ...process.env, VIBESERVER_CONFIG: path.join(T, "config.json"), VIBESERVER_DATA_DIR: path.join(T, "data"),
           VIBESERVER_RUNTIME_DIR: path.join(T, "run"), VIBESERVER_POCKET_DIR: PD, HOME: T },
    stdio: ["ignore", fs.openSync(path.join(T, "out.log"), "w"), "ignore"] });
  let up = false;
  for (let i = 0; i < 60 && !up; i++) {
    await sleep(500);
    try { up = (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(2000) })).ok; } catch {}
  }
  await sleep(1500);
  let doc = null;
  try { doc = JSON.parse(fs.readFileSync(path.join(PD, "pocket-mdns"), "utf8")); } catch {}
  const has5353 = udp5353(srv.pid);
  const out = fs.readFileSync(path.join(T, "out.log"), "utf8");
  srv.kill("SIGTERM"); rtl.kill("SIGTERM");
  await sleep(500);
  fs.rmSync(T, { recursive: true, force: true });
  return { up, doc, has5353, port, out };
}

console.log("a Portable publishes one .local name, through avahi");
// ★ The setup page saves the name the owner typed as both name and mdnsName (vibe_setup_page.h: mdnsName: $("name")).
let r = await run({ configured: true, name: "Nick's SDR", mdnsName: "Nick's SDR" }, true);
ok(r.up, "set up, named \"Nick's SDR\": the server answers");
ok(r.doc && r.doc.label === "nick-s-sdr" && (r.doc.port === r.port || r.doc.port === 0) && r.doc.pin === false,
   "★ it hands the root service {label: \"nick-s-sdr\", its port (0 = the front door's), pin false}", JSON.stringify(r.doc));
ok(!r.has5353 && !/advertising as/.test(r.out), "★ and runs NO mDNS responder of its own (no UDP 5353 socket) — avahi is the one responder");
r = await run(null, true);
ok(r.doc && r.doc.label === "", "a NEW Portable (not set up): an empty label (only the setup name, which the root service owns)", JSON.stringify(r.doc) + " " + r.out.split("\n").filter((l) => /pocket|configured|setup/i.test(l)).slice(0, 6).join(" | "));
ok(!r.has5353, "…and no responder of its own either");
r = await run({ configured: true, name: "Nick's SDR", mdnsName: "Nick's SDR", mdnsAdvertise: false }, true);
ok(r.doc && r.doc.label === "", "set up with mDNS switched off: an empty label (nothing will be published)", JSON.stringify(r.doc));
r = await run({ configured: true, name: "Desk SDR", mdnsName: "Desk SDR" }, false);
ok(r.doc === null, "not a Portable (no marker): nothing written");
ok(/advertising as desk-sdr\.local|desk-sdr\.local will advertise once the front door has a port/.test(r.out), "…and the server's own responder runs exactly as before", r.out.split("\n").filter((l) => /mDNS|\.local/.test(l)).join(" | "));

console.log(`test-pocket-mdns: ${fails ? fails + " FAILED" : "PASS"}`);
process.exit(fails ? 1 : 0);
