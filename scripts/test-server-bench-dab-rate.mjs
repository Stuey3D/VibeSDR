// test-server-bench-dab-rate.mjs — the QUICK benchmark's DAB wide-capture rows go no faster than the radios here (V12,
// Stuart 2026-11-19: "I dont even own a 56msps radio" — the filter-bank row measured 56 MS/s on every box, a Pi 2
// included, 83 % of an i5 core and ~134 MB of test signal). The real server, the real quick run, twice:
//   · an RTL (tops out at 3.2 MS/s: one DAB block) — the filter bank is NOT RUN and says why; the multiplex row is its
//     front end only (no 8 MS/s DDC); no row anywhere is measured above the RTL's fastest rate;
//   · an RSP (offers 8 MS/s and up) — the bank runs, at the RSP's own top rate and below, never 56 MS/s.
// Needs VIBESERVER_BIN and the DAB bench clip (VIBE_DAB_CLIP or vibeserver/bench-clip/dab-bench.vbu8); exit 3 = NOT RUN.
// Silent (no audio is played). ~2 × the quick benchmark's length.
import net from 'node:net'; import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url'; import crypto from 'node:crypto';
const BIN = process.env.VIBESERVER_BIN || '', root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const CLIP = process.env.VIBE_DAB_CLIP || path.join(root, 'vibeserver/bench-clip/dab-bench.vbu8');
if (!fs.existsSync(CLIP)) { console.log(`   not run — no DAB bench clip at ${CLIP}`); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const PASS = 'bench-dab-rate-admin';

async function quickRun(radio) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-benchdab-')); for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const cfgPath = path.join(dir, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, fullMode: true, sharing: 'public', adminPass: PASS, radios: [radio] }));
  fs.copyFileSync(CLIP, path.join(dir, 'dab-bench.vbu8'));   // ★ ensureDabClip finds it beside the config: nothing fetched
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'],
    { env: { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] });
  try {
    for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
    await sleep(1500);
    const base = `http://127.0.0.1:${port}/vibeserver/benchmark`;
    const auth = async () => { const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
      return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`; };
    const r = await fetch(`${base}?${await auth()}`, { method: 'POST' });
    if (!r.ok) throw new Error(`the quick benchmark did not start (${r.status} ${await r.text()})`);
    let sawRunning = false, done = false, labels = new Set();
    for (let i = 0; i < 1800 && !done; i++) {   // ★ up to 15 min
      await sleep(500);
      const p = await (await fetch(`${base}?progress=1&${await auth()}`)).json();
      if (p.running) { sawRunning = true; if (p.label) labels.add(p.label); } else if (sawRunning) done = true;
    }
    if (!done) throw new Error('the quick benchmark did not finish in 15 minutes');
    return { saved: JSON.parse(fs.readFileSync(path.join(dir, 'benchmark.json'), 'utf8')), labels };
  } finally {
    srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(500); fs.rmSync(dir, { recursive: true, force: true });
  }
}
const row = (s, id) => (s.rows || []).find((x) => x.id === id);
const thr = (rw, name) => { const t = rw && rw.threads; if (!t) return undefined; return Array.isArray(t) ? (t.find((x) => x[0] === name || x.name === name) || [])[1] : t[name]; };

try {
  // ── an RTL: one block at most ──
  console.log('   ── an RTL (3.2 MS/s at most)');
  const a = await quickRun({ serial: 'fakertl', driver: 'rtlsdr', label: 'Test RTL', enabled: true, configured: true, users: 4,
                             mode: 'full', rate: 2048000, freq: 98000000, demodMode: 'wfm' });
  const nr = (a.saved.notRun || []).find((n) => n.id === 'dec_dab_bank');
  ok(!row(a.saved, 'dec_dab_bank') && nr && /two DAB blocks/.test(nr.why), `the filter bank is not run, and says why (${nr && nr.why})`);
  ok(![...a.labels].some((l) => /filter bank/.test(l)), '…nor is it a step of the run');
  const mux = row(a.saved, 'dec_dab_mux');
  ok(!!mux && /front end/.test(mux.label) && !/MS\/s\)/.test(mux.label) && mux.pct > 0, `the multiplex row is its front end only (${mux && mux.label}, ${mux && mux.pct})`);
  ok(!!a.saved.dabStations && a.saved.dabStations.muxFrontPct > 0 && !(a.saved.dabStations.ddc8Pct > 0) && !a.saved.dabStations.bank,
     `dabStations keeps the front end, no 8 MS/s DDC, no bank (${JSON.stringify(a.saved.dabStations)})`);
  const fast = (a.saved.rows || []).filter((x) => x.rate > 3200000 && x.pct >= 0);
  ok(fast.length === 0, `no row measured above the RTL's 3.2 MS/s (${fast.map((x) => x.id + '@' + x.rate).join(', ') || 'none'})`);

  // ── an RSP: 8 MS/s and up, never 56 ──
  console.log('   ── an RSP (8 MS/s and up)');
  const b = await quickRun({ serial: 'fakersp', driver: 'sdrplay', label: 'Test RSP', enabled: true, configured: true, users: 4,
                             mode: 'full', rate: 2048000, freq: 98000000, demodMode: 'wfm' });
  const bank = row(b.saved, 'dec_dab_bank');
  ok(!!bank && bank.rate >= 8000000 && bank.rate < 56000000 && !/56 MS\/s/.test(bank.label), `the bank runs at the RSP's own top rate (${bank && bank.label} @ ${bank && bank.rate})`);
  const bk = b.saved.dabStations && b.saved.dabStations.bank;
  ok(!!bk && bk.rates.length >= 1 && Math.max(...bk.rates) <= bank.rate + 1, `…its ladder stops there (${bk && JSON.stringify(bk.rates)})`);
  const mb = row(b.saved, 'dec_dab_mux');
  ok(!!mb && b.saved.dabStations.ddc8Pct > 0, `the multiplex row cuts at 8 MS/s there (${mb && mb.label}, ddc8 ${b.saved.dabStations.ddc8Pct})`);
} catch (e) { fail++; console.log(`   FAIL ${e.message}`); }
console.log(`${fail ? '✗' : '✓'} test-server-bench-dab-rate: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
