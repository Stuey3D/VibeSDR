// test-server-bench-wide.mjs — the WIDE BENCHMARK's HTTP side on the real server (V12, 2026-11-18). A fake rtl_tcp radio
// stands in, configured as an RSP (so the server offers it) — the CPU half runs for real; the USB bus test cannot (a network
// radio is never released), and says so. Checks:
//   · GET /vibeserver/benchmark carries "wideOffer" (the RSP, its rates, its current rate) and nothing is saved by it;
//   · a radio that is not wide-capable is refused, with the reason;
//   · POST ?wide=8000000&radio=<serial> starts, reports progress, and saves "wide":{"<serial>":{…}} with the profiles and
//     a "usb" verdict — and a quick result already in benchmark.json is kept whole beside it.
// Needs VIBESERVER_BIN. Silent (no audio is played).
import net from 'node:net'; import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url'; import crypto from 'node:crypto';
const BIN = process.env.VIBESERVER_BIN || '', root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-benchwide-')); for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const cfgPath = path.join(dir, 'config.json');
const PASS = 'bench-wide-admin';
fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, fullMode: true, sharing: 'public', adminPass: PASS,
  radios: [{ serial: 'fakersp', driver: 'sdrplay', label: 'Test RSP', enabled: true, configured: true, users: 4, mode: 'locked',
             lockFreq: 98000000, lockRate: 2048000, rate: 2048000, freq: 98000000, demodMode: 'wfm' }] }));
// ★ A quick result already saved: the wide run must keep it whole.
const quick = { v: 1, at: 1794900000, rows: [{ id: 'dab', label: 'DAB+', rate: 2048000, pct: 6.5, threads: {} }],
  dabStations: { n: 9, perStationPct: 1.2, frontPct: 7, memKB: 4100, muxFrontPct: 5.5, ddc8Pct: 1.4, cores: 4 }, capture: { lockedMaxHz: 0, fullMaxHz: 0 } };
fs.writeFileSync(path.join(dir, 'benchmark.json'), JSON.stringify(quick));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const LOG = path.join(dir, 'server.log'); const log = fs.openSync(LOG, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'],
  { env: { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] });
const cleanup = async () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(500); fs.rmSync(dir, { recursive: true, force: true }); };
try {
  for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(1500);
  const base = `http://127.0.0.1:${port}/vibeserver/benchmark`;
  // ★ Admin only, as the page does it: a fresh nonce per request, HMAC'd with the password.
  const auth = async () => { const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
    return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`; };
  const noAuth = await fetch(base);
  ok(noAuth.status === 401, 'without the admin password: refused');
  const g = await (await fetch(`${base}?${await auth()}`)).json();
  const o = (g.wideOffer || [])[0];
  ok(!!o && o.serial === 'fakersp' && o.driver === 'sdrplay' && o.rates.includes(8000000) && o.current === 2048000,
     `GET carries the wide offer: the RSP, its rates, its current rate (${JSON.stringify(o || null)})`);
  ok(g.at === quick.at && !!g.dabStations, 'GET still returns the saved quick result');
  ok(!JSON.parse(fs.readFileSync(path.join(dir, 'benchmark.json'), 'utf8')).wideOffer, '…and the offer is never saved');

  const bad = await fetch(`${base}?wide=8000000&radio=nosuch&${await auth()}`, { method: 'POST' });
  ok(bad.status === 409 && /no such radio/.test((await bad.json()).error || ''), 'an unknown radio: refused');
  const badRate = await fetch(`${base}?wide=99000000000&radio=fakersp&${await auth()}`, { method: 'POST' });
  ok(badRate.status === 409 && /not a rate/.test((await badRate.json()).error || ''), 'a rate no radio offers: refused');

  const r = await fetch(`${base}?wide=8000000&radio=fakersp&${await auth()}`, { method: 'POST' });
  const rj = await r.json();
  ok(r.ok && rj.started && rj.wide, `POST wide starts (${JSON.stringify(rj)})`);
  if (!r.ok) throw new Error('the wide benchmark did not start');
  let sawRunning = false, maxSteps = 0, labels = new Set(), done = false;
  for (let i = 0; i < 120 && !done; i++) {
    await sleep(500);
    const p = await (await fetch(`${base}?progress=1&${await auth()}`)).json();
    if (p.running) { sawRunning = true; maxSteps = Math.max(maxSteps, p.steps); if (p.label) labels.add(p.label); }
    else if (sawRunning) done = true;
  }
  ok(sawRunning && done, 'progress: running, then finished');
  // ★ The USB step's label lasts a moment here (a network radio is never released, so the test is skipped): not asked for.
  ok(maxSteps >= 8 && [...labels].some((l) => /Locked range: WFM listeners @ 8 MS\/s/.test(l)) && [...labels].some((l) => /NFM listeners @ 8 MS\/s/.test(l)),
     `…its steps name the work (${[...labels].slice(0, 4).join(' | ')} …)`);
  const saved = JSON.parse(fs.readFileSync(path.join(dir, 'benchmark.json'), 'utf8'));
  const w = saved.wide && saved.wide.fakersp;
  ok(!!w && w.rate === 8000000 && w.label === 'Test RSP' && w.fixed && w.fixed.frontPct >= 0, 'saved under wide.<serial>: the rate, the radio, the fixed cost');
  ok(!!w && w.fm && w.fm.rate === 8000000 && typeof w.fm.n === 'number' && /cpu|ram|capture/.test(w.fm.by)
     && w.narrow && ['nfm', 'am', 'ssb'].includes(w.narrow.mode), `…the WFM and narrow profiles, with their bounds (fm ${w && w.fm && w.fm.n} by ${w && w.fm && w.fm.by})`);
  // ★ WFM alone (Stuart, 2026-11-19): the same run without Advanced RDS's cost — cheaper a listener, so never fewer users.
  ok(!!w && w.fmPlain && w.fmPlain.rate === 8000000 && w.fmPlain.perPct > 0 && w.fmPlain.perPct < w.fm.perPct && w.fmPlain.n >= w.fm.n,
     `…and WFM alone: cheaper a listener, never fewer users (${w && w.fmPlain && w.fmPlain.n} vs ${w && w.fm && w.fm.n} with Advanced RDS)`);
  ok(!!w && w.dab && w.dab.blocks >= 3, `…DAB from the saved quick figures (${w && w.dab ? w.dab.n + ' users, ' + w.dab.blocks + ' blocks' : 'none'})`);
  // ★★★ 2026-11-19: the capture rows moved here from the quick run — their verdict is the USRP's rate gate.
  const cv = w && w.capture;
  ok(!!cv && Array.isArray(cv.rates) && cv.rates.length === 1 && cv.rates[0].hz === 8000000 && typeof cv.lockedMaxHz === 'number'
     && typeof cv.fullMaxHz === 'number', `…the capture verdict at 8 MS/s, as the quick run used to write it (${cv && JSON.stringify({ l: cv.lockedMaxHz, f: cv.fullMaxHz })})`);
  // ★ Both paths measured (their steps are too quick to catch by polling: the verdict is the proof).
  ok(!!cv && cv.rates[0].fullHottestPct >= 0 && cv.rates[0].lockedHottestPct >= 0, '…both the full radio and the locked range measured at 8 MS/s');
  ok(!!w && w.usb && w.usb.ok === false && /cannot be taken off the air/.test(w.usb.notRun || ''), `…the USB test says why it did not run (${w && w.usb && w.usb.notRun})`);
  ok(saved.at === quick.at && JSON.stringify(saved.rows) === JSON.stringify(quick.rows) && JSON.stringify(saved.dabStations) === JSON.stringify(quick.dabStations),
     'the quick result is kept whole beside it');
  const g2 = await (await fetch(`${base}?${await auth()}`)).json();
  ok(g2.wide && g2.wide.fakersp && Array.isArray(g2.wideOffer), 'GET returns the wide result and the offer');
} finally { await cleanup(); }
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
